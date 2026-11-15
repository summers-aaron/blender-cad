/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_cad_kernel
 *
 * NVIDIA SMLib (usd-brep kernel layer) backend of the CAD kernel interface.
 *
 * Design notes:
 * - SMLib has one process-global context whose state is not synchronized. Every call into
 *   SMLib from this file (including destroying an #SmBrep) happens under one recursive mutex,
 *   see #ApiLock. The mutex is recursive because releasing the last reference of a shape inside
 *   an operation destroys its B-rep, which locks again.
 * - Shapes are immutable once wrapped. SMLib operations work in place (booleans even delete
 *   their second operand), so every operation runs on copies (#SmApiBrepCopy keeps element
 *   order, which history relies on).
 * - Element order is the B-rep's own face / edge / vertex list order (#SmBrep::GetFaces etc.),
 *   which is deterministic: lists are insertion ordered and copies keep them.
 * - Every face and edge of a wrapped B-rep carries an #OriginTag with its own index. The
 *   tessellator copies edge attributes onto the polygon edges it makes, which is how mesh edges
 *   are attributed to B-rep edges.
 * - History follows the approach of SMLib's own attribute propagation: before an operation the
 *   faces and edges of each operand copy are tagged with what they are (an #OriginTag whose
 *   Split / Merge / Coincident handlers union the origins), the operation runs, and the tags of
 *   the result say exactly where each face and edge came from. Faces SMLib makes new (fillets,
 *   chamfers) carry no tag and are named after the picked edge whose two faces they separate.
 *   Extrusions are built face by face here, so their history is known by construction.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CAD_kernel.hh"

#include "../intern/kernel_backends.hh"

/* SMLib headers last: they define many short macros (TRUE, SmBoolean, single letters, ...). */
#include <SmApiBrep.h>
#include <SmApiCurves.h>
#include <SmApiGeneral.h>
#include <SmApiPrimitives.h>
#include <SmApiQueries.h>
#include <SmApiSurfaces.h>
#include <SmApiTrimmedSurfaces.h>
#include <SmAssertArray.h>
#include <SmAttribute.h>
#include <SmAxis2Placement.h>
#include <SmBrep.h>
#include <SmCircle.h>
#include <SmCone.h>
#include <SmCylinder.h>
#include <SmEdge.h>
#include <SmEllipse.h>
#include <SmFace.h>
#include <SmFaceuse.h>
#include <SmFilletCorner.h>
#include <SmFilletExecutive.h>
#include <SmFilletStandardSolver.h>
#include <SmLine.h>
#include <SmOffsetSurface.h>
#include <SmPlane.h>
#include <SmPoly.h>
#include <SmRegion.h>
#include <SmShell.h>
#include <SmSphere.h>
#include <SmSurfOfExtrusion.h>
#include <SmSurfOfRevolution.h>
#include <SmTorus.h>
#include <SmVertex.h>

namespace cad::kernel::smlib {

/* -------------------------------------------------------------------- */
/** \name Global Lock
 * \{ */

static std::recursive_mutex &api_mutex()
{
  static std::recursive_mutex mutex;
  return mutex;
}

/** Serializes SMLib calls and makes sure the global context exists. */
class ApiLock {
 public:
  ApiLock() : lock_(api_mutex())
  {
    static std::once_flag once;
    std::call_once(once, [] { SmApiCreateContext(); });
  }

 private:
  std::unique_lock<std::recursive_mutex> lock_;
};

static const SmContext &api_context()
{
  return *SmApiGetOrCreateContext();
}

/** Owns an #SmBrep; deletion happens under the lock. */
struct BrepDeleter {
  void operator()(SmBrep *brep) const
  {
    if (brep) {
      ApiLock lock;
      delete brep;
    }
  }
};
using BrepPtr = std::unique_ptr<SmBrep, BrepDeleter>;

/** Owns an #SmPolyBrep made from a B-rep; its back pointers into the B-rep are disabled before
 * deletion so the destructor does not reach into the B-rep. */
struct PolyBrepDeleter {
  void operator()(SmPolyBrep *poly) const
  {
    if (poly) {
      ApiLock lock;
      poly->SetOKBackPtrs(FALSE);
      delete poly;
    }
  }
};
using PolyBrepPtr = std::unique_ptr<SmPolyBrep, PolyBrepDeleter>;

/** Curves not (yet) handed over to a B-rep. */
class CurveList {
 public:
  CurveList() = default;
  CurveList(const CurveList &) = delete;
  CurveList &operator=(const CurveList &) = delete;
  ~CurveList()
  {
    clear();
  }
  void add(SmCurve *curve)
  {
    curves_.Add(curve);
  }
  SmTArray<SmCurve *> &array()
  {
    return curves_;
  }
  /** Ownership went to a B-rep. */
  void release()
  {
    curves_.SetSize(0);
  }
  void clear()
  {
    if (curves_.GetSize() > 0) {
      ApiLock lock;
      for (ULONG i = 0; i < curves_.GetSize(); i++) {
        delete curves_[i];
      }
      curves_.SetSize(0);
    }
  }

 private:
  SmTArray<SmCurve *> curves_;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Origin Tags
 *
 * An attribute holding a set of numbers, which SMLib carries through its edits: a split piece
 * keeps its parent's numbers, a merge or a coincidence (one boolean operand's element found on
 * the other's) unions them. SMLib's default handlers would keep only one side's attribute.
 * \{ */

/** Above SMLib's own attribute ids, which are all below #SM_AI_USER_1. */
constexpr ULONG INDEX_TAG_ID = SM_AI_USER_1 + 7101;
constexpr ULONG HISTORY_TAG_ID = SM_AI_USER_1 + 7102;

class OriginTag final : public SmAttribute {
 public:
  OriginTag(const ULONG id, std::vector<long> values)
      : SmAttribute(id, SM_AB_COPY), values_(std::move(values))
  {
    normalize();
  }

  SmAttribute *MakeCopy(const SmContext &context) const override
  {
    return new (context) OriginTag(m_lAttributeID, values_);
  }

  const std::vector<long> &values() const
  {
    return values_;
  }

  /* Persistence getters, so a B-rep written to an SMLib file keeps the numbers. */
  ULONG GetNumLongElements() const override
  {
    return ULONG(values_.size());
  }
  const long *GetLongElementsAddress() const override
  {
    return values_.empty() ? nullptr : values_.data();
  }

  void Split(SmAObject *split, SmAObject *child1, SmAObject *child2) override
  {
    for (SmAObject *child : {child1, child2}) {
      if (child && child != split) {
        join(child, m_lAttributeID, values_);
      }
    }
  }

  void Copy(const SmAObject * /*from*/, SmAObject *to) override
  {
    if (to) {
      join(to, m_lAttributeID, values_);
    }
  }

  void Merge(SmAObject *object1, SmAObject *object2, SmAObject *result) override
  {
    if (!result) {
      return;
    }
    /* `this` sits on one of the merged objects (the survivor when it has a tag); the result
     * stands for both. */
    for (SmAObject *object : {object1, object2}) {
      if (object && object != result) {
        if (const OriginTag *other = find(object, m_lAttributeID)) {
          join(result, m_lAttributeID, other->values_);
        }
      }
    }
    join(result, m_lAttributeID, values_);
  }

  void Coincident(SmAObject * /*from*/,
                  SmAObject *to,
                  SmAObject * /*from_brep*/,
                  SmAObject * /*to_brep*/) override
  {
    if (to) {
      join(to, m_lAttributeID, values_);
    }
  }

  static const OriginTag *find(const SmAObject *object, const ULONG id)
  {
    return dynamic_cast<const OriginTag *>(object->FindAttribute(id));
  }

  /** Replaces whatever tag `object` has with exactly `values`. */
  static void set(SmAObject *object, const ULONG id, std::vector<long> values)
  {
    if (SmAttribute *old = object->FindAttribute(id)) {
      object->RemoveAttribute(old);
    }
    object->AddAttribute(new (*object->GetContext()) OriginTag(id, std::move(values)));
  }

  static void remove(SmAObject *object, const ULONG id)
  {
    if (SmAttribute *old = object->FindAttribute(id)) {
      object->RemoveAttribute(old);
    }
  }

  static std::vector<long> read(const SmAObject *object, const ULONG id)
  {
    const OriginTag *tag = find(object, id);
    return tag ? tag->values_ : std::vector<long>();
  }

 private:
  std::vector<long> values_;

  void normalize()
  {
    std::sort(values_.begin(), values_.end());
    values_.erase(std::unique(values_.begin(), values_.end()), values_.end());
  }

  /** Adds `values` to the tag on `object`, creating it when missing. Takes `values` by value:
   * it may be the very tag being extended. */
  static void join(SmAObject *object, const ULONG id, std::vector<long> values)
  {
    if (values.empty()) {
      return;
    }
    SmAttribute *existing = object->FindAttribute(id);
    if (OriginTag *tag = dynamic_cast<OriginTag *>(existing)) {
      tag->values_.insert(tag->values_.end(), values.begin(), values.end());
      tag->normalize();
      return;
    }
    if (existing) {
      object->RemoveAttribute(existing);
    }
    object->AddAttribute(new (*object->GetContext()) OriginTag(id, std::move(values)));
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Conversions
 * \{ */

static SmVector3d to_sm(const Vec3 &v)
{
  return SmVector3d(v[0], v[1], v[2]);
}

static Vec3 to_vec3(const SmVector3d &v)
{
  return {v.x, v.y, v.z};
}

static double vec_length(const Vec3 &v)
{
  return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static double dot(const Vec3 &a, const Vec3 &b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static Vec3 cross(const Vec3 &a, const Vec3 &b)
{
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

static Vec3 scaled(const Vec3 &v, const double s)
{
  return {v[0] * s, v[1] * s, v[2] * s};
}

static Vec3 add(const Vec3 &a, const Vec3 &b)
{
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

static Vec3 sub(const Vec3 &a, const Vec3 &b)
{
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

static Vec3 normalized(const Vec3 &v)
{
  const double len = vec_length(v);
  return len > 0.0 ? scaled(v, 1.0 / len) : v;
}

/** Orthonormal right-handed frame of a sketch plane. */
struct PlaneFrame {
  Vec3 origin;
  Vec3 x;
  Vec3 y;
  Vec3 n;

  explicit PlaneFrame(const Plane &plane)
  {
    origin = plane.origin;
    n = normalized(plane.normal);
    /* Orthogonalize x against the normal so slightly inaccurate frames still work. */
    x = normalized(sub(plane.x_dir, scaled(n, dot(plane.x_dir, n))));
    y = cross(n, x);
  }

  /** World direction -> frame-local coordinates. */
  Vec3 to_local_dir(const Vec3 &d) const
  {
    return {dot(d, x), dot(d, y), dot(d, n)};
  }

  /** Placement mapping frame-local coordinates (shifted by `local_offset`) to world. */
  SmAxis2Placement placement(const Vec3 &local_offset) const
  {
    const Vec3 o = add(origin,
                       add(scaled(x, local_offset[0]),
                           add(scaled(y, local_offset[1]), scaled(n, local_offset[2]))));
    return SmAxis2Placement(to_sm(o), to_sm(x), to_sm(y));
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Error Handling
 * \{ */

static OpResult fail(const Status status, std::string message)
{
  OpResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

/** Runs `fn` under the SMLib lock, converting any exception into a failed #OpResult. */
template<typename Fn> static OpResult guarded(const char *op_name, Fn &&fn)
{
  try {
    ApiLock lock;
    return fn();
  }
  catch (const std::exception &e) {
    return fail(Status::Failed, std::string(op_name) + ": " + e.what());
  }
  catch (...) {
    return fail(Status::Failed, std::string(op_name) + ": unknown error");
  }
}

static bool ok(const SmApiStatus status)
{
  return status == SM_SUCCESS;
}

static std::string status_text(const SmApiStatus status)
{
  return "SMLib status " + std::to_string(long(status));
}

/**
 * Volume, area and centroid; false when SMLib cannot integrate the B-rep. The default relative
 * accuracy of 1e-8 is SMLib's recommended limit: about 1e-8 relative on curved solids (planar
 * ones come out exact), at a cost that grows fast below it (1e-9 is twice as slow, 1e-10 four
 * times).
 */
static bool compute_mass(const SmBrep *brep, MassProperties &r_mass, const double accuracy = 1e-8)
{
  double area = 0.0, volume = 0.0, mass = 0.0;
  SmPoint3d centroid(0.0, 0.0, 0.0);
  SmVector3d moments, products;
  for (const double attempt : {accuracy, accuracy * 100.0}) {
    if (ok(SmApiBrepComputeMassProperties(brep,
                                          attempt,
                                          1.0,
                                          SmPoint3d(0.0, 0.0, 0.0),
                                          area,
                                          volume,
                                          mass,
                                          centroid,
                                          moments,
                                          products)))
    {
      r_mass.volume = volume;
      r_mass.area = area;
      r_mass.center_of_mass = to_vec3(centroid);
      return true;
    }
  }
  return false;
}

/** Full validity check: a closed manifold solid passing SMLib's own topology and geometry
 * assertions, enclosing positive volume. */
static bool check_valid(SmBrep *brep, std::string *r_reason)
{
  auto reason = [&](std::string text) {
    if (r_reason) {
      *r_reason = std::move(text);
    }
    return false;
  };
  SmTArray<SmFace *> faces;
  brep->GetFaces(faces);
  if (faces.GetSize() == 0) {
    return reason("result has no faces");
  }
  if (!brep->IsManifoldSolid()) {
    return reason("result is not a closed manifold solid");
  }
  SmAssertArray asserts;
  if (!brep->AssertValid(&asserts)) {
    std::string text = "result failed SMLib's validity check";
    for (ULONG i = 0; i < asserts.GetSize(); i++) {
      const SmAssertReport *report = asserts[i];
      if (report && !report->m_bOK) {
        text += std::string(": ") + (report->m_pName ? report->m_pName : "") + " (" +
                (report->m_pMessage ? report->m_pMessage : "") + ")";
        break;
      }
    }
    return reason(text);
  }
  /* Only the sign matters here: a coarse integration is enough and much cheaper. */
  MassProperties mass;
  if (!compute_mass(brep, mass, 1e-3)) {
    return reason("result volume cannot be computed");
  }
  if (!(mass.volume > 0.0)) {
    return reason("result encloses no volume (inside out or empty)");
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shape Data
 * \{ */

static bool region_is_void(const SmRegion *region, const SmBrep *brep);

class SmlibShapeData final : public ShapeData {
 public:
  /** Takes ownership of `brep`; the caller holds the lock. Writes the index tags. */
  explicit SmlibShapeData(SmBrep *brep) : brep_(brep)
  {
    SmTArray<SmFace *> faces;
    SmTArray<SmEdge *> edges;
    SmTArray<SmVertex *> vertices;
    brep_->GetFaces(faces);
    brep_->GetEdges(edges);
    brep_->GetVertices(vertices);
    for (ULONG i = 0; i < faces.GetSize(); i++) {
      faces_.push_back(faces[i]);
      face_index_.emplace(faces[i], int(i));
    }
    for (ULONG i = 0; i < edges.GetSize(); i++) {
      edges_.push_back(edges[i]);
      edge_index_.emplace(edges[i], int(i));
    }
    for (ULONG i = 0; i < vertices.GetSize(); i++) {
      vertices_.push_back(vertices[i]);
      vertex_index_.emplace(vertices[i], int(i));
    }

    /* Each material region is one solid. */
    SmTArray<SmRegion *> regions;
    brep_->GetRegions(regions);
    for (ULONG i = 0; i < regions.GetSize(); i++) {
      SmRegion *region = regions[i];
      if (region && !region_is_void(region, brep_)) {
        solids_.push_back(region);
      }
    }
  }

  ~SmlibShapeData() override
  {
    ApiLock lock;
    delete brep_;
  }

  KernelId kernel() const override
  {
    return KernelId::SMLib;
  }
  int faces_num() const override
  {
    return int(faces_.size());
  }
  int edges_num() const override
  {
    return int(edges_.size());
  }
  int vertices_num() const override
  {
    return int(vertices_.size());
  }
  int solids_num() const override
  {
    return int(solids_.size());
  }

  SmBrep *brep() const
  {
    return brep_;
  }
  const std::vector<SmFace *> &faces() const
  {
    return faces_;
  }
  const std::vector<SmEdge *> &edges() const
  {
    return edges_;
  }
  const std::vector<SmVertex *> &vertices() const
  {
    return vertices_;
  }
  int face_index(const SmFace *face) const
  {
    const auto it = face_index_.find(face);
    return it == face_index_.end() ? -1 : it->second;
  }
  int edge_index(const SmEdge *edge) const
  {
    const auto it = edge_index_.find(edge);
    return it == edge_index_.end() ? -1 : it->second;
  }
  int vertex_index(const SmVertex *vertex) const
  {
    const auto it = vertex_index_.find(vertex);
    return it == vertex_index_.end() ? -1 : it->second;
  }
  int solid_of(const SmFace *face) const
  {
    SmFaceuse *fu[2] = {nullptr, nullptr};
    face->GetFaceuses(fu[0], fu[1]);
    for (SmFaceuse *use : fu) {
      if (use && use->GetShell()) {
        const auto it = std::find(solids_.begin(), solids_.end(), use->GetRegion());
        if (it != solids_.end()) {
          return int(it - solids_.begin());
        }
      }
    }
    return 0;
  }

  /** Strips history tags and writes index tags. Only while the shape is not yet shared. */
  void write_index_tags()
  {
    for (size_t i = 0; i < faces_.size(); i++) {
      OriginTag::remove(faces_[i], HISTORY_TAG_ID);
      OriginTag::set(faces_[i], INDEX_TAG_ID, {long(i)});
    }
    for (size_t i = 0; i < edges_.size(); i++) {
      OriginTag::remove(edges_[i], HISTORY_TAG_ID);
      OriginTag::set(edges_[i], INDEX_TAG_ID, {long(i)});
    }
  }

 private:
  SmBrep *brep_;
  std::vector<SmFace *> faces_;
  std::vector<SmEdge *> edges_;
  std::vector<SmVertex *> vertices_;
  std::unordered_map<const void *, int> face_index_;
  std::unordered_map<const void *, int> edge_index_;
  std::unordered_map<const void *, int> vertex_index_;
  std::vector<SmRegion *> solids_;
};

static const SmlibShapeData *smlib_data(const Shape &shape)
{
  if (shape.is_empty() || shape.kernel() != KernelId::SMLib) {
    return nullptr;
  }
  return static_cast<const SmlibShapeData *>(shape.data());
}

/** Tags read off a result before it is wrapped (wrapping replaces them). */
struct ResultTags {
  std::vector<std::vector<long>> faces;
  std::vector<std::vector<long>> edges;
  /** The index tags the result inherited from the operand it was copied from. */
  std::vector<std::vector<long>> face_index;
  std::vector<std::vector<long>> edge_index;
};

/**
 * Wraps a finished B-rep (ownership moves in), validating it. `r_tags` receives the history and
 * inherited index tags of its faces and edges, in the result's own (canonical) order.
 */
static OpResult finish(BrepPtr brep, ResultTags *r_tags = nullptr)
{
  if (!brep) {
    return fail(Status::Failed, "operation produced no shape");
  }
  SmTArray<SmFace *> faces;
  brep->GetFaces(faces);
  if (faces.GetSize() == 0) {
    return fail(Status::Failed, "operation produced an empty result");
  }
  std::string reason;
  const bool valid = check_valid(brep.get(), &reason);
  auto data = std::make_shared<SmlibShapeData>(brep.release());
  if (r_tags) {
    for (const SmFace *face : data->faces()) {
      r_tags->faces.push_back(OriginTag::read(face, HISTORY_TAG_ID));
      r_tags->face_index.push_back(OriginTag::read(face, INDEX_TAG_ID));
    }
    for (const SmEdge *edge : data->edges()) {
      r_tags->edges.push_back(OriginTag::read(edge, HISTORY_TAG_ID));
      r_tags->edge_index.push_back(OriginTag::read(edge, INDEX_TAG_ID));
    }
  }
  data->write_index_tags();

  OpResult result;
  result.shape = Shape(std::shared_ptr<const ShapeData>(std::move(data)));
  result.history.faces.resize(result.shape.faces_num());
  result.history.edges.resize(result.shape.edges_num());
  if (!valid) {
    result.status = Status::InvalidResult;
    result.message = reason;
    return result;
  }
  result.status = Status::Ok;
  return result;
}

/** A deep copy of a wrapped shape's B-rep, for an operation to modify. */
static BrepPtr copy_brep(const SmlibShapeData &data)
{
  SmBrep *copy = nullptr;
  if (!ok(SmApiBrepCopy(data.brep(), copy))) {
    delete copy;
    return nullptr;
  }
  return BrepPtr(copy);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Geometry Helpers
 * \{ */

/** Empty space: the infinite region or an enclosed cavity. */
static bool region_is_void(const SmRegion *region, const SmBrep *brep)
{
  return region == brep->GetInfiniteRegion() || region->IsVoid() == TRUE;
}

/**
 * +1 when the surface normal of `face` points out of the material, -1 otherwise. SMLib keeps one
 * surface per face; the surface normal points into the region of the face's upward face-use,
 * which is empty space for an outward normal (this is how SMLib's own faceting orients faces).
 */
static double face_sign(const SmFace *face)
{
  const SmBrep *brep = face->GetBrep();
  const SmFaceuse *upward = face->GetUpwardFaceuse();
  if (brep && upward && upward->GetShell() && upward->GetShell()->GetRegion() &&
      !region_is_void(upward->GetShell()->GetRegion(), brep))
  {
    return -1.0;
  }
  return 1.0;
}

/** Outward unit normal of `face` at the surface point nearest to `point`. */
static bool face_normal_near(const SmFace *face, const Vec3 &point, Vec3 &r_normal)
{
  const SmSurface *surface = face->GetSurface();
  if (!surface) {
    return false;
  }
  SmPoint3d closest;
  SmTArray<SmPoint2d> uvs;
  double distance = 0.0;
  if (!ok(SmApiSurfaceClosestPoint(surface, to_sm(point), closest, uvs, distance)) ||
      uvs.GetSize() == 0)
  {
    return false;
  }
  SmVector3d normal;
  if (!ok(SmApiEvaluateSurfaceNormal(surface, SmVector2d(uvs[0].x, uvs[0].y), normal))) {
    return false;
  }
  const Vec3 n = normalized(to_vec3(normal));
  if (vec_length(n) < 0.5) {
    return false;
  }
  r_normal = scaled(n, face_sign(face));
  return true;
}

static Vec3 vertex_point(const SmVertex *vertex)
{
  return to_vec3(vertex->GetPoint());
}

static bool edge_point_at(const SmEdge *edge, const double t, Vec3 &r_point)
{
  const SmCurve *curve = edge->GetCurve();
  if (!curve) {
    return false;
  }
  SmPoint3d p;
  if (!ok(curve->EvaluatePoint(t, p))) {
    return false;
  }
  r_point = to_vec3(p);
  return true;
}

static Vec3 edge_midpoint(const SmEdge *edge)
{
  const SmExtent1d interval = edge->GetInterval();
  Vec3 p;
  if (edge_point_at(edge, 0.5 * (interval.GetMin() + interval.GetMax()), p)) {
    return p;
  }
  return scaled(add(vertex_point(edge->GetStartVertex()), vertex_point(edge->GetEndVertex())),
                0.5);
}

static bool edge_is_degenerate(const SmEdge *edge)
{
  const SmCurve *curve = edge->GetCurve();
  if (!curve) {
    return true;
  }
  const SmExtent1d interval = edge->GetInterval();
  return curve->IsDegenerate(1e-9, &interval);
}

static std::vector<SmFace *> edge_faces(const SmEdge *edge)
{
  SmTArray<SmFace *> faces;
  edge->GetFaces(faces);
  std::vector<SmFace *> result;
  for (ULONG i = 0; i < faces.GetSize(); i++) {
    if (std::find(result.begin(), result.end(), faces[i]) == result.end()) {
      result.push_back(faces[i]);
    }
  }
  return result;
}

static std::vector<SmEdge *> face_edges(const SmFace *face)
{
  SmTArray<SmEdge *> edges;
  face->GetEdges(edges);
  std::vector<SmEdge *> result;
  for (ULONG i = 0; i < edges.GetSize(); i++) {
    if (std::find(result.begin(), result.end(), edges[i]) == result.end()) {
      result.push_back(edges[i]);
    }
  }
  return result;
}

/**
 * Area and area centroid of `face`. Not #SmApiFaceComputeCentroid: that integrates about the
 * face's own bounding box center with an accuracy relative to an area estimated from the face
 * box, which crawls on closed faces (a hole's cylinder: seconds). Integrating like SmLib's whole
 * B-rep mass properties do (B-rep box center, B-rep area estimate) is fast and as accurate.
 */
static bool face_area_centroid(const SmFace *face, double &r_area, SmPoint3d &r_centroid)
{
  const SmBrep *brep = face->GetBrep();
  SmExtent3d box;
  if (!brep || !ok(brep->CalculateBoundingBox(box))) {
    return false;
  }
  const SmVector3d size = box.GetSize();
  const double extent = std::max({size.x, size.y, size.z});
  const double estimated_area = extent * extent / 100.0;
  const SmPoint3d origin = box.GetMid();
  for (const double accuracy : {1e-9, 1e-6}) {
    double area = 0.0, volume = 0.0;
    SmTArray<SmVector3d> moments;
    if (ok(face->ComputePreciseProperties(SM_OT_SAME,
                                          accuracy,
                                          origin,
                                          estimated_area,
                                          0.0,
                                          area,
                                          volume,
                                          moments,
                                          SM_PPF_AREA | SM_PPF_AREA_FIRST_MOMENTS)) &&
        moments.GetSize() > 0 && std::abs(area) > SM_EFF_ZERO)
    {
      r_centroid = origin + moments[0] / area;
      r_area = std::abs(area);
      return true;
    }
  }
  return false;
}

static FaceInfo face_info(const SmFace *face)
{
  FaceInfo info;
  const SmSurface *surface = face->GetSurface();
  const double sign = face_sign(face);
  if (surface) {
    switch (surface->GetType()) {
      case SmPlane_TYPE: {
        info.type = SurfaceType::Plane;
        const SmAxis2Placement &position = static_cast<const SmPlane *>(surface)->GetPosition();
        info.axis_origin = to_vec3(position.GetOriginRef());
        info.axis_dir = scaled(
            normalized(cross(to_vec3(position.GetXAxisRef()), to_vec3(position.GetYAxisRef()))),
            sign);
        break;
      }
      case SmCylinder_TYPE: {
        info.type = SurfaceType::Cylinder;
        const SmCylinder *cylinder = static_cast<const SmCylinder *>(surface);
        info.axis_origin = to_vec3(cylinder->GetOrigin());
        info.axis_dir = normalized(to_vec3(cylinder->GetZAxis()));
        info.radius = cylinder->GetRadius();
        break;
      }
      case SmCone_TYPE: {
        /* SMLib fillets of straight edges come out as cones with a zero semi-angle: report those
         * as the cylinders they are. */
        const SmCone *cone = static_cast<const SmCone *>(surface);
        SmAxis2Placement position;
        double radius = 0.0, semi_angle_deg = 0.0;
        if (ok(cone->GetCanonical(position, radius, semi_angle_deg))) {
          info.type = std::abs(semi_angle_deg) < 1e-9 ? SurfaceType::Cylinder : SurfaceType::Cone;
          info.axis_origin = to_vec3(position.GetOriginRef());
          info.axis_dir = normalized(to_vec3(position.GetZAxis()));
          info.radius = radius;
        }
        else {
          info.type = SurfaceType::Cone;
          info.axis_origin = to_vec3(cone->GetOrigin());
          info.axis_dir = normalized(to_vec3(cone->GetZAxis()));
          info.radius = cone->GetBotRadius();
        }
        break;
      }
      case SmSphere_TYPE: {
        info.type = SurfaceType::Sphere;
        const SmSphere *sphere = static_cast<const SmSphere *>(surface);
        info.axis_origin = to_vec3(sphere->GetOrigin());
        info.axis_dir = normalized(to_vec3(sphere->GetZAxis()));
        info.radius = sphere->GetRadius();
        break;
      }
      case SmTorus_TYPE: {
        info.type = SurfaceType::Torus;
        const SmTorus *torus = static_cast<const SmTorus *>(surface);
        info.axis_origin = to_vec3(torus->GetOrigin());
        info.axis_dir = normalized(to_vec3(torus->GetZAxis()));
        info.radius = torus->GetMajorRadius();
        break;
      }
      case SmSurfOfRevolution_TYPE: {
        info.type = SurfaceType::Revolution;
        const SmSurfOfRevolution *revolution = static_cast<const SmSurfOfRevolution *>(surface);
        info.axis_origin = to_vec3(revolution->GetOrigin());
        info.axis_dir = normalized(to_vec3(revolution->GetZAxis()));
        break;
      }
      case SmSurfOfExtrusion_TYPE:
        info.type = SurfaceType::Extrusion;
        break;
      case SmOffsetSurface_TYPE:
        info.type = SurfaceType::Offset;
        break;
      case SmBSplineSurface_TYPE:
        info.type = SurfaceType::BSpline;
        break;
      default:
        info.type = SurfaceType::Other;
        break;
    }
  }

  double area = 0.0;
  SmPoint3d centroid(0.0, 0.0, 0.0);
  if (face_area_centroid(face, area, centroid)) {
    info.area = area;
    info.centroid = to_vec3(centroid);
  }

  SmPoint3d inner;
  if (ok(SmApiFaceInternalPoint(face, inner))) {
    info.sample_point = to_vec3(inner);
  }
  else {
    info.sample_point = info.centroid;
  }
  Vec3 normal;
  if (face_normal_near(face, info.sample_point, normal)) {
    info.sample_normal = normal;
  }
  return info;
}

/** Tangent continuity across `edge`, from the outward normals of its faces at its midpoint. */
static bool edge_is_smooth(const SmEdge *edge, const std::vector<SmFace *> &faces)
{
  if (faces.size() == 1) {
    /* Seam (the face meets itself) or a lamina edge. */
    return edge->IsSeam();
  }
  if (faces.size() != 2) {
    return false;
  }
  const Vec3 mid = edge_midpoint(edge);
  Vec3 n0, n1;
  if (!face_normal_near(faces[0], mid, n0) || !face_normal_near(faces[1], mid, n1)) {
    return false;
  }
  constexpr double smooth_angle = 1e-3;
  const double angle = std::acos(std::clamp(dot(n0, n1), -1.0, 1.0));
  return angle < smooth_angle;
}

static EdgeInfo edge_info(const SmEdge *edge, const SmlibShapeData &data)
{
  EdgeInfo info;
  const std::vector<SmFace *> faces = edge_faces(edge);
  for (size_t i = 0; i < faces.size() && i < 2; i++) {
    info.faces[i] = data.face_index(faces[i]);
  }
  if (faces.size() == 1 && edge->IsSeam()) {
    info.faces[1] = info.faces[0];
  }
  const SmVertex *v0 = edge->GetStartVertex();
  const SmVertex *v1 = edge->GetEndVertex();
  if (v0) {
    info.vertices[0] = data.vertex_index(v0);
    info.start = vertex_point(v0);
  }
  if (v1) {
    info.vertices[1] = data.vertex_index(v1);
    info.end = vertex_point(v1);
  }
  info.degenerate = edge_is_degenerate(edge);
  if (info.degenerate) {
    info.midpoint = info.start;
    info.smooth = true;
    return info;
  }

  const SmExtent1d interval = edge->GetInterval();
  edge_point_at(edge, interval.GetMin(), info.start);
  edge_point_at(edge, interval.GetMax(), info.end);
  info.midpoint = edge_midpoint(edge);

  const SmCurve *curve = edge->GetCurve();
  switch (curve->GetType()) {
    case SmLine_TYPE:
      info.type = CurveType::Line;
      break;
    case SmCircle_TYPE: {
      info.type = CurveType::Circle;
      const SmCircle *circle = static_cast<const SmCircle *>(curve);
      const SmAxis2Placement &position = circle->GetPosition();
      info.center = to_vec3(position.GetOriginRef());
      info.axis = normalized(
          cross(to_vec3(position.GetXAxisRef()), to_vec3(position.GetYAxisRef())));
      info.radius = circle->GetRadius();
      break;
    }
    case SmEllipse_TYPE:
      info.type = CurveType::Ellipse;
      break;
    default: {
      /* Blends build straight and circular edges as splines: report what they are. */
      info.type = curve->IsKindOf(SmBSplineCurve_TYPE) ? CurveType::BSpline : CurveType::Other;
      const double tol = 1e-9 * std::max(1.0, vec_length(sub(info.end, info.start)));
      SmAxis2Placement frame;
      double radius = 0.0, a0 = 0.0, a1 = 0.0;
      if (curve->IsLinear(tol)) {
        info.type = CurveType::Line;
      }
      else if (curve->IsArc(16, 1e-7, frame, radius, a0, a1)) {
        info.type = CurveType::Circle;
        info.center = to_vec3(frame.GetOriginRef());
        info.axis = normalized(to_vec3(frame.GetZAxis()));
        info.radius = radius;
      }
      break;
    }
  }

  double length = 0.0;
  if (ok(SmApiEdgeComputeLength(edge, 1e-9, length))) {
    info.length = length;
  }
  info.smooth = edge_is_smooth(edge, faces);
  return info;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name History
 * \{ */

/** Sources referenced by history tags: a tag value is an index into this table. */
class SourceTable {
 public:
  long add(const SourceRef &source)
  {
    const auto it = std::find(sources_.begin(), sources_.end(), source);
    if (it != sources_.end()) {
      return long(it - sources_.begin());
    }
    sources_.push_back(source);
    return long(sources_.size()) - 1;
  }
  const SourceRef *get(const long value) const
  {
    return (value >= 0 && value < long(sources_.size())) ? &sources_[size_t(value)] : nullptr;
  }

 private:
  std::vector<SourceRef> sources_;
};

/** Tags face i / edge i of `brep` (a copy of input `input`) as face i / edge i of that input. */
static void tag_operand(SmBrep *brep, const int8_t input, SourceTable &table)
{
  SmTArray<SmFace *> faces;
  SmTArray<SmEdge *> edges;
  brep->GetFaces(faces);
  brep->GetEdges(edges);
  for (ULONG i = 0; i < faces.GetSize(); i++) {
    OriginTag::set(faces[i], HISTORY_TAG_ID, {table.add({SourceKind::Face, input, long(i)})});
  }
  for (ULONG i = 0; i < edges.GetSize(); i++) {
    OriginTag::set(edges[i], HISTORY_TAG_ID, {table.add({SourceKind::Edge, input, long(i)})});
  }
}

static void add_source(ElementHistory &h, const SourceRef &source)
{
  if (std::find(h.sources.begin(), h.sources.end(), source) == h.sources.end()) {
    h.sources.push_back(source);
  }
}

static bool same_point(const Vec3 &a, const Vec3 &b, const double tol)
{
  return vec_length(sub(a, b)) <= tol;
}

/**
 * History of a modifying operation (boolean, fillet, chamfer) from the tags on its result.
 * Faces and edges without a tag are left #ElementOrigin::Generated without sources, for the
 * caller to resolve.
 */
static void history_from_tags(const std::vector<const SmlibShapeData *> &inputs,
                              const SmlibShapeData &result,
                              const ResultTags &tags,
                              const SourceTable &table,
                              OpHistory &history)
{
  history.exact = true;
  constexpr double tol = 1e-7;

  /* How many result elements stand for each source. */
  std::map<long, int> face_uses, edge_uses;
  for (const std::vector<long> &t : tags.faces) {
    for (const long v : t) {
      face_uses[v]++;
    }
  }
  for (const std::vector<long> &t : tags.edges) {
    for (const long v : t) {
      edge_uses[v]++;
    }
  }

  auto input_of = [&](const SourceRef &s) -> const SmlibShapeData * {
    return (s.input >= 0 && s.input < int8_t(inputs.size())) ? inputs[s.input] : nullptr;
  };

  /* Edges first: a face is only kept when all its edges are. */
  std::vector<bool> edge_kept(result.edges().size(), false);
  for (size_t i = 0; i < result.edges().size(); i++) {
    ElementHistory &h = history.edges[i];
    const std::vector<long> &t = tags.edges[i];
    for (const long v : t) {
      if (const SourceRef *s = table.get(v)) {
        add_source(h, *s);
      }
    }
    if (h.sources.empty()) {
      h.origin = ElementOrigin::Generated;
      continue;
    }
    if (h.sources.size() > 1) {
      h.origin = ElementOrigin::Merged;
      continue;
    }
    if (edge_uses[t[0]] > 1) {
      h.origin = ElementOrigin::Split;
      continue;
    }
    /* One piece of one input edge: kept when it still runs between the same points. */
    const SourceRef &s = h.sources[0];
    const SmlibShapeData *input = input_of(s);
    const SmEdge *edge = result.edges()[i];
    bool kept = false;
    if (input && s.index >= 0 && s.index < long(input->edges().size())) {
      const SmEdge *original = input->edges()[size_t(s.index)];
      const Vec3 a = vertex_point(edge->GetStartVertex());
      const Vec3 b = vertex_point(edge->GetEndVertex());
      const Vec3 oa = vertex_point(original->GetStartVertex());
      const Vec3 ob = vertex_point(original->GetEndVertex());
      kept = (same_point(a, oa, tol) && same_point(b, ob, tol)) ||
             (same_point(a, ob, tol) && same_point(b, oa, tol));
      kept = kept && same_point(edge_midpoint(edge), edge_midpoint(original), tol);
    }
    h.origin = kept ? ElementOrigin::Kept : ElementOrigin::Modified;
    edge_kept[i] = kept;
  }

  for (size_t i = 0; i < result.faces().size(); i++) {
    ElementHistory &h = history.faces[i];
    const std::vector<long> &t = tags.faces[i];
    for (const long v : t) {
      if (const SourceRef *s = table.get(v)) {
        add_source(h, *s);
      }
    }
    if (h.sources.empty()) {
      h.origin = ElementOrigin::Generated;
      continue;
    }
    if (h.sources.size() > 1) {
      h.origin = ElementOrigin::Merged;
      continue;
    }
    if (face_uses[t[0]] > 1) {
      h.origin = ElementOrigin::Split;
      continue;
    }
    /* One input face, whole: kept when its boundary is exactly the edges it had, untouched. */
    const SourceRef &s = h.sources[0];
    const SmlibShapeData *input = input_of(s);
    bool kept = false;
    if (input && s.index >= 0 && s.index < long(input->faces().size())) {
      std::set<long> before;
      for (const SmEdge *edge : face_edges(input->faces()[size_t(s.index)])) {
        before.insert(input->edge_index(edge));
      }
      std::set<long> now;
      kept = true;
      for (const SmEdge *edge : face_edges(result.faces()[i])) {
        const int ei = result.edge_index(edge);
        if (ei < 0 || !edge_kept[size_t(ei)]) {
          kept = false;
          break;
        }
        const SourceRef &es = history.edges[size_t(ei)].sources[0];
        if (es.input != s.input) {
          kept = false;
          break;
        }
        now.insert(es.index);
      }
      kept = kept && now == before;
    }
    h.origin = kept ? ElementOrigin::Kept : ElementOrigin::Modified;
  }
}

/** New edges (no tag) come from where their faces meet: they inherit both faces' sources. */
static void resolve_new_edges(const SmlibShapeData &result, OpHistory &history)
{
  for (size_t i = 0; i < result.edges().size(); i++) {
    ElementHistory &h = history.edges[i];
    if (!h.sources.empty()) {
      continue;
    }
    h.origin = ElementOrigin::Generated;
    for (const SmFace *face : edge_faces(result.edges()[i])) {
      const int fi = result.face_index(face);
      if (fi < 0) {
        continue;
      }
      for (const SourceRef &s : history.faces[size_t(fi)].sources) {
        add_source(h, s);
      }
    }
  }
}

/**
 * Names the faces a blend (fillet / chamfer) made. SMLib gives them no tag. A blend face is
 * generated from a picked edge whose two faces both lie next to it and no longer meet each
 * other directly. A corner patch between several blends matches no edge: it is generated from
 * the vertex those blends' edges share.
 */
static void resolve_blend_faces(const SmlibShapeData &input,
                                const std::vector<int> &picked,
                                const SmlibShapeData &result,
                                OpHistory &history)
{
  const size_t faces_num = result.faces().size();

  /* Input faces of each result face (from the tags). */
  auto origins = [&](const size_t f) {
    std::set<long> out;
    for (const SourceRef &s : history.faces[f].sources) {
      if (s.kind == SourceKind::Face && s.input == 0) {
        out.insert(s.index);
      }
    }
    return out;
  };

  std::vector<std::set<size_t>> neighbors(faces_num);
  std::set<std::pair<long, long>> meeting;
  for (const SmEdge *edge : result.edges()) {
    const std::vector<SmFace *> faces = edge_faces(edge);
    for (const SmFace *a : faces) {
      for (const SmFace *b : faces) {
        const int ia = result.face_index(a), ib = result.face_index(b);
        if (ia >= 0 && ib >= 0 && ia != ib) {
          neighbors[size_t(ia)].insert(size_t(ib));
        }
      }
    }
    if (faces.size() == 2) {
      const int ia = result.face_index(faces[0]), ib = result.face_index(faces[1]);
      if (ia >= 0 && ib >= 0) {
        for (const long x : origins(size_t(ia))) {
          for (const long y : origins(size_t(ib))) {
            meeting.insert({std::min(x, y), std::max(x, y)});
          }
        }
      }
    }
  }

  /* The two input faces of each picked edge. */
  std::map<int, std::pair<long, long>> picked_faces;
  for (const int e : picked) {
    const std::vector<SmFace *> faces = edge_faces(input.edges()[size_t(e)]);
    if (faces.size() == 2) {
      picked_faces[e] = {input.face_index(faces[0]), input.face_index(faces[1])};
    }
  }

  std::vector<std::vector<int>> blend_edges(faces_num);
  for (size_t f = 0; f < faces_num; f++) {
    if (history.faces[f].origin != ElementOrigin::Generated || !history.faces[f].sources.empty())
    {
      continue;
    }
    std::set<long> around;
    for (const size_t n : neighbors[f]) {
      const std::set<long> o = origins(n);
      around.insert(o.begin(), o.end());
    }
    for (const auto &[e, pair] : picked_faces) {
      const auto [a, b] = pair;
      if (around.count(a) && around.count(b) && !meeting.count({std::min(a, b), std::max(a, b)}))
      {
        blend_edges[f].push_back(e);
      }
    }
    for (const int e : blend_edges[f]) {
      add_source(history.faces[f], {SourceKind::Edge, 0, e});
    }
    if (blend_edges[f].size() > 1) {
      /* Several picked edges fit the faces around it: topology alone cannot tell which. */
      history.exact = false;
    }
  }

  /* Corner patches: next to several blend faces whose edges share a vertex. */
  for (size_t f = 0; f < faces_num; f++) {
    ElementHistory &h = history.faces[f];
    if (h.origin != ElementOrigin::Generated || !h.sources.empty()) {
      continue;
    }
    std::set<int> near_edges;
    for (const size_t n : neighbors[f]) {
      near_edges.insert(blend_edges[n].begin(), blend_edges[n].end());
    }
    std::map<int, int> vertex_count;
    for (const int e : near_edges) {
      const SmEdge *edge = input.edges()[size_t(e)];
      std::set<int> ends = {input.vertex_index(edge->GetStartVertex()),
                            input.vertex_index(edge->GetEndVertex())};
      for (const int v : ends) {
        vertex_count[v]++;
      }
    }
    std::vector<int> shared;
    for (const auto &[v, count] : vertex_count) {
      if (v >= 0 && count >= 2) {
        shared.push_back(v);
      }
    }
    if (shared.size() == 1) {
      add_source(h, {SourceKind::Vertex, 0, shared[0]});
    }
    else {
      /* Not pinned down by topology: name it after the blends next to it. */
      for (const int e : near_edges) {
        add_source(h, {SourceKind::Edge, 0, e});
      }
      history.exact = false;
    }
    if (h.sources.empty()) {
      h.origin = ElementOrigin::Unknown;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Profiles
 * \{ */

constexpr double profile_tolerance = 1e-6;

static Vec2 curve_start(const ProfileCurve &curve)
{
  if (curve.type == ProfileCurveType::BSpline && !curve.poles.empty()) {
    return curve.poles.front();
  }
  return curve.start;
}

static Vec2 curve_end(const ProfileCurve &curve)
{
  if (curve.type == ProfileCurveType::BSpline && !curve.poles.empty()) {
    return curve.poles.back();
  }
  return curve.end;
}

static double dist2d(const Vec2 &a, const Vec2 &b)
{
  return std::hypot(a[0] - b[0], a[1] - b[1]);
}

/** The radius of an arc: given, or from its center and start. */
static double arc_radius(const ProfileCurve &curve)
{
  return curve.radius > 0.0 ? curve.radius : dist2d(curve.center, curve.start);
}

/** Checks that `loop` is a closed, ordered chain of well-formed curves. */
static Status validate_loop(const ProfileLoop &loop, std::string &r_error)
{
  if (loop.curves.empty()) {
    r_error = "empty profile loop";
    return Status::InvalidInput;
  }
  for (const ProfileCurve &curve : loop.curves) {
    const std::string name = "profile curve " + std::to_string(curve.id);
    switch (curve.type) {
      case ProfileCurveType::Line:
        if (dist2d(curve.start, curve.end) <= profile_tolerance) {
          r_error = name + " has zero length";
          return Status::InvalidInput;
        }
        break;
      case ProfileCurveType::Circle:
        if (!(curve.radius > profile_tolerance)) {
          r_error = name + " has no radius";
          return Status::InvalidInput;
        }
        break;
      case ProfileCurveType::Arc: {
        /* Arcs are built from their center, radius and end angles: the ends must lie on the
         * circle, or the arc would not meet its neighbours. */
        const double radius = arc_radius(curve);
        const double tol = profile_tolerance * std::max(1.0, radius);
        if (!(radius > profile_tolerance) ||
            std::abs(dist2d(curve.center, curve.start) - radius) > tol ||
            std::abs(dist2d(curve.center, curve.end) - radius) > tol ||
            dist2d(curve.start, curve.end) <= profile_tolerance)
        {
          r_error = name + " is not a proper arc";
          return Status::InvalidInput;
        }
        break;
      }
      case ProfileCurveType::BSpline:
        if (curve.periodic) {
          r_error = "periodic profile splines are not supported by the SMLib backend yet";
          return Status::Unsupported;
        }
        if (curve.poles.size() < 2 || curve.degree < 1 ||
            curve.knots.size() != curve.poles.size() + size_t(curve.degree) + 1 ||
            (!curve.weights.empty() && curve.weights.size() != curve.poles.size()))
        {
          r_error = name + " is a malformed spline";
          return Status::InvalidInput;
        }
        break;
    }
  }
  if (loop.curves.size() == 1) {
    const ProfileCurve &curve = loop.curves[0];
    if (curve.type != ProfileCurveType::Circle &&
        dist2d(curve_start(curve), curve_end(curve)) > profile_tolerance)
    {
      r_error = "profile loop is not closed";
      return Status::InvalidInput;
    }
    return Status::Ok;
  }
  for (size_t i = 0; i < loop.curves.size(); i++) {
    const ProfileCurve &curve = loop.curves[i];
    const ProfileCurve &next = loop.curves[(i + 1) % loop.curves.size()];
    if (curve.type == ProfileCurveType::Circle) {
      r_error = "a profile circle must be a loop of its own";
      return Status::InvalidInput;
    }
    if (dist2d(curve_end(curve), curve_start(next)) > profile_tolerance) {
      r_error = (i + 1 == loop.curves.size()) ?
                    std::string("profile loop is not closed") :
                    "profile curve " + std::to_string(next.id) + " does not connect to the loop";
      return Status::InvalidInput;
    }
  }
  return Status::Ok;
}

/**
 * Brings a counter-clockwise angle range (degrees) into the form SMLib's analytic circles and
 * cylinders accept: start in [-360, 360), end above start by at most a turn, end <= 360.
 */
static void normalize_arc_degrees(double &a0, double &a1)
{
  a0 = std::fmod(a0, 360.0);
  if (a0 < 0.0) {
    a0 += 360.0;
  }
  a1 = std::fmod(a1, 360.0);
  if (a1 < 0.0) {
    a1 += 360.0;
  }
  if (a1 <= a0 + 1e-12) {
    a1 += 360.0;
  }
  if (a1 > 360.0 + 1e-12) {
    a0 -= 360.0;
    a1 -= 360.0;
  }
}

/** Builds the 3D curve of a profile curve in frame-local coordinates (the plane is z = 0). */
static SmCurve *make_profile_curve(const ProfileCurve &curve, std::string &r_error)
{
  const SmContext &context = api_context();
  switch (curve.type) {
    case ProfileCurveType::Line: {
      SmLine *line = nullptr;
      if (ok(SmApiCreateLineSegment(SmPoint3d(curve.start[0], curve.start[1], 0.0),
                                    SmPoint3d(curve.end[0], curve.end[1], 0.0),
                                    line)))
      {
        return line;
      }
      break;
    }
    case ProfileCurveType::Circle: {
      SmCircle *circle = nullptr;
      if (ok(SmApiCreateCircle(
              SmPoint3d(curve.center[0], curve.center[1], 0.0), curve.radius, circle)))
      {
        return circle;
      }
      break;
    }
    case ProfileCurveType::Arc: {
      const double radius = arc_radius(curve);
      double a0 = std::atan2(curve.start[1] - curve.center[1], curve.start[0] - curve.center[0]);
      double a1 = std::atan2(curve.end[1] - curve.center[1], curve.end[0] - curve.center[0]);
      if (!curve.ccw) {
        /* SMLib arcs run counter-clockwise: build the same arc from its end. */
        std::swap(a0, a1);
      }
      a0 *= 180.0 / M_PI;
      a1 *= 180.0 / M_PI;
      normalize_arc_degrees(a0, a1);
      /* An analytic circle trimmed to the arc, so extrusions get cylinders. */
      SmAxis2Placement frame;
      frame.SetCanonical(SmPoint3d(curve.center[0], curve.center[1], 0.0),
                         SmVector3d(1.0, 0.0, 0.0),
                         SmVector3d(0.0, 1.0, 0.0));
      SmCircle *circle = nullptr;
      if (ok(SmCircle::CreateCanonical(context, frame, radius, circle)) && circle) {
        if (ok(circle->AdjustSTEPInterval(SmExtent1d(a0, a1)))) {
          return circle;
        }
        delete circle;
      }
      SmBSplineCurve *arc = nullptr;
      if (ok(SmApiCreateArc(
              SmPoint3d(curve.center[0], curve.center[1], 0.0), radius, a0, a1, arc)))
      {
        return arc;
      }
      break;
    }
    case ProfileCurveType::BSpline: {
      SmTArray<SmPoint3d> poles;
      for (const Vec2 &p : curve.poles) {
        poles.Add(SmPoint3d(p[0], p[1], 0.0));
      }
      SmTArray<double> knots;
      SmTArray<ULONG> mults;
      for (const double k : curve.knots) {
        if (knots.GetSize() > 0 && std::abs(k - knots[knots.GetSize() - 1]) < 1e-12) {
          mults[mults.GetSize() - 1]++;
        }
        else {
          knots.Add(k);
          mults.Add(1);
        }
      }
      SmTArray<double> weights;
      for (const double w : curve.weights) {
        weights.Add(w);
      }
      SmBSplineCurve *spline = nullptr;
      if (ok(SmApiCreateCanonicalCurve(poles,
                                       knots,
                                       mults,
                                       curve.weights.empty() ? nullptr : &weights,
                                       ULONG(curve.degree),
                                       SM_CF_UNSPECIFIED,
                                       spline)))
      {
        return spline;
      }
      break;
    }
  }
  r_error = "could not build profile curve " + std::to_string(curve.id);
  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Extrusion
 * \{ */

/**
 * The lateral face swept by `curve` along `dir` (unit, frame-local) over `distance` > 0, using
 * analytic surfaces where SMLib has them, like SMLib's own linear sweep does.
 */
static SmSurface *make_side_surface(const SmCurve *curve,
                                    const SmVector3d &dir,
                                    const double distance,
                                    const double tol)
{
  const SmContext &context = api_context();
  const SmBSplineCurve *bspline = dynamic_cast<const SmBSplineCurve *>(curve);
  if (!bspline) {
    return nullptr;
  }

  if (curve->IsLinear()) {
    /* SMLib planes are rectangles: only for a sweep perpendicular to the line. */
    SmPoint3d p0, p1;
    curve->GetEnds(p0, p1);
    SmVector3d along = p1 - p0;
    const double length = along.Length();
    if (length > tol) {
      along /= length;
      if (std::abs(along.Dot(dir)) < SM_EFF_ZERO) {
        return new (context) SmPlane(
            p0, along, dir, SmVector2d(length, distance), SmExtent2d(0.0, 0.0, 1.0, 1.0));
      }
    }
  }
  else {
    SmAxis2Placement frame;
    double radius = 0.0, a0 = 0.0, a1 = 0.0;
    if (curve->IsArc(8, tol, frame, radius, a0, a1) &&
        std::abs(frame.GetZAxis().Dot(dir)) > 1.0 - 1e-12)
    {
      if (frame.GetZAxis().Dot(dir) < 0.0) {
        /* Point the cylinder axis along the sweep: flip about the x axis, which mirrors the
         * angles. */
        frame.RotateAboutAxisAtPoint(M_PI, frame.GetOrigin(), frame.GetXAxis());
        const double b0 = 360.0 - a1, b1 = 360.0 - a0;
        a0 = b0;
        a1 = b1;
      }
      normalize_arc_degrees(a0, a1);
      SmCylinder *cylinder = nullptr;
      if (ok(SmCylinder::CreateCanonical(context, frame, radius, cylinder)) && cylinder) {
        if (std::abs(a1 - a0 - 360.0) < 1e-9) {
          a0 = 0.0;
          a1 = 360.0;
        }
        if (ok(cylinder->AdjustSTEPUVDomain(SmExtent2d(a0, 0.0, a1, distance)))) {
          return cylinder;
        }
        delete cylinder;
      }
    }
  }

  /* General case: a surface of extrusion owning a copy of the curve. */
  SmCurve *copy = nullptr;
  if (!ok(curve->Copy(context, copy)) || !copy) {
    return nullptr;
  }
  SmBSplineCurve *copy_bspline = dynamic_cast<SmBSplineCurve *>(copy);
  if (!copy_bspline) {
    delete copy;
    return nullptr;
  }
  SmSurfOfExtrusion *extrusion = nullptr;
  if (!ok(SmSurfOfExtrusion::CreateCanonical(context, copy_bspline, dir, extrusion)) ||
      !extrusion)
  {
    /* The surface owns the curve once it exists. */
    if (extrusion) {
      delete extrusion;
    }
    else {
      delete copy;
    }
    return nullptr;
  }
  const SmExtent1d interval = curve->IsAnalytic() ? curve->GetSTEPInterval() :
                                                    curve->GetNaturalInterval();
  if (!ok(extrusion->AdjustSTEPUVDomain(
          SmExtent2d(interval.GetMin(), 0.0, interval.GetMax(), distance))))
  {
    delete extrusion;
    return nullptr;
  }
  return extrusion;
}

struct ExtrudeBuild {
  SourceTable table;
  std::string error;
  Status error_status = Status::Failed;
};

/** Detaches `curves` from the edges of `brep` that already took them over, so a failed build
 * can delete both (the curves are still the caller's). */
static void detach_curves(SmBrep *brep, CurveList &curves)
{
  SmTArray<SmEdge *> edges;
  brep->GetEdges(edges);
  SmTArray<SmCurve *> &array = curves.array();
  for (ULONG i = 0; i < edges.GetSize(); i++) {
    for (ULONG j = 0; j < array.GetSize(); j++) {
      if (edges[i]->GetCurve() == array[j]) {
        edges[i]->SetCurve(nullptr, FALSE, FALSE, FALSE);
        break;
      }
    }
  }
}

/**
 * Builds the extrusion of `regions` in frame-local coordinates: the profile plane is z = 0, the
 * sweep runs from the plane along `dir` (unit, local) for `length` (> 0). Faces are built one by
 * one (sides from SMLib's analytic surfaces where it has them, like its own linear sweep), tagged
 * with the source they were made from, then stitched.
 */
static BrepPtr build_extrusion(const std::vector<const ProfileRegion *> &regions,
                               const SmVector3d &dir,
                               const double length,
                               ExtrudeBuild &build)
{
  const SmContext &context = api_context();
  BrepPtr brep_ptr(new (context) SmBrep());
  SmBrep *brep = brep_ptr.get();
  const double tol = brep->GetTolerance();
  SmTemporaryChangeValue<SmBoolean> editing(brep->m_bEditingEnabled, TRUE);

  SmAxis2Placement to_end;
  to_end.Translate(dir * length);

  const long start_cap = build.table.add({SourceKind::Cap, 0, int64_t(CapRole::Start)});
  const long end_cap = build.table.add({SourceKind::Cap, 0, int64_t(CapRole::End)});

  for (const ProfileRegion *region : regions) {
    std::vector<const ProfileLoop *> loops = {&region->outer};
    for (const ProfileLoop &hole : region->holes) {
      loops.push_back(&hole);
    }

    CurveList start_curves, end_curves;
    for (const ProfileLoop *loop : loops) {
      for (const ProfileCurve &curve : loop->curves) {
        SmCurve *sm_curve = make_profile_curve(curve, build.error);
        if (!sm_curve) {
          build.error_status = Status::InvalidInput;
          return nullptr;
        }
        start_curves.add(sm_curve);

        /* Side face. */
        SmSurface *surface = make_side_surface(sm_curve, dir, length, tol);
        if (!surface) {
          build.error = "could not sweep profile curve " + std::to_string(curve.id);
          return nullptr;
        }
        SmFace *side = nullptr;
        if (!ok(brep->CreateFaceFromSurface(surface, surface->GetNaturalUVDomain(), side)) ||
            !side)
        {
          build.error = "could not build the side face of profile curve " +
                        std::to_string(curve.id);
          return nullptr;
        }
        OriginTag::set(
            side, HISTORY_TAG_ID, {build.table.add({SourceKind::ProfileCurve, 0, curve.id})});

        /* End cap curve. */
        SmCurve *end_curve = nullptr;
        if (!ok(sm_curve->Copy(context, end_curve)) || !end_curve) {
          build.error = "could not copy profile curve " + std::to_string(curve.id);
          return nullptr;
        }
        end_curves.add(end_curve);
        if (!ok(end_curve->Transform(to_end))) {
          build.error = "could not move profile curve " + std::to_string(curve.id);
          return nullptr;
        }
      }
    }

    /* Caps. SMLib orders the curves into loops and nests them by containment itself, so the
     * loops need no orientation here (and hole loops given counter-clockwise work). */
    for (const bool is_start : {true, false}) {
      CurveList &curves = is_start ? start_curves : end_curves;
      SmTArray<SmFace *> caps;
      if (!ok(brep->CreatePlanarFacesWith3DCurves(nullptr, curves.array(), tol, caps)) ||
          caps.GetSize() == 0)
      {
        detach_curves(brep, curves);
        build.error = "could not build a planar face from the profile region";
        build.error_status = Status::InvalidInput;
        return nullptr;
      }
      curves.release();
      for (ULONG i = 0; i < caps.GetSize(); i++) {
        OriginTag::set(caps[i], HISTORY_TAG_ID, {is_start ? start_cap : end_cap});
      }
    }
  }

  if (!ok(brep->StitchAndOrient())) {
    build.error = "could not stitch the extrusion faces";
    return nullptr;
  }
  return brep_ptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tessellation
 * \{ */

static Status tessellate_impl(const SmlibShapeData &data,
                              const TessellationParams &params,
                              Tessellation &r_mesh)
{
  SmTessellationParams sm_params;
  sm_params.dChordHeightTol = std::max(params.linear_deflection, 1e-6);
  /* Above 15 degrees SMLib winds some triangles next to fillets against their face (known
   * issue); they are re-wound below anyway, but finer angles avoid slivers there. */
  const double angle_deg = std::clamp(params.angular_deflection * 180.0 / M_PI, 1.0, 15.0);
  sm_params.dCurveAngleTolDeg = angle_deg;
  sm_params.dSurfaceAngleTolDeg = angle_deg;

  SmPolyBrep *poly_raw = nullptr;
  if (!ok(SmApiTessellate(data.brep(), poly_raw, sm_params)) || !poly_raw) {
    PolyBrepPtr cleanup(poly_raw);
    return Status::Failed;
  }
  PolyBrepPtr poly(poly_raw);

  SmTArray<SmPolyVertex *> poly_vertices;
  poly->GetPolyVertices(poly_vertices);
  r_mesh.positions.reserve(poly_vertices.GetSize());
  for (ULONG i = 0; i < poly_vertices.GetSize(); i++) {
    poly_vertices[i]->SetIndexValue(i);
    const SmPoint3d p = poly_vertices[i]->GetPoint();
    r_mesh.positions.push_back({float(p.x), float(p.y), float(p.z)});
  }

  /* Per B-rep face: +1 / -1 when the tessellator's normals point out of / into the material.
   * Which one it is varies (a boolean tool's faces keep their old sense), so it is measured once
   * per face against the outward normal at a polygon corner. 0 = not known yet. */
  std::vector<double> normal_factor(data.faces().size(), 0.0);
  auto factor_of = [&](const int face, const Vec3 &aux_normal, const SmPoint3d &at) {
    double &factor = normal_factor[size_t(face)];
    if (factor == 0.0) {
      Vec3 outward;
      if (face_normal_near(data.faces()[size_t(face)], to_vec3(at), outward)) {
        const double d = dot(outward, aux_normal);
        if (std::abs(d) > 0.5) {
          factor = d > 0.0 ? 1.0 : -1.0;
        }
      }
    }
    return factor;
  };

  /* Polygon edges lying on a B-rep edge carry its index tag (copied by the tessellator). */
  std::vector<std::set<std::pair<int, int>>> edge_segments(data.edges().size());

  SmTArray<SmPolyFace *> poly_faces;
  poly->GetPolyFaces(poly_faces);
  SmTArray<SmPolyEdge *> poly_edges;
  for (ULONG i = 0; i < poly_faces.GetSize(); i++) {
    SmPolyFace *poly_face = poly_faces[i];
    const int face = data.face_index(poly_face->GetOriginalFace());
    if (face < 0) {
      return Status::Failed;
    }
    poly_face->GetPolyEdges(poly_edges);
    const ULONG corners_num = poly_edges.GetSize();
    if (corners_num < 3) {
      continue;
    }
    std::vector<int> corners(corners_num);
    std::vector<Vec3> aux_normals(corners_num);
    std::vector<Float3> normals(corners_num);
    double factor = normal_factor[size_t(face)];
    for (ULONG c = 0; c < corners_num; c++) {
      SmPolyEdge *poly_edge = poly_edges[c];
      corners[c] = int(poly_edge->GetStartPolyVertex()->GetIndexValue());
      const SmPolyVertAuxData *aux = poly_edge->GetAuxData();
      aux_normals[c] = aux ? normalized(to_vec3(aux->m_vNormal)) : Vec3{0.0, 0.0, 0.0};
      if (factor == 0.0 && vec_length(aux_normals[c]) > 0.5) {
        factor = factor_of(face, aux_normals[c], poly_edge->GetStartPolyVertex()->GetPoint());
      }

      for (const long e : OriginTag::read(poly_edge, INDEX_TAG_ID)) {
        if (e >= 0 && e < long(edge_segments.size())) {
          const int a = int(poly_edge->GetStartPolyVertex()->GetIndexValue());
          const int b = int(poly_edge->GetEndPolyVertex()->GetIndexValue());
          if (a != b) {
            edge_segments[size_t(e)].insert({std::min(a, b), std::max(a, b)});
          }
        }
      }
    }

    if (factor == 0.0) {
      factor = face_sign(data.faces()[size_t(face)]);
    }
    for (ULONG c = 0; c < corners_num; c++) {
      const Vec3 n = scaled(aux_normals[c], factor);
      normals[c] = {float(n[0]), float(n[1]), float(n[2])};
    }

    /* Fan-triangulate (polygons are nearly always triangles already). */
    for (ULONG c = 1; c + 1 < corners_num; c++) {
      int idx[3] = {corners[0], corners[c], corners[c + 1]};
      Float3 nrm[3] = {normals[0], normals[c], normals[c + 1]};
      if (idx[0] == idx[1] || idx[1] == idx[2] || idx[0] == idx[2]) {
        continue; /* Collapsed at a pole. */
      }
      /* Wind the triangle so its normal agrees with the outward surface normal. */
      const Float3 &p0 = r_mesh.positions[idx[0]];
      const Float3 &p1 = r_mesh.positions[idx[1]];
      const Float3 &p2 = r_mesh.positions[idx[2]];
      const Vec3 g = cross(Vec3{double(p1[0]) - p0[0], double(p1[1]) - p0[1], double(p1[2]) - p0[2]},
                           Vec3{double(p2[0]) - p0[0], double(p2[1]) - p0[1], double(p2[2]) - p0[2]});
      Vec3 m = {0.0, 0.0, 0.0};
      for (const Float3 &n : nrm) {
        m = add(m, Vec3{n[0], n[1], n[2]});
      }
      if (dot(g, m) < 0.0) {
        std::swap(idx[1], idx[2]);
        std::swap(nrm[1], nrm[2]);
      }
      r_mesh.triangles.push_back({idx[0], idx[1], idx[2]});
      r_mesh.triangle_faces.push_back(face);
      for (const Float3 &n : nrm) {
        r_mesh.corner_normals.push_back(n);
      }
    }
  }

  /* Chain each edge's segments into a polyline from its start vertex to its end vertex. */
  r_mesh.edge_polylines.assign(data.edges().size(), {});
  r_mesh.edge_smooth.assign(data.edges().size(), false);
  for (size_t e = 0; e < data.edges().size(); e++) {
    const SmEdge *edge = data.edges()[e];
    r_mesh.edge_smooth[e] = edge_is_smooth(edge, edge_faces(edge));
    const std::set<std::pair<int, int>> &segments = edge_segments[e];
    if (segments.empty()) {
      continue;
    }
    std::multimap<int, int> next;
    for (const auto &[a, b] : segments) {
      next.emplace(a, b);
      next.emplace(b, a);
    }
    /* Start at the mesh vertex nearest the B-rep edge's start, among the chain's ends (or any
     * vertex for a closed edge). */
    const Vec3 start = vertex_point(edge->GetStartVertex());
    int first = -1;
    double best = 1e300;
    for (const auto &[v, other] : next) {
      const bool is_end = next.count(v) == 1;
      const bool closed = std::all_of(
          next.begin(), next.end(), [&](const auto &item) { return next.count(item.first) == 2; });
      if (!is_end && !closed) {
        continue;
      }
      const Float3 &p = r_mesh.positions[size_t(v)];
      const double d = vec_length(sub(Vec3{p[0], p[1], p[2]}, start));
      if (d < best) {
        best = d;
        first = v;
      }
    }
    if (first < 0) {
      continue;
    }
    std::vector<int> &polyline = r_mesh.edge_polylines[e];
    std::set<std::pair<int, int>> used;
    int current = first;
    polyline.push_back(current);
    while (true) {
      int step = -1;
      const auto range = next.equal_range(current);
      for (auto it = range.first; it != range.second; ++it) {
        const std::pair<int, int> key = {std::min(current, it->second),
                                         std::max(current, it->second)};
        if (!used.count(key)) {
          used.insert(key);
          step = it->second;
          break;
        }
      }
      if (step < 0) {
        break;
      }
      polyline.push_back(step);
      current = step;
    }
  }

  std::set<int> meshed(r_mesh.triangle_faces.begin(), r_mesh.triangle_faces.end());
  return int(meshed.size()) == data.faces_num() ? Status::Ok : Status::InvalidResult;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Kernel Implementation
 * \{ */

class SmlibKernel final : public Kernel {
 public:
  KernelId id() const override
  {
    return KernelId::SMLib;
  }

  const char *name() const override
  {
    return "NVIDIA SMLib";
  }

  Capabilities capabilities() const override
  {
    Capabilities caps;
    caps.exact_history = false;
    caps.unequal_chamfer = false;
    caps.step_export = false;
    caps.step_import = false;
    return caps;
  }

  /* ---------------------------------------------------------------- */

  OpResult box(const BoxParams &params) const override
  {
    return guarded("box", [&]() -> OpResult {
      if (!(params.size[0] > 0.0) || !(params.size[1] > 0.0) || !(params.size[2] > 0.0)) {
        return fail(Status::InvalidInput, "box size must be positive");
      }
      SmBrep *brep = nullptr;
      const SmApiStatus status = SmApiCreateBox(
          to_sm(params.min), params.size[0], params.size[1], params.size[2], brep);
      if (!ok(status) || !brep) {
        BrepPtr cleanup(brep);
        return fail(Status::Failed, "box construction failed (" + status_text(status) + ")");
      }
      OpResult result = finish(BrepPtr(brep));
      if (result.shape.is_empty()) {
        return result;
      }
      const SmlibShapeData &data = *smlib_data(result.shape);
      result.history.exact = true;
      for (size_t i = 0; i < data.faces().size(); i++) {
        const FaceInfo info = face_info(data.faces()[i]);
        const Vec3 &n = info.sample_normal;
        int axis = 0;
        for (int a = 1; a < 3; a++) {
          if (std::abs(n[a]) > std::abs(n[axis])) {
            axis = a;
          }
        }
        const int role = axis * 2 + (n[axis] > 0.0 ? 1 : 0);
        ElementHistory &h = result.history.faces[i];
        h.origin = ElementOrigin::Generated;
        h.sources.push_back({SourceKind::PrimitiveFace, 0, role});
      }
      resolve_new_edges(data, result.history);
      return result;
    });
  }

  /* ---------------------------------------------------------------- */

  OpResult extrude(const ExtrudeParams &params) const override
  {
    return guarded("extrude", [&]() -> OpResult {
      const double total = (params.extent == ExtentMode::TwoSides) ?
                               params.distance + params.distance2 :
                               std::abs(params.distance);
      if (!(total > 1e-9)) {
        return fail(Status::InvalidInput, "extrusion distance must be non-zero");
      }
      if (params.profile.regions.empty()) {
        return fail(Status::InvalidInput, "profile has no regions");
      }
      std::string error;
      for (const ProfileRegion &region : params.profile.regions) {
        Status status = validate_loop(region.outer, error);
        for (size_t i = 0; status == Status::Ok && i < region.holes.size(); i++) {
          status = validate_loop(region.holes[i], error);
        }
        if (status != Status::Ok) {
          return fail(status, error);
        }
      }

      const PlaneFrame frame(params.profile.plane);
      Vec3 dir = {0.0, 0.0, 1.0};
      if (vec_length(params.direction) > 1e-12) {
        dir = normalized(frame.to_local_dir(params.direction));
        if (std::abs(dir[2]) < 1e-9) {
          return fail(Status::InvalidInput, "extrusion direction lies in the profile plane");
        }
      }

      /* Where the sweep starts, measured along `dir` from the profile plane. */
      double start_offset = 0.0;
      double length = params.distance;
      switch (params.extent) {
        case ExtentMode::OneSide:
          break;
        case ExtentMode::Symmetric:
          start_offset = -0.5 * std::abs(params.distance);
          length = std::abs(params.distance);
          break;
        case ExtentMode::TwoSides:
          start_offset = -params.distance2;
          length = params.distance + params.distance2;
          break;
      }
      /* SMLib sweeps a positive distance: sweep against `dir` by flipping it. The start cap stays
       * the face in the start plane either way. */
      Vec3 sweep_dir = dir;
      if (length < 0.0) {
        sweep_dir = scaled(dir, -1.0);
        length = -length;
      }

      std::vector<const ProfileRegion *> regions;
      for (const ProfileRegion &region : params.profile.regions) {
        regions.push_back(&region);
      }
      const SmAxis2Placement placement = frame.placement(scaled(dir, start_offset));

      ExtrudeBuild build;
      BrepPtr brep = build_extrusion(regions, to_sm(sweep_dir), length, build);
      if (!brep) {
        return fail(build.error_status, build.error);
      }
      /* Built at the profile plane in local coordinates: move into place. */
      if (!ok(brep->Transform(placement))) {
        return fail(Status::Failed, "could not place the extrusion");
      }
      ResultTags tags;
      OpResult result = finish(std::move(brep), &tags);

      if (!result.ok() && regions.size() > 1) {
        /* Regions that touch share side faces, which stitching turns into a face between two
         * solids that SMLib cannot measure. Build the regions one by one and fuse them, as a
         * sketch's adjacent regions are meant to (their tags survive the union). */
        build = ExtrudeBuild();
        BrepPtr fused;
        for (const ProfileRegion *region : regions) {
          BrepPtr part = build_extrusion({region}, to_sm(sweep_dir), length, build);
          if (!part) {
            return fail(build.error_status, build.error);
          }
          if (!fused) {
            fused = std::move(part);
            continue;
          }
          SmBrep *out = nullptr;
          const SmApiStatus status = SmApiBooleanUnion(fused.get(), part.get(), out);
          if (!ok(status) || out != fused.get()) {
            return fail(Status::Failed,
                        "could not fuse touching profile regions (" + status_text(status) + ")");
          }
          /* Consumed by SMLib. */
          (void)part.release();
        }
        if (!ok(fused->Transform(placement))) {
          return fail(Status::Failed, "could not place the extrusion");
        }
        tags = ResultTags();
        result = finish(std::move(fused), &tags);
      }
      if (result.shape.is_empty()) {
        return result;
      }
      const SmlibShapeData &data = *smlib_data(result.shape);
      OpHistory &history = result.history;
      history.exact = true;
      for (size_t i = 0; i < data.faces().size(); i++) {
        ElementHistory &h = history.faces[i];
        h.origin = ElementOrigin::Generated;
        for (const long v : tags.faces[i]) {
          if (const SourceRef *s = build.table.get(v)) {
            add_source(h, *s);
          }
        }
        if (h.sources.empty()) {
          h.origin = ElementOrigin::Unknown;
          history.exact = false;
        }
      }
      extrude_edge_history(params, frame, dir, start_offset, data, history);
      return result;
    });
  }

  /* ---------------------------------------------------------------- */

  OpResult boolean(const Shape &base,
                   const std::span<const Shape> tools,
                   const BooleanOp op) const override
  {
    return guarded("boolean", [&]() -> OpResult {
      const SmlibShapeData *base_data = smlib_data(base);
      if (!base_data || tools.empty()) {
        return fail(Status::InvalidInput, "boolean needs a base shape and at least one tool");
      }
      std::vector<const SmlibShapeData *> inputs = {base_data};
      for (const Shape &tool : tools) {
        const SmlibShapeData *tool_data = smlib_data(tool);
        if (!tool_data) {
          return fail(Status::InvalidInput, "boolean tool is not an SMLib shape");
        }
        inputs.push_back(tool_data);
      }
      if (inputs.size() > 127) {
        return fail(Status::InvalidInput, "too many boolean tools");
      }

      SourceTable table;
      BrepPtr result_brep = copy_brep(*base_data);
      if (!result_brep) {
        return fail(Status::Failed, "could not copy the base shape");
      }
      tag_operand(result_brep.get(), 0, table);

      for (size_t k = 1; k < inputs.size(); k++) {
        /* SMLib modifies operand 1 in place and deletes operand 2 on success. */
        BrepPtr tool_brep = copy_brep(*inputs[k]);
        if (!tool_brep) {
          return fail(Status::Failed, "could not copy a boolean tool");
        }
        tag_operand(tool_brep.get(), int8_t(k), table);
        SmBrep *out = nullptr;
        SmApiStatus status = SM_ERR;
        switch (op) {
          case BooleanOp::Union:
            status = SmApiBooleanUnion(result_brep.get(), tool_brep.get(), out);
            break;
          case BooleanOp::Subtract:
            status = SmApiBooleanDifference(result_brep.get(), tool_brep.get(), out);
            break;
          case BooleanOp::Intersect:
            status = SmApiBooleanIntersection(result_brep.get(), tool_brep.get(), out);
            break;
        }
        if (!ok(status) || out != result_brep.get()) {
          return fail(Status::Failed, "boolean operation failed (" + status_text(status) + ")");
        }
        /* Consumed by SMLib. */
        (void)tool_brep.release();
      }

      ResultTags tags;
      OpResult result = finish(std::move(result_brep), &tags);
      if (result.shape.is_empty()) {
        return result;
      }
      const SmlibShapeData &data = *smlib_data(result.shape);
      history_from_tags(inputs, data, tags, table, result.history);
      for (ElementHistory &h : result.history.faces) {
        if (h.sources.empty()) {
          /* Booleans make no faces of their own. */
          h.origin = ElementOrigin::Unknown;
          result.history.exact = false;
        }
      }
      resolve_new_edges(data, result.history);
      return result;
    });
  }

  /* ---------------------------------------------------------------- */

  OpResult fillet(const Shape &shape, const FilletParams &params) const override
  {
    return guarded("fillet", [&]() -> OpResult {
      const SmlibShapeData *data = smlib_data(shape);
      if (!data || params.edges.empty()) {
        return fail(Status::InvalidInput, "fillet needs a shape and at least one edge");
      }
      if (!(params.radius > 0.0)) {
        return fail(Status::InvalidInput, "fillet radius must be positive");
      }
      for (const int index : params.edges) {
        if (index < 0 || index >= data->edges_num()) {
          return fail(Status::InvalidInput, "fillet edge index out of range");
        }
      }
      SourceTable table;
      BrepPtr brep = copy_brep(*data);
      if (!brep) {
        return fail(Status::Failed, "could not copy the shape");
      }
      tag_operand(brep.get(), 0, table);
      SmTArray<SmEdge *> all_edges, edges;
      brep->GetEdges(all_edges);
      for (const int index : params.edges) {
        edges.Add(all_edges[ULONG(index)]);
      }
      const SmApiStatus status = SmApiFilletEdges(brep.get(), edges, params.radius, 1, 1, 1.0);
      if (status == SM_ERR_INVALID_INPUT) {
        return fail(Status::InvalidInput, "fillet radius is below SMLib's minimum");
      }
      if (!ok(status)) {
        return fail(Status::Failed,
                    "fillet failed (radius too large for the geometry, or unsupported corner; " +
                        status_text(status) + ")");
      }
      return finish_blend(*data, params.edges, std::move(brep), table);
    });
  }

  OpResult chamfer(const Shape &shape, const ChamferParams &params) const override
  {
    return guarded("chamfer", [&]() -> OpResult {
      const SmlibShapeData *data = smlib_data(shape);
      if (!data || params.edges.empty()) {
        return fail(Status::InvalidInput, "chamfer needs a shape and at least one edge");
      }
      if (!(params.distance > 0.0)) {
        return fail(Status::InvalidInput, "chamfer distance must be positive");
      }
      if (params.distance2 > 0.0 && std::abs(params.distance2 - params.distance) > 1e-12) {
        OpResult result = fail(Status::Unsupported,
                               "unequal chamfers are not supported by the SMLib backend");
        return result;
      }
      for (const int index : params.edges) {
        if (index < 0 || index >= data->edges_num()) {
          return fail(Status::InvalidInput, "chamfer edge index out of range");
        }
      }
      SourceTable table;
      BrepPtr brep = copy_brep(*data);
      if (!brep) {
        return fail(Status::Failed, "could not copy the shape");
      }
      tag_operand(brep.get(), 0, table);
      SmTArray<SmEdge *> all_edges;
      brep->GetEdges(all_edges);

      const SmContext &context = api_context();
      const double tol = brep->GetTolerance();
      if (!SmFilletExecutive::IsRadiusSupported(params.distance, tol)) {
        return fail(Status::InvalidInput, "chamfer distance is below SMLib's minimum");
      }

      /* The body of SmApiChamferFillet, over the picked edges only. SMLib's chamfer distance is
       * the width across the chamfer face; ours is the setback on each face. */
      SmLinearCrossSectionFSG linear_generator;
      SmMakeSurfaceBlendSIH self_intersection(1.0);
      SmFilletExecutive executive(context, brep.get());
      executive.SetSelfIntersectionHandler(&self_intersection);
      executive.SetDoGlobalMerge(TRUE);
      for (const int index : params.edges) {
        SmEdge *edge = all_edges[ULONG(index)];
        SmEdgeuse *edgeuse = edge->GetBlendEdgeuse();
        if (!edgeuse) {
          return fail(Status::InvalidInput,
                      "chamfer edge " + std::to_string(index) + " cannot be blended");
        }
        const double width = chamfer_width(edge, params.distance);
        SmFilletSolver *solver = new (context) SmConstantDistanceFS(
            context, tol, 30.0 * SM_PI / 180.0, 2.0 * SM_PI / 180.0, width, edgeuse);
        solver->SetFilletSurfaceGenerator(&linear_generator);
        if (!ok(executive.LoadFilletSolver(solver))) {
          return fail(Status::Failed, "could not set up the chamfer");
        }
      }
      if (!ok(executive.CreateFilletCorners())) {
        return fail(Status::Failed, "chamfer corners failed");
      }
      SmTArray<SmFilletCorner *> &corners = executive.GetFilletCorners();
      for (ULONG i = 0; i < corners.GetSize(); i++) {
        corners[i]->SetBevel(TRUE);
      }
      const SmApiStatus status = executive.DoFilleting();
      if (!ok(status)) {
        return fail(Status::Failed,
                    "chamfer failed (distance too large for the geometry; " +
                        status_text(status) + ")");
      }
      return finish_blend(*data, params.edges, std::move(brep), table);
    });
  }

  OpResult transform(const Shape &shape, const Transform &transform) const override
  {
    return guarded("transform", [&]() -> OpResult {
      const SmlibShapeData *data = smlib_data(shape);
      if (!data) {
        return fail(Status::InvalidInput, "transform needs an SMLib shape");
      }
      /* Column-major input: element (row r, column c) is m[c * 4 + r]. Split it into a uniform
       * scale and a rotation. */
      const std::array<double, 16> &m = transform.matrix;
      const Vec3 c0 = {m[0], m[1], m[2]}, c1 = {m[4], m[5], m[6]}, c2 = {m[8], m[9], m[10]};
      const double scale = vec_length(c0);
      if (!(scale > 1e-12) || std::abs(vec_length(c1) - scale) > 1e-9 * scale ||
          std::abs(vec_length(c2) - scale) > 1e-9 * scale ||
          std::abs(dot(c0, c1)) > 1e-9 * scale * scale ||
          dot(cross(c0, c1), c2) <= 0.0)
      {
        return fail(Status::InvalidInput,
                    "transform must be a rotation, translation and positive uniform scale");
      }
      BrepPtr brep = copy_brep(*data);
      if (!brep) {
        return fail(Status::Failed, "could not copy the shape");
      }
      const SmAxis2Placement placement(SmPoint3d(m[12], m[13], m[14]),
                                       to_sm(scaled(c0, 1.0 / scale)),
                                       to_sm(scaled(c1, 1.0 / scale)));
      const SmVector3d scale_vec(scale, scale, scale);
      if (!ok(brep->Transform(placement, std::abs(scale - 1.0) > 1e-15 ? &scale_vec : nullptr))) {
        return fail(Status::Failed, "transform failed");
      }
      ResultTags tags;
      OpResult result = finish(std::move(brep), &tags);
      if (result.shape.is_empty()) {
        return result;
      }
      /* A copy keeps every element (and its index tag): each result element is its input
       * element, moved. */
      OpHistory &history = result.history;
      history.exact = true;
      for (size_t i = 0; i < history.faces.size(); i++) {
        for (const long v : tags.face_index[i]) {
          add_source(history.faces[i], {SourceKind::Face, 0, v});
        }
        history.faces[i].origin = history.faces[i].sources.size() == 1 ? ElementOrigin::Modified :
                                                                          ElementOrigin::Unknown;
      }
      for (size_t i = 0; i < history.edges.size(); i++) {
        for (const long v : tags.edge_index[i]) {
          add_source(history.edges[i], {SourceKind::Edge, 0, v});
        }
        history.edges[i].origin = history.edges[i].sources.size() == 1 ? ElementOrigin::Modified :
                                                                          ElementOrigin::Unknown;
      }
      return result;
    });
  }

  /* ---------------------------------------------------------------- */

  Topology topology(const Shape &shape) const override
  {
    Topology topo;
    const SmlibShapeData *data = smlib_data(shape);
    if (!data) {
      return topo;
    }
    try {
      ApiLock lock;
      topo.faces.reserve(data->faces().size());
      for (const SmFace *face : data->faces()) {
        FaceInfo info = face_info(face);
        info.solid = data->solid_of(face);
        for (const SmEdge *edge : face_edges(face)) {
          info.edges.push_back(data->edge_index(edge));
        }
        topo.faces.push_back(std::move(info));
      }
      topo.edges.reserve(data->edges().size());
      for (const SmEdge *edge : data->edges()) {
        topo.edges.push_back(edge_info(edge, *data));
      }
      topo.vertices.reserve(data->vertices().size());
      for (const SmVertex *vertex : data->vertices()) {
        topo.vertices.push_back(vertex_point(vertex));
      }
    }
    catch (...) {
      /* Partial topology is still useful; callers check counts against the shape. */
    }
    return topo;
  }

  MassProperties mass_properties(const Shape &shape) const override
  {
    MassProperties mass;
    const SmlibShapeData *data = smlib_data(shape);
    if (!data) {
      return mass;
    }
    try {
      ApiLock lock;
      compute_mass(data->brep(), mass);
    }
    catch (...) {
    }
    return mass;
  }

  bool is_valid(const Shape &shape, std::string *r_reason) const override
  {
    const SmlibShapeData *data = smlib_data(shape);
    if (!data) {
      if (r_reason) {
        *r_reason = "not an SMLib shape";
      }
      return false;
    }
    try {
      ApiLock lock;
      return check_valid(data->brep(), r_reason);
    }
    catch (...) {
      if (r_reason) {
        *r_reason = "validity check crashed";
      }
      return false;
    }
  }

  Status tessellate(const Shape &shape,
                    const TessellationParams &params,
                    Tessellation &r_mesh) const override
  {
    r_mesh = Tessellation();
    const SmlibShapeData *data = smlib_data(shape);
    if (!data) {
      return Status::InvalidInput;
    }
    try {
      ApiLock lock;
      const Status status = tessellate_impl(*data, params, r_mesh);
      if (status == Status::Failed) {
        r_mesh = Tessellation();
      }
      return status;
    }
    catch (...) {
      r_mesh = Tessellation();
      return Status::Failed;
    }
  }

  /* ---------------------------------------------------------------- */

  Status export_step(std::span<const Shape> /*shapes*/,
                     std::span<const std::string> /*names*/,
                     const std::string & /*filepath*/,
                     std::string *r_message) const override
  {
    if (r_message) {
      *r_message = "STEP export of SMLib shapes needs the neutral B-rep transfer (not yet)";
    }
    return Status::Unsupported;
  }

  Status import_step(const std::string & /*filepath*/,
                     std::vector<Shape> &r_shapes,
                     std::string *r_message) const override
  {
    r_shapes.clear();
    if (r_message) {
      *r_message = "STEP import into SMLib needs the neutral B-rep transfer (not yet)";
    }
    return Status::Unsupported;
  }

 private:
  /** Chamfer width (SMLib's distance across the chamfer face) for a setback of `setback` on
   * both faces of `edge`: 2 d cos(beta / 2) for faces whose normals are beta apart. */
  static double chamfer_width(const SmEdge *edge, const double setback)
  {
    const std::vector<SmFace *> faces = edge_faces(edge);
    if (faces.size() == 2) {
      const Vec3 mid = edge_midpoint(edge);
      Vec3 n0, n1;
      if (face_normal_near(faces[0], mid, n0) && face_normal_near(faces[1], mid, n1)) {
        const double beta = std::acos(std::clamp(dot(n0, n1), -1.0, 1.0));
        return 2.0 * setback * std::cos(0.5 * beta);
      }
    }
    return setback * std::sqrt(2.0);
  }

  /** Wraps the result of a fillet or chamfer of `input` and works out its history. */
  static OpResult finish_blend(const SmlibShapeData &input,
                               const std::vector<int> &picked,
                               BrepPtr brep,
                               const SourceTable &table)
  {
    ResultTags tags;
    OpResult result = finish(std::move(brep), &tags);
    if (result.shape.is_empty()) {
      return result;
    }
    const SmlibShapeData &data = *smlib_data(result.shape);
    history_from_tags({&input}, data, tags, table, result.history);
    resolve_blend_faces(input, picked, data, result.history);
    resolve_new_edges(data, result.history);
    return result;
  }

  /**
   * Edge history of an extrusion, from the faces each edge bounds: between a side and a cap it
   * comes from the profile curve and the cap, between two sides (or a side and itself, at a
   * seam) from the profile vertex it was swept from.
   */
  static void extrude_edge_history(const ExtrudeParams &params,
                                   const PlaneFrame &frame,
                                   const Vec3 &dir,
                                   const double start_offset,
                                   const SmlibShapeData &data,
                                   OpHistory &history)
  {
    /* Profile vertices (curve start points) in world space on the start plane. */
    struct ProfileVertexInfo {
      int64_t id;
      Vec3 point;
    };
    std::vector<ProfileVertexInfo> profile_vertices;
    for (const ProfileRegion &region : params.profile.regions) {
      std::vector<const ProfileLoop *> loops = {&region.outer};
      for (const ProfileLoop &hole : region.holes) {
        loops.push_back(&hole);
      }
      for (const ProfileLoop *loop : loops) {
        for (const ProfileCurve &curve : loop->curves) {
          Vec2 p = curve_start(curve);
          if (curve.type == ProfileCurveType::Circle) {
            /* SMLib circles start at angle zero. */
            p = {curve.center[0] + curve.radius, curve.center[1]};
          }
          profile_vertices.push_back({curve.id, p_world(frame, dir, start_offset, p)});
        }
      }
    }

    for (size_t i = 0; i < data.edges().size(); i++) {
      const SmEdge *edge = data.edges()[i];
      ElementHistory &h = history.edges[i];
      h.origin = ElementOrigin::Generated;
      std::vector<SourceRef> curves, caps;
      for (const SmFace *face : edge_faces(edge)) {
        const int fi = data.face_index(face);
        if (fi < 0) {
          continue;
        }
        for (const SourceRef &s : history.faces[size_t(fi)].sources) {
          if (s.kind == SourceKind::Cap) {
            caps.push_back(s);
          }
          else if (s.kind == SourceKind::ProfileCurve) {
            curves.push_back(s);
          }
        }
      }
      if (!caps.empty()) {
        for (const SourceRef &s : curves) {
          add_source(h, s);
        }
        for (const SourceRef &s : caps) {
          add_source(h, s);
        }
        continue;
      }
      /* A lateral edge: swept from the profile vertex at its start-plane end. */
      const Vec3 a = vertex_point(edge->GetStartVertex());
      const Vec3 b = vertex_point(edge->GetEndVertex());
      const ProfileVertexInfo *best = nullptr;
      double best_distance = 1e300;
      for (const ProfileVertexInfo &v : profile_vertices) {
        const bool from_curves = std::any_of(curves.begin(), curves.end(), [&](const SourceRef &s) {
          return s.index == v.id;
        });
        if (!from_curves && !curves.empty()) {
          continue;
        }
        const double d = std::min(vec_length(sub(a, v.point)), vec_length(sub(b, v.point)));
        if (d < best_distance) {
          best_distance = d;
          best = &v;
        }
      }
      if (best && best_distance < 1e-5) {
        add_source(h, {SourceKind::ProfileVertex, 0, best->id});
      }
      else {
        for (const SourceRef &s : curves) {
          add_source(h, s);
        }
        if (h.sources.empty()) {
          h.origin = ElementOrigin::Unknown;
        }
      }
    }
  }

  static Vec3 p_world(const PlaneFrame &frame, const Vec3 &dir, const double offset, const Vec2 &p)
  {
    const Vec3 local = add(Vec3{p[0], p[1], 0.0}, scaled(dir, offset));
    return add(frame.origin,
               add(scaled(frame.x, local[0]), add(scaled(frame.y, local[1]), scaled(frame.n, local[2]))));
  }
};

const Kernel *kernel_instance()
{
  static const SmlibKernel kernel;
  return &kernel;
}

/** \} */

}  // namespace cad::kernel::smlib

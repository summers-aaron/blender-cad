/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_cad_kernel
 *
 * OpenCASCADE backend of the CAD kernel interface.
 *
 * Design notes:
 * - Shapes are immutable once wrapped. Nothing here writes into a wrapped shape's topology:
 *   faces and edges are shared between operation inputs and results (and between cached
 *   results on different threads), so mutating them would race. Tessellation therefore meshes
 *   a structural copy, and edge smoothness is computed geometrically instead of through
 *   `BRepLib::EncodeRegularity`.
 * - Element order is `TopExp::MapShapes` order, which is deterministic for a given shape.
 * - History comes from OCCT's own bookkeeping (`Modified` / `Generated` / `IsDeleted`), so it
 *   is exact.
 */

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeShape.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepLib_ToolTriangulatedShape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools.hxx>
#include <BRepTools_History.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeSegment.hxx>
#include <GProp_GProps.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_Printer.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include "CAD_kernel.hh"

#include "../intern/kernel_backends.hh"

namespace cad::kernel::occt {

/* -------------------------------------------------------------------- */
/** \name Shape Data
 * \{ */

class OcctShapeData final : public ShapeData {
 public:
  TopoDS_Shape shape;
  TopTools_IndexedMapOfShape faces;
  TopTools_IndexedMapOfShape edges;
  TopTools_IndexedMapOfShape vertices;
  TopTools_IndexedMapOfShape solids;

  explicit OcctShapeData(TopoDS_Shape s) : shape(std::move(s))
  {
    TopExp::MapShapes(shape, TopAbs_SOLID, solids);
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);
  }

  KernelId kernel() const override
  {
    return KernelId::OCCT;
  }
  int faces_num() const override
  {
    return faces.Extent();
  }
  int edges_num() const override
  {
    return edges.Extent();
  }
  int vertices_num() const override
  {
    return vertices.Extent();
  }
  int solids_num() const override
  {
    return solids.Extent();
  }
};

static const OcctShapeData *occt_data(const Shape &shape)
{
  if (shape.is_empty() || shape.kernel() != KernelId::OCCT) {
    return nullptr;
  }
  return static_cast<const OcctShapeData *>(shape.data());
}

static std::shared_ptr<const OcctShapeData> wrap(const TopoDS_Shape &shape)
{
  return std::make_shared<const OcctShapeData>(shape);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Conversions
 * \{ */

static gp_Pnt to_pnt(const Vec3 &v)
{
  return gp_Pnt(v[0], v[1], v[2]);
}

static gp_Dir to_dir(const Vec3 &v)
{
  return gp_Dir(v[0], v[1], v[2]);
}

static Vec3 to_vec3(const gp_XYZ &p)
{
  return {p.X(), p.Y(), p.Z()};
}

static Vec3 to_vec3(const gp_Pnt &p)
{
  return {p.X(), p.Y(), p.Z()};
}

static Vec3 to_vec3(const gp_Dir &d)
{
  return {d.X(), d.Y(), d.Z()};
}

static double vec_length(const Vec3 &v)
{
  return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/** Maps 2D sketch coordinates into 3D through a plane frame. */
struct PlaneFrame {
  gp_Ax3 ax3;
  gp_Pnt origin;
  gp_Dir x;
  gp_Dir y;
  gp_Dir n;

  explicit PlaneFrame(const Plane &plane)
  {
    origin = to_pnt(plane.origin);
    n = to_dir(plane.normal);
    /* Orthogonalize x against the normal so slightly inaccurate frames still work. */
    gp_Vec xv(plane.x_dir[0], plane.x_dir[1], plane.x_dir[2]);
    xv -= gp_Vec(n) * xv.Dot(gp_Vec(n));
    x = gp_Dir(xv);
    y = n.Crossed(x);
    ax3 = gp_Ax3(origin, n, x);
  }

  gp_Pnt point(const Vec2 &p) const
  {
    return gp_Pnt(origin.XYZ() + x.XYZ() * p[0] + y.XYZ() * p[1]);
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

/** Runs `fn`, converting any exception into a failed #OpResult. */
template<typename Fn> static OpResult guarded(const char *op_name, Fn &&fn)
{
  try {
    return fn();
  }
  catch (const Standard_Failure &e) {
    return fail(Status::Failed, std::string(op_name) + ": " + e.GetMessageString());
  }
  catch (const std::exception &e) {
    return fail(Status::Failed, std::string(op_name) + ": " + e.what());
  }
  catch (...) {
    return fail(Status::Failed, std::string(op_name) + ": unknown error");
  }
}

static bool check_valid(const TopoDS_Shape &shape, std::string *r_reason)
{
  BRepCheck_Analyzer analyzer(shape);
  if (!analyzer.IsValid()) {
    if (r_reason) {
      *r_reason = "result failed the B-rep validity check";
    }
    return false;
  }
  return true;
}

/** Wraps a finished result, validating it. */
static OpResult finish(const TopoDS_Shape &shape)
{
  if (shape.IsNull()) {
    return fail(Status::Failed, "operation produced no shape");
  }
  std::string reason;
  OpResult result;
  result.shape = Shape(wrap(shape));
  if (!check_valid(shape, &reason)) {
    result.status = Status::InvalidResult;
    result.message = reason;
    return result;
  }
  result.status = Status::Ok;
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name History Extraction
 * \{ */

/** Uniform access to OCCT's two history APIs. */
class HistoryQuery {
 public:
  virtual ~HistoryQuery() = default;
  virtual const TopTools_ListOfShape &modified(const TopoDS_Shape &s) = 0;
  virtual const TopTools_ListOfShape &generated(const TopoDS_Shape &s) = 0;
  virtual bool removed(const TopoDS_Shape &s) = 0;
};

class MakeShapeQuery final : public HistoryQuery {
 public:
  explicit MakeShapeQuery(BRepBuilderAPI_MakeShape &maker) : maker_(maker) {}
  const TopTools_ListOfShape &modified(const TopoDS_Shape &s) override
  {
    return maker_.Modified(s);
  }
  const TopTools_ListOfShape &generated(const TopoDS_Shape &s) override
  {
    return maker_.Generated(s);
  }
  bool removed(const TopoDS_Shape &s) override
  {
    return maker_.IsDeleted(s);
  }

 private:
  BRepBuilderAPI_MakeShape &maker_;
};

class ToolsHistoryQuery final : public HistoryQuery {
 public:
  explicit ToolsHistoryQuery(const Handle(BRepTools_History) & history) : history_(history) {}
  const TopTools_ListOfShape &modified(const TopoDS_Shape &s) override
  {
    return history_->Modified(s);
  }
  const TopTools_ListOfShape &generated(const TopoDS_Shape &s) override
  {
    return history_->Generated(s);
  }
  bool removed(const TopoDS_Shape &s) override
  {
    return history_->IsRemoved(s);
  }

 private:
  Handle(BRepTools_History) history_;
};

static ElementOrigin combine_origin(const ElementOrigin current, const ElementOrigin incoming)
{
  if (current == ElementOrigin::Unknown) {
    return incoming;
  }
  if (current == incoming) {
    /* A second source of the same kind means several inputs became this element. */
    if (incoming == ElementOrigin::Modified || incoming == ElementOrigin::Split) {
      return ElementOrigin::Merged;
    }
    return current;
  }
  /* Kept wins over everything: the element is literally an input element. */
  if (current == ElementOrigin::Kept || incoming == ElementOrigin::Kept) {
    return ElementOrigin::Kept;
  }
  if (current == ElementOrigin::Generated || incoming == ElementOrigin::Generated) {
    /* Modified + generated: treat as modified, the generated source is secondary. */
    return ElementOrigin::Modified;
  }
  return ElementOrigin::Merged;
}

static void record(std::vector<ElementHistory> &table,
                   const TopTools_IndexedMapOfShape &result_map,
                   const TopoDS_Shape &image,
                   const ElementOrigin origin,
                   const SourceRef &source)
{
  const int index = result_map.FindIndex(image);
  if (index == 0) {
    return;
  }
  ElementHistory &entry = table[index - 1];
  if (std::find(entry.sources.begin(), entry.sources.end(), source) != entry.sources.end()) {
    return;
  }
  entry.origin = combine_origin(entry.origin, origin);
  entry.sources.push_back(source);
}

/** Routes one image (face or edge) of `source` into the right history table. */
static void record_image(OpHistory &history,
                         const OcctShapeData &result,
                         const TopoDS_Shape &image,
                         const ElementOrigin origin,
                         const SourceRef &source)
{
  if (image.ShapeType() == TopAbs_FACE) {
    record(history.faces, result.faces, image, origin, source);
  }
  else if (image.ShapeType() == TopAbs_EDGE) {
    record(history.edges, result.edges, image, origin, source);
  }
  else if (image.ShapeType() < TopAbs_FACE) {
    /* A compound/shell/solid image: record each of its faces. */
    for (TopExp_Explorer exp(image, TopAbs_FACE); exp.More(); exp.Next()) {
      record(history.faces, result.faces, exp.Current(), origin, source);
    }
  }
}

static void history_for_elements(OpHistory &history,
                                 const OcctShapeData &result,
                                 HistoryQuery &query,
                                 const TopTools_IndexedMapOfShape &elements,
                                 const SourceKind kind,
                                 const int8_t input)
{
  const TopTools_IndexedMapOfShape &result_same_dim = (kind == SourceKind::Face) ? result.faces :
                                                      (kind == SourceKind::Edge) ? result.edges :
                                                                                   result.vertices;
  for (int i = 1; i <= elements.Extent(); i++) {
    const TopoDS_Shape &element = elements(i);
    const SourceRef source{kind, input, i - 1};

    if (kind != SourceKind::Vertex && result_same_dim.Contains(element)) {
      record_image(history, result, element, ElementOrigin::Kept, source);
    }
    else if (kind != SourceKind::Vertex && !query.removed(element)) {
      const TopTools_ListOfShape &images = query.modified(element);
      const ElementOrigin origin = images.Extent() > 1 ? ElementOrigin::Split :
                                                         ElementOrigin::Modified;
      for (const TopoDS_Shape &image : images) {
        record_image(history, result, image, origin, source);
      }
    }
    for (const TopoDS_Shape &image : query.generated(element)) {
      /* Only lower-to-higher dimension generation is interesting (edge -> face etc.). */
      if (image.ShapeType() < element.ShapeType()) {
        record_image(history, result, image, ElementOrigin::Generated, source);
      }
    }
  }
}

/** Fills face and edge history of `result` from the inputs of a modifying operation. */
static OpHistory history_from_query(const std::vector<const OcctShapeData *> &inputs,
                                    const OcctShapeData &result,
                                    HistoryQuery &query)
{
  OpHistory history;
  history.faces.resize(result.faces.Extent());
  history.edges.resize(result.edges.Extent());
  history.exact = true;
  for (int8_t input = 0; input < int8_t(inputs.size()); input++) {
    const OcctShapeData &data = *inputs[input];
    history_for_elements(history, result, query, data.faces, SourceKind::Face, input);
    history_for_elements(history, result, query, data.edges, SourceKind::Edge, input);
    history_for_elements(history, result, query, data.vertices, SourceKind::Vertex, input);
  }
  return history;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Profiles
 * \{ */

static gp_Circ make_circle(const PlaneFrame &frame, const ProfileCurve &curve)
{
  return gp_Circ(gp_Ax2(frame.point(curve.center), frame.n, frame.x), curve.radius);
}

/** Builds the edge of a profile curve, oriented from `start` to `end`. */
static TopoDS_Edge make_profile_edge(const PlaneFrame &frame, const ProfileCurve &curve)
{
  switch (curve.type) {
    case ProfileCurveType::Line: {
      return BRepBuilderAPI_MakeEdge(frame.point(curve.start), frame.point(curve.end)).Edge();
    }
    case ProfileCurveType::Arc: {
      const gp_Circ circ = make_circle(frame, curve);
      if (curve.ccw) {
        return BRepBuilderAPI_MakeEdge(circ, frame.point(curve.start), frame.point(curve.end))
            .Edge();
      }
      TopoDS_Edge edge =
          BRepBuilderAPI_MakeEdge(circ, frame.point(curve.end), frame.point(curve.start)).Edge();
      edge.Reverse();
      return edge;
    }
    case ProfileCurveType::Circle: {
      return BRepBuilderAPI_MakeEdge(make_circle(frame, curve)).Edge();
    }
    case ProfileCurveType::BSpline: {
      const int poles_num = int(curve.poles.size());
      TColgp_Array1OfPnt poles(1, poles_num);
      for (int i = 0; i < poles_num; i++) {
        poles.SetValue(i + 1, frame.point(curve.poles[i]));
      }
      TColStd_Array1OfReal weights(1, poles_num);
      for (int i = 0; i < poles_num; i++) {
        weights.SetValue(i + 1, curve.weights.empty() ? 1.0 : curve.weights[i]);
      }
      /* Expanded knot vector -> unique knots + multiplicities. */
      std::vector<double> knots;
      std::vector<int> mults;
      for (const double k : curve.knots) {
        if (!knots.empty() && std::abs(k - knots.back()) < 1e-12) {
          mults.back()++;
        }
        else {
          knots.push_back(k);
          mults.push_back(1);
        }
      }
      TColStd_Array1OfReal knot_array(1, int(knots.size()));
      TColStd_Array1OfInteger mult_array(1, int(knots.size()));
      for (int i = 0; i < int(knots.size()); i++) {
        knot_array.SetValue(i + 1, knots[i]);
        mult_array.SetValue(i + 1, mults[i]);
      }
      Handle(Geom_BSplineCurve) spline = new Geom_BSplineCurve(
          poles, weights, knot_array, mult_array, curve.degree, curve.periodic);
      return BRepBuilderAPI_MakeEdge(spline).Edge();
    }
  }
  return TopoDS_Edge();
}

/** Points along a curve from start to end, for orientation tests. */
static void sample_curve(const ProfileCurve &curve, std::vector<Vec2> &r_points)
{
  constexpr int segments = 16;
  switch (curve.type) {
    case ProfileCurveType::Line:
      r_points.push_back(curve.start);
      break;
    case ProfileCurveType::Arc: {
      const double a0 = std::atan2(curve.start[1] - curve.center[1],
                                   curve.start[0] - curve.center[0]);
      double a1 = std::atan2(curve.end[1] - curve.center[1], curve.end[0] - curve.center[0]);
      if (curve.ccw) {
        while (a1 <= a0) {
          a1 += 2.0 * M_PI;
        }
      }
      else {
        while (a1 >= a0) {
          a1 -= 2.0 * M_PI;
        }
      }
      for (int i = 0; i < segments; i++) {
        const double a = a0 + (a1 - a0) * double(i) / segments;
        r_points.push_back(
            {curve.center[0] + curve.radius * std::cos(a), curve.center[1] + curve.radius * std::sin(a)});
      }
      break;
    }
    case ProfileCurveType::Circle:
      for (int i = 0; i < segments; i++) {
        const double a = 2.0 * M_PI * double(i) / segments;
        r_points.push_back(
            {curve.center[0] + curve.radius * std::cos(a), curve.center[1] + curve.radius * std::sin(a)});
      }
      break;
    case ProfileCurveType::BSpline:
      /* Control polygon orientation matches the curve's for simple (non self-intersecting)
       * closed loops, which is all a profile may contain. */
      for (const Vec2 &p : curve.poles) {
        r_points.push_back(p);
      }
      break;
  }
}

static double loop_signed_area(const ProfileLoop &loop)
{
  std::vector<Vec2> points;
  for (const ProfileCurve &curve : loop.curves) {
    sample_curve(curve, points);
  }
  double area = 0.0;
  for (size_t i = 0; i < points.size(); i++) {
    const Vec2 &a = points[i];
    const Vec2 &b = points[(i + 1) % points.size()];
    area += a[0] * b[1] - b[0] * a[1];
  }
  return 0.5 * area;
}

struct ProfileTopology {
  /** One planar face per region. */
  std::vector<TopoDS_Face> faces;
  /** Profile curve id per edge TShape. */
  std::unordered_map<const TopoDS_TShape *, int64_t> edge_curve;
  /** Profile curve id whose start vertex this is, per vertex TShape. */
  std::unordered_map<const TopoDS_TShape *, int64_t> vertex_curve;
};

/**
 * Builds a wire for `loop`, oriented counter-clockwise (seen from the plane normal) when
 * `want_ccw`, clockwise otherwise. Orientation is decided from the sketch data itself, so the
 * resulting face needs no shape healing (which could replace edges and lose their curve ids).
 */
static bool make_loop_wire(const PlaneFrame &frame,
                           const ProfileLoop &loop,
                           const bool want_ccw,
                           ProfileTopology &topo,
                           TopoDS_Wire &r_wire,
                           std::string &r_error)
{
  if (loop.curves.empty()) {
    r_error = "empty profile loop";
    return false;
  }
  BRepBuilderAPI_MakeWire make_wire;
  for (const ProfileCurve &curve : loop.curves) {
    const TopoDS_Edge edge = make_profile_edge(frame, curve);
    if (edge.IsNull()) {
      r_error = "could not build profile curve " + std::to_string(curve.id);
      return false;
    }
    make_wire.Add(edge);
    if (!make_wire.IsDone()) {
      r_error = "profile curve " + std::to_string(curve.id) + " does not connect to the loop";
      return false;
    }
    /* The wire builder may substitute vertices to connect edges: map the edge as it sits in
     * the wire, not the one we built. */
    const TopoDS_Edge &placed = make_wire.Edge();
    topo.edge_curve[placed.TShape().get()] = curve.id;
    const TopoDS_Vertex start = TopExp::FirstVertex(placed, true);
    if (!start.IsNull()) {
      topo.vertex_curve.emplace(start.TShape().get(), curve.id);
    }
  }
  TopoDS_Wire wire = make_wire.Wire();
  if (!BRep_Tool::IsClosed(wire)) {
    r_error = "profile loop is not closed";
    return false;
  }
  const bool is_ccw = loop_signed_area(loop) > 0.0;
  if (is_ccw != want_ccw) {
    wire.Reverse();
  }
  r_wire = wire;
  return true;
}

static bool make_profile_faces(const Profile &profile,
                               ProfileTopology &topo,
                               std::string &r_error)
{
  if (profile.regions.empty()) {
    r_error = "profile has no regions";
    return false;
  }
  const PlaneFrame frame(profile.plane);
  const gp_Pln pln(frame.ax3);
  for (const ProfileRegion &region : profile.regions) {
    TopoDS_Wire outer;
    if (!make_loop_wire(frame, region.outer, true, topo, outer, r_error)) {
      return false;
    }
    BRepBuilderAPI_MakeFace make_face(pln, outer, true);
    if (!make_face.IsDone()) {
      r_error = "could not build a face from the outer loop";
      return false;
    }
    for (const ProfileLoop &hole : region.holes) {
      TopoDS_Wire hole_wire;
      if (!make_loop_wire(frame, hole, false, topo, hole_wire, r_error)) {
        return false;
      }
      make_face.Add(hole_wire);
    }
    if (!make_face.IsDone()) {
      r_error = "could not add a hole to the profile face";
      return false;
    }
    topo.faces.push_back(make_face.Face());
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Geometry Helpers
 * \{ */

/** Outward normal of `face` at (u, v), honoring face orientation. */
static bool face_normal_at(const TopoDS_Face &face,
                           const BRepAdaptor_Surface &surface,
                           const double u,
                           const double v,
                           gp_Pnt &r_point,
                           gp_Dir &r_normal)
{
  BRepLProp_SLProps props(surface, u, v, 1, 1e-7);
  r_point = props.Value();
  if (!props.IsNormalDefined()) {
    return false;
  }
  r_normal = props.Normal();
  if (face.Orientation() == TopAbs_REVERSED) {
    r_normal.Reverse();
  }
  return true;
}

/** A (u, v) strictly inside the face's trimmed domain, searched on a growing grid. */
static bool face_inner_uv(const TopoDS_Face &face, double &r_u, double &r_v)
{
  double umin, umax, vmin, vmax;
  BRepTools::UVBounds(face, umin, umax, vmin, vmax);
  const double tol = BRep_Tool::Tolerance(face);
  for (const int n : {1, 3, 7, 15}) {
    for (int i = 1; i <= n; i++) {
      for (int j = 1; j <= n; j++) {
        const double u = umin + (umax - umin) * double(i) / double(n + 1);
        const double v = vmin + (vmax - vmin) * double(j) / double(n + 1);
        BRepClass_FaceClassifier classifier(face, gp_Pnt2d(u, v), tol);
        if (classifier.State() == TopAbs_IN) {
          r_u = u;
          r_v = v;
          return true;
        }
      }
    }
  }
  r_u = 0.5 * (umin + umax);
  r_v = 0.5 * (vmin + vmax);
  return false;
}

/** Normal of `face` where `edge` passes at curve parameter `t`. */
static bool face_normal_along_edge(const TopoDS_Edge &edge,
                                   const TopoDS_Face &face,
                                   const double t,
                                   gp_Dir &r_normal)
{
  double first, last;
  const Handle(Geom2d_Curve) pcurve = BRep_Tool::CurveOnSurface(edge, face, first, last);
  if (pcurve.IsNull()) {
    return false;
  }
  const gp_Pnt2d uv = pcurve->Value(t);
  const BRepAdaptor_Surface surface(face);
  gp_Pnt point;
  return face_normal_at(face, surface, uv.X(), uv.Y(), point, r_normal);
}

static FaceInfo face_info(const TopoDS_Face &face)
{
  FaceInfo info;
  const BRepAdaptor_Surface surface(face);
  switch (surface.GetType()) {
    case GeomAbs_Plane: {
      info.type = SurfaceType::Plane;
      const gp_Pln pln = surface.Plane();
      info.axis_origin = to_vec3(pln.Location());
      gp_Dir n = pln.Axis().Direction();
      if (face.Orientation() == TopAbs_REVERSED) {
        n.Reverse();
      }
      info.axis_dir = to_vec3(n);
      break;
    }
    case GeomAbs_Cylinder: {
      info.type = SurfaceType::Cylinder;
      const gp_Cylinder cyl = surface.Cylinder();
      info.axis_origin = to_vec3(cyl.Location());
      info.axis_dir = to_vec3(cyl.Axis().Direction());
      info.radius = cyl.Radius();
      break;
    }
    case GeomAbs_Cone: {
      info.type = SurfaceType::Cone;
      const gp_Cone cone = surface.Cone();
      info.axis_origin = to_vec3(cone.Apex());
      info.axis_dir = to_vec3(cone.Axis().Direction());
      info.radius = cone.RefRadius();
      break;
    }
    case GeomAbs_Sphere: {
      info.type = SurfaceType::Sphere;
      const gp_Sphere sphere = surface.Sphere();
      info.axis_origin = to_vec3(sphere.Location());
      info.axis_dir = to_vec3(sphere.Position().Direction());
      info.radius = sphere.Radius();
      break;
    }
    case GeomAbs_Torus: {
      info.type = SurfaceType::Torus;
      const gp_Torus torus = surface.Torus();
      info.axis_origin = to_vec3(torus.Location());
      info.axis_dir = to_vec3(torus.Axis().Direction());
      info.radius = torus.MajorRadius();
      break;
    }
    case GeomAbs_BSplineSurface:
    case GeomAbs_BezierSurface:
      info.type = SurfaceType::BSpline;
      break;
    case GeomAbs_SurfaceOfRevolution:
      info.type = SurfaceType::Revolution;
      break;
    case GeomAbs_SurfaceOfExtrusion:
      info.type = SurfaceType::Extrusion;
      break;
    case GeomAbs_OffsetSurface:
      info.type = SurfaceType::Offset;
      break;
    default:
      info.type = SurfaceType::Other;
      break;
  }

  GProp_GProps props;
  BRepGProp::SurfaceProperties(face, props);
  info.area = props.Mass();
  info.centroid = to_vec3(props.CentreOfMass());

  double u, v;
  face_inner_uv(face, u, v);
  gp_Pnt point;
  gp_Dir normal;
  if (face_normal_at(face, surface, u, v, point, normal)) {
    info.sample_normal = to_vec3(normal);
  }
  info.sample_point = to_vec3(point);
  return info;
}

static EdgeInfo edge_info(const TopoDS_Edge &edge,
                          const TopTools_IndexedMapOfShape &vertices,
                          const TopTools_ListOfShape &adjacent_faces,
                          const TopTools_IndexedMapOfShape &faces)
{
  EdgeInfo info;
  info.degenerate = BRep_Tool::Degenerated(edge);

  int slot = 0;
  for (const TopoDS_Shape &face : adjacent_faces) {
    if (slot < 2) {
      info.faces[slot++] = faces.FindIndex(face) - 1;
    }
  }
  if (slot == 1 && BRep_Tool::IsClosed(edge, TopoDS::Face(adjacent_faces.First()))) {
    /* Seam edge: the same face on both sides. */
    info.faces[1] = info.faces[0];
  }

  TopoDS_Vertex v0, v1;
  TopExp::Vertices(edge, v0, v1);
  if (!v0.IsNull()) {
    info.vertices[0] = vertices.FindIndex(v0) - 1;
  }
  if (!v1.IsNull()) {
    info.vertices[1] = vertices.FindIndex(v1) - 1;
  }

  if (info.degenerate) {
    if (!v0.IsNull()) {
      info.start = info.end = info.midpoint = to_vec3(BRep_Tool::Pnt(v0));
    }
    info.smooth = true;
    return info;
  }

  const BRepAdaptor_Curve curve(edge);
  const double t0 = curve.FirstParameter();
  const double t1 = curve.LastParameter();
  const double tm = 0.5 * (t0 + t1);
  info.start = to_vec3(curve.Value(t0));
  info.end = to_vec3(curve.Value(t1));
  info.midpoint = to_vec3(curve.Value(tm));
  switch (curve.GetType()) {
    case GeomAbs_Line:
      info.type = CurveType::Line;
      break;
    case GeomAbs_Circle: {
      info.type = CurveType::Circle;
      const gp_Circ circ = curve.Circle();
      info.center = to_vec3(circ.Location());
      info.axis = to_vec3(circ.Axis().Direction());
      info.radius = circ.Radius();
      break;
    }
    case GeomAbs_Ellipse:
      info.type = CurveType::Ellipse;
      break;
    case GeomAbs_BSplineCurve:
    case GeomAbs_BezierCurve:
      info.type = CurveType::BSpline;
      break;
    default:
      info.type = CurveType::Other;
      break;
  }

  GProp_GProps props;
  BRepGProp::LinearProperties(edge, props);
  info.length = props.Mass();

  /* Smoothness from the face normals on both sides of the edge midpoint. */
  if (info.faces[0] >= 0 && info.faces[1] >= 0) {
    if (info.faces[0] == info.faces[1]) {
      info.smooth = true;
    }
    else {
      gp_Dir n0, n1;
      const TopoDS_Face &f0 = TopoDS::Face(faces(info.faces[0] + 1));
      const TopoDS_Face &f1 = TopoDS::Face(faces(info.faces[1] + 1));
      if (face_normal_along_edge(edge, f0, tm, n0) && face_normal_along_edge(edge, f1, tm, n1)) {
        constexpr double smooth_angle = 1e-3;
        info.smooth = n0.Angle(n1) < smooth_angle;
      }
    }
  }
  return info;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Kernel Implementation
 * \{ */

/** STEP translation uses process-global settings (`Interface_Static`). */
static std::mutex &step_mutex()
{
  static std::mutex mutex;
  return mutex;
}

class OcctKernel final : public Kernel {
 public:
  KernelId id() const override
  {
    return KernelId::OCCT;
  }

  const char *name() const override
  {
    return "OpenCASCADE";
  }

  Capabilities capabilities() const override
  {
    Capabilities caps;
    caps.exact_history = true;
    caps.unequal_chamfer = true;
    caps.step_export = true;
    caps.step_import = true;
    return caps;
  }

  /* ---------------------------------------------------------------- */

  OpResult box(const BoxParams &params) const override
  {
    return guarded("box", [&]() -> OpResult {
      if (params.size[0] <= 0.0 || params.size[1] <= 0.0 || params.size[2] <= 0.0) {
        return fail(Status::InvalidInput, "box size must be positive");
      }
      BRepPrimAPI_MakeBox maker(
          to_pnt(params.min), params.size[0], params.size[1], params.size[2]);
      maker.Build();
      if (!maker.IsDone()) {
        return fail(Status::Failed, "box construction failed");
      }
      OpResult result = finish(maker.Shape());
      if (result.shape.is_empty()) {
        return result;
      }
      const OcctShapeData &data = *occt_data(result.shape);
      result.history.faces.resize(data.faces.Extent());
      result.history.edges.resize(data.edges.Extent());
      result.history.exact = true;
      for (int i = 1; i <= data.faces.Extent(); i++) {
        const FaceInfo info = face_info(TopoDS::Face(data.faces(i)));
        const Vec3 &n = info.sample_normal;
        int axis = 0;
        for (int a = 1; a < 3; a++) {
          if (std::abs(n[a]) > std::abs(n[axis])) {
            axis = a;
          }
        }
        const int role = axis * 2 + (n[axis] > 0.0 ? 1 : 0);
        ElementHistory &h = result.history.faces[i - 1];
        h.origin = ElementOrigin::Generated;
        h.sources.push_back({SourceKind::PrimitiveFace, 0, role});
      }
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

      ProfileTopology topo;
      std::string error;
      if (!make_profile_faces(params.profile, topo, error)) {
        return fail(Status::InvalidInput, error);
      }

      gp_Dir dir = to_dir(params.profile.plane.normal);
      if (vec_length(params.direction) > 1e-12) {
        dir = to_dir(params.direction);
      }

      /* Where the sweep starts, measured along `dir` from the profile plane. */
      double start_offset = 0.0;
      double length = params.distance;
      switch (params.extent) {
        case ExtentMode::OneSide:
          break;
        case ExtentMode::Symmetric:
          start_offset = -0.5 * params.distance;
          length = params.distance;
          break;
        case ExtentMode::TwoSides:
          start_offset = -params.distance2;
          length = params.distance + params.distance2;
          break;
      }

      TopoDS_Shape base;
      if (topo.faces.size() == 1) {
        base = topo.faces[0];
      }
      else {
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        for (const TopoDS_Face &face : topo.faces) {
          builder.Add(compound, face);
        }
        base = compound;
      }
      if (start_offset != 0.0) {
        gp_Trsf move;
        move.SetTranslation(gp_Vec(dir) * start_offset);
        /* No copy: only the location changes, so edge TShapes (and curve ids) survive. */
        base = BRepBuilderAPI_Transform(base, move, false).Shape();
      }

      BRepPrimAPI_MakePrism prism(base, gp_Vec(dir) * length, false, true);
      prism.Build();
      if (!prism.IsDone()) {
        return fail(Status::Failed, "extrusion failed");
      }
      TopoDS_Shape shape = prism.Shape();
      /* A sweep against the face normal comes out inside-out: flip it. */
      GProp_GProps props;
      BRepGProp::VolumeProperties(shape, props);
      if (props.Mass() < 0.0) {
        shape.Reverse();
      }

      OpResult result = finish(shape);
      if (result.shape.is_empty()) {
        return result;
      }
      const OcctShapeData &data = *occt_data(result.shape);
      OpHistory &history = result.history;
      history.faces.resize(data.faces.Extent());
      history.edges.resize(data.edges.Extent());
      history.exact = true;

      auto curve_of = [&](const TopoDS_Shape &s, int64_t &r_id) {
        const auto it = topo.edge_curve.find(s.TShape().get());
        if (it == topo.edge_curve.end()) {
          return false;
        }
        r_id = it->second;
        return true;
      };

      for (TopExp_Explorer exp(base, TopAbs_FACE); exp.More(); exp.Next()) {
        const TopoDS_Shape &face = exp.Current();
        const SourceRef start_cap{SourceKind::Cap, 0, int64_t(CapRole::Start)};
        const SourceRef end_cap{SourceKind::Cap, 0, int64_t(CapRole::End)};
        record(history.faces, data.faces, prism.FirstShape(face), ElementOrigin::Generated,
               start_cap);
        record(history.faces, data.faces, prism.LastShape(face), ElementOrigin::Generated,
               end_cap);

        TopTools_IndexedMapOfShape face_edges;
        TopExp::MapShapes(face, TopAbs_EDGE, face_edges);
        for (int i = 1; i <= face_edges.Extent(); i++) {
          const TopoDS_Shape &edge = face_edges(i);
          int64_t curve_id;
          if (!curve_of(edge, curve_id)) {
            continue;
          }
          const SourceRef curve_src{SourceKind::ProfileCurve, 0, curve_id};
          for (const TopoDS_Shape &side : prism.Generated(edge)) {
            record_image(history, data, side, ElementOrigin::Generated, curve_src);
          }
          record(history.edges, data.edges, prism.FirstShape(edge), ElementOrigin::Generated,
                 curve_src);
          record(history.edges, data.edges, prism.FirstShape(edge), ElementOrigin::Generated,
                 start_cap);
          record(history.edges, data.edges, prism.LastShape(edge), ElementOrigin::Generated,
                 curve_src);
          record(history.edges, data.edges, prism.LastShape(edge), ElementOrigin::Generated,
                 end_cap);
        }

        TopTools_IndexedMapOfShape face_vertices;
        TopExp::MapShapes(face, TopAbs_VERTEX, face_vertices);
        for (int i = 1; i <= face_vertices.Extent(); i++) {
          const TopoDS_Shape &vertex = face_vertices(i);
          const auto it = topo.vertex_curve.find(vertex.TShape().get());
          if (it == topo.vertex_curve.end()) {
            continue;
          }
          const SourceRef vertex_src{SourceKind::ProfileVertex, 0, it->second};
          for (const TopoDS_Shape &lateral : prism.Generated(vertex)) {
            record_image(history, data, lateral, ElementOrigin::Generated, vertex_src);
          }
        }
      }
      return result;
    });
  }

  /* ---------------------------------------------------------------- */

  OpResult boolean(const Shape &base,
                   const std::span<const Shape> tools,
                   const BooleanOp op) const override
  {
    return guarded("boolean", [&]() -> OpResult {
      const OcctShapeData *base_data = occt_data(base);
      if (!base_data || tools.empty()) {
        return fail(Status::InvalidInput, "boolean needs a base shape and at least one tool");
      }
      std::vector<const OcctShapeData *> inputs = {base_data};
      TopTools_ListOfShape arguments, tool_list;
      arguments.Append(base_data->shape);
      for (const Shape &tool : tools) {
        const OcctShapeData *tool_data = occt_data(tool);
        if (!tool_data) {
          return fail(Status::InvalidInput, "boolean tool is not an OpenCASCADE shape");
        }
        inputs.push_back(tool_data);
        tool_list.Append(tool_data->shape);
      }

      std::unique_ptr<BRepAlgoAPI_BooleanOperation> algo;
      switch (op) {
        case BooleanOp::Union:
          algo = std::make_unique<BRepAlgoAPI_Fuse>();
          break;
        case BooleanOp::Subtract:
          algo = std::make_unique<BRepAlgoAPI_Cut>();
          break;
        case BooleanOp::Intersect:
          algo = std::make_unique<BRepAlgoAPI_Common>();
          break;
      }
      algo->SetArguments(arguments);
      algo->SetTools(tool_list);
      /* Never modify the inputs: they are shared with other (cached) results. */
      algo->SetNonDestructive(true);
      algo->SetRunParallel(true);
      algo->SetToFillHistory(true);
      algo->Build();
      if (!algo->IsDone() || algo->HasErrors()) {
        return fail(Status::Failed, "boolean operation failed");
      }

      Handle(BRepTools_History) history = algo->History();
      TopoDS_Shape shape = algo->Shape();

      /* Merge coplanar / co-cylindrical faces split by the boolean (model "refine"). */
      ShapeUpgrade_UnifySameDomain unify(shape, true, true, false);
      unify.SetSafeInputMode(true);
      unify.Build();
      shape = unify.Shape();
      if (history.IsNull()) {
        history = unify.History();
      }
      else {
        history->Merge(unify.History());
      }

      OpResult result = finish(shape);
      if (result.shape.is_empty()) {
        return result;
      }
      if (!history.IsNull()) {
        ToolsHistoryQuery query(history);
        result.history = history_from_query(inputs, *occt_data(result.shape), query);
      }
      return result;
    });
  }

  /* ---------------------------------------------------------------- */

  OpResult fillet(const Shape &shape, const FilletParams &params) const override
  {
    return guarded("fillet", [&]() -> OpResult {
      const OcctShapeData *data = occt_data(shape);
      if (!data || params.edges.empty()) {
        return fail(Status::InvalidInput, "fillet needs a shape and at least one edge");
      }
      if (!(params.radius > 0.0)) {
        return fail(Status::InvalidInput, "fillet radius must be positive");
      }
      BRepFilletAPI_MakeFillet maker(data->shape);
      for (const int index : params.edges) {
        if (index < 0 || index >= data->edges.Extent()) {
          return fail(Status::InvalidInput, "fillet edge index out of range");
        }
        maker.Add(params.radius, TopoDS::Edge(data->edges(index + 1)));
      }
      maker.Build();
      if (!maker.IsDone()) {
        return fail(Status::Failed,
                    "fillet failed (radius too large for the geometry, or unsupported corner)");
      }
      OpResult result = finish(maker.Shape());
      if (result.shape.is_empty()) {
        return result;
      }
      MakeShapeQuery query(maker);
      result.history = history_from_query({data}, *occt_data(result.shape), query);
      return result;
    });
  }

  OpResult chamfer(const Shape &shape, const ChamferParams &params) const override
  {
    return guarded("chamfer", [&]() -> OpResult {
      const OcctShapeData *data = occt_data(shape);
      if (!data || params.edges.empty()) {
        return fail(Status::InvalidInput, "chamfer needs a shape and at least one edge");
      }
      if (!(params.distance > 0.0)) {
        return fail(Status::InvalidInput, "chamfer distance must be positive");
      }
      TopTools_IndexedDataMapOfShapeListOfShape edge_faces;
      TopExp::MapShapesAndAncestors(data->shape, TopAbs_EDGE, TopAbs_FACE, edge_faces);

      BRepFilletAPI_MakeChamfer maker(data->shape);
      for (size_t i = 0; i < params.edges.size(); i++) {
        const int index = params.edges[i];
        if (index < 0 || index >= data->edges.Extent()) {
          return fail(Status::InvalidInput, "chamfer edge index out of range");
        }
        const TopoDS_Edge &edge = TopoDS::Edge(data->edges(index + 1));
        if (params.distance2 > 0.0) {
          TopoDS_Face face;
          if (i < params.reference_faces.size() && params.reference_faces[i] >= 0 &&
              params.reference_faces[i] < data->faces.Extent())
          {
            face = TopoDS::Face(data->faces(params.reference_faces[i] + 1));
          }
          else {
            const TopTools_ListOfShape *faces = edge_faces.Seek(edge);
            if (!faces || faces->IsEmpty()) {
              return fail(Status::InvalidInput, "chamfer edge has no adjacent face");
            }
            face = TopoDS::Face(faces->First());
          }
          maker.Add(params.distance, params.distance2, edge, face);
        }
        else {
          maker.Add(params.distance, edge);
        }
      }
      maker.Build();
      if (!maker.IsDone()) {
        return fail(Status::Failed, "chamfer failed (distance too large for the geometry)");
      }
      OpResult result = finish(maker.Shape());
      if (result.shape.is_empty()) {
        return result;
      }
      MakeShapeQuery query(maker);
      result.history = history_from_query({data}, *occt_data(result.shape), query);
      return result;
    });
  }

  OpResult transform(const Shape &shape, const Transform &transform) const override
  {
    return guarded("transform", [&]() -> OpResult {
      const OcctShapeData *data = occt_data(shape);
      if (!data) {
        return fail(Status::InvalidInput, "transform needs an OpenCASCADE shape");
      }
      const std::array<double, 16> &m = transform.matrix;
      gp_Trsf trsf;
      /* Column-major input: element (row r, column c) is m[c * 4 + r]. */
      trsf.SetValues(m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13], m[2], m[6], m[10], m[14]);
      BRepBuilderAPI_Transform maker(data->shape, trsf, true);
      if (!maker.IsDone()) {
        return fail(Status::Failed, "transform failed");
      }
      OpResult result = finish(maker.Shape());
      if (result.shape.is_empty()) {
        return result;
      }
      MakeShapeQuery query(maker);
      result.history = history_from_query({data}, *occt_data(result.shape), query);
      return result;
    });
  }

  /* ---------------------------------------------------------------- */

  Topology topology(const Shape &shape) const override
  {
    Topology topo;
    const OcctShapeData *data = occt_data(shape);
    if (!data) {
      return topo;
    }
    try {
      TopTools_IndexedDataMapOfShapeListOfShape edge_faces;
      TopExp::MapShapesAndAncestors(data->shape, TopAbs_EDGE, TopAbs_FACE, edge_faces);

      std::vector<int> face_solid(data->faces.Extent(), 0);
      for (int s = 1; s <= data->solids.Extent(); s++) {
        for (TopExp_Explorer exp(data->solids(s), TopAbs_FACE); exp.More(); exp.Next()) {
          const int index = data->faces.FindIndex(exp.Current());
          if (index > 0) {
            face_solid[index - 1] = s - 1;
          }
        }
      }

      topo.faces.reserve(data->faces.Extent());
      for (int i = 1; i <= data->faces.Extent(); i++) {
        const TopoDS_Face &face = TopoDS::Face(data->faces(i));
        FaceInfo info = face_info(face);
        info.solid = face_solid[i - 1];
        TopTools_IndexedMapOfShape face_edges;
        TopExp::MapShapes(face, TopAbs_EDGE, face_edges);
        for (int e = 1; e <= face_edges.Extent(); e++) {
          info.edges.push_back(data->edges.FindIndex(face_edges(e)) - 1);
        }
        topo.faces.push_back(std::move(info));
      }

      const TopTools_ListOfShape no_faces;
      topo.edges.reserve(data->edges.Extent());
      for (int i = 1; i <= data->edges.Extent(); i++) {
        const TopoDS_Edge &edge = TopoDS::Edge(data->edges(i));
        const TopTools_ListOfShape *faces = edge_faces.Seek(edge);
        topo.edges.push_back(edge_info(edge, data->vertices, faces ? *faces : no_faces,
                                       data->faces));
      }

      topo.vertices.reserve(data->vertices.Extent());
      for (int i = 1; i <= data->vertices.Extent(); i++) {
        topo.vertices.push_back(to_vec3(BRep_Tool::Pnt(TopoDS::Vertex(data->vertices(i)))));
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
    const OcctShapeData *data = occt_data(shape);
    if (!data) {
      return mass;
    }
    try {
      GProp_GProps volume;
      BRepGProp::VolumeProperties(data->shape, volume);
      mass.volume = volume.Mass();
      mass.center_of_mass = to_vec3(volume.CentreOfMass());
      GProp_GProps surface;
      BRepGProp::SurfaceProperties(data->shape, surface);
      mass.area = surface.Mass();
    }
    catch (...) {
    }
    return mass;
  }

  bool is_valid(const Shape &shape, std::string *r_reason) const override
  {
    const OcctShapeData *data = occt_data(shape);
    if (!data) {
      if (r_reason) {
        *r_reason = "not an OpenCASCADE shape";
      }
      return false;
    }
    try {
      return check_valid(data->shape, r_reason);
    }
    catch (...) {
      if (r_reason) {
        *r_reason = "validity check crashed";
      }
      return false;
    }
  }

  /* ---------------------------------------------------------------- */

  Status tessellate(const Shape &shape,
                    const TessellationParams &params,
                    Tessellation &r_mesh) const override
  {
    r_mesh = Tessellation();
    const OcctShapeData *data = occt_data(shape);
    if (!data) {
      return Status::InvalidInput;
    }
    try {
      return tessellate_impl(*data, params, r_mesh);
    }
    catch (...) {
      r_mesh = Tessellation();
      return Status::Failed;
    }
  }

  /* ---------------------------------------------------------------- */

  Status export_step(const std::span<const Shape> shapes,
                     const std::span<const std::string> /*names*/,
                     const std::string &filepath,
                     std::string *r_message) const override
  {
    std::lock_guard lock(step_mutex());
    try {
      STEPControl_Writer writer;
      Interface_Static::SetCVal("write.step.unit", "MM");
      Interface_Static::SetCVal("write.step.schema", "AP214IS");
      for (const Shape &shape : shapes) {
        const OcctShapeData *data = occt_data(shape);
        if (!data) {
          if (r_message) {
            *r_message = "STEP export needs OpenCASCADE shapes";
          }
          return Status::InvalidInput;
        }
        if (writer.Transfer(data->shape, STEPControl_AsIs) != IFSelect_RetDone) {
          if (r_message) {
            *r_message = "could not translate a shape to STEP";
          }
          return Status::Failed;
        }
      }
      if (writer.Write(filepath.c_str()) != IFSelect_RetDone) {
        if (r_message) {
          *r_message = "could not write " + filepath;
        }
        return Status::Failed;
      }
      return Status::Ok;
    }
    catch (const Standard_Failure &e) {
      if (r_message) {
        *r_message = e.GetMessageString();
      }
      return Status::Failed;
    }
    catch (...) {
      return Status::Failed;
    }
  }

  Status import_step(const std::string &filepath,
                     std::vector<Shape> &r_shapes,
                     std::string *r_message) const override
  {
    std::lock_guard lock(step_mutex());
    r_shapes.clear();
    try {
      STEPControl_Reader reader;
      if (reader.ReadFile(filepath.c_str()) != IFSelect_RetDone) {
        if (r_message) {
          *r_message = "could not read " + filepath;
        }
        return Status::Failed;
      }
      reader.TransferRoots();
      for (int i = 1; i <= reader.NbShapes(); i++) {
        const TopoDS_Shape s = reader.Shape(i);
        if (!s.IsNull()) {
          r_shapes.emplace_back(wrap(s));
        }
      }
      return r_shapes.empty() ? Status::Failed : Status::Ok;
    }
    catch (const Standard_Failure &e) {
      if (r_message) {
        *r_message = e.GetMessageString();
      }
      return Status::Failed;
    }
    catch (...) {
      return Status::Failed;
    }
  }

 private:
  static Status tessellate_impl(const OcctShapeData &data,
                                const TessellationParams &params,
                                Tessellation &r_mesh)
  {
    /* Mesh a structural copy: meshing stores triangulations on the faces, and the faces of
     * `data` are shared with other shapes that may be meshed or read concurrently. Geometry is
     * shared (read-only), topology is copied, so element order is unchanged. */
    BRepBuilderAPI_Copy copier(data.shape, false, false);
    const TopoDS_Shape copy = copier.Shape();
    TopTools_IndexedMapOfShape faces, edges, vertices;
    TopExp::MapShapes(copy, TopAbs_FACE, faces);
    TopExp::MapShapes(copy, TopAbs_EDGE, edges);
    TopExp::MapShapes(copy, TopAbs_VERTEX, vertices);
    if (faces.Extent() != data.faces.Extent() || edges.Extent() != data.edges.Extent()) {
      return Status::Failed;
    }

    BRepMesh_IncrementalMesh mesher(
        copy, params.linear_deflection, false, params.angular_deflection, true);
    (void)mesher;

    std::vector<Float3> &positions = r_mesh.positions;
    auto add_position = [&](const gp_Pnt &p) {
      positions.push_back({float(p.X()), float(p.Y()), float(p.Z())});
      return int(positions.size()) - 1;
    };

    std::vector<int> vertex_index(vertices.Extent(), -1);
    for (int i = 1; i <= vertices.Extent(); i++) {
      vertex_index[i - 1] = add_position(BRep_Tool::Pnt(TopoDS::Vertex(vertices(i))));
    }
    auto vertex_of = [&](const TopoDS_Vertex &v) {
      const int i = v.IsNull() ? 0 : vertices.FindIndex(v);
      return i > 0 ? vertex_index[i - 1] : -1;
    };

    r_mesh.edge_polylines.assign(edges.Extent(), {});
    std::vector<bool> edge_done(edges.Extent(), false);
    bool all_faces_meshed = true;

    for (int fi = 1; fi <= faces.Extent(); fi++) {
      const TopoDS_Face &face = TopoDS::Face(faces(fi));
      TopLoc_Location loc;
      const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
      if (tri.IsNull() || tri->NbTriangles() == 0) {
        all_faces_meshed = false;
        continue;
      }
      const gp_Trsf trsf = loc.Transformation();
      const int nodes_num = tri->NbNodes();
      std::vector<int> node_index(nodes_num + 1, -1);

      /* Weld boundary nodes onto the shared per-edge polylines. */
      for (TopExp_Explorer exp(face, TopAbs_EDGE); exp.More(); exp.Next()) {
        const TopoDS_Edge &edge = TopoDS::Edge(exp.Current());
        const int ei = edges.FindIndex(edge) - 1;
        if (ei < 0) {
          continue;
        }
        const Handle(Poly_PolygonOnTriangulation) polygon =
            BRep_Tool::PolygonOnTriangulation(edge, tri, loc);
        if (polygon.IsNull()) {
          continue;
        }
        const TColStd_Array1OfInteger &poly_nodes = polygon->Nodes();
        const int n = poly_nodes.Length();
        const int lower = poly_nodes.Lower();

        if (BRep_Tool::Degenerated(edge)) {
          TopoDS_Vertex v0, v1;
          TopExp::Vertices(edge, v0, v1);
          const int vi = vertex_of(v0);
          for (int k = 0; k < n; k++) {
            node_index[poly_nodes(lower + k)] = vi;
          }
          continue;
        }

        std::vector<int> &polyline = r_mesh.edge_polylines[ei];
        if (!edge_done[ei]) {
          /* First face to see this edge defines its polyline. Nodes run in increasing edge
           * parameter, i.e. from the forward edge's first vertex to its last. */
          edge_done[ei] = true;
          TopoDS_Vertex v_first, v_last;
          TopExp::Vertices(TopoDS::Edge(edge.Oriented(TopAbs_FORWARD)), v_first, v_last);
          polyline.resize(n);
          for (int k = 0; k < n; k++) {
            if (k == 0 && !v_first.IsNull()) {
              polyline[k] = vertex_of(v_first);
            }
            else if (k == n - 1 && !v_last.IsNull()) {
              polyline[k] = vertex_of(v_last);
            }
            else {
              polyline[k] = add_position(tri->Node(poly_nodes(lower + k)).Transformed(trsf));
            }
          }
        }
        if (int(polyline.size()) == n) {
          for (int k = 0; k < n; k++) {
            node_index[poly_nodes(lower + k)] = polyline[k];
          }
        }
      }

      /* Interior nodes. */
      for (int i = 1; i <= nodes_num; i++) {
        if (node_index[i] == -1) {
          node_index[i] = add_position(tri->Node(i).Transformed(trsf));
        }
      }

      if (!tri->HasNormals()) {
        BRepLib_ToolTriangulatedShape::ComputeNormals(face, tri);
      }
      const bool reversed = face.Orientation() == TopAbs_REVERSED;
      auto corner_normal = [&](const int node) -> Float3 {
        if (!tri->HasNormals()) {
          return {0.0f, 0.0f, 0.0f};
        }
        gp_Dir n = tri->Normal(node).Transformed(trsf);
        if (reversed) {
          n.Reverse();
        }
        return {float(n.X()), float(n.Y()), float(n.Z())};
      };

      for (int t = 1; t <= tri->NbTriangles(); t++) {
        int n1, n2, n3;
        tri->Triangle(t).Get(n1, n2, n3);
        if (reversed) {
          std::swap(n2, n3);
        }
        const int a = node_index[n1], b = node_index[n2], c = node_index[n3];
        if (a == b || b == c || a == c || a < 0 || b < 0 || c < 0) {
          continue; /* Collapsed at a pole or apex. */
        }
        r_mesh.triangles.push_back({a, b, c});
        r_mesh.triangle_faces.push_back(fi - 1);
        r_mesh.corner_normals.push_back(corner_normal(n1));
        r_mesh.corner_normals.push_back(corner_normal(n2));
        r_mesh.corner_normals.push_back(corner_normal(n3));
      }
    }

    /* Smoothness per edge, from the original (unmeshed) topology. */
    TopTools_IndexedDataMapOfShapeListOfShape edge_faces;
    TopExp::MapShapesAndAncestors(data.shape, TopAbs_EDGE, TopAbs_FACE, edge_faces);
    const TopTools_ListOfShape no_faces;
    r_mesh.edge_smooth.resize(data.edges.Extent());
    for (int i = 1; i <= data.edges.Extent(); i++) {
      const TopoDS_Edge &edge = TopoDS::Edge(data.edges(i));
      const TopTools_ListOfShape *adjacent = edge_faces.Seek(edge);
      r_mesh.edge_smooth[i - 1] =
          edge_info(edge, data.vertices, adjacent ? *adjacent : no_faces, data.faces).smooth;
    }

    return all_faces_meshed ? Status::Ok : Status::InvalidResult;
  }
};

/** OCCT reports progress and statistics (e.g. every STEP transfer) on stdout by default.
 * Keep only failures. */
static void quiet_messages()
{
  const Handle(Message_Messenger) &messenger = Message::DefaultMessenger();
  for (Message_SequenceOfPrinters::Iterator it(messenger->Printers()); it.More(); it.Next()) {
    it.Value()->SetTraceLevel(Message_Fail);
  }
}

const Kernel *kernel_instance()
{
  static const OcctKernel kernel = [] {
    quiet_messages();
    return OcctKernel();
  }();
  return &kernel;
}

/** \} */

}  // namespace cad::kernel::occt

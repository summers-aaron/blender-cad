/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_cad_kernel
 *
 * Kernel-neutral B-rep modeling interface.
 *
 * Everything parametric CAD needs from a geometry kernel goes through this header: building
 * solids from sketch profiles, booleans, fillets, topology queries, topology history (for
 * persistent naming), tessellation for display, mass properties and data exchange.
 *
 * Backends (OpenCASCADE, NVIDIA SMLib) implement #Kernel in their own translation units. This
 * header deliberately uses only the C++ standard library: no Blender, OCCT or SMLib types leak
 * out, so backends can be swapped and their (macro-heavy) headers stay contained.
 *
 * Conventions:
 * - Lengths are millimeters, angles radians.
 * - Faces, edges and vertices of a #Shape are addressed by index into a canonical,
 *   deterministic per-shape order. The same shape always reports the same order.
 * - No function throws. Failures are reported through #Status.
 * - Thread safety: every #Kernel method may be called from any thread. Backends whose
 *   library is not thread-safe serialize internally.
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace cad::kernel {

/* -------------------------------------------------------------------- */
/** \name Basic Types
 * \{ */

using Vec2 = std::array<double, 2>;
using Vec3 = std::array<double, 3>;
using Float3 = std::array<float, 3>;

/** Identifies a kernel backend. Stored in files: never reorder or reuse values. */
enum class KernelId : int8_t {
  OCCT = 0,
  SMLib = 1,
};

enum class Status : int8_t {
  Ok = 0,
  /** The kernel tried and failed (e.g. a fillet radius too large for the geometry). */
  Failed,
  /** The input is malformed (open profile, bad index, non-positive distance, ...). */
  InvalidInput,
  /** The backend does not implement this operation or option. */
  Unsupported,
  /** The kernel produced a result, but it failed validation. */
  InvalidResult,
};

const char *status_name(Status status);

/** Right-handed frame of a sketch plane: `y = normal x x_dir`. */
struct Plane {
  Vec3 origin = {0.0, 0.0, 0.0};
  Vec3 x_dir = {1.0, 0.0, 0.0};
  Vec3 normal = {0.0, 0.0, 1.0};
};

/** Rigid transform plus uniform scale, column-major 4x4 (OpenGL / Blender layout). */
struct Transform {
  std::array<double, 16> matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Profiles (sketch output)
 * \{ */

enum class ProfileCurveType : int8_t {
  Line,
  /** Circular arc from `start` to `end` around `center`, counter-clockwise when `ccw`. */
  Arc,
  /** Full circle around `center`; `start` / `end` unused. */
  Circle,
  /** Non-rational or rational B-spline in plane coordinates. */
  BSpline,
};

/** One curve of a profile loop, in 2D plane coordinates. */
struct ProfileCurve {
  ProfileCurveType type = ProfileCurveType::Line;
  /** Sketch entity id, carried into topology history (side faces of an extrusion are named
   * after the curve that swept them). */
  int64_t id = 0;
  Vec2 start = {0.0, 0.0};
  Vec2 end = {0.0, 0.0};
  Vec2 center = {0.0, 0.0};
  double radius = 0.0;
  bool ccw = true;
  /* B-spline data. */
  int degree = 3;
  std::vector<Vec2> poles;
  /** Empty for non-rational. */
  std::vector<double> weights;
  /** Full knot vector (with multiplicities expanded). */
  std::vector<double> knots;
  bool periodic = false;
};

/** A closed, ordered chain of curves (each curve's end is the next curve's start). */
struct ProfileLoop {
  std::vector<ProfileCurve> curves;
};

/** A planar region: one outer loop and any number of hole loops inside it. */
struct ProfileRegion {
  ProfileLoop outer;
  std::vector<ProfileLoop> holes;
};

/** The regions of one sketch selected for a feature. Regions must not overlap. */
struct Profile {
  Plane plane;
  std::vector<ProfileRegion> regions;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shapes
 * \{ */

/**
 * Backend-owned shape data. Backends derive from this; it is immutable once created, so it can
 * be shared between threads, results and caches freely.
 */
class ShapeData {
 public:
  virtual ~ShapeData() = default;
  virtual KernelId kernel() const = 0;
  virtual int faces_num() const = 0;
  virtual int edges_num() const = 0;
  virtual int vertices_num() const = 0;
  /** Number of solids (bodies). A shape may hold several disjoint solids. */
  virtual int solids_num() const = 0;
};

/** Cheap-to-copy immutable handle to a B-rep (one or more solids). May be empty. */
class Shape {
 public:
  Shape() = default;
  explicit Shape(std::shared_ptr<const ShapeData> data) : data_(std::move(data)) {}

  bool is_empty() const
  {
    return !data_;
  }
  KernelId kernel() const
  {
    return data_->kernel();
  }
  int faces_num() const
  {
    return data_ ? data_->faces_num() : 0;
  }
  int edges_num() const
  {
    return data_ ? data_->edges_num() : 0;
  }
  int vertices_num() const
  {
    return data_ ? data_->vertices_num() : 0;
  }
  int solids_num() const
  {
    return data_ ? data_->solids_num() : 0;
  }
  const ShapeData *data() const
  {
    return data_.get();
  }
  const std::shared_ptr<const ShapeData> &data_ptr() const
  {
    return data_;
  }

 private:
  std::shared_ptr<const ShapeData> data_;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Topology History
 *
 * For every face and edge of an operation's result, where it came from. Persistent naming is
 * built from this, so it must be exact wherever #OpHistory::exact is set.
 * \{ */

enum class ElementOrigin : int8_t {
  /** Unknown provenance (backend could not tell). */
  Unknown = 0,
  /** Same element as in an input, untouched. */
  Kept,
  /** An input element changed (trimmed, extended, ...), still one-to-one. */
  Modified,
  /** New element created from a lower-dimensional source (a face swept from an edge or curve,
   * a fillet face from the filleted edge, ...). */
  Generated,
  /** One of several pieces an input element was split into. */
  Split,
  /** Several input elements merged into this one. */
  Merged,
};

enum class SourceKind : int8_t {
  /** `index` is a face index of input shape `input`. */
  Face,
  /** `index` is an edge index of input shape `input`. */
  Edge,
  /** `index` is a vertex index of input shape `input`. */
  Vertex,
  /** `index` is a #ProfileCurve::id. */
  ProfileCurve,
  /** `index` is the id of the profile curve whose start vertex this comes from. */
  ProfileVertex,
  /** `index` is a #CapRole (start / end cap of a sweep). */
  Cap,
  /** `index` is a primitive-specific face role (see #BoxFace). */
  PrimitiveFace,
};

enum class CapRole : int8_t {
  Start = 0,
  End = 1,
};

enum class BoxFace : int8_t {
  MinX = 0,
  MaxX,
  MinY,
  MaxY,
  MinZ,
  MaxZ,
};

struct SourceRef {
  SourceKind kind = SourceKind::Face;
  /** Which input shape (0 = first argument, 1.. = tools), unused for profile/cap/primitive. */
  int8_t input = 0;
  int64_t index = 0;

  friend bool operator==(const SourceRef &a, const SourceRef &b) = default;
};

struct ElementHistory {
  ElementOrigin origin = ElementOrigin::Unknown;
  std::vector<SourceRef> sources;
};

struct OpHistory {
  /** Indexed by result face / edge index. */
  std::vector<ElementHistory> faces;
  std::vector<ElementHistory> edges;
  /** True when every element's provenance comes from the kernel's own bookkeeping rather than
   * geometric matching. */
  bool exact = false;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operations
 * \{ */

struct OpResult {
  Status status = Status::Failed;
  Shape shape;
  OpHistory history;
  /** Human-readable reason on failure, or a warning on success. */
  std::string message;

  bool ok() const
  {
    return status == Status::Ok;
  }
};

struct BoxParams {
  Vec3 min = {0.0, 0.0, 0.0};
  Vec3 size = {10.0, 10.0, 10.0};
};

enum class ExtentMode : int8_t {
  /** From the profile plane, `distance` along the direction. */
  OneSide = 0,
  /** `distance / 2` to each side of the profile plane. */
  Symmetric,
  /** `distance` along the direction and `distance2` against it. */
  TwoSides,
};

struct ExtrudeParams {
  Profile profile;
  ExtentMode extent = ExtentMode::OneSide;
  /** May be negative to extrude against the plane normal (one side only). */
  double distance = 10.0;
  double distance2 = 0.0;
  /** Extrusion direction; zero means the profile plane normal. */
  Vec3 direction = {0.0, 0.0, 0.0};
};

enum class BooleanOp : int8_t {
  Union = 0,
  Subtract,
  Intersect,
};

struct FilletParams {
  /** Edge indices of the input shape. */
  std::vector<int> edges;
  double radius = 1.0;
};

struct ChamferParams {
  std::vector<int> edges;
  double distance = 1.0;
  /** Second distance for unequal chamfers; <= 0 means equal. Measured on `reference_faces`. */
  double distance2 = 0.0;
  /** Per edge: the face the first distance is measured on (-1 = kernel's choice). */
  std::vector<int> reference_faces;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Queries
 * \{ */

enum class SurfaceType : int8_t {
  Plane = 0,
  Cylinder,
  Cone,
  Sphere,
  Torus,
  BSpline,
  Revolution,
  Extrusion,
  Offset,
  Other,
};

enum class CurveType : int8_t {
  Line = 0,
  Circle,
  Ellipse,
  BSpline,
  Other,
};

struct FaceInfo {
  SurfaceType type = SurfaceType::Other;
  double area = 0.0;
  Vec3 centroid = {0.0, 0.0, 0.0};
  /** A point guaranteed to be on the face (inside its boundary) and the outward normal there.
   * Used for picking and as the geometric fallback of persistent references. */
  Vec3 sample_point = {0.0, 0.0, 0.0};
  Vec3 sample_normal = {0.0, 0.0, 1.0};
  /** Axis for cylinders, cones, spheres, tori; plane origin and normal for planes. */
  Vec3 axis_origin = {0.0, 0.0, 0.0};
  Vec3 axis_dir = {0.0, 0.0, 1.0};
  /** Radius for cylinder/sphere/torus (major), 0 otherwise. */
  double radius = 0.0;
  /** Indices of the edges bounding this face. */
  std::vector<int> edges;
  /** Index of the solid this face belongs to. */
  int solid = 0;
};

struct EdgeInfo {
  CurveType type = CurveType::Other;
  /** The (up to) two faces meeting at this edge, -1 when absent (seam: both the same). */
  std::array<int, 2> faces = {-1, -1};
  std::array<int, 2> vertices = {-1, -1};
  Vec3 start = {0.0, 0.0, 0.0};
  Vec3 end = {0.0, 0.0, 0.0};
  Vec3 midpoint = {0.0, 0.0, 0.0};
  double length = 0.0;
  /** Tangent-continuous (G1 or better) across the two faces: no visible crease. */
  bool smooth = false;
  bool degenerate = false;
  /** Circle center/axis/radius when `type == Circle`. */
  Vec3 center = {0.0, 0.0, 0.0};
  Vec3 axis = {0.0, 0.0, 1.0};
  double radius = 0.0;
};

struct Topology {
  std::vector<FaceInfo> faces;
  std::vector<EdgeInfo> edges;
  std::vector<Vec3> vertices;
};

struct MassProperties {
  double volume = 0.0;
  double area = 0.0;
  Vec3 center_of_mass = {0.0, 0.0, 0.0};
};

struct TessellationParams {
  /** Maximum chordal deviation in mm. */
  double linear_deflection = 0.05;
  /** Maximum angle between adjacent facet normals, radians. */
  double angular_deflection = 0.35;
};

/**
 * Display/export mesh of a shape, welded across faces: vertices on a B-rep edge are shared by
 * both faces, so the mesh is closed wherever the B-rep is.
 */
struct Tessellation {
  std::vector<Float3> positions;
  std::vector<std::array<int, 3>> triangles;
  /** Per triangle: index of the B-rep face it belongs to. */
  std::vector<int> triangle_faces;
  /** Per triangle corner (3 per triangle): surface normal, oriented outward. */
  std::vector<Float3> corner_normals;
  /** Per B-rep edge: the polyline through mesh vertices that lies on it (empty if degenerate). */
  std::vector<std::vector<int>> edge_polylines;
  /** Per B-rep edge: #EdgeInfo::smooth. */
  std::vector<bool> edge_smooth;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Kernel
 * \{ */

struct Capabilities {
  /** Topology history is exact for all supported operations. */
  bool exact_history = false;
  bool unequal_chamfer = false;
  bool step_export = false;
  bool step_import = false;
};

class Kernel {
 public:
  virtual ~Kernel() = default;

  virtual KernelId id() const = 0;
  /** UI name, e.g. "OpenCASCADE". */
  virtual const char *name() const = 0;
  virtual Capabilities capabilities() const = 0;

  /* Construction. */
  virtual OpResult box(const BoxParams &params) const = 0;
  virtual OpResult extrude(const ExtrudeParams &params) const = 0;

  /* Modification. `base` is input 0, `tools` are inputs 1... in the history. */
  virtual OpResult boolean(const Shape &base,
                           std::span<const Shape> tools,
                           BooleanOp op) const = 0;
  virtual OpResult fillet(const Shape &shape, const FilletParams &params) const = 0;
  virtual OpResult chamfer(const Shape &shape, const ChamferParams &params) const = 0;
  virtual OpResult transform(const Shape &shape, const Transform &transform) const = 0;

  /* Queries. */
  virtual Topology topology(const Shape &shape) const = 0;
  virtual MassProperties mass_properties(const Shape &shape) const = 0;
  /** Full validity check (topology and geometry). */
  virtual bool is_valid(const Shape &shape, std::string *r_reason) const = 0;
  virtual Status tessellate(const Shape &shape,
                            const TessellationParams &params,
                            Tessellation &r_mesh) const = 0;

  /* Exchange. */
  virtual Status export_step(std::span<const Shape> shapes,
                             std::span<const std::string> names,
                             const std::string &filepath,
                             std::string *r_message) const = 0;
  virtual Status import_step(const std::string &filepath,
                             std::vector<Shape> &r_shapes,
                             std::string *r_message) const = 0;
};

/** The backend for `id`, or null when it was not compiled in. Lives for the whole process. */
const Kernel *kernel_get(KernelId id);

/** Backends compiled into this build, default (preferred) kernel first. */
std::vector<KernelId> kernels_available();

/** \} */

}  // namespace cad::kernel

/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cad
 *
 * Timeline evaluation, the fingerprint cache and persistent naming.
 *
 * Persistent naming: every face gets a name composed from the kernel's topology history, so it
 * survives upstream edits as long as the element still exists:
 *   - `F<uid>:top`, `F<uid>:left`, ...      box faces
 *   - `F<uid>:start`, `F<uid>:end`           extrusion caps
 *   - `F<uid>:side:<curve uid>`              extrusion side swept by a sketch curve
 *   - `F<uid>:blend(<edge name>)`            fillet / chamfer face made from an edge
 *   - kept / modified faces keep their input's name; merged faces the smallest one
 *   - `<name>#[<neighbours>]`                pieces of a split face, told apart by the faces
 *                                            around them; `~<k>` as a last resort
 * Edges are named after the faces they separate: `<faceA>|<faceB>` (sorted), `~<k>` when the
 * same two faces meet more than once.
 *
 * References (fillet edges, ...) store the name and the picked point; they resolve exactly by
 * name, then by matching face names without split suffixes, then by position.
 */

#include <algorithm>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>

#include "BLI_function_ref.hh"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"

#include "CAD_eval.hh"

namespace blender::cad {

using kernel::ElementHistory;
using kernel::ElementOrigin;
using kernel::OpHistory;
using kernel::OpResult;
using kernel::SourceKind;
using kernel::SourceRef;

const char *feature_type_name(const FeatureType type)
{
  switch (type) {
    case FeatureType::Sketch:
      return "Sketch";
    case FeatureType::Box:
      return "Box";
    case FeatureType::Extrude:
      return "Extrude";
    case FeatureType::Fillet:
      return "Fillet";
    case FeatureType::Chamfer:
      return "Chamfer";
  }
  return "Feature";
}

const Feature *Document::find_feature(const int uid) const
{
  for (const Feature &feature : features) {
    if (feature.uid == uid) {
      return &feature;
    }
  }
  return nullptr;
}

int Body::find_face(const StringRef name) const
{
  for (const int i : face_names.index_range()) {
    if (face_names[i] == name) {
      return i;
    }
  }
  return -1;
}

int Body::find_edge(const StringRef name) const
{
  for (const int i : edge_names.index_range()) {
    if (edge_names[i] == name) {
      return i;
    }
  }
  return -1;
}

const FeatureResult *EvalResult::find(const int uid) const
{
  for (const FeatureResult &result : features) {
    if (result.uid == uid) {
      return &result;
    }
  }
  return nullptr;
}

int EvalResult::errors_num() const
{
  int count = 0;
  for (const FeatureResult &result : features) {
    count += result.status == FeatureStatus::Error ? 1 : 0;
  }
  return count;
}

/* -------------------------------------------------------------------- */
/** \name Fingerprints
 * \{ */

class Hasher {
 public:
  explicit Hasher(const uint64_t seed) : h_(seed ^ 0xcbf29ce484222325ull) {}

  Hasher &bytes(const void *data, const size_t size)
  {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; i++) {
      h_ ^= p[i];
      h_ *= 0x100000001b3ull;
    }
    return *this;
  }
  template<typename T> Hasher &add(const T &value)
  {
    static_assert(std::is_trivially_copyable_v<T>);
    return bytes(&value, sizeof(T));
  }
  Hasher &add_string(const StringRef s)
  {
    add(int64_t(s.size()));
    return bytes(s.data(), size_t(s.size()));
  }
  uint64_t get() const
  {
    return h_;
  }

 private:
  uint64_t h_;
};

static void hash_plane(Hasher &h, const kernel::Plane &plane)
{
  h.add(plane.origin).add(plane.x_dir).add(plane.normal);
}

static void hash_sketch(Hasher &h, const SketchData &sketch)
{
  hash_plane(h, sketch.plane);
  h.add(int64_t(sketch.curves.size()));
  for (const SketchCurve &c : sketch.curves) {
    h.add(c.uid).add(c.type).add(c.start).add(c.end).add(c.center).add(c.radius).add(c.ccw).add(
        c.construction);
  }
}

static void hash_refs(Hasher &h, const Span<TopoRef> refs)
{
  h.add(int64_t(refs.size()));
  for (const TopoRef &ref : refs) {
    h.add_string(ref.name).add(ref.point);
  }
}

static uint64_t feature_fingerprint(const uint64_t previous,
                                    const Document &doc,
                                    const Feature &feature)
{
  Hasher h(previous);
  h.add(doc.kernel).add(feature.uid).add(feature.type()).add(feature.suppressed);
  if (feature.suppressed) {
    return h.get();
  }
  std::visit(
      [&](const auto &data) {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, SketchData>) {
          hash_sketch(h, data);
        }
        else if constexpr (std::is_same_v<T, BoxData>) {
          h.add(data.min).add(data.size);
        }
        else if constexpr (std::is_same_v<T, ExtrudeData>) {
          h.add(data.sketch_uid).add(data.extent).add(data.distance).add(data.distance2).add(
              data.operation);
          h.add(int64_t(data.regions.size()));
          for (const int64_t key : data.regions) {
            h.add(key);
          }
          /* The profile is part of this feature's input. */
          if (const Feature *sketch = doc.find_feature(data.sketch_uid)) {
            if (const SketchData *sk = std::get_if<SketchData>(&sketch->data)) {
              hash_sketch(h, *sk);
            }
          }
        }
        else if constexpr (std::is_same_v<T, FilletData>) {
          h.add(data.radius);
          hash_refs(h, data.edges);
        }
        else if constexpr (std::is_same_v<T, ChamferData>) {
          h.add(data.distance);
          hash_refs(h, data.edges);
        }
      },
      feature.data);
  return h.get();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cache
 * \{ */

struct CachedOutcome {
  FeatureStatus status = FeatureStatus::Ok;
  std::string message;
  std::shared_ptr<const ModelState> state;
};

/** Small LRU of feature outcomes keyed by fingerprint, shared by all parts and threads. */
class OutcomeCache {
 public:
  static constexpr size_t capacity = 512;

  std::optional<CachedOutcome> lookup(const uint64_t key)
  {
    std::lock_guard lock(mutex_);
    const auto it = map_.find(key);
    if (it == map_.end()) {
      return std::nullopt;
    }
    order_.splice(order_.begin(), order_, it->second.second);
    return it->second.first;
  }

  void store(const uint64_t key, CachedOutcome outcome)
  {
    std::lock_guard lock(mutex_);
    const auto it = map_.find(key);
    if (it != map_.end()) {
      it->second.first = std::move(outcome);
      order_.splice(order_.begin(), order_, it->second.second);
      return;
    }
    order_.push_front(key);
    map_.emplace(key, std::make_pair(std::move(outcome), order_.begin()));
    while (map_.size() > capacity) {
      map_.erase(order_.back());
      order_.pop_back();
    }
  }

  void clear()
  {
    std::lock_guard lock(mutex_);
    map_.clear();
    order_.clear();
  }

 private:
  std::mutex mutex_;
  std::list<uint64_t> order_;
  std::unordered_map<uint64_t, std::pair<CachedOutcome, std::list<uint64_t>::iterator>> map_;
};

static OutcomeCache &outcome_cache()
{
  static OutcomeCache cache;
  return cache;
}

void eval_cache_clear()
{
  outcome_cache().clear();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Naming
 * \{ */

static std::string feature_prefix(const int uid)
{
  return "F" + std::to_string(uid) + ":";
}

/** A name without the split disambiguation suffixes (`#[...]`, `~k`). */
static std::string base_name(const StringRef name)
{
  std::string result;
  int depth = 0;
  for (size_t i = 0; i < size_t(name.size()); i++) {
    const char c = name[int64_t(i)];
    if (depth == 0 && (c == '#' || c == '~')) {
      /* Skip the suffix: `#[...]` or `~digits`. */
      if (c == '#' && i + 1 < size_t(name.size()) && name[int64_t(i + 1)] == '[') {
        int bracket = 0;
        for (; i < size_t(name.size()); i++) {
          if (name[int64_t(i)] == '[') {
            bracket++;
          }
          else if (name[int64_t(i)] == ']' && --bracket == 0) {
            break;
          }
        }
        continue;
      }
      if (c == '~') {
        while (i + 1 < size_t(name.size()) && std::isdigit(name[int64_t(i + 1)])) {
          i++;
        }
        continue;
      }
    }
    if (c == '(' || c == '[') {
      depth++;
    }
    else if (c == ')' || c == ']') {
      depth--;
    }
    result.push_back(c);
  }
  return result;
}

/** Names of the inputs of an operation, indexed like #SourceRef::input. */
struct InputNames {
  Vector<const Body *> bodies;
};

/** How a generated face is named, per source kind (feature-specific roles). */
using GeneratedNamer = FunctionRef<std::string(const SourceRef &source)>;

static bool less_double3(const double3 &a, const double3 &b)
{
  constexpr double eps = 1e-6;
  for (int i = 0; i < 3; i++) {
    if (std::abs(a[i] - b[i]) > eps) {
      return a[i] < b[i];
    }
  }
  return false;
}

static double3 to_double3(const kernel::Vec3 &v)
{
  return {v[0], v[1], v[2]};
}

/** Makes duplicate names unique: first by neighbouring face names, then by position. */
static void disambiguate_faces(Vector<std::string> &names, const kernel::Topology &topo)
{
  Map<std::string, Vector<int>> groups;
  for (const int i : names.index_range()) {
    groups.lookup_or_add_default(names[i]).append(i);
  }
  const Vector<std::string> base = names;
  for (const auto item : groups.items()) {
    const Vector<int> &members = item.value;
    if (members.size() < 2) {
      continue;
    }
    /* Neighbour name sets, minus what all pieces share. */
    Vector<Vector<std::string>> neighbours(members.size());
    for (const int m : members.index_range()) {
      Set<std::string> set;
      for (const int edge : topo.faces[members[m]].edges) {
        for (const int f : topo.edges[edge].faces) {
          if (f >= 0 && f != members[m]) {
            set.add(base[f]);
          }
        }
      }
      for (const std::string &s : set) {
        neighbours[m].append(s);
      }
    }
    for (const int m : members.index_range()) {
      Vector<std::string> unique;
      for (const std::string &s : neighbours[m]) {
        bool everywhere = true;
        for (const int o : members.index_range()) {
          if (!neighbours[o].contains(s)) {
            everywhere = false;
            break;
          }
        }
        if (!everywhere) {
          unique.append(s);
        }
      }
      std::sort(unique.begin(), unique.end());
      if (!unique.is_empty()) {
        std::string suffix = "#[";
        for (const int u : unique.index_range()) {
          suffix += (u ? "," : "") + unique[u];
        }
        names[members[m]] = item.key + suffix + "]";
      }
    }
  }

  /* Whatever is still duplicated gets an ordinal by position. */
  Map<std::string, Vector<int>> still;
  for (const int i : names.index_range()) {
    still.lookup_or_add_default(names[i]).append(i);
  }
  for (const auto item : still.items()) {
    Vector<int> members = item.value;
    if (members.size() < 2) {
      continue;
    }
    std::sort(members.begin(), members.end(), [&](int a, int b) {
      return less_double3(to_double3(topo.faces[a].centroid), to_double3(topo.faces[b].centroid));
    });
    for (const int k : members.index_range()) {
      names[members[k]] = item.key + "~" + std::to_string(k);
    }
  }
}

static Vector<std::string> name_edges(const Span<std::string> face_names,
                                      const kernel::Topology &topo)
{
  Vector<std::string> names(topo.edges.size());
  for (const int e : names.index_range()) {
    const kernel::EdgeInfo &edge = topo.edges[e];
    std::string a = edge.faces[0] >= 0 ? face_names[edge.faces[0]] : "";
    std::string b = edge.faces[1] >= 0 ? face_names[edge.faces[1]] : "";
    if (b < a) {
      std::swap(a, b);
    }
    names[e] = a + "|" + b;
  }
  Map<std::string, Vector<int>> groups;
  for (const int i : names.index_range()) {
    groups.lookup_or_add_default(names[i]).append(i);
  }
  for (const auto item : groups.items()) {
    Vector<int> members = item.value;
    if (members.size() < 2) {
      continue;
    }
    std::sort(members.begin(), members.end(), [&](int a, int b) {
      return less_double3(to_double3(topo.edges[a].midpoint), to_double3(topo.edges[b].midpoint));
    });
    for (const int k : members.index_range()) {
      names[members[k]] = item.key + "~" + std::to_string(k);
    }
  }
  return names;
}

/** Names every face of an operation result from its history. */
static Vector<std::string> name_faces(const int uid,
                                      const OpHistory &history,
                                      const InputNames &inputs,
                                      const kernel::Topology &topo,
                                      const GeneratedNamer generated_name)
{
  const std::string prefix = feature_prefix(uid);
  Vector<std::string> names(topo.faces.size());
  for (const int i : names.index_range()) {
    const ElementHistory *h = i < int(history.faces.size()) ? &history.faces[i] : nullptr;
    std::string name;
    if (h) {
      /* Faces that existed before keep (the smallest of) their input names. */
      Vector<std::string> inherited;
      for (const SourceRef &s : h->sources) {
        if (s.kind == SourceKind::Face && h->origin != ElementOrigin::Generated &&
            s.input < inputs.bodies.size() && s.index < inputs.bodies[s.input]->face_names.size())
        {
          inherited.append(base_name(inputs.bodies[s.input]->face_names[s.index]));
        }
      }
      if (!inherited.is_empty()) {
        name = *std::min_element(inherited.begin(), inherited.end());
      }
      else {
        Vector<std::string> parts;
        for (const SourceRef &s : h->sources) {
          std::string part = generated_name(s);
          if (part.empty()) {
            if (s.kind == SourceKind::Edge && s.input < inputs.bodies.size() &&
                s.index < inputs.bodies[s.input]->edge_names.size())
            {
              part = "blend(" + inputs.bodies[s.input]->edge_names[s.index] + ")";
            }
            else if (s.kind == SourceKind::Vertex) {
              part = "corner";
            }
            else if (s.kind == SourceKind::Face && s.input < inputs.bodies.size() &&
                     s.index < inputs.bodies[s.input]->face_names.size())
            {
              part = "from(" + inputs.bodies[s.input]->face_names[s.index] + ")";
            }
          }
          if (!part.empty()) {
            parts.append(part);
          }
        }
        std::sort(parts.begin(), parts.end());
        for (const int p : parts.index_range()) {
          name += (p ? "+" : "") + parts[p];
        }
        if (!name.empty()) {
          name = prefix + name;
        }
      }
    }
    names[i] = name.empty() ? prefix + "face" : name;
  }
  disambiguate_faces(names, topo);
  return names;
}

/** Builds a body from an operation result, naming its faces and edges. */
static Body make_body(const kernel::Kernel &kernel,
                      const OpResult &op,
                      const int uid,
                      const InputNames &inputs,
                      const GeneratedNamer generated_name)
{
  Body body;
  body.shape = op.shape;
  body.feature_uid = uid;
  auto topo = std::make_shared<kernel::Topology>(kernel.topology(op.shape));
  body.face_names = name_faces(uid, op.history, inputs, *topo, generated_name);
  body.edge_names = name_edges(body.face_names, *topo);
  body.topology = std::move(topo);
  return body;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reference Resolution
 * \{ */

struct ResolvedRef {
  int body = -1;
  int index = -1;
  /** Set when the name did not match exactly. */
  std::string warning;
};

static std::pair<std::string, std::string> split_edge_name(const StringRef name)
{
  /* Split at the top-level '|' (names may contain '|' inside parentheses). */
  int depth = 0;
  for (int64_t i = 0; i < name.size(); i++) {
    const char c = name[i];
    if (c == '(' || c == '[') {
      depth++;
    }
    else if (c == ')' || c == ']') {
      depth--;
    }
    else if (c == '|' && depth == 0) {
      return {std::string(name.substr(0, i)), std::string(name.substr(i + 1))};
    }
  }
  return {std::string(name), ""};
}

static ResolvedRef resolve_edge(const ModelState &state, const TopoRef &ref)
{
  ResolvedRef best;
  /* 1. Exact name. */
  for (const int b : state.bodies.index_range()) {
    const int e = state.bodies[b].find_edge(ref.name);
    if (e >= 0) {
      return {b, e, ""};
    }
  }

  /* 2. Same pair of faces, ignoring split suffixes: the edge still exists but a face around it
   * was split or the edge itself was. Nearest candidate to the picked point wins. */
  auto [ref_a, ref_b] = split_edge_name(ref.name);
  ref_a = base_name(ref_a);
  ref_b = base_name(ref_b);
  double best_distance = std::numeric_limits<double>::max();
  for (const int b : state.bodies.index_range()) {
    const Body &body = state.bodies[b];
    for (const int e : body.edge_names.index_range()) {
      auto [a, bb] = split_edge_name(body.edge_names[e]);
      a = base_name(a);
      bb = base_name(bb);
      if ((a == ref_a && bb == ref_b) || (a == ref_b && bb == ref_a)) {
        const double d = math::distance(to_double3(body.topology->edges[e].midpoint), ref.point);
        if (d < best_distance) {
          best_distance = d;
          best = {b, e, "edge reference \"" + ref.name + "\" re-bound to a split piece"};
        }
      }
    }
  }
  if (best.index >= 0) {
    return best;
  }

  /* 3. Position only. */
  for (const int b : state.bodies.index_range()) {
    const Body &body = state.bodies[b];
    for (int e = 0; e < int(body.topology->edges.size()); e++) {
      const double d = math::distance(to_double3(body.topology->edges[e].midpoint), ref.point);
      if (d < best_distance) {
        best_distance = d;
        best = {b, e, "edge reference \"" + ref.name + "\" was lost and re-bound by position"};
      }
    }
  }
  return best;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Features
 * \{ */

struct Outcome {
  FeatureStatus status = FeatureStatus::Ok;
  std::string message;
  std::shared_ptr<const ModelState> state;
};

static Outcome error(std::shared_ptr<const ModelState> previous, std::string message)
{
  return {FeatureStatus::Error, std::move(message), std::move(previous)};
}

static std::string next_body_name(const ModelState &state)
{
  int n = 1;
  while (true) {
    const std::string name = "Body" + std::to_string(n);
    bool used = false;
    for (const Body &body : state.bodies) {
      used |= body.name == name;
    }
    if (!used) {
      return name;
    }
    n++;
  }
}

static Outcome eval_box(const kernel::Kernel &kernel,
                        const Feature &feature,
                        const BoxData &data,
                        const std::shared_ptr<const ModelState> &previous)
{
  kernel::BoxParams params;
  params.min = {data.min.x, data.min.y, data.min.z};
  params.size = {data.size.x, data.size.y, data.size.z};
  const OpResult op = kernel.box(params);
  if (!op.ok()) {
    return error(previous, op.message);
  }
  static const char *roles[] = {"left", "right", "front", "back", "bottom", "top"};
  Body body = make_body(kernel, op, feature.uid, {}, [&](const SourceRef &s) -> std::string {
    if (s.kind == SourceKind::PrimitiveFace && s.index >= 0 && s.index < 6) {
      return roles[s.index];
    }
    return "";
  });
  auto state = std::make_shared<ModelState>(*previous);
  body.name = next_body_name(*state);
  state->bodies.append(std::move(body));
  return {FeatureStatus::Ok, op.message, std::move(state)};
}

static Outcome eval_extrude(const kernel::Kernel &kernel,
                            const Document &doc,
                            const Feature &feature,
                            const ExtrudeData &data,
                            const std::shared_ptr<const ModelState> &previous)
{
  const Feature *sketch_feature = doc.find_feature(data.sketch_uid);
  const SketchData *sketch = sketch_feature ? std::get_if<SketchData>(&sketch_feature->data) :
                                              nullptr;
  if (!sketch) {
    return error(previous, "the profile sketch is missing");
  }
  const SketchProfiles profiles = sketch_profiles(*sketch);
  kernel::ExtrudeParams params;
  params.profile.plane = sketch->plane;
  std::string warning;
  for (const SketchRegion &region : profiles.regions) {
    if (data.regions.is_empty() || data.regions.contains(region.key)) {
      params.profile.regions.push_back(region.region);
    }
  }
  if (params.profile.regions.empty()) {
    return error(previous,
                 profiles.regions.is_empty() ? "the sketch has no closed profile" :
                                               "the selected profiles no longer exist");
  }
  if (int(params.profile.regions.size()) < int(data.regions.size())) {
    warning = "some selected profiles no longer exist";
  }
  params.extent = data.extent;
  params.distance = data.distance;
  params.distance2 = data.distance2;

  const OpResult prism = kernel.extrude(params);
  if (!prism.ok()) {
    return error(previous, prism.message);
  }
  auto extrude_names = [&](const SourceRef &s) -> std::string {
    switch (s.kind) {
      case SourceKind::Cap:
        return s.index == int64_t(kernel::CapRole::Start) ? "start" : "end";
      case SourceKind::ProfileCurve:
        return "side:" + std::to_string(s.index);
      default:
        return "";
    }
  };
  Body tool = make_body(kernel, prism, feature.uid, {}, extrude_names);

  auto state = std::make_shared<ModelState>(*previous);
  BodyOperation operation = data.operation;
  if (state->bodies.is_empty()) {
    if (operation == BodyOperation::Cut || operation == BodyOperation::Intersect) {
      return error(previous, "nothing to cut or intersect: there is no body yet");
    }
    operation = BodyOperation::NewBody;
  }
  if (operation == BodyOperation::NewBody) {
    tool.name = next_body_name(*state);
    state->bodies.append(std::move(tool));
    return {warning.empty() ? FeatureStatus::Ok : FeatureStatus::Warning, warning, state};
  }

  const Body &target = state->bodies[0];
  const kernel::BooleanOp op = operation == BodyOperation::Join ? kernel::BooleanOp::Union :
                               operation == BodyOperation::Cut  ? kernel::BooleanOp::Subtract :
                                                                  kernel::BooleanOp::Intersect;
  const kernel::Shape tools[] = {tool.shape};
  const OpResult result = kernel.boolean(target.shape, tools, op);
  if (!result.ok()) {
    return error(previous, result.message);
  }
  InputNames inputs;
  inputs.bodies = {&target, &tool};
  Body body = make_body(kernel, result, feature.uid, inputs, extrude_names);
  body.name = target.name;
  state->bodies[0] = std::move(body);
  return {warning.empty() ? FeatureStatus::Ok : FeatureStatus::Warning, warning, state};
}

template<typename Data, typename RunFn>
static Outcome eval_edge_feature(const kernel::Kernel &kernel,
                                 const Feature &feature,
                                 const Data &data,
                                 const std::shared_ptr<const ModelState> &previous,
                                 RunFn &&run)
{
  if (data.edges.is_empty()) {
    return error(previous, "no edges selected");
  }
  if (previous->bodies.is_empty()) {
    return error(previous, "there is no body to modify");
  }
  int body_index = -1;
  std::vector<int> edges;
  std::string warning;
  for (const TopoRef &ref : data.edges) {
    const ResolvedRef resolved = resolve_edge(*previous, ref);
    if (resolved.index < 0) {
      return error(previous, "edge \"" + ref.name + "\" not found");
    }
    if (body_index >= 0 && resolved.body != body_index) {
      return error(previous, "all edges must belong to the same body");
    }
    body_index = resolved.body;
    if (std::find(edges.begin(), edges.end(), resolved.index) == edges.end()) {
      edges.push_back(resolved.index);
    }
    if (!resolved.warning.empty() && warning.empty()) {
      warning = resolved.warning;
    }
  }
  const Body &target = previous->bodies[body_index];
  const OpResult result = run(target.shape, edges);
  if (!result.ok()) {
    return error(previous, result.message);
  }
  InputNames inputs;
  inputs.bodies = {&target};
  Body body = make_body(
      kernel, result, feature.uid, inputs, [](const SourceRef &) { return std::string(); });
  body.name = target.name;
  auto state = std::make_shared<ModelState>(*previous);
  state->bodies[body_index] = std::move(body);
  return {warning.empty() ? FeatureStatus::Ok : FeatureStatus::Warning, warning, state};
}

static Outcome eval_feature(const kernel::Kernel &kernel,
                            const Document &doc,
                            const Feature &feature,
                            const std::shared_ptr<const ModelState> &previous)
{
  try {
    return std::visit(
        [&](const auto &data) -> Outcome {
          using T = std::decay_t<decltype(data)>;
          if constexpr (std::is_same_v<T, SketchData>) {
            const SketchProfiles profiles = sketch_profiles(data);
            if (!profiles.open_curves.is_empty() && profiles.regions.is_empty()) {
              return {FeatureStatus::Warning, "the sketch has no closed profile", previous};
            }
            return {FeatureStatus::Ok, "", previous};
          }
          else if constexpr (std::is_same_v<T, BoxData>) {
            return eval_box(kernel, feature, data, previous);
          }
          else if constexpr (std::is_same_v<T, ExtrudeData>) {
            return eval_extrude(kernel, doc, feature, data, previous);
          }
          else if constexpr (std::is_same_v<T, FilletData>) {
            return eval_edge_feature(
                kernel, feature, data, previous, [&](const kernel::Shape &s, std::vector<int> e) {
                  kernel::FilletParams params;
                  params.edges = std::move(e);
                  params.radius = data.radius;
                  return kernel.fillet(s, params);
                });
          }
          else if constexpr (std::is_same_v<T, ChamferData>) {
            return eval_edge_feature(
                kernel, feature, data, previous, [&](const kernel::Shape &s, std::vector<int> e) {
                  kernel::ChamferParams params;
                  params.edges = std::move(e);
                  params.distance = data.distance;
                  return kernel.chamfer(s, params);
                });
          }
        },
        feature.data);
  }
  catch (const std::exception &e) {
    return error(previous, std::string("internal error: ") + e.what());
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Evaluation
 * \{ */

EvalResult evaluate(const Document &doc)
{
  EvalResult result;
  std::shared_ptr<const ModelState> state = std::make_shared<ModelState>();
  const kernel::Kernel *kernel = kernel::kernel_get(doc.kernel);

  uint64_t fingerprint = 0x5ca1ab1e;
  bool rolled_back = false;
  for (const Feature &feature : doc.features) {
    result.features.append({});
    FeatureResult &fr = result.features.last();
    fr.uid = feature.uid;
    if (rolled_back) {
      fr.status = FeatureStatus::RolledBack;
      fr.state = state;
      continue;
    }
    if (!kernel) {
      fr.status = FeatureStatus::Error;
      fr.message = "the selected geometry kernel is not available in this build";
      fr.state = state;
    }
    else {
      fingerprint = feature_fingerprint(fingerprint, doc, feature);
      fr.fingerprint = fingerprint;
      if (feature.suppressed) {
        fr.status = FeatureStatus::Suppressed;
        fr.state = state;
      }
      else if (std::optional<CachedOutcome> cached = outcome_cache().lookup(fingerprint)) {
        fr.status = cached->status;
        fr.message = std::move(cached->message);
        fr.state = std::move(cached->state);
        fr.cached = true;
      }
      else {
        Outcome outcome = eval_feature(*kernel, doc, feature, state);
        outcome_cache().store(fingerprint, {outcome.status, outcome.message, outcome.state});
        fr.status = outcome.status;
        fr.message = std::move(outcome.message);
        fr.state = std::move(outcome.state);
      }
      state = fr.state;
    }
    if (feature.uid == doc.rollback_uid) {
      rolled_back = true;
    }
  }
  result.final_state = state;
  return result;
}

/** \} */

}  // namespace blender::cad

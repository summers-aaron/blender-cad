/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cad
 *
 * Turns solved sketch curves into closed regions for extrusion.
 *
 * Loops are found on the endpoint graph of the (non-construction) curves: a connected set of
 * curves whose endpoints all join exactly two curves is one closed loop. Loops are nested by
 * containment: a loop at even depth is a region's outer boundary, the loops directly inside it
 * are its holes. Overlapping or crossing curves (a full planar arrangement) are not split yet.
 */

#include <algorithm>
#include <cmath>
#include <numeric>

#include <numbers>

#include "BLI_math_vector.hh"

#include "CAD_eval.hh"

namespace blender::cad {

/** Endpoints closer than this (mm) are the same point. */
static constexpr double point_tolerance = 1e-6;

static double2 arc_point(const SketchCurve &c, const double angle)
{
  return {c.center.x + c.radius * std::cos(angle), c.center.y + c.radius * std::sin(angle)};
}

/** Points along `c` from its start, excluding its end. */
static void sample(const SketchCurve &c, Vector<double2> &r_points)
{
  constexpr int segments = 24;
  switch (c.type) {
    case SketchCurveType::Line:
      r_points.append(c.start);
      break;
    case SketchCurveType::Arc: {
      const double a0 = std::atan2(c.start.y - c.center.y, c.start.x - c.center.x);
      double a1 = std::atan2(c.end.y - c.center.y, c.end.x - c.center.x);
      if (c.ccw) {
        while (a1 <= a0) {
          a1 += 2.0 * std::numbers::pi;
        }
      }
      else {
        while (a1 >= a0) {
          a1 -= 2.0 * std::numbers::pi;
        }
      }
      for (int i = 0; i < segments; i++) {
        r_points.append(arc_point(c, a0 + (a1 - a0) * double(i) / segments));
      }
      break;
    }
    case SketchCurveType::Circle:
      for (int i = 0; i < segments; i++) {
        r_points.append(arc_point(c, 2.0 * std::numbers::pi * double(i) / segments));
      }
      break;
  }
}

static double2 midpoint(const SketchCurve &c)
{
  switch (c.type) {
    case SketchCurveType::Line:
      return (c.start + c.end) * 0.5;
    case SketchCurveType::Arc: {
      Vector<double2> points;
      sample(c, points);
      return points[points.size() / 2];
    }
    case SketchCurveType::Circle:
      return arc_point(c, 0.0);
  }
  return c.start;
}

static double signed_area(Span<double2> polygon)
{
  double area = 0.0;
  for (const int i : polygon.index_range()) {
    const double2 &a = polygon[i];
    const double2 &b = polygon[(i + 1) % polygon.size()];
    area += a.x * b.y - b.x * a.y;
  }
  return 0.5 * area;
}

static bool point_in_polygon(const double2 &p, Span<double2> polygon)
{
  bool inside = false;
  for (int i = 0, j = int(polygon.size()) - 1; i < int(polygon.size()); j = i++) {
    const double2 &a = polygon[i];
    const double2 &b = polygon[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
      inside = !inside;
    }
  }
  return inside;
}

static SketchCurve reversed(SketchCurve c)
{
  std::swap(c.start, c.end);
  c.ccw = !c.ccw;
  return c;
}

static kernel::ProfileCurve to_kernel(const SketchCurve &c)
{
  kernel::ProfileCurve k;
  k.id = c.uid;
  k.start = {c.start.x, c.start.y};
  k.end = {c.end.x, c.end.y};
  k.center = {c.center.x, c.center.y};
  k.radius = c.radius;
  k.ccw = c.ccw;
  switch (c.type) {
    case SketchCurveType::Line:
      k.type = kernel::ProfileCurveType::Line;
      break;
    case SketchCurveType::Arc:
      k.type = kernel::ProfileCurveType::Arc;
      break;
    case SketchCurveType::Circle:
      k.type = kernel::ProfileCurveType::Circle;
      break;
  }
  return k;
}

struct Loop {
  Vector<SketchCurve> curves;
  Vector<double2> polygon;
  double area = 0.0;
  int64_t key = 0;
  int depth = 0;
  int parent = -1;
};

SketchProfiles sketch_profiles(const SketchData &sketch)
{
  SketchProfiles result;
  Vector<Loop> loops;

  /* Open curves (lines, arcs) go through the endpoint graph; circles are loops by themselves. */
  Vector<SketchCurve> open;
  for (const SketchCurve &c : sketch.curves) {
    if (c.construction) {
      continue;
    }
    if (c.type == SketchCurveType::Circle) {
      if (c.radius > point_tolerance) {
        Loop loop;
        loop.curves.append(c);
        loops.append(std::move(loop));
      }
      continue;
    }
    if (math::distance(c.start, c.end) < point_tolerance) {
      continue; /* Degenerate. */
    }
    open.append(c);
  }

  /* Merge coincident endpoints into nodes. */
  Vector<double2> nodes;
  auto node_of = [&](const double2 &p) {
    for (const int i : nodes.index_range()) {
      if (math::distance(nodes[i], p) < point_tolerance) {
        return i;
      }
    }
    nodes.append(p);
    return int(nodes.size()) - 1;
  };
  Vector<int2> curve_nodes;
  for (const SketchCurve &c : open) {
    curve_nodes.append({node_of(c.start), node_of(c.end)});
  }
  Vector<Vector<int>> node_curves(nodes.size());
  for (const int ci : open.index_range()) {
    node_curves[curve_nodes[ci][0]].append(ci);
    node_curves[curve_nodes[ci][1]].append(ci);
  }

  /* Connected components of curves. */
  Vector<int> component(open.size(), -1);
  int components_num = 0;
  for (const int seed : open.index_range()) {
    if (component[seed] != -1) {
      continue;
    }
    Vector<int> stack = {seed};
    component[seed] = components_num;
    while (!stack.is_empty()) {
      const int ci = stack.pop_last();
      for (const int node : {curve_nodes[ci][0], curve_nodes[ci][1]}) {
        for (const int other : node_curves[node]) {
          if (component[other] == -1) {
            component[other] = components_num;
            stack.append(other);
          }
        }
      }
    }
    components_num++;
  }

  for (int comp = 0; comp < components_num; comp++) {
    Vector<int> members;
    for (const int ci : open.index_range()) {
      if (component[ci] == comp) {
        members.append(ci);
      }
    }
    /* A simple cycle: every node of the component joins exactly two curves. */
    bool is_cycle = true;
    for (const int ci : members) {
      for (const int node : {curve_nodes[ci][0], curve_nodes[ci][1]}) {
        if (node_curves[node].size() != 2) {
          is_cycle = false;
        }
      }
    }
    if (!is_cycle) {
      for (const int ci : members) {
        result.open_curves.append(open[ci].uid);
      }
      continue;
    }

    /* Walk the cycle from its smallest uid, chaining end to start. */
    const int first = *std::min_element(members.begin(), members.end(), [&](int a, int b) {
      return open[a].uid < open[b].uid;
    });
    Loop loop;
    int current = first;
    int at_node = curve_nodes[first][1];
    loop.curves.append(open[first]);
    while (true) {
      const Vector<int> &candidates = node_curves[at_node];
      const int next = candidates[0] == current ? candidates[1] : candidates[0];
      if (next == first) {
        break;
      }
      if (curve_nodes[next][0] == at_node) {
        loop.curves.append(open[next]);
        at_node = curve_nodes[next][1];
      }
      else {
        loop.curves.append(reversed(open[next]));
        at_node = curve_nodes[next][0];
      }
      current = next;
    }
    loops.append(std::move(loop));
  }

  /* Geometry of each loop. */
  for (Loop &loop : loops) {
    for (const SketchCurve &c : loop.curves) {
      sample(c, loop.polygon);
    }
    loop.area = signed_area(loop.polygon);
    loop.key = loop.curves[0].uid;
    for (const SketchCurve &c : loop.curves) {
      loop.key = std::min(loop.key, c.uid);
    }
  }

  /* Nesting: parent = the smallest loop containing this one. */
  for (const int i : loops.index_range()) {
    const double2 probe = midpoint(loops[i].curves[0]);
    double parent_area = 0.0;
    for (const int j : loops.index_range()) {
      if (i == j || std::abs(loops[j].area) <= std::abs(loops[i].area)) {
        continue;
      }
      if (point_in_polygon(probe, loops[j].polygon)) {
        loops[i].depth++;
        if (loops[i].parent == -1 || std::abs(loops[j].area) < parent_area) {
          loops[i].parent = j;
          parent_area = std::abs(loops[j].area);
        }
      }
    }
  }

  for (const int i : loops.index_range()) {
    const Loop &outer = loops[i];
    if (outer.depth % 2 != 0) {
      continue;
    }
    SketchRegion region;
    region.key = outer.key;
    region.area = std::abs(outer.area);
    for (const SketchCurve &c : outer.curves) {
      region.region.outer.curves.push_back(to_kernel(c));
    }
    for (const int j : loops.index_range()) {
      if (loops[j].parent == i && loops[j].depth == outer.depth + 1) {
        kernel::ProfileLoop hole;
        for (const SketchCurve &c : loops[j].curves) {
          hole.curves.push_back(to_kernel(c));
        }
        region.region.holes.push_back(std::move(hole));
        region.area -= std::abs(loops[j].area);
      }
    }
    result.regions.append(std::move(region));
  }
  std::sort(result.regions.begin(),
            result.regions.end(),
            [](const SketchRegion &a, const SketchRegion &b) { return a.key < b.key; });
  return result;
}

}  // namespace blender::cad

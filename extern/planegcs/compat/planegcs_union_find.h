/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later */

/* Replaces the `boost::adjacency_list` + `boost::connected_components` pair that PlaneGCS
 * uses to split a sketch into independent sub-systems.
 *
 * Component numbering matches Boost's: components are numbered 0, 1, 2, ... in order of
 * their lowest vertex index (Boost's DFS starts a new component at the first unvisited
 * vertex in index order), so solver behavior is unchanged. */

#pragma once

#include <numeric>
#include <vector>

namespace planegcs {

class UnionFindGraph {
 public:
  void add_vertex()
  {
    parent_.push_back(int(parent_.size()));
  }

  int num_vertices() const
  {
    return int(parent_.size());
  }

  void add_edge(const int a, const int b)
  {
    const int ra = find(a);
    const int rb = find(b);
    if (ra != rb) {
      /* Keep the smaller index as root: deterministic and cheap. */
      if (ra < rb) {
        parent_[rb] = ra;
      }
      else {
        parent_[ra] = rb;
      }
    }
  }

  /** Writes a component id per vertex into `components`, returns the number of components. */
  int connected_components(int *components)
  {
    std::vector<int> root_to_component(parent_.size(), -1);
    int count = 0;
    for (int v = 0; v < int(parent_.size()); v++) {
      const int root = find(v);
      if (root_to_component[root] == -1) {
        root_to_component[root] = count++;
      }
      components[v] = root_to_component[root];
    }
    return count;
  }

 private:
  int find(int v)
  {
    while (parent_[v] != v) {
      parent_[v] = parent_[parent_[v]];
      v = parent_[v];
    }
    return v;
  }

  std::vector<int> parent_;
};

}  // namespace planegcs

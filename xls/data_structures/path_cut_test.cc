// Copyright 2021 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/data_structures/path_cut.h"

#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "absl/status/statusor.h"
#include "absl/types/optional.h"
#include "xls/common/logging/log_message.h"
#include "xls/common/logging/logging.h"
#include "xls/common/strong_int.h"

namespace xls {
namespace {

// Increment a bitvector (e.g.: considered as an unsigned integer), returning
// false if there was overflow and true otherwise.
bool IncrementBitVector(std::vector<bool>* vec) {
  bool carry = true;
  for (int32_t i = 0; i < vec->size(); i++) {
    if (carry) {
      carry = (*vec)[i];
      (*vec)[i] = !(*vec)[i];
    }
  }
  return !carry;
}

// Convert a bitvector whose length is equal to the number of edges in the path
// into a cut on the path, where bits equal to 1 represent cut edges and bits
// equal to 0 represent uncut edges.
PathCut CutFromBitVector(const PathGraph& path, const std::vector<bool>& vec) {
  XLS_CHECK_EQ(vec.size(), path.NumEdges())
      << "Size of bitvector must be equal to number of edges";
  std::vector<PathEdgeId> cut_edges;
  for (int32_t i = 0; i < path.NumEdges(); i++) {
    if (vec[i]) {
      cut_edges.push_back(PathEdgeId(i));
    }
  }
  return CutEdgesToPathCut(path, cut_edges);
}

// Enumerate all cuts for a path, calling the given function on each cut.
void EnumerateAllCuts(const PathGraph& path,
                      std::function<void(const PathCut&)> callback) {
  std::vector<bool> bitvector;
  bitvector.resize(path.NumEdges(), false);
  do {
    PathCut cut = CutFromBitVector(path, bitvector);
    callback(cut);
  } while (IncrementBitVector(&bitvector));
}

// Determine whether the given cut violates the max node weight constraint.
bool PathCutIsValid(const PathGraph& path, const PathCut& cut,
                    PathNodeWeight maximum_weight) {
  for (const std::vector<PathNodeId>& piece : cut) {
    PathNodeWeight piece_weight(0);
    for (PathNodeId node : piece) {
      piece_weight += path.WeightOfNode(node);
    }
    if (piece_weight > maximum_weight) {
      return false;
    }
  }
  return true;
}

// Compute the cost of the given cut.
PathEdgeWeight PathCutCost(const PathGraph& path, const PathCut& cut) {
  PathEdgeWeight result(0);
  for (int i = 0; i < cut.size(); i++) {
    if (absl::optional<PathEdgeId> edge =
            path.NodeSuccessorEdge(cut[i].back())) {
      result += path.WeightOfEdge(*edge);
    }
  }
  return result;
}

// A brute force solution for the path cut problem, to compare against the
// smarter dynamic programming solution in `path_cut.cc`.
absl::optional<PathCut> BruteForcePathCut(const PathGraph& path,
                                          PathNodeWeight maximum_weight) {
  absl::optional<PathCut> best;
  absl::optional<PathEdgeWeight> best_cost;
  EnumerateAllCuts(path, [&](const PathCut& cut) {
    if (!PathCutIsValid(path, cut, maximum_weight)) {
      return;
    }
    PathEdgeWeight cost = PathCutCost(path, cut);
    if (!best_cost || (cost < *best_cost)) {
      best = cut;
      best_cost = cost;
    }
  });
  return best;
}

using PNW = PathNodeWeight;
using PEW = PathEdgeWeight;

TEST(PathCutTest, SingleNodeTest) {
  // (50)
  PathGraph path = *PathGraph::Create({PNW(50)}, {});
  EXPECT_EQ(ComputePathCut(path, PNW(30)), absl::nullopt);
  EXPECT_EQ(ComputePathCut(path, PNW(70)).value(), PathCut({{PathNodeId(0)}}));
}

TEST(PathCutTest, SimpleTest) {
  // (50) >-- 10 --> (10) >-- 10 --> (20) >-- 10 --> (50)
  PathGraph path = *PathGraph::Create({PNW(50), PNW(10), PNW(20), PNW(50)},
                                      {PEW(10), PEW(10), PEW(10)});
  EXPECT_EQ(ComputePathCut(path, PNW(70)),
            absl::make_optional<PathCut>({{PathNodeId(0), PathNodeId(1)},
                                          {PathNodeId(2), PathNodeId(3)}}));
  for (int32_t i = 0; i < 100; i += 5) {
    EXPECT_EQ(ComputePathCut(path, PNW(i)), BruteForcePathCut(path, PNW(i)));
  }
}

TEST(PathCutTest, ComplexTest) {
  // Generated by fair dice roll
  PathGraph path = *PathGraph::Create(
      {PNW(17), PNW(16), PNW(18), PNW(93), PNW(55), PNW(75), PNW(51), PNW(63)},
      {PEW(23), PEW(34), PEW(61), PEW(22), PEW(76), PEW(54), PEW(77)});
  for (int32_t i = 0; i < 300; i += 1) {
    XLS_VLOG(3) << "i = " << i << "\n";
    PNW max_weight(i);
    absl::optional<PathCut> smart = ComputePathCut(path, max_weight);
    absl::optional<PathCut> brute = BruteForcePathCut(path, max_weight);
    EXPECT_EQ(smart.has_value(), brute.has_value());
    if (smart.has_value()) {
      XLS_VLOG(3) << "brute = " << PathCutToString(*brute) << "\n";
      XLS_VLOG(3) << "smart = " << PathCutToString(*smart) << "\n";
      EXPECT_TRUE(PathCutIsValid(path, *brute, max_weight));
      EXPECT_TRUE(PathCutIsValid(path, *smart, max_weight));
      EXPECT_EQ(PathCutCost(path, *smart), PathCutCost(path, *brute));
    }
  }
}

}  // namespace
}  // namespace xls
#include <iostream>
#include <vector>

#include "thor/alternates.h"

using namespace valhalla::thor;
using namespace valhalla::baldr;
using namespace valhalla::midgard;

/*
 * Viability tests for alternate paths based on M. Kobitzsch's Alternative Route
 * Techniques (2015). Tests verify limited sharing between segments, bounded
 * stretch, and local optimiality. Any candidate path that meets all the criteria
 * may be considered a valid alternate to the shortest path.
 */
namespace {
// Defaults thresholds
// TODO: global stretch parameter should depend on the optimal route cost/duration.
// For an optimal route that takes 10min it's okay to have an alternative that takes 15min. But
// it's unreasonable to propose an alternative that takes 15 hours if the optimal one takes 10 hours.
float kAtMostLonger = 1.25f; // stretch threshold
// Alternative route shouldn't contain unreasonable detours. We should skip an alternative
// if it has a detour longer than 2 x cost of the corresponding path in the optimal route.
float kAtMostLongerDetour = 2.f;
float kAtMostShared = 0.75f; // sharing threshold
// float kAtLeastOptimal = 0.2f; // local optimality threshold
} // namespace

namespace valhalla {
namespace thor {
float get_max_sharing(const valhalla::Location& origin, const valhalla::Location& destination) {
  PointLL from(origin.path_edges(0).ll().lng(), origin.path_edges(0).ll().lat());
  PointLL to(destination.path_edges(0).ll().lng(), destination.path_edges(0).ll().lat());
  auto distance = from.Distance(to);

  // 10km
  if (distance < 10000.) {
    return 0.50;
  }
  // 20km
  else if (distance < 20000.) {
    return 0.60;
  }
  // 50km
  else if (distance < 50000.) {
    return 0.65;
  }
  // 100km
  else if (distance < 100000.) {
    return 0.70;
  }
  return kAtMostShared;
}

// Bounded stretch. We use cost as an approximation for stretch, to filter out
// candidate connections that are much more costly than the optimal cost. Culls
// the list of connections to only those within the stretch tolerance
void filter_alternates_by_stretch(std::vector<CandidateConnection>& connections) {
  std::sort(connections.begin(), connections.end());
  auto max_cost = connections.front().cost * kAtMostLonger;
  auto new_end = std::lower_bound(connections.begin(), connections.end(), max_cost);
  connections.erase(new_end, connections.end());
}

// get a cost of a path between indexes 'first' and 'last'
inline sif::Cost get_segment_cost(const std::vector<PathInfo>& path, size_t first, size_t last) {
  auto cost = path[last].elapsed_cost - path[first].transition_cost;
  if (first > 0)
    cost -= path[first - 1].elapsed_cost;
  return cost;
};

// Find a different segment for two routes. By design bidirectional astar returns routes that have
// only one different segment. Also the first and last edges are the same for each two routes.
std::pair<std::pair<size_t, size_t>, std::pair<size_t, size_t>>
find_diff_segment(const std::vector<PathInfo>& path_1, const std::vector<PathInfo>& path_2) {

  size_t idx_1_first = 0;
  size_t idx_2_first = 0;

  // find first different edge
  while (idx_1_first < path_1.size() && idx_2_first < path_2.size() &&
         path_1[idx_1_first].edgeid == path_2[idx_2_first].edgeid) {
    ++idx_1_first;
    ++idx_2_first;
  }
  // check corner cases: stop if we didn't find a different edge
  if (idx_1_first == path_1.size())
    return {{idx_1_first, idx_1_first}, {idx_2_first, std::max(idx_2_first, path_2.size() - 1)}};
  else if (idx_2_first == path_2.size())
    return {{idx_1_first, std::max(idx_1_first, path_1.size() - 1)}, {idx_2_first, idx_2_first}};

  size_t idx_1_last = path_1.size() - 1;
  size_t idx_2_last = path_2.size() - 1;
  // find last different edge
  while (idx_1_last > idx_1_first && idx_2_last > idx_2_first &&
         path_1[idx_1_last].edgeid == path_2[idx_2_last].edgeid) {
    --idx_1_last;
    --idx_2_last;
  }

  return {{idx_1_first, idx_1_last}, {idx_2_first, idx_2_last}};
}

// Check if the candidate path contains unreasonable long detours comparing to the optimal path.
bool validate_alternate_by_stretch(const std::vector<PathInfo>& optimal_path,
                                   const std::vector<PathInfo>& candidate_path) {
  const auto unique_segments = find_diff_segment(optimal_path, candidate_path);
  const auto& unique_optimal_segment = unique_segments.first;
  const auto& unique_candidate_segment = unique_segments.second;

  if (unique_optimal_segment.first == optimal_path.size()) {
    // return true if the paths are equal, otherwise the optimal path is a subpath of the alternative
    if (unique_candidate_segment.first < candidate_path.size()) {
      LOG_DEBUG("Candidate alternate rejected by local stretch");
      return false;
    }
    return true;
  }

  const auto optimal_segment_cost =
      get_segment_cost(optimal_path, unique_optimal_segment.first, unique_optimal_segment.second);
  const auto candidate_segment_cost = get_segment_cost(candidate_path, unique_candidate_segment.first,
                                                       unique_candidate_segment.second);

  // check if detour is reasonable
  if (kAtMostLongerDetour * optimal_segment_cost.cost < candidate_segment_cost.cost) {
    LOG_DEBUG("Candidate alternate rejected by local stretch");
    return false;
  }
  return true;
}

// Limited Sharing. Compare length of edge segments shared between optimal path and
// candidate path. If they share more than kAtMostShared throw out this alternate.
// Note that you should recover all shortcuts before call this function.
bool validate_alternate_by_sharing(std::vector<std::unordered_set<GraphId>>& shared_edgeids,
                                   const std::vector<std::vector<PathInfo>>& paths,
                                   const std::vector<PathInfo>& candidate_path,
                                   float at_most_shared) {

  // we will calculate the overlap in edge duration between the candidate_path and paths (paths is a
  // vector of the fastest path + any alternates already chosen)
  if (paths.size() > shared_edgeids.size())
    shared_edgeids.resize(paths.size());

  // we check each accepted path against the candidate
  for (size_t i = 0; i < paths.size(); ++i) {
    // cache edge ids encountered on the current best path. Don't care about shortcuts because they
    // have already been recovered.
    auto& shared = shared_edgeids[i];
    if (shared.empty()) {
      for (const auto& pi : paths[i])
        shared.insert(pi.edgeid);
    }

    // if an edge on the candidate_path is encountered that is also on one of the existing paths,
    // we count it as a "shared" edge
    float shared_length = 0.f, total_length = 0.f;
    for (const auto& cpi : candidate_path) {
      const auto length = &cpi == &candidate_path.front()
                              ? cpi.path_distance
                              : cpi.path_distance - (&cpi - 1)->path_distance;
      total_length += length;
      if (shared.find(cpi.edgeid) != shared.end()) {
        shared_length += length;
      }
    }

    // throw this alternate away if it shares more than at_most_shared with any of the chosen paths
    assert(total_length > 0);
    if ((shared_length / total_length) > at_most_shared) {
      LOG_DEBUG("Candidate alternate rejected by sharing");
      return false;
    }
  }

  LOG_DEBUG("Candidate alternate accepted");
  // this is a viable alternate
  return true;
}

bool validate_alternate_by_local_optimality(const std::vector<PathInfo>&) {
  // [TODO] NOT IMPLEMENTED
  return true;
}
} // namespace thor
} // namespace valhalla

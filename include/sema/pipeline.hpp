#pragma once

#include <vector>

#include "sema/geo.hpp"
#include "sema/military_filter.hpp"
#include "sema/privacy.hpp"
#include "sema/state_vector.hpp"

namespace sema {

struct FilterStats {
    std::size_t total = 0;
    std::size_t kept = 0;
    std::size_t dropped_no_position = 0;
    std::size_t dropped_outside = 0;
    std::size_t dropped_military = 0;
    std::size_t dropped_opt_out = 0;
};

// Second stage after the bounding-box query: keeps aircraft with a position
// inside `area` and drops military/government aircraft and aircraft on the
// opt-out list. Both are dropped first so that nothing about them is
// processed further.
std::vector<StateVector> filter_states(std::vector<StateVector> states, const MultiPolygon& area,
                                       const MilitaryFilter& military, FilterStats& stats,
                                       const OptOutList* opt_out = nullptr);

}  // namespace sema

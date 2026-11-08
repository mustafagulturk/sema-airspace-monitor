#include "sema/pipeline.hpp"

namespace sema {

std::vector<StateVector> filter_states(std::vector<StateVector> states, const MultiPolygon& area,
                                       const MilitaryFilter& military, FilterStats& stats,
                                       const OptOutList* opt_out) {
    std::vector<StateVector> kept;
    kept.reserve(states.size());
    for (auto& s : states) {
        ++stats.total;
        if (opt_out != nullptr && opt_out->contains(s)) {
            ++stats.dropped_opt_out;
            continue;
        }
        if (military.classify(s)) {
            ++stats.dropped_military;
            continue;
        }
        if (!s.latitude || !s.longitude) {
            ++stats.dropped_no_position;
            continue;
        }
        if (!area.contains({*s.latitude, *s.longitude})) {
            ++stats.dropped_outside;
            continue;
        }
        ++stats.kept;
        kept.push_back(std::move(s));
    }
    return kept;
}

}  // namespace sema

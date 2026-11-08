#pragma once

#include <cstddef>
#include <vector>

#include <nlohmann/json.hpp>

#include "sema/flights.hpp"

namespace sema {

struct StatsQuery {
    double from = 0.0;  // Unix seconds, inclusive
    double to = 0.0;    // exclusive
    // Days and hours are counted in this local time (Turkey: UTC+3, no DST).
    int utc_offset_s = 3 * 3600;
    std::size_t top_n = 10;
};

// Traffic statistics over the flights that started in [from, to) and count
// (airborne, confirmed). Pure function of its inputs, so the same code serves
// the in-memory replay and the PostgreSQL-backed live site.
//
// {"from", "to", "utc_offset_s",
//  "totals":   {"flights", "aircraft", "departures", "arrivals", "days"},
//  "daily":    [{"date": "YYYY-MM-DD", "flights", "aircraft", "departures", "arrivals"}],
//  "hours":    [{"hour": 0..23, "total", "avg"}]   flights in the air in that local hour,
//                                                     total and per day
//  "peak_hour", "busiest_day",
//  "airports": [{"icao", "iata", "name", "city", "departures", "arrivals", "total"}],
//  "routes":   [{"from", "to", "flights"}]             top_n airport pairs
//  "corridors":{"cell_deg", "max", "cells": [[lat_min, lon_min, flights], ...]}}
nlohmann::json compute_traffic_stats(const std::vector<FlightRecord>& flights, const AirportIndex& airports,
                                     const StatsQuery& query);

}  // namespace sema

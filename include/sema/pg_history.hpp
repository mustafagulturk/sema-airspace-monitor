#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "sema/history.hpp"

namespace sema {

// History read from PostgreSQL: replays come from the raw `positions` table
// (kept for SEMA_RETENTION_HOURS), statistics from the `flights` table, and
// the collection streak from `fetch_log`. Uses its own connection, opened on
// first use and reopened after an error; calls are serialised.
class PgHistory : public HistorySource {
public:
    explicit PgHistory(std::string conninfo);
    ~PgHistory() override;
    PgHistory(const PgHistory&) = delete;
    PgHistory& operator=(const PgHistory&) = delete;

    std::optional<TimeRange> available() override;
    std::vector<ReplayAircraft> replay(double from, double to) override;
    std::vector<FlightRecord> flights(double from, double to) override;
    std::optional<TimeRange> collection_streak() override;
    // The collector already writes positions and flights through PgStore.
    void record(const StatesSnapshot& /*snapshot*/, const std::vector<FlightRecord>& /*ended*/) override {}

    struct Impl;  // the connection (public only so pg_history.cpp helpers can name it)

private:
    std::string conninfo_;
    std::mutex mu_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sema

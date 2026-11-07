#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "sema/state_vector.hpp"

namespace sema {

// Serialises a snapshot in OpenSky's /states/all format, so parse_states()
// reads it back unchanged. Used to record already-filtered snapshots (civil
// aircraft inside the area only) for offline replay.
std::string to_opensky_json(const StatesSnapshot& snapshot);

// Appends snapshots as JSON Lines (one /states/all document per line).
class SnapshotWriter {
public:
    explicit SnapshotWriter(const std::filesystem::path& path);
    void write(const StatesSnapshot& snapshot);

private:
    std::ofstream out_;
};

// Reads a JSON Lines recording written by SnapshotWriter. Blank lines are
// skipped; a malformed line throws std::runtime_error naming the line.
class SnapshotReader {
public:
    explicit SnapshotReader(const std::filesystem::path& path);
    std::optional<StatesSnapshot> next();
    [[nodiscard]] std::size_t skipped_rows() const { return skipped_rows_; }

private:
    std::ifstream in_;
    std::size_t line_no_ = 0;
    std::size_t skipped_rows_ = 0;
};

}  // namespace sema

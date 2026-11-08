#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace sema {

// Maps a URL path (no query string) to a regular file under `root`, or
// nullopt. "/" and paths ending in "/" map to index.html. Rejects "..",
// backslashes, NUL and anything whose real path (after symlinks) leaves
// `root`.
std::optional<std::filesystem::path> resolve_static_path(const std::filesystem::path& root, std::string_view url_path);

// Content-Type for a file name, by extension.
std::string_view mime_type(const std::filesystem::path& file);

}  // namespace sema

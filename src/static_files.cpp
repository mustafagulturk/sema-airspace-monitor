#include "sema/static_files.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <system_error>
#include <utility>

namespace sema {

std::optional<std::filesystem::path> resolve_static_path(const std::filesystem::path& root, std::string_view url_path) {
    namespace fs = std::filesystem;
    if (url_path.empty() || url_path.front() != '/') return std::nullopt;
    if (url_path.find('\\') != std::string_view::npos || url_path.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    std::string rel(url_path.substr(1));
    if (rel.empty() || rel.back() == '/') rel += "index.html";

    // Reject any ".." segment outright instead of relying on normalisation.
    std::size_t start = 0;
    while (start <= rel.size()) {
        const auto end = std::min(rel.find('/', start), rel.size());
        if (rel.compare(start, end - start, "..") == 0 && end - start == 2) return std::nullopt;
        start = end + 1;
    }

    std::error_code ec;
    const fs::path real_root = fs::canonical(root, ec);
    if (ec) return std::nullopt;
    fs::path real = fs::canonical(real_root / rel, ec);
    if (ec || !fs::is_regular_file(real, ec)) return std::nullopt;
    // Prefix check on whole path components, so /web2 does not pass for /web.
    auto r = real_root.begin();
    auto p = real.begin();
    for (; r != real_root.end(); ++r, ++p) {
        if (p == real.end() || *p != *r) return std::nullopt;
    }
    return real;
}

std::string_view mime_type(const std::filesystem::path& file) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 14> kTypes{{
        {".html", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".json", "application/json"},
        {".geojson", "application/geo+json"},
        {".map", "application/json"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".ico", "image/x-icon"},
        {".webp", "image/webp"},
        {".woff2", "font/woff2"},
        {".txt", "text/plain; charset=utf-8"},
        {".webmanifest", "application/manifest+json"},
    }};
    const std::string ext = file.extension().string();
    for (const auto& [e, t] : kTypes) {
        if (e == ext) return t;
    }
    return "application/octet-stream";
}

}  // namespace sema

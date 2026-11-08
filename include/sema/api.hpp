#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>

namespace sema {

// Minimal HTTP request/response for the JSON API and /metrics, independent of
// the HTTP library so handlers can be unit tested without a server.
struct ApiRequest {
    std::string path;                          // e.g. "/api/stats"
    std::map<std::string, std::string> query;  // decoded query parameters
};

struct ApiResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    int max_age_s = 0;  // Cache-Control max-age; 0 means no-cache
};

// Returns nullopt for paths it does not serve.
using ApiHandler = std::function<std::optional<ApiResponse>(const ApiRequest&)>;

// Parses "a=1&b=x%20y" (no leading '?'). Later duplicates win.
std::map<std::string, std::string> parse_query(const std::string& query);

}  // namespace sema

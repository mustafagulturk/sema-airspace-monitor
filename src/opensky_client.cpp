#include "sema/opensky_client.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <exception>

namespace sema {
namespace {

std::optional<int> header_int(const HttpResponse& r, const std::string& key) {
    auto it = r.headers.find(key);
    if (it == r.headers.end()) return std::nullopt;
    int v = 0;
    const auto& s = it->second;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{}) return std::nullopt;
    return v;
}

std::string fmt_coord(double v) {
    char buf[32];
    if (std::snprintf(buf, sizeof buf, "%.4f", v) < 0) return "0";
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

}  // namespace

const char* to_string(FetchResult::Status s) {
    switch (s) {
        case FetchResult::Status::Ok: return "ok";
        case FetchResult::Status::RateLimited: return "rate_limited";
        case FetchResult::Status::AuthFailed: return "auth_failed";
        case FetchResult::Status::Failed: return "failed";
    }
    return "unknown";
}

std::string states_url(const std::string& api_url, const BoundingBox& box) {
    return api_url + "/states/all?lamin=" + fmt_coord(box.lat_min) + "&lomin=" + fmt_coord(box.lon_min) +
           "&lamax=" + fmt_coord(box.lat_max) + "&lomax=" + fmt_coord(box.lon_max);
}

OpenSkyClient::OpenSkyClient(OpenSkyConfig cfg, HttpTransport& http, Clock& clock)
    : cfg_(std::move(cfg)), http_(http), clock_(clock) {}

std::chrono::milliseconds OpenSkyClient::backoff_for(int attempt) const {
    // attempt is 1-based: the wait after the first failure is initial_backoff.
    auto d = cfg_.initial_backoff;
    for (int i = 1; i < attempt && d < cfg_.max_backoff; ++i) d *= 2;
    return std::min(d, cfg_.max_backoff);
}

OpenSkyClient::TokenStatus OpenSkyClient::ensure_token(std::string& error) {
    if (!token_.empty() && clock_.now() + cfg_.token_refresh_margin < token_expiry_) return TokenStatus::Ok;
    token_.clear();

    HttpRequest req;
    req.method = HttpRequest::Method::Post;
    req.url = cfg_.token_url;
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.body = "grant_type=client_credentials&client_id=" + url_encode(cfg_.client_id) +
               "&client_secret=" + url_encode(cfg_.client_secret);

    const auto issued = clock_.now();
    const HttpResponse resp = http_.send(req);
    if (resp.status == 400 || resp.status == 401 || resp.status == 403) {
        error = "token endpoint rejected credentials (HTTP " + std::to_string(resp.status) + ")";
        return TokenStatus::Rejected;
    }
    if (resp.status != 200) {
        error = resp.status == 0 ? "token request failed: " + resp.transport_error
                                 : "token endpoint returned HTTP " + std::to_string(resp.status);
        return TokenStatus::Unavailable;
    }
    try {
        const auto j = nlohmann::json::parse(resp.body);
        token_ = j.at("access_token").get<std::string>();
        const auto expires_in = j.value("expires_in", 1800);
        token_expiry_ = issued + std::chrono::seconds(expires_in);
    } catch (const std::exception&) {
        token_.clear();
        error = "token endpoint returned an unexpected body";
        return TokenStatus::Unavailable;
    }
    if (token_.empty()) {
        error = "token endpoint returned an empty token";
        return TokenStatus::Unavailable;
    }
    return TokenStatus::Ok;
}

FetchResult OpenSkyClient::fetch_states(const BoundingBox& box) {
    FetchResult out;
    out.credits_cost = opensky_credit_cost(box);
    const auto start = clock_.now();
    auto finish = [&]() -> FetchResult& {
        out.latency = std::chrono::duration_cast<std::chrono::milliseconds>(clock_.now() - start);
        return out;
    };

    const std::string url = states_url(cfg_.api_url, box);
    bool refreshed_after_401 = false;

    while (out.attempts < cfg_.max_attempts) {
        ++out.attempts;

        std::string err;
        const TokenStatus ts = ensure_token(err);
        bool transient = false;
        if (ts == TokenStatus::Rejected) {
            out.status = FetchResult::Status::AuthFailed;
            out.error = err;
            return finish();
        }
        if (ts == TokenStatus::Unavailable) {
            out.error = err;
            transient = true;
        } else {
            HttpRequest req;
            req.url = url;
            req.headers["Authorization"] = "Bearer " + token_;
            const HttpResponse resp = http_.send(req);
            out.http_status = resp.status;
            if (auto rem = header_int(resp, "x-rate-limit-remaining")) out.credits_remaining = rem;

            if (resp.status == 200) {
                try {
                    ParseResult pr = parse_states(resp.body);
                    out.snapshot = std::move(pr.snapshot);
                    out.skipped_rows = pr.skipped_rows;
                    out.status = FetchResult::Status::Ok;
                    out.error.clear();
                } catch (const std::exception& e) {
                    out.status = FetchResult::Status::Failed;
                    out.error = std::string("malformed states response: ") + e.what();
                }
                return finish();
            }
            if (resp.status == 429) {
                out.status = FetchResult::Status::RateLimited;
                if (auto ra = header_int(resp, "x-rate-limit-retry-after-seconds")) {
                    out.retry_after = std::chrono::seconds(*ra);
                }
                out.error = "OpenSky credits exhausted (HTTP 429)";
                return finish();
            }
            if (resp.status == 401) {
                invalidate_token();
                if (refreshed_after_401) {
                    out.status = FetchResult::Status::AuthFailed;
                    out.error = "API rejected a freshly issued token (HTTP 401)";
                    return finish();
                }
                refreshed_after_401 = true;
                out.error = "access token rejected (HTTP 401), refreshing";
                continue;  // retry immediately with a new token
            }
            if (resp.status == 0 || resp.status >= 500) {
                transient = true;
                out.error = resp.status == 0 ? "transport error: " + resp.transport_error
                                             : "server error HTTP " + std::to_string(resp.status);
            } else {
                out.status = FetchResult::Status::Failed;
                out.error = "unexpected HTTP " + std::to_string(resp.status);
                return finish();
            }
        }

        if (transient && out.attempts < cfg_.max_attempts) {
            if (!clock_.sleep_for(backoff_for(out.attempts))) {
                out.status = FetchResult::Status::Failed;
                out.error += " (stopped during backoff)";
                return finish();
            }
        }
    }
    out.status = FetchResult::Status::Failed;
    out.error = "giving up after " + std::to_string(out.attempts) + " attempts: " + out.error;
    return finish();
}

}  // namespace sema

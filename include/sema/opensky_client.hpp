#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "sema/clock.hpp"
#include "sema/geo.hpp"
#include "sema/http.hpp"
#include "sema/state_vector.hpp"

namespace sema {

struct OpenSkyConfig {
    std::string client_id;
    std::string client_secret;
    std::string token_url = "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token";
    std::string api_url = "https://opensky-network.org/api";
    int max_attempts = 4;                                   // per fetch, for transient errors
    std::chrono::milliseconds initial_backoff{2000};        // doubles each retry
    std::chrono::milliseconds max_backoff{60000};
    std::chrono::seconds token_refresh_margin{60};          // refresh this long before expiry
};

struct FetchResult {
    enum class Status {
        Ok,
        RateLimited,   // HTTP 429: daily credits used up; wait retry_after
        AuthFailed,    // credentials rejected
        Failed,        // retries exhausted or unexpected response
    };
    Status status = Status::Failed;
    std::optional<StatesSnapshot> snapshot;
    std::size_t skipped_rows = 0;
    int http_status = 0;
    int attempts = 0;
    int credits_cost = 0;
    std::optional<int> credits_remaining;   // X-Rate-Limit-Remaining
    std::optional<std::chrono::seconds> retry_after;  // X-Rate-Limit-Retry-After-Seconds
    std::chrono::milliseconds latency{0};
    std::string error;
};

const char* to_string(FetchResult::Status s);

// OpenSky REST client using OAuth2 client credentials.
// Not thread safe; use one instance from the collector thread.
class OpenSkyClient {
public:
    OpenSkyClient(OpenSkyConfig cfg, HttpTransport& http, Clock& clock);

    FetchResult fetch_states(const BoundingBox& box);

private:
    enum class TokenStatus { Ok, Rejected, Unavailable };
    TokenStatus ensure_token(std::string& error);
    void invalidate_token() { token_.clear(); }
    std::chrono::milliseconds backoff_for(int attempt) const;

    OpenSkyConfig cfg_;
    HttpTransport& http_;
    Clock& clock_;
    std::string token_;
    Clock::time_point token_expiry_{};
};

std::string states_url(const std::string& api_url, const BoundingBox& box);

}  // namespace sema

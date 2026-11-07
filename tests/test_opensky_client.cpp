#include <gtest/gtest.h>

#include <deque>
#include <thread>

#include "sema/clock.hpp"
#include "sema/opensky_client.hpp"

using namespace std::chrono_literals;
using sema::FetchResult;
using sema::HttpRequest;
using sema::HttpResponse;

namespace {

class FakeTransport : public sema::HttpTransport {
public:
    std::deque<HttpResponse> responses;
    std::vector<HttpRequest> requests;

    HttpResponse send(const HttpRequest& req) override {
        requests.push_back(req);
        if (responses.empty()) {
            ADD_FAILURE() << "unexpected request to " << req.url;
            return HttpResponse{500, {}, "", ""};
        }
        auto r = responses.front();
        responses.pop_front();
        return r;
    }
    void push(int status, std::string body = "", std::map<std::string, std::string> headers = {}) {
        HttpResponse r;
        r.status = status;
        r.body = std::move(body);
        r.headers = std::move(headers);
        if (status == 0) r.transport_error = "Couldn't resolve host name";
        responses.push_back(std::move(r));
    }
    void push_token(const std::string& token = "tok-1", int expires_in = 1800) {
        push(200, R"({"access_token":")" + token + R"(","expires_in":)" + std::to_string(expires_in) +
                      R"(,"token_type":"Bearer"})");
    }
    void push_states(std::map<std::string, std::string> headers = {}) {
        push(200,
             R"({"time":1700000000,"states":[["4bb1c3","THY1AB  ","Turkey",1700000000,1700000000,28.9,41.0,10000.0,false,230.0,90.0,0.0,null,10200.0,"1234",false,0]]})",
             std::move(headers));
    }
    std::size_t token_requests() const {
        std::size_t n = 0;
        for (const auto& r : requests) n += r.method == HttpRequest::Method::Post;
        return n;
    }
};

class FakeClock : public sema::Clock {
public:
    time_point t{std::chrono::system_clock::from_time_t(1700000000)};
    std::vector<std::chrono::milliseconds> sleeps;
    bool stop_on_sleep = false;

    time_point now() const override { return t; }
    bool sleep_for(std::chrono::milliseconds d) override {
        sleeps.push_back(d);
        if (stop_on_sleep) return false;
        t += d;
        return true;
    }
};

constexpr sema::BoundingBox kTurkey{35.7, 25.4, 42.3, 44.85};

sema::OpenSkyConfig config() {
    sema::OpenSkyConfig c;
    c.client_id = "my-client";
    c.client_secret = "s3cr&t=value";
    c.token_url = "https://auth.example/token";
    c.api_url = "https://api.example/api";
    return c;
}

struct Fixture : ::testing::Test {
    FakeTransport http;
    FakeClock clock;
    sema::OpenSkyClient client{config(), http, clock};
};

}  // namespace

TEST(StatesUrl, FormatsBoundingBoxCompactly) {
    EXPECT_EQ(sema::states_url("https://x/api", kTurkey),
              "https://x/api/states/all?lamin=35.7&lomin=25.4&lamax=42.3&lomax=44.85");
    EXPECT_EQ(sema::states_url("https://x/api", {36, 26, 42, 45}),
              "https://x/api/states/all?lamin=36&lomin=26&lamax=42&lomax=45");
}

TEST(UrlEncode, EscapesReservedCharacters) {
    EXPECT_EQ(sema::url_encode("a b&c=d/é"), "a%20b%26c%3Dd%2F%C3%A9");
    EXPECT_EQ(sema::url_encode("AZaz09-_.~"), "AZaz09-_.~");
}

TEST_F(Fixture, SuccessfulFetchSendsTokenRequestAndBearer) {
    http.push_token("abc");
    http.push_states({{"x-rate-limit-remaining", "3997"}});

    const FetchResult r = client.fetch_states(kTurkey);

    ASSERT_EQ(r.status, FetchResult::Status::Ok) << r.error;
    ASSERT_TRUE(r.snapshot);
    EXPECT_EQ(r.snapshot->states.size(), 1U);
    EXPECT_EQ(r.http_status, 200);
    EXPECT_EQ(r.attempts, 1);
    EXPECT_EQ(r.credits_cost, 3);
    EXPECT_EQ(r.credits_remaining, 3997);

    ASSERT_EQ(http.requests.size(), 2U);
    const auto& tok = http.requests[0];
    EXPECT_EQ(tok.method, HttpRequest::Method::Post);
    EXPECT_EQ(tok.url, "https://auth.example/token");
    EXPECT_EQ(tok.headers.at("Content-Type"), "application/x-www-form-urlencoded");
    EXPECT_EQ(tok.body, "grant_type=client_credentials&client_id=my-client&client_secret=s3cr%26t%3Dvalue");

    const auto& get = http.requests[1];
    EXPECT_EQ(get.method, HttpRequest::Method::Get);
    EXPECT_EQ(get.url, "https://api.example/api/states/all?lamin=35.7&lomin=25.4&lamax=42.3&lomax=44.85");
    EXPECT_EQ(get.headers.at("Authorization"), "Bearer abc");
}

TEST_F(Fixture, TokenIsReusedAcrossFetches) {
    http.push_token();
    http.push_states();
    http.push_states();
    EXPECT_EQ(client.fetch_states(kTurkey).status, FetchResult::Status::Ok);
    clock.t += 90s;
    EXPECT_EQ(client.fetch_states(kTurkey).status, FetchResult::Status::Ok);
    EXPECT_EQ(http.token_requests(), 1U);
}

TEST_F(Fixture, TokenRefreshedBeforeExpiry) {
    http.push_token("first", 1800);
    http.push_states();
    http.push_token("second", 1800);
    http.push_states();
    EXPECT_EQ(client.fetch_states(kTurkey).status, FetchResult::Status::Ok);
    clock.t += 1800s - 59s;  // inside the 60 s refresh margin
    EXPECT_EQ(client.fetch_states(kTurkey).status, FetchResult::Status::Ok);
    EXPECT_EQ(http.token_requests(), 2U);
    EXPECT_EQ(http.requests.back().headers.at("Authorization"), "Bearer second");
}

TEST_F(Fixture, Unauthorized401RefreshesTokenOnceAndRetries) {
    http.push_token("old");
    http.push(401);
    http.push_token("new");
    http.push_states();
    const auto r = client.fetch_states(kTurkey);
    EXPECT_EQ(r.status, FetchResult::Status::Ok) << r.error;
    EXPECT_EQ(r.attempts, 2);
    EXPECT_TRUE(clock.sleeps.empty());
    EXPECT_EQ(http.requests.back().headers.at("Authorization"), "Bearer new");
}

TEST_F(Fixture, Repeated401IsAuthFailure) {
    http.push_token("a");
    http.push(401);
    http.push_token("b");
    http.push(401);
    const auto r = client.fetch_states(kTurkey);
    EXPECT_EQ(r.status, FetchResult::Status::AuthFailed);
}

TEST_F(Fixture, ServerErrorsBackOffExponentially) {
    http.push_token();
    http.push(503);
    http.push(502);
    http.push_states();
    const auto r = client.fetch_states(kTurkey);
    EXPECT_EQ(r.status, FetchResult::Status::Ok) << r.error;
    EXPECT_EQ(r.attempts, 3);
    EXPECT_EQ(clock.sleeps, (std::vector<std::chrono::milliseconds>{2s, 4s}));
    EXPECT_EQ(r.latency, 6s);
    EXPECT_EQ(http.token_requests(), 1U);
}

TEST(OpenSkyClientBackoff, GivesUpAfterMaxAttemptsAndCapsBackoff) {
    FakeTransport http;
    FakeClock clock;
    auto cfg = config();
    cfg.max_attempts = 5;
    cfg.initial_backoff = 2s;
    cfg.max_backoff = 5s;
    sema::OpenSkyClient client(cfg, http, clock);
    http.push_token();
    for (int i = 0; i < 5; ++i) http.push(500);

    const auto r = client.fetch_states(kTurkey);
    EXPECT_EQ(r.status, FetchResult::Status::Failed);
    EXPECT_EQ(r.attempts, 5);
    EXPECT_EQ(r.http_status, 500);
    EXPECT_EQ(clock.sleeps, (std::vector<std::chrono::milliseconds>{2s, 4s, 5s, 5s}));
    EXPECT_NE(r.error.find("giving up"), std::string::npos);
}

TEST_F(Fixture, TransportErrorIsRetried) {
    http.push_token();
    http.push(0);
    http.push_states();
    const auto r = client.fetch_states(kTurkey);
    EXPECT_EQ(r.status, FetchResult::Status::Ok) << r.error;
    EXPECT_EQ(clock.sleeps.size(), 1U);
}

TEST_F(Fixture, TokenEndpointOutageIsRetried) {
    http.push(503);
    http.push_token();
    http.push_states();
    const auto r = client.fetch_states(kTurkey);
    EXPECT_EQ(r.status, FetchResult::Status::Ok) << r.error;
    EXPECT_EQ(clock.sleeps.size(), 1U);
}

TEST_F(Fixture, RateLimitReturnsRetryAfterWithoutWaiting) {
    http.push_token();
    http.push(429, "", {{"x-rate-limit-retry-after-seconds", "5400"}, {"x-rate-limit-remaining", "0"}});
    const auto r = client.fetch_states(kTurkey);
    EXPECT_EQ(r.status, FetchResult::Status::RateLimited);
    ASSERT_TRUE(r.retry_after);
    EXPECT_EQ(*r.retry_after, 5400s);
    EXPECT_EQ(r.credits_remaining, 0);
    EXPECT_TRUE(clock.sleeps.empty());
    EXPECT_EQ(r.attempts, 1);
}

TEST_F(Fixture, RejectedCredentialsAreAuthFailureAndSecretNotLeaked) {
    http.push(401, R"({"error":"unauthorized_client"})");
    const auto r = client.fetch_states(kTurkey);
    EXPECT_EQ(r.status, FetchResult::Status::AuthFailed);
    EXPECT_EQ(r.error.find("s3cr"), std::string::npos);
    EXPECT_EQ(http.requests.size(), 1U);
    EXPECT_TRUE(clock.sleeps.empty());
}

TEST_F(Fixture, MalformedJsonFailsWithoutRetry) {
    http.push_token();
    http.push(200, "{not json");
    const auto r = client.fetch_states(kTurkey);
    EXPECT_EQ(r.status, FetchResult::Status::Failed);
    EXPECT_EQ(r.attempts, 1);
    EXPECT_TRUE(clock.sleeps.empty());
    EXPECT_NE(r.error.find("malformed"), std::string::npos);
}

TEST_F(Fixture, UnexpectedClientErrorIsNotRetried) {
    http.push_token();
    http.push(400);
    const auto r = client.fetch_states(kTurkey);
    EXPECT_EQ(r.status, FetchResult::Status::Failed);
    EXPECT_EQ(r.attempts, 1);
}

TEST_F(Fixture, StopDuringBackoffAborts) {
    clock.stop_on_sleep = true;
    http.push_token();
    http.push(503);
    const auto r = client.fetch_states(kTurkey);
    EXPECT_EQ(r.status, FetchResult::Status::Failed);
    EXPECT_EQ(r.attempts, 1);
    EXPECT_NE(r.error.find("stopped"), std::string::npos);
}

TEST(SystemClock, RequestStopWakesSleeper) {
    sema::SystemClock clock;
    bool completed = true;
    const auto start = std::chrono::steady_clock::now();
    std::thread sleeper([&] { completed = clock.sleep_for(60s); });
    std::this_thread::sleep_for(50ms);
    clock.request_stop();
    sleeper.join();
    EXPECT_FALSE(completed);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
    EXPECT_TRUE(clock.stop_requested());
    EXPECT_FALSE(clock.sleep_for(10s));  // later sleeps return immediately
}

TEST(SystemClock, ShortSleepCompletes) {
    sema::SystemClock clock;
    EXPECT_TRUE(clock.sleep_for(5ms));
}

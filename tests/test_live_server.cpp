#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include "sema/live_server.hpp"

using namespace sema;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
namespace fs = std::filesystem;

namespace {

struct Client {
    net::io_context ioc;
    websocket::stream<tcp::socket> ws{ioc};

    explicit Client(std::uint16_t port) {
        tcp::resolver resolver(ioc);
        net::connect(ws.next_layer(), resolver.resolve("127.0.0.1", std::to_string(port)));
        ws.handshake("127.0.0.1", "/ws");
    }
    std::string read() {
        beast::flat_buffer b;
        ws.read(b);
        return beast::buffers_to_string(b.data());
    }
};

http::response<http::string_body> get(std::uint16_t port, const std::string& target,
                                      http::verb verb = http::verb::get) {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);
    stream.connect(resolver.resolve("127.0.0.1", std::to_string(port)));
    http::request<http::empty_body> req{verb, target, 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(false);
    http::write(stream, req);
    beast::flat_buffer b;
    http::response_parser<http::string_body> parser;
    if (verb == http::verb::head) parser.skip(true);
    http::read(stream, b, parser);
    return parser.release();
}

bool wait_for(const std::function<bool()>& cond) {
    for (int i = 0; i < 200; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

class LiveServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("sema_live_" + std::to_string(::getpid()) + "_" +
                                            ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::create_directories(dir_ / "web" / "assets");
        std::ofstream(dir_ / "web" / "index.html") << "<!doctype html><title>sema</title>";
        std::ofstream(dir_ / "web" / "assets" / "app-abc.js") << "console.log(1)";
        std::ofstream(dir_ / "secret.txt") << "secret";
        std::ofstream(dir_ / "boundary.geojson") << R"({"type":"FeatureCollection","features":[]})";
    }
    void TearDown() override { fs::remove_all(dir_); }

    LiveServerConfig config() const {
        LiveServerConfig c;
        c.port = 0;
        c.web_root = dir_ / "web";
        c.files["/boundary.geojson"] = dir_ / "boundary.geojson";
        return c;
    }

    fs::path dir_;
};

}  // namespace

TEST_F(LiveServerTest, NewClientGetsLatestOfEachTopicThenUpdates) {
    LiveServer server(config());
    server.start();
    ASSERT_NE(server.port(), 0);
    server.publish("hello", R"({"type":"hello"})");
    server.publish("snapshot", R"({"type":"snapshot","n":1})");
    server.publish("coverage", R"({"type":"coverage"})");
    server.publish("snapshot", R"({"type":"snapshot","n":2})");  // replaces n=1

    Client c(server.port());
    EXPECT_EQ(c.read(), R"({"type":"hello"})");
    EXPECT_EQ(c.read(), R"({"type":"snapshot","n":2})");
    EXPECT_EQ(c.read(), R"({"type":"coverage"})");
    ASSERT_TRUE(wait_for([&] { return server.client_count() == 1; }));

    server.publish("snapshot", R"({"type":"snapshot","n":3})");
    EXPECT_EQ(c.read(), R"({"type":"snapshot","n":3})");
}

TEST_F(LiveServerTest, BroadcastsToSeveralClientsFromAnotherThread) {
    LiveServer server(config());
    server.start();
    Client a(server.port());
    Client b(server.port());
    ASSERT_TRUE(wait_for([&] { return server.client_count() == 2; }));

    std::thread publisher([&] {
        for (int i = 0; i < 5; ++i) server.publish("snapshot", "m" + std::to_string(i));
    });
    publisher.join();
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(a.read(), "m" + std::to_string(i));
        EXPECT_EQ(b.read(), "m" + std::to_string(i));
    }
}

TEST_F(LiveServerTest, ClosedClientsAreForgotten) {
    LiveServer server(config());
    server.start();
    {
        Client c(server.port());
        ASSERT_TRUE(wait_for([&] { return server.client_count() == 1; }));
        c.ws.close(websocket::close_code::normal);
    }
    EXPECT_TRUE(wait_for([&] { return server.client_count() == 0; }));
}

TEST_F(LiveServerTest, ServesStaticFilesAndHealth) {
    LiveServer server(config());
    server.start();
    auto r = get(server.port(), "/");
    EXPECT_EQ(r.result(), http::status::ok);
    EXPECT_EQ(r.body(), "<!doctype html><title>sema</title>");
    EXPECT_EQ(r[http::field::content_type], "text/html; charset=utf-8");
    EXPECT_EQ(r[http::field::cache_control], "no-cache");

    r = get(server.port(), "/assets/app-abc.js?v=1");
    EXPECT_EQ(r.result(), http::status::ok);
    EXPECT_EQ(r.body(), "console.log(1)");
    EXPECT_NE(std::string(r[http::field::cache_control]).find("immutable"), std::string::npos);

    r = get(server.port(), "/boundary.geojson");
    EXPECT_EQ(r.result(), http::status::ok);
    EXPECT_EQ(r[http::field::content_type], "application/geo+json");

    r = get(server.port(), "/healthz");
    EXPECT_EQ(r.result(), http::status::ok);
    EXPECT_NE(r.body().find("\"clients\":0"), std::string::npos);

    r = get(server.port(), "/index.html", http::verb::head);
    EXPECT_EQ(r.result(), http::status::ok);
    EXPECT_EQ(r[http::field::content_length], "34");
}

TEST_F(LiveServerTest, RefusesTraversalUnknownPathsAndOtherMethods) {
    LiveServer server(config());
    server.start();
    EXPECT_EQ(get(server.port(), "/../secret.txt").result(), http::status::not_found);
    EXPECT_EQ(get(server.port(), "/nope.js").result(), http::status::not_found);
    EXPECT_EQ(get(server.port(), "/", http::verb::post).result(), http::status::method_not_allowed);
}

TEST_F(LiveServerTest, RefusesClientsOverTheLimit) {
    auto cfg = config();
    cfg.max_clients = 1;
    LiveServer server(cfg);
    server.start();
    Client a(server.port());
    ASSERT_TRUE(wait_for([&] { return server.client_count() == 1; }));
    EXPECT_THROW(Client b(server.port()), beast::system_error);
}

TEST_F(LiveServerTest, DisconnectsAClientThatStopsReading) {
    auto cfg = config();
    cfg.max_queued_messages = 2;
    LiveServer server(cfg);
    server.start();
    Client slow(server.port());
    ASSERT_TRUE(wait_for([&] { return server.client_count() == 1; }));
    // Large messages fill the socket buffers, then the server-side queue.
    const std::string big(1 << 20, 'x');
    EXPECT_TRUE(wait_for([&] {
        server.publish("snapshot", big);
        return server.client_count() == 0;
    }));
}

TEST_F(LiveServerTest, FailsToBindAPortInUse) {
    LiveServer a(config());
    a.start();
    auto cfg = config();
    cfg.port = a.port();
    LiveServer b(cfg);
    // SO_REUSEADDR does not allow two listeners on Linux.
    EXPECT_THROW(b.start(), std::runtime_error);
}

TEST_F(LiveServerTest, AnswersApiRequestsOnTheWorkerThread) {
    auto cfg = config();
    std::atomic<int> calls{0};
    const auto io_thread_hint = std::this_thread::get_id();
    std::atomic<bool> ran_on_other_thread{false};
    cfg.api = [&](const ApiRequest& req) -> std::optional<ApiResponse> {
        ++calls;
        ran_on_other_thread = std::this_thread::get_id() != io_thread_hint;
        if (req.path == "/metrics") return ApiResponse{200, "text/plain; version=0.0.4", "sema_up 1\n", 0};
        if (req.path == "/api/echo") {
            return ApiResponse{200, "application/json", R"({"from":")" + req.query.at("from") + "\"}", 60};
        }
        if (req.path == "/api/boom") throw std::runtime_error("kaput");
        return std::nullopt;
    };
    LiveServer server(cfg);
    server.start();

    auto r = get(server.port(), "/api/echo?from=12%2034&x=1");
    EXPECT_EQ(r.result(), http::status::ok);
    EXPECT_EQ(r.body(), R"({"from":"12 34"})");
    EXPECT_EQ(r[http::field::content_type], "application/json");
    EXPECT_EQ(r[http::field::cache_control], "public, max-age=60");
    EXPECT_TRUE(ran_on_other_thread);

    r = get(server.port(), "/metrics");
    EXPECT_EQ(r.result(), http::status::ok);
    EXPECT_EQ(r.body(), "sema_up 1\n");
    EXPECT_EQ(r[http::field::cache_control], "no-cache");

    EXPECT_EQ(get(server.port(), "/api/unknown").result(), http::status::not_found);
    r = get(server.port(), "/api/boom");
    EXPECT_EQ(r.result(), http::status::internal_server_error);
    EXPECT_NE(r.body().find("kaput"), std::string::npos);
    EXPECT_EQ(calls.load(), 4);

    // Static files and health still work.
    EXPECT_EQ(get(server.port(), "/healthz").result(), http::status::ok);
}

TEST_F(LiveServerTest, ApiPathsAre404WithoutAHandler) {
    LiveServer server(config());
    server.start();
    EXPECT_EQ(get(server.port(), "/api/status").result(), http::status::not_found);
    EXPECT_EQ(get(server.port(), "/metrics").result(), http::status::not_found);
}

TEST_F(LiveServerTest, StopsCleanlyWithASlowApiCallInFlight) {
    auto cfg = config();
    std::atomic<bool> entered{false};
    cfg.api = [&](const ApiRequest&) -> std::optional<ApiResponse> {
        entered = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return ApiResponse{200, "application/json", "{}", 0};
    };
    auto server = std::make_unique<LiveServer>(cfg);
    server->start();
    const auto port = server->port();
    std::thread client([port] {
        try {
            (void)get(port, "/api/slow");
        } catch (const std::exception&) {
            // The server may close the connection while stopping.
        }
    });
    ASSERT_TRUE(wait_for([&] { return entered.load(); }));
    server.reset();  // must wait for the handler, not crash
    client.join();
}

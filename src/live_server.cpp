#include "sema/live_server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include "sema/static_files.hpp"

namespace sema {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using Message = std::shared_ptr<const std::string>;

constexpr std::string_view kServerName = "sema";

class WsSession;

// State shared by the server and its sessions. Sessions keep it alive with a
// shared_ptr, so it must not own the io_context (that would be a cycle).
struct Hub {
    LiveServerConfig cfg;
    mutable std::mutex mu;
    std::vector<std::pair<std::string, Message>> latest;  // first-publish order
    std::vector<std::weak_ptr<WsSession>> sessions;
    // API worker, owned by LiveServer::Impl and joined in stop() after the I/O
    // thread, so it outlives every handler that can reach it.
    net::thread_pool* workers = nullptr;
    std::atomic<std::size_t> api_in_flight{0};

    explicit Hub(LiveServerConfig c) : cfg(std::move(c)) {}

    void prune_locked() {
        std::erase_if(sessions, [](const auto& w) { return w.expired(); });
    }
    std::size_t clients() const {
        const std::scoped_lock lock(mu);
        return static_cast<std::size_t>(
            std::count_if(sessions.begin(), sessions.end(), [](const auto& w) { return !w.expired(); }));
    }
    // Registers a session and returns what it must be sent first. Done under
    // the same lock as publish(), so a session sees every message exactly once.
    std::vector<Message> join(const std::shared_ptr<WsSession>& s) {
        const std::scoped_lock lock(mu);
        prune_locked();
        sessions.push_back(s);
        std::vector<Message> out;
        out.reserve(latest.size());
        for (const auto& [_, m] : latest) out.push_back(m);
        return out;
    }
};

class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    WsSession(tcp::socket&& socket, std::shared_ptr<Hub> hub) : ws_(std::move(socket)), hub_(std::move(hub)) {}

    void run(const http::request<http::string_body>& req) {
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) { res.set(http::field::server, kServerName); }));
        websocket::permessage_deflate pmd;
        pmd.server_enable = true;
        ws_.set_option(pmd);
        ws_.read_message_max(4096);
        ws_.async_accept(req, beast::bind_front_handler(&WsSession::on_accept, shared_from_this()));
    }

    net::any_io_executor executor() { return ws_.get_executor(); }

    // Must run on the session's executor.
    void send(Message m) {
        if (closed_) return;
        if (queue_.size() >= hub_->cfg.max_queued_messages) {
            // Slow client: drop it instead of buffering without bound.
            closed_ = true;
            queue_.clear();
            beast::error_code ec;
            std::ignore = beast::get_lowest_layer(ws_).socket().close(ec);
            return;
        }
        queue_.push_back(std::move(m));
        if (queue_.size() == 1) do_write();
    }

private:
    void on_accept(beast::error_code ec) {
        if (ec) return;
        for (auto& m : hub_->join(shared_from_this())) send(std::move(m));
        do_read();
    }

    void do_read() { ws_.async_read(in_, beast::bind_front_handler(&WsSession::on_read, shared_from_this())); }

    void on_read(beast::error_code ec, std::size_t /*bytes*/) {
        if (ec) {
            closed_ = true;
            return;
        }
        in_.consume(in_.size());  // clients only listen
        do_read();
    }

    void do_write() {
        ws_.text(true);
        ws_.async_write(net::buffer(*queue_.front()),
                        beast::bind_front_handler(&WsSession::on_write, shared_from_this()));
    }

    void on_write(beast::error_code ec, std::size_t /*bytes*/) {
        if (ec) {
            closed_ = true;
            queue_.clear();
            return;
        }
        if (!queue_.empty()) queue_.pop_front();
        if (!queue_.empty()) do_write();
    }

    websocket::stream<beast::tcp_stream> ws_;
    std::shared_ptr<Hub> hub_;
    beast::flat_buffer in_;
    std::deque<Message> queue_;
    bool closed_ = false;
};

std::string_view path_of(std::string_view target) {
    const auto q = target.find_first_of("?#");
    return q == std::string_view::npos ? target : target.substr(0, q);
}

std::string_view query_of(std::string_view target) {
    const auto q = target.find('?');
    if (q == std::string_view::npos) return {};
    auto rest = target.substr(q + 1);
    if (const auto h = rest.find('#'); h != std::string_view::npos) rest = rest.substr(0, h);
    return rest;
}

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket&& socket, std::shared_ptr<Hub> hub) : stream_(std::move(socket)), hub_(std::move(hub)) {}

    void run() { net::dispatch(stream_.get_executor(), beast::bind_front_handler(&HttpSession::do_read, shared_from_this())); }

private:
    void do_read() {
        parser_.emplace();
        parser_->header_limit(8 * 1024);
        parser_->body_limit(std::uint64_t{8} * 1024);
        stream_.expires_after(std::chrono::seconds(30));
        http::async_read(stream_, buffer_, *parser_, beast::bind_front_handler(&HttpSession::on_read, shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t /*bytes*/) {
        if (ec == http::error::end_of_stream) {
            std::ignore = stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
            return;
        }
        if (ec) return;
        if (!parser_) return;
        auto req = parser_->release();

        if (websocket::is_upgrade(req)) {
            if (path_of(req.target()) != "/ws") return send(text_response(req, http::status::not_found, "not found\n"));
            if (hub_->clients() >= hub_->cfg.max_clients) {
                return send(text_response(req, http::status::service_unavailable, "too many clients\n"));
            }
            stream_.expires_never();
            std::make_shared<WsSession>(stream_.release_socket(), hub_)->run(req);
            return;
        }
        handle(std::move(req));
    }

    static http::response<http::string_body> text_response(const http::request<http::string_body>& req,
                                                           http::status status, std::string body,
                                                           std::string_view type = "text/plain; charset=utf-8") {
        http::response<http::string_body> res{status, req.version()};
        res.set(http::field::server, kServerName);
        res.set(http::field::content_type, type);
        res.set(http::field::cache_control, "no-cache");
        res.set("X-Content-Type-Options", "nosniff");
        res.keep_alive(req.keep_alive());
        res.body() = std::move(body);
        res.prepare_payload();
        if (req.method() == http::verb::head) res.body().clear();
        return res;
    }

    void handle(http::request<http::string_body>&& req) {
        if (req.method() != http::verb::get && req.method() != http::verb::head) {
            auto res = text_response(req, http::status::method_not_allowed, "method not allowed\n");
            res.set(http::field::allow, "GET, HEAD");
            return send(std::move(res));
        }
        const std::string_view path = path_of(req.target());
        if (path == "/healthz") {
            const nlohmann::json j{{"status", "ok"}, {"clients", hub_->clients()}};
            return send(text_response(req, http::status::ok, j.dump() + "\n", "application/json"));
        }
        if (hub_->cfg.api && hub_->workers != nullptr && (path.starts_with("/api/") || path == "/metrics")) {
            return handle_api(std::move(req));
        }

        std::optional<std::filesystem::path> file;
        if (auto it = hub_->cfg.files.find(std::string(path)); it != hub_->cfg.files.end()) {
            file = it->second;
        } else if (!hub_->cfg.web_root.empty()) {
            file = resolve_static_path(hub_->cfg.web_root, path);
        }
        if (!file) return send(text_response(req, http::status::not_found, "not found\n"));

        http::file_body::value_type body;
        beast::error_code ec;
        body.open(file->c_str(), beast::file_mode::scan, ec);
        if (ec) return send(text_response(req, http::status::not_found, "not found\n"));
        const auto size = body.size();

        const bool immutable = path.starts_with("/assets/");  // Vite puts a content hash in these names
        auto set_headers = [&](auto& res) {
            res.set(http::field::server, kServerName);
            res.set(http::field::content_type, mime_type(*file));
            res.set(http::field::cache_control, immutable ? "public, max-age=31536000, immutable" : "no-cache");
            res.set("X-Content-Type-Options", "nosniff");
            res.content_length(size);
            res.keep_alive(req.keep_alive());
        };
        if (req.method() == http::verb::head) {
            http::response<http::empty_body> res{http::status::ok, req.version()};
            set_headers(res);
            return send(std::move(res));
        }
        http::response<http::file_body> res{std::piecewise_construct, std::make_tuple(std::move(body)),
                                             std::make_tuple(http::status::ok, req.version())};
        set_headers(res);
        send(std::move(res));
    }

    // Runs the API handler on the worker thread, then answers on this
    // session's strand.
    void handle_api(http::request<http::string_body>&& req) {
        if (hub_->api_in_flight.fetch_add(1) >= hub_->cfg.max_api_in_flight) {
            --hub_->api_in_flight;
            return send(text_response(req, http::status::service_unavailable, "busy\n"));
        }
        ApiRequest ar;
        ar.path = std::string(path_of(req.target()));
        ar.query = parse_query(std::string(query_of(req.target())));
        auto rq = std::make_shared<http::request<http::string_body>>(std::move(req));
        net::post(*hub_->workers, [self = shared_from_this(), rq, ar = std::move(ar)] {
            std::optional<ApiResponse> r;
            try {
                r = self->hub_->cfg.api(ar);
            } catch (const std::exception& e) {
                r = ApiResponse{500, "text/plain; charset=utf-8", std::string("internal error: ") + e.what() + "\n", 0};
            }
            --self->hub_->api_in_flight;
            net::post(self->stream_.get_executor(), [self, rq, r = std::move(r)] {
                if (!r) return self->send(text_response(*rq, http::status::not_found, "not found\n"));
                auto res = text_response(*rq, static_cast<http::status>(r->status), r->body, r->content_type);
                if (r->max_age_s > 0) {
                    res.set(http::field::cache_control, "public, max-age=" + std::to_string(r->max_age_s));
                }
                self->send(std::move(res));
            });
        });
    }

    template <class Body>
    void send(http::response<Body>&& res) {
        auto sp = std::make_shared<http::response<Body>>(std::move(res));
        const bool keep_alive = sp->keep_alive();
        http::async_write(stream_, *sp,
                          [self = shared_from_this(), sp, keep_alive](beast::error_code ec, std::size_t /*bytes*/) {
                              self->on_write(keep_alive, ec);
                          });
    }

    void on_write(bool keep_alive, beast::error_code ec) {
        if (ec) return;
        if (!keep_alive) {
            std::ignore = stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
            return;
        }
        do_read();
    }

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    std::shared_ptr<Hub> hub_;
    std::optional<http::request_parser<http::string_body>> parser_;
};

}  // namespace

struct LiveServer::Impl {
    std::shared_ptr<Hub> hub;
    net::io_context ioc{1};
    net::thread_pool workers{1};
    tcp::acceptor acceptor{ioc};
    std::thread thread;
    std::uint16_t port = 0;
    bool started = false;
    bool stopped = false;

    explicit Impl(LiveServerConfig cfg) : hub(std::make_shared<Hub>(std::move(cfg))) { hub->workers = &workers; }

    void do_accept() {
        acceptor.async_accept(net::make_strand(ioc), [this](beast::error_code ec, tcp::socket socket) {
            if (ec == net::error::operation_aborted) return;
            if (!ec) std::make_shared<HttpSession>(std::move(socket), hub)->run();
            do_accept();
        });
    }
};

LiveServer::LiveServer(LiveServerConfig config) : impl_(std::make_shared<Impl>(std::move(config))) {}

LiveServer::~LiveServer() { stop(); }

void LiveServer::start() {
    auto& im = *impl_;
    if (im.started) throw std::logic_error("LiveServer already started");
    beast::error_code ec;
    const auto addr = net::ip::make_address(im.hub->cfg.address, ec);
    if (ec) throw std::runtime_error("invalid listen address '" + im.hub->cfg.address + "'");
    const tcp::endpoint ep{addr, im.hub->cfg.port};
    std::ignore = im.acceptor.open(ep.protocol(), ec);
    if (!ec) std::ignore = im.acceptor.set_option(net::socket_base::reuse_address(true), ec);
    if (!ec) std::ignore = im.acceptor.bind(ep, ec);
    if (!ec) std::ignore = im.acceptor.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        throw std::runtime_error("cannot listen on " + im.hub->cfg.address + ":" + std::to_string(im.hub->cfg.port) +
                                 ": " + ec.message());
    }
    im.port = im.acceptor.local_endpoint().port();
    im.started = true;
    im.do_accept();
    im.thread = std::thread([&im] { im.ioc.run(); });
}

void LiveServer::stop() {
    auto& im = *impl_;
    if (!im.started || im.stopped) return;
    im.stopped = true;
    im.ioc.stop();
    if (im.thread.joinable()) im.thread.join();
    // Queued API work is dropped; a handler already running finishes first.
    im.workers.stop();
    im.workers.join();
    beast::error_code ec;
    std::ignore = im.acceptor.close(ec);
}

std::uint16_t LiveServer::port() const { return impl_->port; }

std::size_t LiveServer::client_count() const { return impl_->hub->clients(); }

void LiveServer::publish(const std::string& topic, std::string message) {
    auto m = std::make_shared<const std::string>(std::move(message));
    std::vector<std::shared_ptr<WsSession>> targets;
    {
        const std::scoped_lock lock(impl_->hub->mu);
        auto& latest = impl_->hub->latest;
        auto it = std::find_if(latest.begin(), latest.end(), [&](const auto& kv) { return kv.first == topic; });
        if (it == latest.end()) {
            latest.emplace_back(topic, m);
        } else {
            it->second = m;
        }
        impl_->hub->prune_locked();
        for (const auto& w : impl_->hub->sessions) {
            if (auto s = w.lock()) targets.push_back(std::move(s));
        }
    }
    for (auto& s : targets) {
        auto ex = s->executor();
        net::post(ex, [s = std::move(s), m] { s->send(m); });
    }
}

}  // namespace sema

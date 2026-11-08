#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace sema {

struct LiveServerConfig {
    std::string address = "127.0.0.1";
    std::uint16_t port = 8080;  // 0 picks a free port (see LiveServer::port())
    // Directory with the built web map (web/dist). Empty: no static files.
    std::filesystem::path web_root;
    // Extra files served at fixed paths, e.g. {"/boundary.geojson", data/turkey_boundary.geojson}.
    std::map<std::string, std::filesystem::path> files;
    std::size_t max_clients = 500;
    // A client that falls this many messages behind is disconnected rather
    // than letting its queue grow without bound.
    std::size_t max_queued_messages = 8;
};

// HTTP + WebSocket server for the live map (Boost.Beast, one I/O thread).
//
//   GET /ws        WebSocket: on connect the client gets the latest message of
//                  every topic, in the order the topics were first published,
//                  then every new message as it is published. Clients only
//                  listen; anything they send is ignored.
//   GET /healthz   {"status":"ok","clients":N}
//   GET /<path>    static files from `web_root` and `files`
//
// publish() may be called from any thread.
class LiveServer {
public:
    explicit LiveServer(LiveServerConfig config);
    ~LiveServer();
    LiveServer(const LiveServer&) = delete;
    LiveServer& operator=(const LiveServer&) = delete;

    // Binds and starts the I/O thread. Throws std::runtime_error if the
    // address cannot be bound.
    void start();
    void stop();

    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] std::size_t client_count() const;

    // Stores `message` as the latest of `topic` and sends it to every client.
    void publish(const std::string& topic, std::string message);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace sema

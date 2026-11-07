#pragma once

#include <chrono>
#include <map>
#include <string>

namespace sema {

struct HttpRequest {
    enum class Method { Get, Post } method = Method::Get;
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    std::chrono::milliseconds timeout{20000};
};

struct HttpResponse {
    int status = 0;  // 0 means transport error (DNS, TLS, timeout...)
    std::map<std::string, std::string> headers;  // keys lower-cased
    std::string body;
    std::string transport_error;
};

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual HttpResponse send(const HttpRequest& req) = 0;
};

// libcurl implementation. One instance per thread.
class CurlTransport final : public HttpTransport {
public:
    CurlTransport();
    ~CurlTransport() override;
    CurlTransport(const CurlTransport&) = delete;
    CurlTransport& operator=(const CurlTransport&) = delete;

    HttpResponse send(const HttpRequest& req) override;

private:
    void* handle_;  // CURL*
};

std::string url_encode(const std::string& s);

}  // namespace sema

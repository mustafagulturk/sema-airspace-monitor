#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <stdexcept>

#include "sema/http.hpp"

namespace sema {
namespace {

void global_init_once() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::size_t header_cb(char* buffer, std::size_t size, std::size_t nitems, void* userdata) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    const std::size_t n = size * nitems;
    std::string line(buffer, n);
    if (line.rfind("HTTP/", 0) == 0) {
        headers->clear();  // new response (e.g. after 100-continue)
        return n;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) return n;
    std::string key = trim(line.substr(0, colon));
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    (*headers)[key] = trim(line.substr(colon + 1));
    return n;
}

struct SlistGuard {
    curl_slist* list = nullptr;
    ~SlistGuard() { curl_slist_free_all(list); }
};

}  // namespace

CurlTransport::CurlTransport() {
    global_init_once();
    handle_ = curl_easy_init();
    if (handle_ == nullptr) throw std::runtime_error("curl_easy_init failed");
}

CurlTransport::~CurlTransport() { curl_easy_cleanup(static_cast<CURL*>(handle_)); }

HttpResponse CurlTransport::send(const HttpRequest& req) {
    auto* curl = static_cast<CURL*>(handle_);
    curl_easy_reset(curl);

    HttpResponse resp;
    SlistGuard hdrs;
    for (const auto& [k, v] : req.headers) {
        std::string line = k;
        line += ": ";
        line += v;
        hdrs.list = curl_slist_append(hdrs.list, line.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(req.timeout.count()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // allow gzip
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "sema-airspace-monitor/0.1");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs.list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.headers);
    if (req.method == HttpRequest::Method::Post) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    }

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        resp.status = 0;
        resp.transport_error = curl_easy_strerror(rc);
        resp.body.clear();
        return resp;
    }
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    resp.status = static_cast<int>(code);
    return resp;
}

std::string url_encode(const std::string& s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace sema

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace sema {

// Time source + sleep, injectable for tests.
class Clock {
public:
    using time_point = std::chrono::system_clock::time_point;
    virtual ~Clock() = default;
    virtual time_point now() const = 0;
    // Returns false if the sleep was interrupted by a stop request.
    virtual bool sleep_for(std::chrono::milliseconds d) = 0;
};

class SystemClock final : public Clock {
public:
    time_point now() const override { return std::chrono::system_clock::now(); }
    bool sleep_for(std::chrono::milliseconds d) override;
    // Wakes any sleeper and makes further sleeps return immediately.
    void request_stop();
    bool stop_requested() const { return stop_.load(); }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
};

}  // namespace sema

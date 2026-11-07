#include "sema/clock.hpp"

namespace sema {

bool SystemClock::sleep_for(std::chrono::milliseconds d) {
    std::unique_lock lock(mu_);
    return !cv_.wait_for(lock, d, [this] { return stop_.load(); });
}

void SystemClock::request_stop() {
    {
        std::lock_guard lock(mu_);
        stop_.store(true);
    }
    cv_.notify_all();
}

}  // namespace sema

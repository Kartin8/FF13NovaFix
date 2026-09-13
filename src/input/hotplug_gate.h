#pragma once

#include <atomic>
#include <cstdint>

namespace novafix::input {

enum class HotplugTrigger {
    None,
    DeviceChange,
    Retry,
};

class HotplugGate {
public:
    explicit HotplugGate(std::uint64_t retryIntervalMs = 0,
                         std::uint64_t retryWindowMs = 0);

    void Reset();
    void Notify();
    void Resolve();
    HotplugTrigger Consume(std::uint64_t now);

private:
    const std::uint64_t retryIntervalMs_;
    const std::uint64_t retryWindowMs_;
    std::atomic_bool changed_{false};
    std::atomic<std::uint64_t> nextRetryAt_{0};
    std::atomic<std::uint64_t> retryUntilAt_{0};
};

} // namespace novafix::input

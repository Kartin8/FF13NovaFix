#include "input/hotplug_gate.h"

namespace novafix::input {

HotplugGate::HotplugGate(std::uint64_t retryIntervalMs,
                         std::uint64_t retryWindowMs)
    : retryIntervalMs_(retryIntervalMs),
      retryWindowMs_(retryWindowMs) {}

void HotplugGate::Reset() {
    changed_.store(false, std::memory_order_release);
    nextRetryAt_.store(0, std::memory_order_release);
    retryUntilAt_.store(0, std::memory_order_release);
}

void HotplugGate::Notify() {
    changed_.store(true, std::memory_order_release);
}

void HotplugGate::Resolve() {
    nextRetryAt_.store(0, std::memory_order_release);
    retryUntilAt_.store(0, std::memory_order_release);
}

HotplugTrigger HotplugGate::Consume(std::uint64_t now) {
    if (changed_.load(std::memory_order_acquire) &&
        changed_.exchange(false, std::memory_order_acq_rel)) {
        if (retryIntervalMs_ && retryWindowMs_) {
            nextRetryAt_.store(now + retryIntervalMs_, std::memory_order_release);
            retryUntilAt_.store(now + retryWindowMs_, std::memory_order_release);
        }
        return HotplugTrigger::DeviceChange;
    }
    const std::uint64_t retryUntil = retryUntilAt_.load(std::memory_order_acquire);
    if (retryUntil) {
        if (now > retryUntil) {
            Resolve();
        } else if (now >= nextRetryAt_.load(std::memory_order_acquire)) {
            nextRetryAt_.store(now + retryIntervalMs_, std::memory_order_release);
            return HotplugTrigger::Retry;
        }
    }
    return HotplugTrigger::None;
}

} // namespace novafix::input

#include "input/activity.h"

#include <atomic>

namespace novafix::input {
namespace {

std::atomic<InputSource> g_source{InputSource::None};

} // namespace

void NotifyActivity(InputSource source) {
    if (source == InputSource::None) return;
    if (g_source.load(std::memory_order_relaxed) == source) return;
    if (g_source.exchange(source, std::memory_order_acq_rel) == source) return;
}

InputSource LatestActivity() {
    return g_source.load(std::memory_order_acquire);
}

} // namespace novafix::input

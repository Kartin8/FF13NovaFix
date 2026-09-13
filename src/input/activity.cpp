#include "input/activity.h"

#include "diagnostics/log.h"

#include <atomic>

namespace novafix::input {
namespace {

std::atomic<InputSource> g_source{InputSource::None};

} // namespace

void NotifyActivity(InputSource source) {
    if (source == InputSource::None) return;
    if (g_source.load(std::memory_order_relaxed) == source) return;
    const InputSource previous = g_source.exchange(source, std::memory_order_acq_rel);
    if (previous == source) return;
    Log("Active input source changed: %s", InputSourceName(source));
}

InputSource LatestActivity() {
    return g_source.load(std::memory_order_acquire);
}

const char* InputSourceName(InputSource source) {
    switch (source) {
    case InputSource::None: return "none";
    case InputSource::KeyboardMouse: return "keyboard/mouse";
    case InputSource::Controller: return "controller";
    }
    return "unknown";
}

} // namespace novafix::input

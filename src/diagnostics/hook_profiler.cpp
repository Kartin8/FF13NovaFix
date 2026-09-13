#include "diagnostics/hook_profiler.h"

#include "diagnostics/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

namespace novafix::hook_profiler {
namespace {

struct Counters {
    std::atomic<std::uint64_t> calls{};
    std::atomic<std::uint64_t> totalTicks{};
    std::atomic<std::uint64_t> maximumTicks{};
    std::atomic_uint estimated{};
};

constexpr std::array<const char*, static_cast<std::size_t>(Point::Count)>
    kNames{{
        "features.updates",
        "controller.vibration-backend",
        "controller.vibration-nonzero",
        "controller.vibration-set-state",
        "input.directinput-get-state-total",
        "input.directinput-get-state-original",
        "input.directinput-controller-get-state-original",
        "input.directinput-keyboard-get-state-original",
        "input.directinput-mouse-get-state-original",
        "input.directinput-unknown-get-state-original",
        "input.directinput-controller-state-changed-original",
        "input.directinput-controller-state-unchanged-original",
        "input.directinput-controller-latency-under-0.1ms",
        "input.directinput-controller-latency-0.1-to-1ms",
        "input.directinput-controller-latency-1-to-4ms",
        "input.directinput-controller-latency-4-to-8ms",
        "input.directinput-controller-latency-8-to-16ms",
        "input.directinput-controller-latency-16ms-or-more",
        "input.directinput-controller-transient-failure",
        "input.directinput-controller-other-failure",
        "input.directinput-observe-state",
        "input.directinput-get-data-total",
        "input.directinput-get-data-original",
        "lr.prompts.refresh-total",
        "lr.prompts.refresh-original",
        "lr.prompts.guide-rebuild",
        "lr.prompts.label-lookup",
        "lr.prompts.captured-source-copy",
        "lr.prompts.label-original",
        "lr.prompts.label-bookkeeping",
        "lr.prompts.label-destroy-total",
        "lr.prompts.label-destroy-original",
        "lr.prompts.xinput-original",
        "lr.prompts.xinput-total",
        "lr.prompts.platform-query-original",
        "lr.prompts.platform-query-total",
        "lr.schemata.navigate-total",
        "lr.schemata.navigate-original",
        "lr.schemata.confirm-total",
        "lr.schemata.confirm-original",
        "lr.schemata.update-total",
        "lr.schemata.update-original",
    }};

constexpr std::size_t kBuiltinPointCount =
    static_cast<std::size_t>(Point::Count);
constexpr std::size_t kMaximumDynamicPoints = 128u;
constexpr std::size_t kMaximumDynamicNameBytes = 80u;
std::array<Counters, kBuiltinPointCount + kMaximumDynamicPoints> g_counters{};
std::array<std::array<char, kMaximumDynamicNameBytes>,
           kMaximumDynamicPoints> g_dynamicNames{};
std::atomic_size_t g_dynamicPointCount{};
SRWLOCK g_dynamicPointLock = SRWLOCK_INIT;
std::atomic<std::int64_t> g_lastReport{};

std::int64_t Frequency() {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    return frequency;
}

std::int64_t Now() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

void Record(std::size_t index, std::uint64_t ticks,
            std::uint32_t weight) {
    if (index >= g_counters.size()) return;
    Counters& counters = g_counters[index];
    counters.calls.fetch_add(weight, std::memory_order_relaxed);
    counters.totalTicks.fetch_add(
        ticks * static_cast<std::uint64_t>(weight),
        std::memory_order_relaxed);
    if (weight > 1u) {
        counters.estimated.store(1u, std::memory_order_relaxed);
    }
    std::uint64_t maximum =
        counters.maximumTicks.load(std::memory_order_relaxed);
    while (maximum < ticks &&
           !counters.maximumTicks.compare_exchange_weak(
               maximum, ticks, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

const char* PointName(std::size_t index) {
    if (index < kBuiltinPointCount) return kNames[index];
    const std::size_t dynamicIndex = index - kBuiltinPointCount;
    return dynamicIndex < g_dynamicPointCount.load(std::memory_order_acquire)
        ? g_dynamicNames[dynamicIndex].data() : "unknown";
}

} // namespace

DynamicPoint RegisterDynamicPoint(std::string_view name) {
    if (name.empty()) return {};

    AcquireSRWLockExclusive(&g_dynamicPointLock);
    std::size_t count = g_dynamicPointCount.load(std::memory_order_relaxed);
    for (std::size_t index = 0u; index < count; ++index) {
        if (name == g_dynamicNames[index].data()) {
            ReleaseSRWLockExclusive(&g_dynamicPointLock);
            return DynamicPoint{
                static_cast<std::uint16_t>(kBuiltinPointCount + index)};
        }
    }
    if (count >= kMaximumDynamicPoints) {
        ReleaseSRWLockExclusive(&g_dynamicPointLock);
        return {};
    }

    auto& destination = g_dynamicNames[count];
    const std::size_t copied = (std::min)(
        name.size(), destination.size() - 1u);
    std::memcpy(destination.data(), name.data(), copied);
    destination[copied] = '\0';
    g_dynamicPointCount.store(count + 1u, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_dynamicPointLock);
    return DynamicPoint{
        static_cast<std::uint16_t>(kBuiltinPointCount + count)};
}

void RecordMeasuredTicks(Point point, std::uint64_t ticks) {
    Record(static_cast<std::size_t>(point), ticks, 1u);
}

Scope::Scope(Point point)
    : index_(static_cast<std::uint16_t>(point)) {
    active_ = IsLogEnabled(LogLevel::Debug) && Frequency() > 0;
    if (active_) start_ = Now();
}

Scope::Scope(Point point, bool selected, std::uint32_t weight)
    : index_(static_cast<std::uint16_t>(point)), weight_(weight) {
    active_ = selected && weight_ != 0u &&
        IsLogEnabled(LogLevel::Debug) && Frequency() > 0;
    if (active_) start_ = Now();
}

Scope::Scope(DynamicPoint point) : index_(point.index) {
    if (index_ == DynamicPoint::kInvalid) return;
    active_ = IsLogEnabled(LogLevel::Debug) && Frequency() > 0;
    if (active_) start_ = Now();
}

Scope::Scope(DynamicPoint point, bool selected, std::uint32_t weight)
    : index_(point.index), weight_(weight) {
    if (index_ == DynamicPoint::kInvalid) return;
    active_ = selected && weight_ != 0u &&
        IsLogEnabled(LogLevel::Debug) && Frequency() > 0;
    if (active_) start_ = Now();
}

Scope::~Scope() {
    Stop();
}

void Scope::Stop() {
    if (!active_) return;
    const std::int64_t elapsed = Now() - start_;
    if (elapsed >= 0) {
        Record(index_, static_cast<std::uint64_t>(elapsed), weight_);
    }
    active_ = false;
}

void ReportIfDue() {
    if (!IsLogEnabled(LogLevel::Debug)) return;
    const std::int64_t frequency = Frequency();
    if (frequency <= 0) return;

    const std::int64_t now = Now();
    std::int64_t previous = g_lastReport.load(std::memory_order_relaxed);
    if (!previous) {
        g_lastReport.compare_exchange_strong(
            previous, now, std::memory_order_relaxed);
        return;
    }
    if (now - previous < frequency * 10) return;
    if (!g_lastReport.compare_exchange_strong(
            previous, now, std::memory_order_relaxed)) {
        return;
    }

    const double windowMilliseconds =
        static_cast<double>(now - previous) * 1000.0 /
        static_cast<double>(frequency);
    const std::size_t activePointCount = kBuiltinPointCount +
        g_dynamicPointCount.load(std::memory_order_acquire);
    for (std::size_t index = 0u; index < activePointCount; ++index) {
        Counters& counters = g_counters[index];
        const std::uint64_t calls =
            counters.calls.exchange(0u, std::memory_order_relaxed);
        const std::uint64_t total =
            counters.totalTicks.exchange(0u, std::memory_order_relaxed);
        const std::uint64_t maximum =
            counters.maximumTicks.exchange(0u, std::memory_order_relaxed);
        const unsigned estimated =
            counters.estimated.exchange(0u, std::memory_order_relaxed);
        if (!calls) continue;

        const double totalMilliseconds =
            static_cast<double>(total) * 1000.0 /
            static_cast<double>(frequency);
        const double maximumMilliseconds =
            static_cast<double>(maximum) * 1000.0 /
            static_cast<double>(frequency);
        const double averageMicroseconds =
            totalMilliseconds * 1000.0 / static_cast<double>(calls);
        const double windowPercent = windowMilliseconds > 0.0
            ? totalMilliseconds * 100.0 / windowMilliseconds : 0.0;
        LogDebug("Hook timing: name=%s calls=%llu total=%.3fms avg=%.3fus max=%.3fms window=%.2f%% estimated=%u",
                 PointName(index), static_cast<unsigned long long>(calls),
                 totalMilliseconds, averageMicroseconds,
                 maximumMilliseconds, windowPercent, estimated);
    }
}

} // namespace novafix::hook_profiler

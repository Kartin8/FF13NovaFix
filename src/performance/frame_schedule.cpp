#include "performance/frame_schedule.h"

#include <cmath>

namespace novafix::performance {

FrameSchedule::FrameSchedule(std::int64_t frequency) : frequency_(frequency) {}

void FrameSchedule::SetFrequency(std::int64_t frequency) {
    frequency_ = frequency;
    Reset();
}

void FrameSchedule::Reset() {
    nextDeadline_ = 0.0;
    rateNumerator_ = 0;
    rateDenominator_ = 0;
}

std::optional<std::int64_t> FrameSchedule::NextDeadline(
    std::int64_t now, unsigned framesPerSecond) {
    return NextDeadlineImpl(now, framesPerSecond, 1u, false);
}

std::optional<std::int64_t> FrameSchedule::NextAlignedDeadline(
    std::int64_t now, std::uint64_t rateNumerator,
    std::uint64_t rateDenominator) {
    return NextDeadlineImpl(
        now, rateNumerator, rateDenominator, true);
}

std::optional<std::int64_t> FrameSchedule::NextDeadlineImpl(
    std::int64_t now, std::uint64_t rateNumerator,
    std::uint64_t rateDenominator, bool alignAfterMiss) {
    if (frequency_ <= 0 || rateNumerator == 0u || rateDenominator == 0u) {
        Reset();
        return std::nullopt;
    }

    const double period = static_cast<double>(frequency_) *
                          static_cast<double>(rateDenominator) /
                          static_cast<double>(rateNumerator);
    const double current = static_cast<double>(now);
    const bool missedDeadline = current > nextDeadline_;
    const bool clockMovedBackward =
        current + period * 4.0 < nextDeadline_;
    if (rateNumerator_ != rateNumerator ||
        rateDenominator_ != rateDenominator || nextDeadline_ <= 0.0 ||
        clockMovedBackward || (missedDeadline && !alignAfterMiss)) {
        rateNumerator_ = rateNumerator;
        rateDenominator_ = rateDenominator;
        nextDeadline_ = current + period;
        return std::nullopt;
    }

    if (missedDeadline) {
        const double missedPeriods =
            std::floor((current - nextDeadline_) / period) + 1.0;
        nextDeadline_ += missedPeriods * period;
        // The frame workload has already exceeded the target interval.  Keep
        // the absolute phase for recovery, but never add another wait here:
        // doing so quantizes sub-target performance down to a lower divisor
        // (for example, ~52 FPS to ~45 FPS against a 144-Hz grid)
        return std::nullopt;
    }

    const std::int64_t deadline =
        static_cast<std::int64_t>(std::llround(nextDeadline_));
    nextDeadline_ += period;
    return deadline;
}

} // namespace novafix::performance

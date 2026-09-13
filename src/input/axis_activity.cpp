#include "input/axis_activity.h"

#include <algorithm>
#include <cstdlib>

namespace novafix::input {
namespace {

constexpr std::int64_t kSmallAxisMinimum = 0;
constexpr std::int64_t kSmallAxisMaximum = 255;
constexpr std::int64_t kSmallAxisHalfRange = 128;
constexpr std::int64_t kLargeAxisHalfRange = 32767;

std::int64_t Distance(std::int32_t first, std::int32_t second) {
    return std::llabs(static_cast<long long>(first) - static_cast<long long>(second));
}

} // namespace

void AxisActivityTracker::Calibrate(std::span<const std::int32_t> axes) {
    // XIII requests a 0..255 axis range instead of the usual 0..65535
    const bool compactRange = std::all_of(axes.begin(), axes.end(), [](std::int32_t value) {
        return value >= kSmallAxisMinimum && value <= kSmallAxisMaximum;
    });
    std::int64_t halfRange = compactRange ? kSmallAxisHalfRange : kLargeAxisHalfRange;
    if (!compactRange) {
        for (const std::int32_t value : axes) {
            halfRange = (std::max)(halfRange, std::llabs(static_cast<long long>(value)));
        }
    }

    activationDeadzone_ = (std::max)(std::int64_t{8}, halfRange / 4);
    releaseDeadzone_ = (std::max)(std::int64_t{4}, halfRange / 8);
    movementStep_ = releaseDeadzone_;
}

bool AxisActivityTracker::Observe(std::span<const std::int32_t> axes) {
    const std::size_t count = (std::min)(axes.size(), kMaxAxes);
    if (!ready_ || count != axisCount_) {
        Reset();
        axisCount_ = count;
        std::copy_n(axes.begin(), count, neutral_.begin());
        std::copy_n(axes.begin(), count, lastReported_.begin());
        Calibrate(axes.first(count));
        ready_ = true;
        return false;
    }

    bool active = false;
    for (std::size_t index = 0; index < count; ++index) {
        const std::int64_t fromNeutral = Distance(axes[index], neutral_[index]);
        if (!engaged_[index]) {
            if (fromNeutral >= activationDeadzone_) {
                engaged_[index] = true;
                lastReported_[index] = axes[index];
                active = true;
            }
            continue;
        }

        if (fromNeutral <= releaseDeadzone_) {
            engaged_[index] = false;
            lastReported_[index] = axes[index];
        } else if (Distance(axes[index], lastReported_[index]) >= movementStep_) {
            lastReported_[index] = axes[index];
            active = true;
        }
    }
    return active;
}

void AxisActivityTracker::Reset() {
    neutral_.fill(0);
    lastReported_.fill(0);
    engaged_.fill(false);
    axisCount_ = 0;
    activationDeadzone_ = 0;
    releaseDeadzone_ = 0;
    movementStep_ = 0;
    ready_ = false;
}

} // namespace novafix::input

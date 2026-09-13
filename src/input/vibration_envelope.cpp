#include "input/vibration_envelope.h"

#include <algorithm>

namespace novafix::input {

MotorEnvelopeOutput VibrationEnvelope::Observe(std::uint16_t low,
                                                std::uint16_t high,
                                                std::uint64_t nowMs) {
    if (low != 0 || high != 0) {
        if (nowMs >= untilMs_) {
            low_ = low;
            high_ = high;
        } else {
            low_ = std::max(low_, low);
            high_ = std::max(high_, high);
        }
        untilMs_ = nowMs + kReleaseTailMs;
    }
    return Tick(nowMs);
}

MotorEnvelopeOutput VibrationEnvelope::Tick(std::uint64_t nowMs) {
    if (untilMs_ == 0 || nowMs >= untilMs_) {
        Reset();
        return {};
    }
    return {low_, high_, true};
}

void VibrationEnvelope::Reset() {
    low_ = 0;
    high_ = 0;
    untilMs_ = 0;
}

} // namespace novafix::input

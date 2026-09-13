#pragma once

#include <cstdint>

namespace novafix::input {

struct MotorEnvelopeOutput {
    std::uint16_t low{};
    std::uint16_t high{};
    bool active{};
};

class VibrationEnvelope {
public:
    static constexpr std::uint64_t kReleaseTailMs = 140;

    MotorEnvelopeOutput Observe(std::uint16_t low, std::uint16_t high,
                                std::uint64_t nowMs);
    MotorEnvelopeOutput Tick(std::uint64_t nowMs);
    void Reset();

private:
    std::uint16_t low_{};
    std::uint16_t high_{};
    std::uint64_t untilMs_{};
};

} // namespace novafix::input

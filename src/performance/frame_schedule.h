#pragma once

#include <cstdint>
#include <optional>

namespace novafix::performance {

class FrameSchedule {
public:
    explicit FrameSchedule(std::int64_t frequency = 0);

    void SetFrequency(std::int64_t frequency);
    void Reset();
    std::optional<std::int64_t> NextDeadline(std::int64_t now, unsigned framesPerSecond);
    std::optional<std::int64_t> NextAlignedDeadline(
        std::int64_t now, std::uint64_t rateNumerator,
        std::uint64_t rateDenominator = 1u);

private:
    std::optional<std::int64_t> NextDeadlineImpl(
        std::int64_t now, std::uint64_t rateNumerator,
        std::uint64_t rateDenominator, bool alignAfterMiss);

    std::int64_t frequency_{};
    double nextDeadline_{};
    std::uint64_t rateNumerator_{};
    std::uint64_t rateDenominator_{};
};

} // namespace novafix::performance

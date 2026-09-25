#pragma once

#include <cstddef>

namespace novafix::game::ff13_ui {

class ScopedMinimapTilt {
public:
    ScopedMinimapTilt(void* product, bool enabled)
        : product_(enabled ? product : nullptr) {
        if (!product_) return;
        original_ = Tilt();
        Tilt() = 0.0f;
    }

    ~ScopedMinimapTilt() {
        if (product_) Tilt() = original_;
    }

    ScopedMinimapTilt(const ScopedMinimapTilt&) = delete;
    ScopedMinimapTilt& operator=(const ScopedMinimapTilt&) = delete;

private:
    float& Tilt() const {
        constexpr std::size_t kTiltOffset = 36u * sizeof(float);
        return *reinterpret_cast<float*>(
            static_cast<std::byte*>(product_) + kTiltOffset);
    }

    void* product_{};
    float original_{};
};

} // namespace novafix::game::ff13_ui

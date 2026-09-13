#pragma once

namespace novafix::game::scissor_fix {

class ScopedBypass final {
public:
    ScopedBypass();
    ~ScopedBypass();

    ScopedBypass(const ScopedBypass&) = delete;
    ScopedBypass& operator=(const ScopedBypass&) = delete;
};

void Initialize();
void Pump();
bool IsActive();
bool ShouldTransformCurrentThread();

} // namespace novafix::game::scissor_fix

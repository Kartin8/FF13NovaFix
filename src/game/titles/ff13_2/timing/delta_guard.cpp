#include "game/titles/ff13_2/timing/delta_guard.h"

#include "common/guarded_call.h"

#include <windows.h>

#include <cstdint>

namespace novafix::game::xiii2_delta_guard {
namespace {

bool ProtectedRead(const std::uint32_t* address, std::uint32_t& value) noexcept {
#if defined(_MSC_VER)
    __try {
        value = *address;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return guarded_call::Run([&] {
        value = *address;
    });
#endif
}

bool ProtectedWrite(std::uint32_t* address, std::uint32_t value) noexcept {
#if defined(_MSC_VER)
    __try {
        *address = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return guarded_call::Run([&] {
        *address = value;
    });
#endif
}

} // namespace

bool TryRead(const std::uint32_t* result,
             const std::uint32_t* expectedOutput,
             std::uint32_t& value) noexcept {
    if (!result || result != expectedOutput) {
        return false;
    }
    return ProtectedRead(result, value);
}

bool TryWrite(std::uint32_t* result,
              const std::uint32_t* expectedOutput,
              std::uint32_t value) noexcept {
    if (!result || result != expectedOutput) {
        return false;
    }
    return ProtectedWrite(result, value);
}

} // namespace novafix::game::xiii2_delta_guard

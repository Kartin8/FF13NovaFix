#pragma once

#include <memory>
#include <type_traits>
#include <utility>

extern "C" int __cdecl NovaFixInvokeGuarded(
    void (__cdecl* callback)(void*), void* context,
    unsigned long* exceptionCode);

namespace novafix::guarded_call {

template <typename Function>
bool Run(Function&& function, unsigned long* exceptionCode = nullptr) noexcept {
    using StoredFunction = std::remove_reference_t<Function>;
    auto thunk = [](void* context) {
        (*static_cast<StoredFunction*>(context))();
    };

    unsigned long ignored{};
    return NovaFixInvokeGuarded(thunk, std::addressof(function),
                               exceptionCode ? exceptionCode : &ignored) != 0;
}

template <typename Result, typename Function>
Result ResultOr(Function&& function, Result fallback,
                unsigned long* exceptionCode = nullptr) noexcept {
    Result result = std::move(fallback);
    Run([&] { result = function(); }, exceptionCode);
    return result;
}

} // namespace novafix::guarded_call

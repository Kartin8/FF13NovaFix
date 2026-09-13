extern "C" unsigned long __cdecl _exception_code();

namespace {
constexpr int kExecuteHandler = 1;
}

extern "C" int __cdecl NovaFixInvokeGuarded(
    void (__cdecl* callback)(void*), void* context,
    unsigned long* exceptionCode) {
    __try {
        callback(context);
        return 1;
    } __except ((*exceptionCode = _exception_code(), kExecuteHandler)) {
        return 0;
    }
}

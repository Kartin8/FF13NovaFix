#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace novafix::game {
class ImageView;
}

namespace novafix::game::startup_module_notification {

struct Backend {
    bool (*install)(const ImageView& image){};
    const char* deferredLog{};
};

// Retains the backend only for the bounded CEG installation retry
void Queue(std::wstring message, std::wstring signature,
           const Backend& backend);

// Device creation is the primary runtime-ready signal; Present is a bounded
// fallback for protected pages which become available slightly later
void RetryDeferredInstallation();

bool Pending();
bool BeginOpening(std::wstring& message);
void FinishOpening(void* product, void (*onOpened)());
void Clear();
bool IsTrackedProduct(void* product);
bool CompleteProduct(void* product, std::wstring& signature);
bool MarkProductClosed(void* product);
std::wstring CompletePending();

bool NativeMessageText(std::wstring_view source, unsigned appId,
                       std::vector<char>& result);
bool ConfirmHeld();
void Persist(std::wstring signature, const char* title);

} // namespace novafix::game::startup_module_notification

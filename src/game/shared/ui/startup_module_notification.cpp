#include "game/shared/ui/startup_module_notification.h"

#include "compat/addons/native_notifications.h"
#include "compat/addons/native_notification_text.h"
#include "diagnostics/log.h"
#include "game/core/image_view.h"
#include "input/xinput_backend.h"
#include "settings/game_settings.h"
#include "settings/performance_settings.h"

#include <windows.h>
#include <xinput.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace novafix::game::startup_module_notification {
namespace {

constexpr ULONGLONG kDeferredInstallWindowMs = 30'000u;

struct PendingNotification {
    std::wstring message;
    std::wstring signature;
    void* product{};
    bool opening{};
    bool opened{};
};

std::mutex g_mutex;
PendingNotification g_pending;
std::atomic_bool g_pendingActive{};
std::atomic_bool g_installPending{};
std::atomic_bool g_installed{};
std::atomic_uint64_t g_installDeadline{};
std::atomic<const Backend*> g_backend{};

std::wstring SteamLanguage(unsigned appId) {
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!length || length >= std::size(executable)) return L"english";

    std::filesystem::path cursor =
        std::filesystem::path(std::wstring_view(executable, length))
            .parent_path();
    for (unsigned depth = 0; depth != 10 && !cursor.empty(); ++depth) {
        const std::filesystem::path manifest = cursor /
            (L"appmanifest_" + std::to_wstring(appId) + L".acf");
        std::ifstream stream(manifest, std::ios::binary);
        if (stream) {
            const std::string contents(
                (std::istreambuf_iterator<char>(stream)),
                std::istreambuf_iterator<char>());
            const std::string key = "\"language\"";
            const std::size_t keyAt = contents.find(key);
            if (keyAt != std::string::npos) {
                const std::size_t valueBegin = contents.find(
                    '"', keyAt + key.size());
                const std::size_t valueEnd = valueBegin == std::string::npos
                    ? std::string::npos
                    : contents.find('"', valueBegin + 1u);
                if (valueEnd != std::string::npos) {
                    const std::string value = contents.substr(
                        valueBegin + 1u, valueEnd - valueBegin - 1u);
                    return std::wstring(value.begin(), value.end());
                }
            }
        }
        cursor = cursor.parent_path();
    }
    return L"english";
}

struct LocalizedHeading {
    std::wstring_view language;
    std::wstring_view text;
    UINT codePage;
};

constexpr std::array<LocalizedHeading, 8> kHeadings{{
    {L"japanese", L"\x6B63\x5E38\x306B\x8AAD\x307F\x8FBC\x307E\x308C\x305F\x30A2\x30C9\x30AA\x30F3:", 932u},
    {L"english", L"Successfully loaded add-ons:", 1252u},
    {L"italian", L"Componenti aggiuntivi caricati correttamente:", 1252u},
    {L"german", L"Erfolgreich geladene Add-ons:", 1252u},
    {L"french", L"Extensions chargees avec succes :", 1252u},
    {L"spanish", L"Complementos cargados correctamente:", 1252u},
    {L"koreana", L"\xC131\xACF5\xC801\xC73C\xB85C \xBD88\xB7EC\xC628 \xC560\xB4DC\xC628:", 949u},
    {L"tchinese", L"\x6210\x529F\x8F09\x5165\x7684\x9644\x52A0\x5143\x4EF6\xFF1A", 950u},
}};

const LocalizedHeading& HeadingFor(unsigned appId) {
    std::wstring language = SteamLanguage(appId);
    if (!settings::OverridesGameLanguage()) {
        for (const auto& candidate : kHeadings) {
            if (_wcsicmp(std::wstring(candidate.language).c_str(),
                         language.c_str()) == 0) {
                return candidate;
            }
        }
        return kHeadings[1];
    }
    switch (settings::ActiveGame().interfaceLanguage) {
    case settings::InterfaceLanguage::English:
        language = L"english";
        break;
    case settings::InterfaceLanguage::Japanese:
        language = L"japanese";
        break;
    case settings::InterfaceLanguage::German:
        language = L"german";
        break;
    case settings::InterfaceLanguage::French:
        language = L"french";
        break;
    case settings::InterfaceLanguage::Italian:
        language = L"italian";
        break;
    case settings::InterfaceLanguage::Spanish:
        language = L"spanish";
        break;
    case settings::InterfaceLanguage::Korean:
        language = L"koreana";
        break;
    case settings::InterfaceLanguage::TraditionalChinese:
        language = L"tchinese";
        break;
    default:
        language = L"english";
        break;
    }
    for (const auto& candidate : kHeadings) {
        if (_wcsicmp(std::wstring(candidate.language).c_str(),
                     language.c_str()) == 0) {
            return candidate;
        }
    }
    return kHeadings[1];
}

bool Encode(std::wstring_view source, UINT codePage,
            std::vector<char>& result) {
    result.clear();
    result.reserve(source.size() + 1u);
    std::size_t offset{};
    while (offset < source.size()) {
        std::size_t glyphAt = offset;
        while (glyphAt < source.size() &&
               !compat::native_notification_text::IsGlyph(
                   source[glyphAt])) {
            ++glyphAt;
        }
        if (glyphAt != offset) {
            const int characters = static_cast<int>(glyphAt - offset);
            const int bytes = WideCharToMultiByte(
                codePage, 0, source.data() + offset, characters,
                nullptr, 0, "?", nullptr);
            if (bytes <= 0 || result.size() +
                    static_cast<std::size_t>(bytes) > 8192u) {
                return false;
            }
            const std::size_t writeAt = result.size();
            result.resize(writeAt + static_cast<std::size_t>(bytes));
            if (WideCharToMultiByte(
                    codePage, 0, source.data() + offset, characters,
                    result.data() + writeAt, bytes, "?", nullptr) != bytes) {
                return false;
            }
        }
        if (glyphAt == source.size()) break;
        if (result.size() + 2u > 8192u) return false;
        const auto encoded =
            compat::native_notification_text::EncodeGlyph(source[glyphAt]);
        result.insert(result.end(), encoded.begin(), encoded.end());
        offset = glyphAt + 1u;
    }
    result.push_back('\0');
    return result.size() > 1u;
}

// g_mutex must be held by the caller
void ClearLocked() {
    g_pending = {};
    g_pendingActive.store(false, std::memory_order_release);
}

} // namespace

void EnsureInstalled(const Backend& backend) {
    if (!backend.install) return;
    if (g_installed.load(std::memory_order_acquire)) return;
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) return;

    g_backend.store(&backend, std::memory_order_release);
    if (backend.install(*image)) {
        g_installed.store(true, std::memory_order_release);
        g_installPending.store(false, std::memory_order_release);
        g_installDeadline.store(0u, std::memory_order_release);
        return;
    }

    g_installDeadline.store(
        GetTickCount64() + kDeferredInstallWindowMs,
        std::memory_order_release);
    const bool alreadyPending =
        g_installPending.exchange(true, std::memory_order_acq_rel);
    if (!alreadyPending && backend.deferredLog) {
        Log("%s", backend.deferredLog);
    }
}

void Queue(std::wstring message, std::wstring signature,
           const Backend& backend) {
    if (!backend.install) return;

    {
        std::scoped_lock lock(g_mutex);
        if (!g_pending.message.empty()) return;
        g_pending.message = std::move(message);
        g_pending.signature = std::move(signature);
        g_pendingActive.store(true, std::memory_order_release);
    }
    EnsureInstalled(backend);
}

void RetryDeferredInstallation() {
    if (!g_installPending.load(std::memory_order_acquire)) return;

    const ULONGLONG deadline =
        g_installDeadline.load(std::memory_order_acquire);
    if (deadline != 0u && GetTickCount64() >= deadline) {
        g_installPending.store(false, std::memory_order_release);
        g_installDeadline.store(0u, std::memory_order_release);
        Clear();
        compat::native_notifications::Configure(false, false);
        LogWarning("Native notification hooks disabled: CEG runtime contract remained unavailable for %llu ms",
                   static_cast<unsigned long long>(
                       kDeferredInstallWindowMs));
        return;
    }

    if (g_installed.load(std::memory_order_acquire)) {
        g_installPending.store(false, std::memory_order_release);
        g_installDeadline.store(0u, std::memory_order_release);
        return;
    }

    const Backend* const backend =
        g_backend.load(std::memory_order_acquire);
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!backend || !backend->install || !image ||
        !backend->install(*image)) {
        return;
    }

    g_installed.store(true, std::memory_order_release);
    g_installPending.store(false, std::memory_order_release);
    g_installDeadline.store(0u, std::memory_order_release);
}

bool Pending() {
    return g_pendingActive.load(std::memory_order_acquire);
}

bool BeginOpening(std::wstring& message) {
    if (!Pending()) return false;
    std::scoped_lock lock(g_mutex);
    if (g_pending.message.empty() || g_pending.opening ||
        g_pending.opened) {
        return false;
    }
    g_pending.opening = true;
    message = g_pending.message;
    return true;
}

void FinishOpening(void* product, void (*onOpened)()) {
    std::scoped_lock lock(g_mutex);
    g_pending.opening = false;
    if (!product) return;
    g_pending.product = product;
    g_pending.opened = true;
    if (onOpened) onOpened();
}

void Clear() {
    std::scoped_lock lock(g_mutex);
    ClearLocked();
}

bool IsTrackedProduct(void* product) {
    if (!product || !Pending()) return false;
    std::scoped_lock lock(g_mutex);
    return g_pending.opened && g_pending.product == product;
}

bool CompleteProduct(void* product, std::wstring& signature) {
    if (!product || !Pending()) return false;
    std::scoped_lock lock(g_mutex);
    if (!g_pending.opened || g_pending.product != product) return false;
    signature = std::move(g_pending.signature);
    ClearLocked();
    return true;
}

bool MarkProductClosed(void* product) {
    if (!product || !Pending()) return false;
    std::scoped_lock lock(g_mutex);
    if (!g_pending.opened || g_pending.product != product) return false;
    g_pending.product = nullptr;
    g_pending.opened = false;
    return true;
}

std::wstring CompletePending() {
    std::scoped_lock lock(g_mutex);
    std::wstring signature = std::move(g_pending.signature);
    ClearLocked();
    return signature;
}

bool NativeMessageText(std::wstring_view source, unsigned appId,
                       std::vector<char>& result) {
    const LocalizedHeading& selected = HeadingFor(appId);
    std::wstring localized(selected.text);
    const std::size_t details = source.find(L'\n');
    if (details != std::wstring_view::npos) {
        localized.append(source.substr(details));
    }
    return Encode(localized, selected.codePage, result);
}

bool NativeText(std::wstring_view source, unsigned appId,
                std::vector<char>& result) {
    if (source.empty()) return false;
    return Encode(source, HeadingFor(appId).codePage, result);
}

bool ConfirmHeld() {
    if ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0) return true;
    for (unsigned index = 0; index != 4; ++index) {
        std::uint16_t buttons = 0;
        if (input::xinput::GetButtons(index, buttons) &&
            (buttons & XINPUT_GAMEPAD_A) != 0) {
            return true;
        }
    }
    return false;
}

void Persist(std::wstring signature, const char* title) {
    if (!signature.empty() &&
        !settings::SaveLoadedModulesNotificationSignature(signature)) {
        LogWarning("%s loaded-module signature could not be persisted",
                   title);
    }
}

} // namespace novafix::game::startup_module_notification

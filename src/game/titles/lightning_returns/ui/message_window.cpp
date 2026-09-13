#include "game/titles/lightning_returns/ui/message_window.h"

#include "diagnostics/log.h"
#include "game/core/image_view.h"
#include "patch/memory_access.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace novafix::game::lr_message_window {
namespace {

constexpr std::uint32_t kCreateSystemMessageRva = 0x003C1500u;
constexpr std::uint32_t kSetGuiLayerRva = 0x0044D540u;
constexpr std::uint32_t kGameLanguageRva = 0x00353E00u;
constexpr std::uint32_t kGameWideToTextRva = 0x003BE2C0u;
constexpr short kMissingGameGlyph = 0x25A1;

using CreateSystemMessageFn = void* (__cdecl*)(const char*, void*, int, int);
using SetGuiLayerFn = void (__thiscall*)(void*, int);
using GameLanguageFn = int (__cdecl*)();
using GameWideToTextFn = int (__cdecl*)(char*, const wchar_t*, short);

struct NativeApi {
    CreateSystemMessageFn createSystemMessage{};
    SetGuiLayerFn setGuiLayer{};
    GameLanguageFn gameLanguage{};
    GameWideToTextFn gameWideToText{};
};

std::mutex g_apiMutex;
NativeApi g_api;

NativeApi Api() {
    std::scoped_lock lock(g_apiMutex);
    return g_api;
}

bool Complete(const NativeApi& api) {
    return api.createSystemMessage && api.setGuiLayer &&
           api.gameLanguage && api.gameWideToText;
}

} // namespace

bool Bind(const ImageView& image) {
    std::scoped_lock lock(g_apiMutex);
    if (Complete(g_api)) return true;

    const auto createSystemMessage = image.At(kCreateSystemMessageRva, 1u);
    const auto setGuiLayer = image.At(kSetGuiLayerRva, 1u);
    const auto gameLanguage = image.At(kGameLanguageRva, 1u);
    const auto gameWideToText = image.At(kGameWideToTextRva, 1u);
    if (!createSystemMessage || !setGuiLayer || !gameLanguage ||
        !gameWideToText) {
        return false;
    }

    g_api = {
        reinterpret_cast<CreateSystemMessageFn>(
            const_cast<std::byte*>(createSystemMessage->data())),
        reinterpret_cast<SetGuiLayerFn>(
            const_cast<std::byte*>(setGuiLayer->data())),
        reinterpret_cast<GameLanguageFn>(
            const_cast<std::byte*>(gameLanguage->data())),
        reinterpret_cast<GameWideToTextFn>(
            const_cast<std::byte*>(gameWideToText->data())),
    };
    return true;
}

bool Available() {
    return Complete(Api());
}

int Language() {
    const NativeApi api = Api();
    return api.gameLanguage ? api.gameLanguage() : 1;
}

bool AppendText(std::vector<char>& destination, std::wstring_view source,
                std::size_t maximumBytes) {
    const NativeApi api = Api();
    if (!api.gameWideToText) return false;

    const std::wstring stable(source);
    const int bytes = api.gameWideToText(
        nullptr, stable.c_str(), kMissingGameGlyph);
    if (bytes < 0 || static_cast<std::size_t>(bytes) > maximumBytes) {
        return false;
    }

    const std::size_t offset = destination.size();
    destination.resize(offset + static_cast<std::size_t>(bytes) + 1u);
    const bool converted = bytes == 0 ||
        api.gameWideToText(destination.data() + offset, stable.c_str(),
                           kMissingGameGlyph) == bytes;
    destination.resize(offset + static_cast<std::size_t>(bytes));
    return converted;
}

bool ConvertText(std::wstring_view source, std::vector<char>& text) {
    text.clear();
    if (!AppendText(text, source, 8192u)) return false;
    text.push_back('\0');
    return true;
}

void* Open(const char* message, const char* purpose, bool logFailure) {
    const NativeApi api = Api();
    if (!message || !*message || !Complete(api)) return nullptr;

    void* const product =
        api.createSystemMessage(message, nullptr, 0, 0);
    if (!product || !patch::memory::IsReadable(product, 60u)) {
        if (logFailure) {
            Log("LR %s native status product is unavailable",
                purpose);
        }
        return nullptr;
    }

    auto* const bytes = static_cast<std::byte*>(product);
    auto* const flags = reinterpret_cast<std::uint32_t*>(bytes + 56u);
    *flags |= 8u;
    api.setGuiLayer(product, 2);

    void** const vtable = *static_cast<void***>(product);
    if (!vtable ||
        !patch::memory::IsReadable(vtable, 80u * sizeof(void*))) {
        if (logFailure) {
            Log("LR %s native status vtable is unavailable",
                purpose);
        }
        return nullptr;
    }

    using OpenGuiProductFn = void (__thiscall*)(void*, int, int);
    const auto open = reinterpret_cast<OpenGuiProductFn>(vtable[79]);
    if (!open) {
        if (logFailure) {
            Log("LR %s native status opener is unavailable",
                purpose);
        }
        return nullptr;
    }

    *flags |= 0x80u;
    open(product, 0, 1);
    return product;
}

bool Close(void* product) {
    if (!product || !patch::memory::IsReadable(product, sizeof(void*))) {
        return false;
    }
    void** const vtable = *static_cast<void***>(product);
    if (!vtable ||
        !patch::memory::IsReadable(vtable, 81u * sizeof(void*))) {
        return false;
    }

    using CloseGuiProductFn = int (__thiscall*)(void*, int);
    const auto close = reinterpret_cast<CloseGuiProductFn>(vtable[80]);
    if (!close) return false;
    close(product, 0);
    return true;
}

} // namespace novafix::game::lr_message_window

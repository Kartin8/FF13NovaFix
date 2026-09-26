#include "game/titles/lightning_returns/ui/message_window.h"

#include "compat/addons/native_notification_text.h"
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
constexpr std::uint32_t kCreateChoiceMessageRva = 0x003A5480u;
constexpr std::uint32_t kProductLookupRva = 0x003A5360u;
constexpr std::uint32_t kSelectChoiceRva = 0x0043F0E0u;
constexpr std::uint32_t kGuiManagerSlotRva = 0x04CF19F4u;
constexpr std::uint32_t kAlignMessageRva = 0x00441500u;
constexpr std::uint32_t kSetGuiLayerRva = 0x0044D540u;
constexpr std::uint32_t kGameLanguageRva = 0x00353E00u;
constexpr std::uint32_t kGameWideToTextRva = 0x003BE2C0u;
constexpr short kMissingGameGlyph = 0x25A1;

using CreateSystemMessageFn = void* (__cdecl*)(const char*, void*, int, int);
using CreateChoiceMessageFn = int (__thiscall*)(
    void*, int, int, int, int, const char*, int, int, int, int, int, int,
    int, int);
using ProductLookupFn = void* (__thiscall*)(void*, int, int);
using SelectChoiceFn = int (__thiscall*)(void*, int);
using AlignMessageFn = int (__thiscall*)(void*, int, int);
using SetGuiLayerFn = void (__thiscall*)(void*, int);
using GameLanguageFn = int (__cdecl*)();
using GameWideToTextFn = int (__cdecl*)(char*, const wchar_t*, short);

struct NativeApi {
    CreateSystemMessageFn createSystemMessage{};
    CreateChoiceMessageFn createChoiceMessage{};
    ProductLookupFn productLookup{};
    SelectChoiceFn selectChoice{};
    void** guiManagerSlot{};
    AlignMessageFn alignMessage{};
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
    return api.createSystemMessage && api.createChoiceMessage &&
           api.productLookup && api.selectChoice && api.guiManagerSlot &&
           api.alignMessage &&
           api.setGuiLayer &&
           api.gameLanguage && api.gameWideToText;
}

} // namespace

bool Bind(const ImageView& image) {
    std::scoped_lock lock(g_apiMutex);
    if (Complete(g_api)) return true;

    const auto createSystemMessage = image.At(kCreateSystemMessageRva, 1u);
    const auto createChoiceMessage = image.At(kCreateChoiceMessageRva, 1u);
    const auto productLookup = image.At(kProductLookupRva, 1u);
    const auto selectChoice = image.At(kSelectChoiceRva, 1u);
    const auto guiManagerSlot = image.At(kGuiManagerSlotRva, sizeof(void*));
    const auto alignMessage = image.At(kAlignMessageRva, 1u);
    const auto setGuiLayer = image.At(kSetGuiLayerRva, 1u);
    const auto gameLanguage = image.At(kGameLanguageRva, 1u);
    const auto gameWideToText = image.At(kGameWideToTextRva, 1u);
    if (!createSystemMessage || !createChoiceMessage || !productLookup ||
        !selectChoice || !guiManagerSlot || !alignMessage || !setGuiLayer ||
        !gameLanguage || !gameWideToText) {
        return false;
    }

    g_api = {
        reinterpret_cast<CreateSystemMessageFn>(
            const_cast<std::byte*>(createSystemMessage->data())),
        reinterpret_cast<CreateChoiceMessageFn>(
            const_cast<std::byte*>(createChoiceMessage->data())),
        reinterpret_cast<ProductLookupFn>(
            const_cast<std::byte*>(productLookup->data())),
        reinterpret_cast<SelectChoiceFn>(
            const_cast<std::byte*>(selectChoice->data())),
        reinterpret_cast<void**>(
            const_cast<std::byte*>(guiManagerSlot->data())),
        reinterpret_cast<AlignMessageFn>(
            const_cast<std::byte*>(alignMessage->data())),
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

    const std::size_t initialSize = destination.size();
    std::size_t offset{};
    while (offset < source.size()) {
        std::size_t glyphAt = offset;
        while (glyphAt < source.size() &&
               !compat::native_notification_text::IsGlyph(
                   source[glyphAt])) {
            ++glyphAt;
        }
        if (glyphAt != offset) {
            const std::wstring stable(source.substr(offset, glyphAt - offset));
            const int bytes = api.gameWideToText(
                nullptr, stable.c_str(), kMissingGameGlyph);
            if (bytes < 0 || destination.size() - initialSize +
                    static_cast<std::size_t>(bytes) > maximumBytes) {
                destination.resize(initialSize);
                return false;
            }
            const std::size_t writeAt = destination.size();
            destination.resize(writeAt + static_cast<std::size_t>(bytes) + 1u);
            const bool converted = bytes == 0 ||
                api.gameWideToText(destination.data() + writeAt,
                                   stable.c_str(), kMissingGameGlyph) == bytes;
            destination.resize(writeAt + static_cast<std::size_t>(bytes));
            if (!converted) {
                destination.resize(initialSize);
                return false;
            }
        }
        if (glyphAt == source.size()) break;
        if (destination.size() - initialSize + 2u > maximumBytes) {
            destination.resize(initialSize);
            return false;
        }
        const auto encoded =
            compat::native_notification_text::EncodeGlyph(source[glyphAt]);
        destination.insert(destination.end(), encoded.begin(), encoded.end());
        offset = glyphAt + 1u;
    }
    return destination.size() != initialSize;
}

bool ConvertText(std::wstring_view source, std::vector<char>& text) {
    text.clear();
    if (!AppendText(text, source, 8192u)) return false;
    text.push_back('\0');
    return true;
}

void* Open(const char* message, const char* purpose, bool logFailure,
           int layer, int horizontalAlignment, int verticalAlignment) {
    const NativeApi api = Api();
    if (!message || !*message || !Complete(api)) return nullptr;

    void* const product =
        api.createSystemMessage(message, nullptr, 0, 0);
    if (!product || !patch::memory::IsReadable(product, 60u)) {
        if (logFailure) {
            LogWarning("LR %s native status product is unavailable",
                purpose);
        }
        return nullptr;
    }

    auto* const bytes = static_cast<std::byte*>(product);
    auto* const flags = reinterpret_cast<std::uint32_t*>(bytes + 56u);
    *flags |= 8u;
    api.alignMessage(product, horizontalAlignment, verticalAlignment);
    api.setGuiLayer(product, layer);

    void** const vtable = *static_cast<void***>(product);
    if (!vtable ||
        !patch::memory::IsReadable(vtable, 80u * sizeof(void*))) {
        if (logFailure) {
            LogWarning("LR %s native status vtable is unavailable",
                purpose);
        }
        return nullptr;
    }

    using OpenGuiProductFn = void (__thiscall*)(void*, int, int);
    const auto open = reinterpret_cast<OpenGuiProductFn>(vtable[79]);
    if (!open) {
        if (logFailure) {
            LogWarning("LR %s native status opener is unavailable",
                purpose);
        }
        return nullptr;
    }

    *flags |= 0x80u;
    open(product, 0, 1);
    return product;
}

void* OpenChoice(const char* message, const char* purpose, bool logFailure,
                 int layer, int horizontalAlignment,
                 int verticalAlignment) {
    const NativeApi api = Api();
    if (!message || !*message || !Complete(api) || !*api.guiManagerSlot) {
        return nullptr;
    }
    const int productId = api.createChoiceMessage(
        *api.guiManagerSlot, 0, 400, 0, 0, message, 1, 0,
        0, 0, 1, 1, 0, 0);
    void* const wrapper = api.productLookup(
        *api.guiManagerSlot, productId, 0);
    if (!wrapper || !patch::memory::IsReadable(wrapper, sizeof(void*))) {
        if (logFailure) {
            LogWarning("LR %s native choice wrapper is unavailable", purpose);
        }
        return nullptr;
    }
    void** const wrapperVtable = *static_cast<void***>(wrapper);
    if (!wrapperVtable || !wrapperVtable[1]) return nullptr;
    using GetProductFn = void* (__thiscall*)(void*);
    void* const product =
        reinterpret_cast<GetProductFn>(wrapperVtable[1])(wrapper);
    if (!product || !patch::memory::IsReadable(product, 60u)) return nullptr;

    auto* const flags = reinterpret_cast<std::uint32_t*>(
        static_cast<std::byte*>(product) + 56u);
    *flags |= 0x88u;
    api.alignMessage(product, horizontalAlignment, verticalAlignment);
    api.setGuiLayer(product, layer);
    void** const vtable = *static_cast<void***>(product);
    if (!vtable || !patch::memory::IsReadable(vtable, 80u * sizeof(void*))) {
        return nullptr;
    }
    using OpenGuiProductFn = void (__thiscall*)(void*, int, int);
    const auto open = reinterpret_cast<OpenGuiProductFn>(vtable[79]);
    if (!open) return nullptr;
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

bool FocusChoice(void* product) {
    const NativeApi api = Api();
    if (!product || !api.selectChoice ||
        !patch::memory::IsReadable(product, 148u)) {
        return false;
    }
    const auto* const selection = reinterpret_cast<const std::int32_t*>(
        static_cast<const std::byte*>(product) + 144u);
    if (*selection >= 0) return true;
    api.selectChoice(product, 1);
    return *selection >= 0;
}

} // namespace novafix::game::lr_message_window

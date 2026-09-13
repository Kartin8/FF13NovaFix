#include "game/titles/lightning_returns/snapshot/message_editor_redirect.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/image_view.h"
#include "game/titles/lightning_returns/ui/text_editor/editor.h"
#include "game/titles/lightning_returns/ui/text_editor/native_api.h"
#include "patch/memory_access.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace novafix::game::lr_snapshot_message_editor {
namespace {

constexpr std::array<patch::RelativeCallSite, 1> kConstructorCalls{{
    {0x000584C7u, {0xE8, 0x24, 0xA3, 0xFF, 0xFF}},
}};
constexpr std::array<patch::RelativeCallSite, 1> kOpenCalls{{
    {0x00052918u, {0xE8, 0x03, 0xFF, 0x2F, 0x00}},
}};
constexpr std::array<patch::RelativeCallSite, 1> kDoneCalls{{
    {0x0004FBEFu, {0xE8, 0xDC, 0x2C, 0x30, 0x00}},
}};
constexpr std::array<patch::RelativeCallSite, 1> kCanceledCalls{{
    {0x0004FC02u, {0xE8, 0xF9, 0x2C, 0x30, 0x00}},
}};

constexpr std::uint32_t kConstructorRva = 0x000527F0u;
constexpr std::uint32_t kOskOpenRva = 0x00352820u;
constexpr std::uint32_t kOskDoneRva = 0x003528D0u;
constexpr std::uint32_t kOskCanceledRva = 0x00352900u;

using ConstructorFn = int (__thiscall*)(
    void*, const char*, int, char*, int, void*, int);
using OskOpenFn = int (__thiscall*)(void*, const char*);
using OskStateFn = int (__thiscall*)(void*);

ConstructorFn g_originalConstructor{};
OskOpenFn g_originalOpen{};
OskStateFn g_originalDone{};
OskStateFn g_originalCanceled{};
std::atomic<void*> g_context{};
std::atomic<void*> g_completedContext{};
std::atomic_bool g_canceled{};

bool StoreMessage(void* context, std::string_view text) {
    if (!context || !patch::memory::IsReadable(context, 20u)) return false;

    auto* fields = static_cast<std::uint32_t*>(context);
    auto* destination = reinterpret_cast<wchar_t*>(fields[2]);
    const int maximumCharacters = static_cast<int>(fields[3]);
    if (!destination || maximumCharacters <= 0 || maximumCharacters > 4096 ||
        !patch::memory::IsReadable(
            destination,
            (static_cast<std::size_t>(maximumCharacters) + 1u) *
                sizeof(wchar_t))) {
        return false;
    }

    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int required = MultiByteToWideChar(
        codePage, flags, text.data(), static_cast<int>(text.size()),
        nullptr, 0);
    if (required <= 0 && !text.empty()) {
        codePage = CP_ACP;
        flags = 0;
        required = MultiByteToWideChar(
            codePage, flags, text.data(), static_cast<int>(text.size()),
            nullptr, 0);
    }
    if (required < 0) return false;

    std::wstring converted(static_cast<std::size_t>(required), L'\0');
    if (required > 0 && MultiByteToWideChar(
            codePage, flags, text.data(), static_cast<int>(text.size()),
            converted.data(), required) != required) {
        return false;
    }

    std::size_t length = (std::min)(
        converted.size(), static_cast<std::size_t>(maximumCharacters));
    if (length > 0 && length < converted.size() &&
        IS_HIGH_SURROGATE(converted[length - 1u]) &&
        IS_LOW_SURROGATE(converted[length])) {
        --length;
    }
    if (length) {
        std::memcpy(destination, converted.data(), length * sizeof(wchar_t));
    }
    destination[length] = L'\0';
    return true;
}

int __fastcall OpenHook(void* context, void*, const char* initialText) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-compose-open-total");
    const OskOpenFn original = g_originalOpen;
    if (!lr_text_editor::BackendRedirectRequested() || !context ||
        !patch::memory::IsReadable(context, 16u)) {
        return original ? original(context, initialText) : 0;
    }

    if (context == g_context.load(std::memory_order_acquire) &&
        lr_text_editor::IsOpen()) {
        return 1;
    }

    const auto* fields = static_cast<const std::uint32_t*>(context);
    const int maximumCharacters = static_cast<int>(fields[3]);
    if (maximumCharacters <= 0 || maximumCharacters >= 512 ||
        !lr_text_editor::OpenFromGameText(
            initialText, static_cast<std::size_t>(maximumCharacters))) {
        return original ? original(context, initialText) : 0;
    }

    g_context.store(context, std::memory_order_release);
    g_completedContext.store(nullptr, std::memory_order_release);
    g_canceled.store(false, std::memory_order_release);
    return 1;
}

int __fastcall DoneHook(void* context, void*) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-compose-done-total");
    if (context != g_context.load(std::memory_order_acquire)) {
        return g_originalDone ? g_originalDone(context) : 0;
    }
    if (context == g_completedContext.load(std::memory_order_acquire)) {
        return 1;
    }

    std::string result;
    bool submitted = false;
    if (!lr_text_editor::TakeResult(result, submitted)) return 0;

    const bool canceled = !submitted || !StoreMessage(context, result);
    g_canceled.store(canceled, std::memory_order_release);
    g_completedContext.store(context, std::memory_order_release);
    return 1;
}

int __fastcall CanceledHook(void* context, void*) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-compose-cancel-total");
    if (context != g_completedContext.load(std::memory_order_acquire)) {
        return g_originalCanceled ? g_originalCanceled(context) : 0;
    }

    const bool canceled = g_canceled.load(std::memory_order_acquire);
    g_context.store(nullptr, std::memory_order_release);
    g_completedContext.store(nullptr, std::memory_order_release);
    return canceled ? 1 : 0;
}

int __fastcall ConstructorHook(
    void* editor, void*, const char* title, int maximumCharacters,
    char* buffer, int bufferBytes, void* listener, int allowEmpty) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-compose-constructor-total");
    const ConstructorFn original = g_originalConstructor;
    if (!original) return 0;

    lr_text_editor::BackendRedirectScope redirect;
    return original(editor, title, maximumCharacters, buffer, bufferBytes,
                    listener, allowEmpty);
}

} // namespace

bool HasExpectedContract(const ImageView& image) {
    return image.At(kConstructorRva, 1u) &&
        image.At(kOskOpenRva, 1u) &&
        image.At(kOskDoneRva, 1u) &&
        image.At(kOskCanceledRva, 1u);
}

bool AddRedirects(patch::Transaction& transaction,
                  const ImageView& image) {
    const auto constructor = image.At(kConstructorRva, 1u);
    const auto open = image.At(kOskOpenRva, 1u);
    const auto done = image.At(kOskDoneRva, 1u);
    const auto canceled = image.At(kOskCanceledRva, 1u);
    if (!constructor || !open || !done || !canceled) return false;

    g_originalConstructor = reinterpret_cast<ConstructorFn>(
        const_cast<std::byte*>(constructor->data()));
    g_originalOpen = reinterpret_cast<OskOpenFn>(
        const_cast<std::byte*>(open->data()));
    g_originalDone = reinterpret_cast<OskStateFn>(
        const_cast<std::byte*>(done->data()));
    g_originalCanceled = reinterpret_cast<OskStateFn>(
        const_cast<std::byte*>(canceled->data()));

    return patch::AddRelativeCalls(
               transaction, image, kConstructorCalls,
               reinterpret_cast<const void*>(&ConstructorHook)) &&
        patch::AddRelativeCalls(
               transaction, image, kOpenCalls,
               reinterpret_cast<const void*>(&OpenHook)) &&
        patch::AddRelativeCalls(
               transaction, image, kDoneCalls,
               reinterpret_cast<const void*>(&DoneHook)) &&
        patch::AddRelativeCalls(
               transaction, image, kCanceledCalls,
               reinterpret_cast<const void*>(&CanceledHook));
}

void ClearBindings() {
    g_originalConstructor = nullptr;
    g_originalOpen = nullptr;
    g_originalDone = nullptr;
    g_originalCanceled = nullptr;
    g_context.store(nullptr, std::memory_order_release);
    g_completedContext.store(nullptr, std::memory_order_release);
    g_canceled.store(false, std::memory_order_release);
}

} // namespace novafix::game::lr_snapshot_message_editor

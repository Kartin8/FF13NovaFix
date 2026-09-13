#include "game/titles/ff13/ui/prompt_layout_registry.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/image_view.h"
#include "game/shared/ui/prompt_source_store.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"
#include "patch/vtable_hook.h"

#include <windows.h>
#include <intrin.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace novafix::game::ff13_prompt_layout_registry {
namespace {

constexpr std::uint32_t kTextLayoutRebuildRva = 0x0063B920u;
// Exact extent of sub_A3B920. Its A80090 call is the embedded GuiLabel path;
// all other native A80090 callers own standalone RenderText objects
constexpr std::size_t kTextLayoutRebuildSize = 0x126u;
constexpr std::uint32_t kRenderTextRebuildRva = 0x00680090u;
constexpr std::uint32_t kButtonControlClassifierRva = 0x006929D0u;
constexpr std::uint32_t kRenderTextVtableRva = 0x00D9FEA8u;

constexpr std::size_t kRenderTextOffset = 0xE0u;
constexpr std::size_t kRenderTextReadableSize = 0x1ACu;
constexpr std::size_t kTextWidgetReadableSize = 1156u;
constexpr std::size_t kCapturedPromptCapacity = 1024u;
constexpr std::size_t kCapturedPromptIndexCapacity = 2048u;
constexpr std::size_t kMaximumCapturedSourceBytes = 2048u;

constexpr std::array<std::uint8_t, 15> kRenderTextRebuildPrefix{
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x20, 0x01, 0x00,
    0x00, 0x89, 0x8D, 0xF4, 0xFE, 0xFF, 0xFF};
constexpr std::array<std::uint8_t, 16> kTextLayoutRebuildPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20, 0x89, 0x4D,
    0xE0, 0x8B, 0x45, 0x08, 0x0F, 0xB7, 0x48, 0x0C};
constexpr std::array<std::uint8_t, 13> kButtonControlClassifierPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x0F,
    0xB7, 0x45, 0x08, 0xC1, 0xF8, 0x08};

using RenderTextRebuildFn = unsigned int (__thiscall*)(void* renderText,
                                                       const void* source,
                                                       int preserveMode);
using TextLayoutRebuildFn = int (__thiscall*)(void* widget,
                                              const void* sourceState);
using ButtonControlClassifierFn = int (__cdecl*)(unsigned int encodedCode);
using RenderTextDestructorFn = void* (__thiscall*)(void* renderText,
                                                   int destroyFlags);

patch::Detour32 g_renderTextRebuildDetour;
patch::Detour32 g_buttonControlClassifierDetour;
RenderTextRebuildFn g_originalRenderTextRebuild{};
TextLayoutRebuildFn g_textLayoutRebuild{};
ButtonControlClassifierFn g_originalButtonControlClassifier{};
RenderTextDestructorFn g_originalRenderTextDestructor{};
void* g_renderTextVtable{};
SRWLOCK g_capturedPromptLock = SRWLOCK_INIT;
thread_local bool g_replayingCapturedPrompt{};

struct CapturedPromptSource {
    void* renderText{};
    std::uint16_t byteCount{};
    int rebuildMode{};
    bool layoutOwned{};
    std::array<std::byte, kMaximumCapturedSourceBytes> bytes{};
};

using CapturedPromptStore = PromptSourceStore<
    CapturedPromptSource, kCapturedPromptCapacity,
    kCapturedPromptIndexCapacity>;
using CapturedPromptLookup = CapturedPromptStore::Lookup;
CapturedPromptStore g_capturedPrompts;

struct PendingPromptCapture {
    bool containsButton{};
};

thread_local PendingPromptCapture* g_pendingPromptCapture{};

int __cdecl ButtonControlClassifierHook(unsigned int encodedCode) {
    ButtonControlClassifierFn original =
        g_originalButtonControlClassifier;
    const int result = original ? original(encodedCode) : 0;
    if (result != 0 && g_pendingPromptCapture) {
        g_pendingPromptCapture->containsButton = true;
    }
    return result;
}

void RememberPromptSource(void* renderText, const void* source,
                          bool containsButton, int preserveMode,
                          bool layoutOwned) {
    if (!renderText) return;

    std::array<std::byte, kMaximumCapturedSourceBytes> copy;
    std::uint16_t byteCount{};
    const bool captureReady = containsButton &&
        CopyBoundedPromptSource(source, copy, byteCount);

    AcquireSRWLockExclusive(&g_capturedPromptLock);
    const CapturedPromptLookup lookup = g_capturedPrompts.Find(renderText);
    if (!captureReady) {
        g_capturedPrompts.Erase(lookup);
        ReleaseSRWLockExclusive(&g_capturedPromptLock);
        return;
    }

    CapturedPromptSource* const destination =
        g_capturedPrompts.Insert(renderText, lookup);
    if (destination) {
        destination->renderText = renderText;
        destination->byteCount = byteCount;
        destination->rebuildMode = preserveMode;
        destination->layoutOwned = layoutOwned;
        std::memcpy(destination->bytes.data(), copy.data(), byteCount);
    }
    ReleaseSRWLockExclusive(&g_capturedPromptLock);
}

void ErasePromptSource(void* renderText) {
    if (!renderText) return;
    AcquireSRWLockExclusive(&g_capturedPromptLock);
    g_capturedPrompts.Erase(g_capturedPrompts.Find(renderText));
    ReleaseSRWLockExclusive(&g_capturedPromptLock);
}

void* __fastcall RenderTextDestructorHook(void* renderText, void*,
                                          int destroyFlags) {
    ErasePromptSource(renderText);
    RenderTextDestructorFn original = g_originalRenderTextDestructor;
    return original ? original(renderText, destroyFlags) : renderText;
}

bool CalledThroughTextLayout(const void* returnAddress) {
    if (!returnAddress || !g_textLayoutRebuild) return false;
    const auto caller = reinterpret_cast<std::uintptr_t>(returnAddress);
    const auto begin = reinterpret_cast<std::uintptr_t>(g_textLayoutRebuild);
    return caller >= begin && caller - begin < kTextLayoutRebuildSize;
}

unsigned int __fastcall RenderTextRebuildHook(void* renderText, void*,
                                               const void* source,
                                               int preserveMode) {
    static const auto totalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13.prompt-render-text-total");
    static const auto originalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13.prompt-render-text-original");
    static const auto capturePoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13.prompt-object-capture");
    thread_local std::uint32_t sampleCounter{};
    const bool selected = (sampleCounter++ & 15u) == 0u;
    hook_profiler::Scope totalTiming(totalPoint, selected, 16u);
    RenderTextRebuildFn original = g_originalRenderTextRebuild;
    if (!original) return 0u;
    if (g_replayingCapturedPrompt) {
        hook_profiler::Scope originalTiming(
            originalPoint, selected, 16u);
        return original(renderText, source, preserveMode);
    }

    const bool layoutOwned = CalledThroughTextLayout(_ReturnAddress());

    PendingPromptCapture pending{};
    PendingPromptCapture* const previousPending =
        g_pendingPromptCapture;
    g_pendingPromptCapture = &pending;
    unsigned int result{};
    {
        hook_profiler::Scope originalTiming(
            originalPoint, selected, 16u);
        result = original(renderText, source, preserveMode);
    }
    g_pendingPromptCapture = previousPending;

    {
        hook_profiler::Scope captureTiming(
            capturePoint, selected, 16u);
        void* observedVtable{};
        if (renderText) {
            std::memcpy(&observedVtable, renderText,
                        sizeof(observedVtable));
        }
        RememberPromptSource(
            renderText, source,
            pending.containsButton &&
                observedVtable == g_renderTextVtable,
            preserveMode, layoutOwned);
    }
    return result;
}

void ClearState() {
    g_textLayoutRebuild = nullptr;
    g_renderTextVtable = nullptr;
    AcquireSRWLockExclusive(&g_capturedPromptLock);
    g_capturedPrompts = {};
    ReleaseSRWLockExclusive(&g_capturedPromptLock);
}

} // namespace

bool Install(const ImageView& image) {
    const auto textLayoutRebuildRange = image.At(
        kTextLayoutRebuildRva, kTextLayoutRebuildPrefix.size());
    const auto renderTextRebuildRange = image.At(
        kRenderTextRebuildRva, kRenderTextRebuildPrefix.size());
    const auto buttonControlClassifierRange = image.At(
        kButtonControlClassifierRva,
        kButtonControlClassifierPrefix.size());
    const auto renderTextVtableAddress = image.Address32(
        kRenderTextVtableRva, sizeof(void*));
    if (!textLayoutRebuildRange || !renderTextRebuildRange ||
        !buttonControlClassifierRange || !renderTextVtableAddress) {
        LogWarning("XIII prompt layout registry disabled: RVA contract is outside the image or x86 address space");
        return false;
    }

    std::byte* const textLayoutRebuild = const_cast<std::byte*>(
        textLayoutRebuildRange->data());
    std::byte* const renderTextRebuild = const_cast<std::byte*>(
        renderTextRebuildRange->data());
    std::byte* const buttonControlClassifier =
        const_cast<std::byte*>(buttonControlClassifierRange->data());
    if (std::memcmp(textLayoutRebuild, kTextLayoutRebuildPrefix.data(),
                    kTextLayoutRebuildPrefix.size()) != 0 ||
        std::memcmp(renderTextRebuild, kRenderTextRebuildPrefix.data(),
                    kRenderTextRebuildPrefix.size()) != 0 ||
        std::memcmp(buttonControlClassifier,
                    kButtonControlClassifierPrefix.data(),
                    kButtonControlClassifierPrefix.size()) != 0) {
        LogWarning("XIII prompt layout registry disabled: executable contract mismatch");
        return false;
    }

    g_textLayoutRebuild =
        reinterpret_cast<TextLayoutRebuildFn>(textLayoutRebuild);
    g_renderTextVtable = reinterpret_cast<void*>(*renderTextVtableAddress);

    if (!g_buttonControlClassifierDetour.Install(
            "ff13-prompt-native-classifier-observer",
            buttonControlClassifier,
            reinterpret_cast<void*>(&ButtonControlClassifierHook),
            std::as_bytes(std::span{kButtonControlClassifierPrefix}),
            reinterpret_cast<void**>(
                &g_originalButtonControlClassifier))) {
        ClearState();
        LogWarning("XIII prompt layout registry disabled: native button-classifier observer failed");
        return false;
    }

    if (!g_renderTextRebuildDetour.Install(
            "ff13-prompt-source-capture", renderTextRebuild,
            reinterpret_cast<void*>(&RenderTextRebuildHook),
            std::as_bytes(std::span{kRenderTextRebuildPrefix}),
            reinterpret_cast<void**>(&g_originalRenderTextRebuild))) {
        const bool clean =
            g_buttonControlClassifierDetour.RevertAndClear(
                g_originalButtonControlClassifier);
        if (clean) {
            ClearState();
        } else {
            LogError("XIII prompt layout registry rollback incomplete: live classifier binding retained");
        }
        LogWarning("XIII prompt layout registry disabled: render-text builder detour failed");
        return false;
    }

    return true;
}

bool InstallLifetimeObserver() {
    if (!g_renderTextVtable) return false;
    auto** const renderTextVtable = static_cast<void**>(g_renderTextVtable);
    return ReplaceVtableSlot(
        "ff13-prompt-render-text-lifetime", renderTextVtable, 0u,
        reinterpret_cast<void*>(&RenderTextDestructorHook),
        reinterpret_cast<void**>(&g_originalRenderTextDestructor));
}

bool Revert() {
    bool clean = true;
    clean = g_renderTextRebuildDetour.RevertAndClear(
                g_originalRenderTextRebuild) && clean;
    clean = g_buttonControlClassifierDetour.RevertAndClear(
                g_originalButtonControlClassifier) && clean;
    if (clean) ClearState();
    return clean;
}

RefreshResult Refresh() {
    RefreshResult result{};
    if (!g_originalRenderTextRebuild || !g_textLayoutRebuild ||
        !g_renderTextVtable) {
        return result;
    }
    NOVAFIX_PROFILE_NAMED_SCOPE(
        refreshTiming, "hook.ff13.prompt-registry-refresh");

    // A3B920 -> A80090 identifies the layout owner as RenderText - 0xE0
    // Refresh registered owners directly: bottom guides are outside the GUI tree
    // Standalone labels replay A80090; destructor tracking excludes dead objects
    AcquireSRWLockShared(&g_capturedPromptLock);
    g_replayingCapturedPrompt = true;
    for (std::size_t index = 0u;
         index < g_capturedPrompts.AllocatedCount();
         ++index) {
        const CapturedPromptSource& captured = g_capturedPrompts.At(index);
        if (!captured.renderText || captured.byteCount == 0u ||
            !patch::memory::IsReadable(captured.renderText,
                                       kRenderTextReadableSize)) {
            continue;
        }

        void* observedVtable{};
        std::memcpy(&observedVtable, captured.renderText,
                    sizeof(observedVtable));
        if (observedVtable != g_renderTextVtable) continue;

        if (captured.layoutOwned) {
            auto* const widget =
                static_cast<std::byte*>(captured.renderText) -
                kRenderTextOffset;
            if (!patch::memory::IsReadable(
                    widget, kTextWidgetReadableSize)) {
                continue;
            }

            std::array<std::byte, 16> sourceState{};
            const void* stableSource = captured.bytes.data();
            const std::uint16_t sourceFlags =
                captured.rebuildMode != 0 ? 1u : 0u;
            const std::int16_t sourceOffset = 0;
            std::memcpy(sourceState.data() + 4u, &stableSource,
                        sizeof(stableSource));
            std::memcpy(sourceState.data() + 12u, &sourceFlags,
                        sizeof(sourceFlags));
            std::memcpy(sourceState.data() + 14u, &sourceOffset,
                        sizeof(sourceOffset));
            g_textLayoutRebuild(widget, sourceState.data());
            ++result.layoutOwned;
        } else {
            g_originalRenderTextRebuild(
                captured.renderText, captured.bytes.data(),
                captured.rebuildMode);
            ++result.direct;
        }
    }
    g_replayingCapturedPrompt = false;
    ReleaseSRWLockShared(&g_capturedPromptLock);
    return result;
}

} // namespace novafix::game::ff13_prompt_layout_registry

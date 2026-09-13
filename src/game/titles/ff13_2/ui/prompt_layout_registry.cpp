#include "game/titles/ff13_2/ui/prompt_layout_registry.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/image_view.h"
#include "game/shared/ui/prompt_source_store.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace novafix::game::xiii2_prompt_layout_registry {
namespace {

constexpr std::uint32_t kSecurityCookieRva = 0x01FA90DCu;
constexpr std::uint32_t kTextLayoutRebuildRva = 0x004C40A0u;
constexpr std::uint32_t kRenderTextRebuildRva = 0x004EA600u;
constexpr std::uint32_t kButtonControlClassifierRva = 0x004F7160u;
constexpr std::uint32_t kGuiDrawRva = 0x00492880u;
constexpr std::uint32_t kRenderTextVtableRva = 0x00A4C030u;
constexpr std::size_t kGuiRootOffset = 0x34u;
constexpr std::size_t kNodeFirstChildOffset = 0x08u;
constexpr std::size_t kNodeNextSiblingOffset = 0x14u;
constexpr std::size_t kTraversalCapacity = 4096u;
constexpr std::size_t kRootCapacity = 256u;
constexpr std::size_t kRenderTextOffset = 0xE0u;
constexpr std::size_t kRenderTextFlagsOffset = 0x164u;
constexpr std::size_t kRenderTextReadableSize = 0x16Cu;
constexpr std::size_t kCapturedPromptCapacity = 1024u;
constexpr std::size_t kCapturedPromptIndexCapacity = 2048u;
constexpr std::size_t kMaximumCapturedSourceBytes = 2048u;

constexpr std::array<std::uint8_t, 16> kGuiDrawPrefix{
    0x83, 0xEC, 0x40, 0x56, 0x8B, 0xF1, 0x83, 0x7E,
    0x34, 0x00, 0x74, 0x35, 0x80, 0x7E, 0x3A, 0x03};
constexpr std::array<std::uint8_t, 13> kButtonControlClassifierPrefix{
    0x53, 0x8B, 0x5C, 0x24, 0x08, 0xC1, 0xEB,
    0x08, 0xE8, 0xD3, 0xB5, 0xDB, 0xFF};
constexpr std::array<std::uint8_t, 16> kTextLayoutRebuildPrefix{
    0x8B, 0x44, 0x24, 0x04, 0x0F, 0xBF, 0x50, 0x0A,
    0x03, 0x10, 0x83, 0xEC, 0x10, 0x56, 0x8B, 0xF1};

using RenderTextRebuildFn = int (__thiscall*)(void* renderText,
                                               const void* source,
                                               int preserveMode);
using TextLayoutRebuildFn = int (__thiscall*)(void* widget,
                                               const void* sourceState);
using ButtonControlClassifierFn = int (__cdecl*)(unsigned int encodedCode);
using GuiDrawFn = int (__thiscall*)(void* owner, int layer);

patch::Detour32 g_guiDrawDetour;
patch::Detour32 g_renderTextRebuildDetour;
patch::Detour32 g_buttonControlClassifierDetour;
GuiDrawFn g_originalGuiDraw{};
RenderTextRebuildFn g_originalRenderTextRebuild{};
TextLayoutRebuildFn g_textLayoutRebuild{};
ButtonControlClassifierFn g_originalButtonControlClassifier{};
bool g_reflowPending{};
bool g_reflowObserved{};
std::array<void*, kRootCapacity> g_reflowedRoots{};
std::size_t g_reflowedRootCount{};
std::array<void*, kTraversalCapacity> g_reflowedRenderTexts{};
std::size_t g_reflowedRenderTextCount{};
void* g_renderTextVtable{};
SRWLOCK g_capturedPromptLock = SRWLOCK_INIT;
thread_local bool g_replayingCapturedPrompt{};

struct PromptParseObservation {
    bool containsButton{};
};

thread_local PromptParseObservation* g_promptParseObservation{};

struct CapturedPromptSource {
    void* renderText{};
    std::uint16_t byteCount{};
    std::array<std::byte, kMaximumCapturedSourceBytes> bytes{};
};

using CapturedPromptStore = PromptSourceStore<
    CapturedPromptSource, kCapturedPromptCapacity,
    kCapturedPromptIndexCapacity>;
using CapturedPromptLookup = CapturedPromptStore::Lookup;
CapturedPromptStore g_capturedPrompts;

std::array<std::byte, 8> AbsoluteLoadPrefix(
    const std::array<std::byte, 4>& opcode,
    std::uint32_t absoluteAddress) {
    std::array<std::byte, 8> result{};
    std::memcpy(result.data(), opcode.data(), opcode.size());
    std::memcpy(result.data() + opcode.size(), &absoluteAddress,
                sizeof(absoluteAddress));
    return result;
}

bool RootAlreadyReflowed(void* root) {
    for (std::size_t index = 0; index < g_reflowedRootCount; ++index) {
        if (g_reflowedRoots[index] == root) return true;
    }
    return false;
}

void RememberPromptSource(void* renderText, const void* source,
                          bool containsButton) {
    if (!renderText) return;
    NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(
        captureTiming, "hook.ff13-2.prompt-source-capture", 16u);

    if (!containsButton) {
        AcquireSRWLockExclusive(&g_capturedPromptLock);
        g_capturedPrompts.Erase(g_capturedPrompts.Find(renderText));
        ReleaseSRWLockExclusive(&g_capturedPromptLock);
        return;
    }

    std::array<std::byte, kMaximumCapturedSourceBytes> copy;
    std::uint16_t byteCount{};
    if (!CopyBoundedPromptSource(source, copy, byteCount)) return;

    AcquireSRWLockExclusive(&g_capturedPromptLock);
    const CapturedPromptLookup lookup = g_capturedPrompts.Find(renderText);
    CapturedPromptSource* const destination =
        g_capturedPrompts.Insert(renderText, lookup);
    if (!destination) {
        ReleaseSRWLockExclusive(&g_capturedPromptLock);
        return;
    }
    destination->renderText = renderText;
    destination->byteCount = byteCount;
    std::memcpy(destination->bytes.data(), copy.data(), byteCount);
    ReleaseSRWLockExclusive(&g_capturedPromptLock);
}

int __cdecl ButtonControlClassifierHook(unsigned int encodedCode) {
    ButtonControlClassifierFn original = g_originalButtonControlClassifier;
    const int result = original ? original(encodedCode) : 0;
    if (result != 0 && g_promptParseObservation) {
        g_promptParseObservation->containsButton = true;
    }
    return result;
}

bool RebuildCapturedPrompt(void* widget, void* renderText,
                           int preserveMode) {
    if (!widget || !renderText || !g_textLayoutRebuild ||
        !patch::memory::IsReadable(widget, kRenderTextOffset + 1u)) {
        return false;
    }

    bool rebuilt = false;
    AcquireSRWLockShared(&g_capturedPromptLock);
    const CapturedPromptLookup lookup = g_capturedPrompts.Find(renderText);
    if (lookup.found()) {
        const CapturedPromptSource& captured =
            g_capturedPrompts.At(lookup.record);
        if (captured.byteCount != 0u) {
            std::array<std::byte, 12> sourceState{};
            const void* stableSource = captured.bytes.data();
            const std::uint16_t sourceFlags =
                preserveMode != 0 ? 1u : 0u;
            const std::int16_t sourceOffset = 0;
            std::memcpy(sourceState.data(), &stableSource,
                        sizeof(stableSource));
            std::memcpy(sourceState.data() + 8u, &sourceFlags,
                        sizeof(sourceFlags));
            std::memcpy(sourceState.data() + 10u, &sourceOffset,
                        sizeof(sourceOffset));

            g_replayingCapturedPrompt = true;
            g_textLayoutRebuild(widget, sourceState.data());
            g_replayingCapturedPrompt = false;
            rebuilt = true;
        }
    }
    ReleaseSRWLockShared(&g_capturedPromptLock);
    return rebuilt;
}

bool HasCapturedPrompt(void* renderText) {
    AcquireSRWLockShared(&g_capturedPromptLock);
    const CapturedPromptLookup lookup = g_capturedPrompts.Find(renderText);
    const bool captured = lookup.found() &&
        g_capturedPrompts.At(lookup.record).byteCount != 0u;
    ReleaseSRWLockShared(&g_capturedPromptLock);
    return captured;
}

int __fastcall RenderTextRebuildHook(void* renderText, void*,
                                     const void* source,
                                     int preserveMode) {
    static const auto totalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13-2.prompt-render-text-total");
    static const auto originalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13-2.prompt-render-text-original");
    thread_local std::uint32_t totalSampleCounter{};
    hook_profiler::Scope totalTiming(
        totalPoint, (totalSampleCounter++ & 15u) == 0u, 16u);
    RenderTextRebuildFn original = g_originalRenderTextRebuild;
    if (!original) return 0;

    thread_local std::uint32_t originalSampleCounter{};
    const bool selected = (originalSampleCounter++ & 15u) == 0u;
    if (g_replayingCapturedPrompt) {
        hook_profiler::Scope originalTiming(originalPoint, selected, 16u);
        return original(renderText, source, preserveMode);
    }

    PromptParseObservation observation{};
    PromptParseObservation* const previousObservation =
        g_promptParseObservation;
    g_promptParseObservation = &observation;
    int result{};
    {
        hook_profiler::Scope originalTiming(originalPoint, selected, 16u);
        result = original(renderText, source, preserveMode);
    }
    g_promptParseObservation = previousObservation;
    RememberPromptSource(renderText, source, observation.containsButton);
    return result;
}

class ReadableRegionCache {
public:
    bool Contains(const void* address, std::size_t size) {
        if (!address || size == 0u) return false;
        const auto begin = reinterpret_cast<std::uintptr_t>(address);
        if (begin > (std::numeric_limits<std::uintptr_t>::max)() - size) {
            return false;
        }
        const std::uintptr_t end = begin + size;
        for (const Region& region : regions_) {
            if (begin >= region.begin && end <= region.end) return true;
        }

        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(address, &information, sizeof(information)) !=
                sizeof(information) ||
            information.State != MEM_COMMIT ||
            !ReadableProtection(information.Protect)) {
            return false;
        }
        const auto regionBegin = reinterpret_cast<std::uintptr_t>(
            information.BaseAddress);
        if (regionBegin >
            (std::numeric_limits<std::uintptr_t>::max)() -
                information.RegionSize) {
            return false;
        }
        const std::uintptr_t regionEnd = regionBegin +
            static_cast<std::uintptr_t>(information.RegionSize);
        if (begin < regionBegin || end > regionEnd) {
            return patch::memory::IsReadable(address, size);
        }
        regions_[nextRegion_] = {regionBegin, regionEnd};
        nextRegion_ = (nextRegion_ + 1u) % regions_.size();
        return true;
    }

private:
    struct Region {
        std::uintptr_t begin{};
        std::uintptr_t end{};
    };

    static bool ReadableProtection(DWORD protection) {
        if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
            return false;
        }
        switch (protection & 0xFFu) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

    std::array<Region, 64u> regions_{};
    std::size_t nextRegion_{};
};

std::size_t ReflowRoot(void* root) {
    if (!root || !g_originalRenderTextRebuild ||
        !g_originalButtonControlClassifier || !g_renderTextVtable) {
        return 0;
    }
    NOVAFIX_PROFILE_NAMED_SCOPE(
        reflowTiming, "hook.ff13-2.prompt-reflow-root");

    std::array<void*, kTraversalCapacity> pending{};
    std::size_t pendingCount = 1;
    pending[0] = root;
    std::size_t visited = 0;
    std::size_t refreshed = 0;
    ReadableRegionCache readableRegions;

    while (pendingCount != 0 && visited < kTraversalCapacity) {
        void* const node = pending[--pendingCount];
        ++visited;
        if (!readableRegions.Contains(
                node, kNodeNextSiblingOffset + sizeof(void*))) {
            continue;
        }

        auto* const bytes = static_cast<std::byte*>(node);
        void* firstChild{};
        void* nextSibling{};
        std::memcpy(&firstChild, bytes + kNodeFirstChildOffset,
                    sizeof(firstChild));
        std::memcpy(&nextSibling, bytes + kNodeNextSiblingOffset,
                    sizeof(nextSibling));
        if (nextSibling && pendingCount < pending.size()) {
            pending[pendingCount++] = nextSibling;
        }
        if (firstChild && pendingCount < pending.size()) {
            pending[pendingCount++] = firstChild;
        }

        auto* const renderText = bytes + kRenderTextOffset;
        if (!HasCapturedPrompt(renderText) ||
            !readableRegions.Contains(renderText, kRenderTextReadableSize)) {
            continue;
        }

        void* embeddedVtable{};
        std::memcpy(&embeddedVtable, renderText, sizeof(embeddedVtable));
        if (embeddedVtable != g_renderTextVtable) continue;

        bool alreadyRebuilt = false;
        for (std::size_t index = 0; index < g_reflowedRenderTextCount;
             ++index) {
            if (g_reflowedRenderTexts[index] != renderText) continue;
            alreadyRebuilt = true;
            break;
        }
        if (alreadyRebuilt ||
            g_reflowedRenderTextCount >= g_reflowedRenderTexts.size()) {
            continue;
        }

        std::uint16_t flags{};
        std::memcpy(&flags, renderText + kRenderTextFlagsOffset,
                    sizeof(flags));
        g_reflowedRenderTexts[g_reflowedRenderTextCount++] = renderText;
        if (!RebuildCapturedPrompt(
                node, renderText, (flags & 2u) != 0u ? 1 : 0)) {
            --g_reflowedRenderTextCount;
            continue;
        }
        ++refreshed;
    }
    return refreshed;
}

int __fastcall GuiDrawHook(void* owner, void*, int layer) {
    static const auto totalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13-2.prompt-gui-draw-total");
    static const auto originalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13-2.prompt-gui-draw-original");
    thread_local std::uint32_t totalSampleCounter{};
    hook_profiler::Scope totalTiming(
        totalPoint, (totalSampleCounter++ & 15u) == 0u, 16u);
    GuiDrawFn original = g_originalGuiDraw;
    if (!original) return 0;

    if (g_reflowPending && owner &&
        patch::memory::IsReadable(
            owner, kGuiRootOffset + sizeof(void*))) {
        void* root{};
        std::memcpy(&root, static_cast<std::byte*>(owner) + kGuiRootOffset,
                    sizeof(root));
        if (root && !RootAlreadyReflowed(root)) {
            if (g_reflowedRootCount < g_reflowedRoots.size()) {
                g_reflowedRoots[g_reflowedRootCount++] = root;
            }
            (void)ReflowRoot(root);
            g_reflowObserved = true;
        }
    }

    thread_local std::uint32_t originalSampleCounter{};
    hook_profiler::Scope originalTiming(
        originalPoint, (originalSampleCounter++ & 15u) == 0u, 16u);
    return original(owner, layer);
}

void ClearState() {
    g_renderTextVtable = nullptr;
    g_textLayoutRebuild = nullptr;
    g_reflowPending = false;
    g_reflowObserved = false;
    g_reflowedRoots = {};
    g_reflowedRootCount = 0u;
    g_reflowedRenderTexts = {};
    g_reflowedRenderTextCount = 0u;
    AcquireSRWLockExclusive(&g_capturedPromptLock);
    g_capturedPrompts = {};
    ReleaseSRWLockExclusive(&g_capturedPromptLock);
}

} // namespace

bool Install(const ImageView& image) {
    const auto renderTextRebuildRange = image.At(kRenderTextRebuildRva, 8u);
    const auto textLayoutRebuildRange = image.At(
        kTextLayoutRebuildRva, kTextLayoutRebuildPrefix.size());
    const auto buttonControlClassifierRange = image.At(
        kButtonControlClassifierRva, kButtonControlClassifierPrefix.size());
    const auto guiDrawRange = image.At(kGuiDrawRva, kGuiDrawPrefix.size());
    const auto renderTextVtableAddress = image.Address32(
        kRenderTextVtableRva, sizeof(void*));
    const auto cookieAddress = image.Address32(
        kSecurityCookieRva, sizeof(std::uint32_t));
    if (!renderTextRebuildRange || !textLayoutRebuildRange ||
        !buttonControlClassifierRange || !guiDrawRange ||
        !renderTextVtableAddress || !cookieAddress) {
        LogWarning("XIII-2 prompt layout registry disabled: RVA contract is outside the image or x86 address space");
        return false;
    }

    std::byte* const renderTextRebuild =
        const_cast<std::byte*>(renderTextRebuildRange->data());
    std::byte* const textLayoutRebuild =
        const_cast<std::byte*>(textLayoutRebuildRange->data());
    std::byte* const buttonControlClassifier =
        const_cast<std::byte*>(buttonControlClassifierRange->data());
    std::byte* const guiDraw =
        const_cast<std::byte*>(guiDrawRange->data());
    const std::array<std::byte, 8> renderTextRebuildPrefix =
        AbsoluteLoadPrefix(
            {std::byte{0x83}, std::byte{0xEC}, std::byte{0x60},
             std::byte{0xA1}},
            *cookieAddress);
    if (std::memcmp(renderTextRebuild, renderTextRebuildPrefix.data(),
                    renderTextRebuildPrefix.size()) != 0 ||
        std::memcmp(textLayoutRebuild, kTextLayoutRebuildPrefix.data(),
                    kTextLayoutRebuildPrefix.size()) != 0 ||
        std::memcmp(buttonControlClassifier,
                    kButtonControlClassifierPrefix.data(),
                    kButtonControlClassifierPrefix.size()) != 0 ||
        std::memcmp(guiDraw, kGuiDrawPrefix.data(),
                    kGuiDrawPrefix.size()) != 0) {
        LogWarning("XIII-2 prompt layout registry disabled: executable contract mismatch");
        return false;
    }

    g_renderTextVtable = reinterpret_cast<void*>(*renderTextVtableAddress);
    g_textLayoutRebuild =
        reinterpret_cast<TextLayoutRebuildFn>(textLayoutRebuild);

    if (!g_buttonControlClassifierDetour.Install(
            "ff13-2-prompt-native-classifier-observer",
            buttonControlClassifier,
            reinterpret_cast<void*>(&ButtonControlClassifierHook),
            std::as_bytes(std::span{kButtonControlClassifierPrefix}),
            reinterpret_cast<void**>(&g_originalButtonControlClassifier))) {
        ClearState();
        LogWarning("XIII-2 prompt layout registry disabled: native button-classifier observer failed");
        return false;
    }

    if (!g_renderTextRebuildDetour.Install(
            "ff13-2-prompt-source-capture", renderTextRebuild,
            reinterpret_cast<void*>(&RenderTextRebuildHook),
            renderTextRebuildPrefix,
            reinterpret_cast<void**>(&g_originalRenderTextRebuild))) {
        const bool clean = g_buttonControlClassifierDetour.RevertAndClear(
            g_originalButtonControlClassifier);
        if (clean) {
            ClearState();
        } else {
            LogError("XIII-2 prompt layout registry rollback incomplete: live classifier binding retained");
        }
        LogWarning("XIII-2 prompt layout registry disabled: render-text rebuild detour failed");
        return false;
    }

    if (!g_guiDrawDetour.Install(
            "ff13-2-visible-prompt-reflow", guiDraw,
            reinterpret_cast<void*>(&GuiDrawHook),
            std::as_bytes(std::span{kGuiDrawPrefix}),
            reinterpret_cast<void**>(&g_originalGuiDraw))) {
        g_originalGuiDraw = nullptr;
        const bool captureClean =
            g_renderTextRebuildDetour.RevertAndClear(
                g_originalRenderTextRebuild);
        const bool classifierClean =
            g_buttonControlClassifierDetour.RevertAndClear(
                g_originalButtonControlClassifier);
        if (captureClean && classifierClean) {
            ClearState();
        } else {
            LogError("XIII-2 prompt layout registry rollback incomplete: live capture bindings retained");
        }
        LogWarning("XIII-2 prompt layout registry disabled: GUI draw detour failed");
        return false;
    }
    return true;
}

bool Revert() {
    bool clean = true;
    clean = g_guiDrawDetour.RevertAndClear(g_originalGuiDraw) && clean;
    clean = g_renderTextRebuildDetour.RevertAndClear(
                g_originalRenderTextRebuild) && clean;
    clean = g_buttonControlClassifierDetour.RevertAndClear(
                g_originalButtonControlClassifier) && clean;
    if (clean) ClearState();
    return clean;
}

void QueueVisibleReflow() {
    g_reflowedRoots.fill(nullptr);
    g_reflowedRootCount = 0u;
    g_reflowedRenderTexts.fill(nullptr);
    g_reflowedRenderTextCount = 0u;
    g_reflowObserved = false;
    g_reflowPending = true;
}

void CompleteVisibleReflow() {
    if (!g_reflowPending || !g_reflowObserved) return;
    g_reflowPending = false;
}

} // namespace novafix::game::xiii2_prompt_layout_registry

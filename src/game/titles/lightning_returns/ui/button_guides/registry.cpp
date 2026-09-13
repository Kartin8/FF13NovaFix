#include "game/titles/lightning_returns/ui/button_guides/registry.h"
#include "game/titles/lightning_returns/ui/button_guides/snapshot_hud_guide.h"

#include "diagnostics/log.h"
#include "diagnostics/hook_profiler.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"

#include <windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

namespace novafix::game::lr_prompt_guides {
namespace {

constexpr std::uint32_t kTextLabelDestructorRva = 0x0003C9C0u;
constexpr std::uint32_t kTextLabelRebuildRva = 0x00484920u;
constexpr std::uint32_t kTextLabelWrapperRva = 0x004849C0u;
constexpr std::uint32_t kButtonGlyphMeasureRva = 0x004B4B60u;
constexpr std::uint32_t kSnsCameraModeConstructorRva = 0x000486C0u;
constexpr std::uint32_t kLayoutNodeFindRva = 0x003BEC60u;
constexpr std::uint32_t kGameAllocateRva = 0x00392DB0u;
constexpr std::uint32_t kTextLabelBaseConstructorRva = 0x00484440u;
constexpr std::uint32_t kTextLabelTailConstructorRva = 0x003BAEC0u;
constexpr std::uint32_t kTextLabelInitializeRva = 0x004849E0u;
constexpr std::uint32_t kTextLabelGeometryRva = 0x004844E0u;
constexpr std::uint32_t kGuiComponentConstructorRva = 0x0047F960u;
constexpr std::uint32_t kGuiTextLabelVtableRva = 0x009F912Cu;
constexpr std::uint32_t kGameLanguageRva = 0x00353E00u;
constexpr std::uint32_t kGameWideToTextRva = 0x003BE2C0u;
constexpr std::size_t kMaximumCapturedSourceBytes = 2048u;
constexpr std::size_t kMaximumCapturedGuides = 512u;
static_assert((kMaximumCapturedGuides & (kMaximumCapturedGuides - 1u)) == 0u);
constexpr std::size_t kGuideIndexBuckets = 1024u;
static_assert((kGuideIndexBuckets & (kGuideIndexBuckets - 1u)) == 0u);

struct GuideAssignCallSite {
    std::uint32_t rva;
    std::array<std::uint8_t, 5> expected;
};

constexpr std::array<GuideAssignCallSite, 3> kGuideAssignCalls{{
    {0x0008DD96u, {0xE8, 0x25, 0x6C, 0x3F, 0x00}},
    {0x00048BE1u, {0xE8, 0xDA, 0xBD, 0x43, 0x00}},
    {0x00048C9Bu, {0xE8, 0x20, 0xBD, 0x43, 0x00}},
}};

constexpr std::array<std::uint8_t, 5> kTextLabelDestructorPrefix{
    0x55, 0x8B, 0xEC, 0x6A, 0xFF,
};
constexpr std::array<std::uint8_t, 16> kTextLabelRebuildPrefix{
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C, 0x83, 0xEC,
    0x10, 0x56, 0x57, 0x8B, 0xF1, 0x8B, 0x4D, 0x08,
};
constexpr std::array<std::uint8_t, 5> kTextLabelWrapperPrefix{
    0x55, 0x8B, 0xEC, 0x8B, 0x45,
};
constexpr std::array<std::uint8_t, 6> kButtonGlyphMeasurePrefix{
    0x55, 0x8B, 0xEC, 0x56, 0x6A, 0x00,
};
constexpr std::array<std::uint8_t, 5> kSnsCameraModeConstructorPrefix{
    0x55, 0x8B, 0xEC, 0x6A, 0xFF,
};
constexpr std::array<std::uint8_t, 8> kLayoutNodeFindPrefix{
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x85, 0xC0,
};
constexpr std::array<std::uint8_t, 5> kGameAllocatePrefix{
    0x55, 0x8B, 0xEC, 0x5D, 0xE9,
};
constexpr std::array<std::uint8_t, 5> kTextLabelBaseConstructorPrefix{
    0x55, 0x8B, 0xEC, 0x6A, 0xFF,
};
constexpr std::array<std::uint8_t, 6> kTextLabelTailConstructorPrefix{
    0x8B, 0xC1, 0x33, 0xC9, 0xC7, 0x40,
};
constexpr std::array<std::uint8_t, 6> kTextLabelInitializePrefix{
    0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57,
};
constexpr std::array<std::uint8_t, 6> kTextLabelGeometryPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10,
};
constexpr std::array<std::uint8_t, 3> kGuiComponentConstructorPrefix{
    0x56, 0x8B, 0xF1,
};
constexpr std::array<std::uint8_t, 8> kGameLanguagePrefix{
    0xE8, 0x7B, 0xFE, 0xFF, 0xFF, 0x85, 0xC0, 0xA1,
};
constexpr std::array<std::uint8_t, 8> kGameWideToTextPrefix{
    0x55, 0x8B, 0xEC, 0xE8, 0x38, 0x5B, 0xF9, 0xFF,
};
using TextLabelDestructorFn = int (__thiscall*)(void*, int);
using TextLabelRebuildFn = int (__thiscall*)(void*, const void*, int);
using TextLabelWrapperFn = void (__thiscall*)(void*, const void*);
using ButtonGlyphMeasureFn = int (__cdecl*)(std::int16_t);
using SnsCameraModeConstructorFn = int (__thiscall*)(void*, int, int);
using LayoutNodeFindFn = void* (__cdecl*)(void*, int);
using GameAllocateFn = void* (__cdecl*)(std::size_t);
using TextLabelBaseConstructorFn = void* (__thiscall*)(void*);
using TextLabelTailConstructorFn = void (__thiscall*)(void*);
using TextLabelInitializeFn = int (__thiscall*)(
    void*, void*, short, short, int, int, const void*);
using TextLabelGeometryFn = void (__thiscall*)(void*, int, int, int);
using GuiComponentConstructorFn = void* (__thiscall*)(void*);
using GameLanguageFn = int (__cdecl*)();
using GameWideToTextFn = int (__cdecl*)(char*, const wchar_t*, short);

struct CapturedGuide {
    enum class Kind : std::uint8_t {
        None,
        SaveLoad,
        Snapshot,
        StandaloneSnapshotHud,
    };

    void* label{};
    void* vtable{};
    int preserveMode{};
    std::uint16_t byteCount{};
    Kind kind{Kind::None};
    void* alignmentContainer{};
    void* alignmentReferenceLabel{};
    float alignmentBaseX{};
    float alignmentRowStepX{};
    std::uint64_t refreshGeneration{};
    std::array<std::byte, kMaximumCapturedSourceBytes> bytes{};
};

struct GuideIndexEntry {
    void* label{};
    std::uint16_t guideIndex{};
};

patch::Detour32 g_textLabelDestructorDetour;
patch::Detour32 g_textLabelRebuildDetour;
patch::Detour32 g_buttonGlyphMeasureDetour;
patch::Detour32 g_snsCameraModeConstructorDetour;
std::unique_ptr<patch::Transaction> g_guideAssignPatch;
TextLabelDestructorFn g_originalTextLabelDestructor{};
TextLabelRebuildFn g_textLabelRebuild{};
TextLabelWrapperFn g_textLabelWrapper{};
ButtonGlyphMeasureFn g_originalButtonGlyphMeasure{};
SnsCameraModeConstructorFn g_originalSnsCameraModeConstructor{};
LayoutNodeFindFn g_layoutNodeFind{};
GameAllocateFn g_gameAllocate{};
TextLabelBaseConstructorFn g_textLabelBaseConstructor{};
TextLabelTailConstructorFn g_textLabelTailConstructor{};
TextLabelInitializeFn g_textLabelInitialize{};
TextLabelGeometryFn g_textLabelGeometry{};
GuiComponentConstructorFn g_guiComponentConstructor{};
void* g_guiTextLabelVtable{};
GameLanguageFn g_gameLanguage{};
GameWideToTextFn g_gameWideToText{};
SRWLOCK g_guideLock = SRWLOCK_INIT;
std::array<CapturedGuide, kMaximumCapturedGuides> g_guides{};
std::array<GuideIndexEntry, kGuideIndexBuckets> g_guideIndex{};
std::size_t g_guideCount{};
std::recursive_mutex g_nativeGuideMutex;
std::atomic_uint64_t g_activeRefreshGeneration{};
void* g_snapshotStandaloneLabel{};
void* g_snapshotAlignmentReferenceLabel{};

struct PendingLabelCapture {
    bool containsButton{};
};

thread_local PendingLabelCapture* g_pendingLabelCapture{};

bool IsLiveGuide(const CapturedGuide& guide) {
    return guide.label != nullptr;
}

std::size_t GuideBucket(void* label) {
    const auto value = reinterpret_cast<std::uintptr_t>(label);
    return (value >> 4u) & (g_guideIndex.size() - 1u);
}

void InsertGuideIndexLocked(void* label, std::size_t guideIndex) {
    const std::size_t start = GuideBucket(label);
    for (std::size_t probe = 0u; probe < g_guideIndex.size(); ++probe) {
        GuideIndexEntry& candidate =
            g_guideIndex[(start + probe) & (g_guideIndex.size() - 1u)];
        if (!candidate.label) {
            candidate.label = label;
            candidate.guideIndex = static_cast<std::uint16_t>(guideIndex);
            return;
        }
    }
}

void RebuildGuideIndexLocked() {
    g_guideIndex = {};
    for (std::size_t index = 0u; index < g_guideCount; ++index) {
        InsertGuideIndexLocked(g_guides[index].label, index);
    }
}

CapturedGuide* FindGuideLocked(void* label) {
    if (!label) return nullptr;

    const std::size_t start = GuideBucket(label);
    for (std::size_t probe = 0u; probe < g_guideIndex.size(); ++probe) {
        const GuideIndexEntry& candidate =
            g_guideIndex[(start + probe) & (g_guideIndex.size() - 1u)];
        if (!candidate.label) return nullptr;
        if (candidate.label == label &&
            candidate.guideIndex < g_guideCount) {
            CapturedGuide& guide = g_guides[candidate.guideIndex];
            return guide.label == label ? &guide : nullptr;
        }
    }
    return nullptr;
}

CapturedGuide* ReserveGuideLocked(void* label) {
    if (!label) return nullptr;
    if (CapturedGuide* existing = FindGuideLocked(label)) return existing;
    if (g_guideCount >= g_guides.size()) return nullptr;

    const std::size_t index = g_guideCount++;
    CapturedGuide& guide = g_guides[index];
    guide = {};
    guide.label = label;
    InsertGuideIndexLocked(label, index);
    return &guide;
}

void ForgetGuideLocked(CapturedGuide& guide) {
    const auto index = static_cast<std::size_t>(&guide - g_guides.data());
    if (index >= g_guideCount) return;

    const std::size_t last = g_guideCount - 1u;
    if (index != last) {
        g_guides[index] = std::move(g_guides[last]);
    }
    g_guides[last] = {};
    --g_guideCount;
    RebuildGuideIndexLocked();
}

bool CopyRawSource(
    const void* source,
    std::array<std::byte, kMaximumCapturedSourceBytes>& output,
    std::uint16_t& byteCount) {
    byteCount = 0u;
    if (!source) return false;

    const auto* const bytes = static_cast<const std::byte*>(source);
    std::size_t offset = 0u;
    while (offset < output.size()) {
        const auto first = std::to_integer<unsigned char>(bytes[offset]);
        output[offset] = bytes[offset];
        if (first == 0u) {
            byteCount = static_cast<std::uint16_t>(offset + 1u);
            return true;
        }
        if ((first & 0x80u) == 0u) {
            ++offset;
            continue;
        }
        if (offset + 1u >= output.size()) return false;

        output[offset + 1u] = bytes[offset + 1u];
        offset += 2u;
    }
    return false;
}

int __cdecl ButtonGlyphMeasureHook(std::int16_t encoded) {
    if (g_pendingLabelCapture) {
        g_pendingLabelCapture->containsButton = true;
    }
    const ButtonGlyphMeasureFn original = g_originalButtonGlyphMeasure;
    return original ? original(encoded) : 0;
}

int __fastcall TextLabelRebuildHook(void* label, void*, const void* source,
                                    int preserveMode) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        totalTiming, "hook.lr.prompt-label-rebuild-total");
    TextLabelRebuildFn original = g_textLabelRebuild;
    if (!original) return 0;

    // Capture before the native parser sees the buffer. Complex dialog text
    // contains inline layout/color controls, and rebuilding from the parser's
    // post-call view can preserve partially consumed control state
    std::array<std::byte, kMaximumCapturedSourceBytes> sourceCopy{};
    std::uint16_t byteCount{};
    bool sourceCopyReady = false;
    {
        hook_profiler::Scope profile(
            hook_profiler::Point::LrPromptCapturedSourceCopy);
        sourceCopyReady = CopyRawSource(source, sourceCopy, byteCount);
    }

    PendingLabelCapture pending{};
    PendingLabelCapture* const previousPending = g_pendingLabelCapture;
    g_pendingLabelCapture = &pending;
    int result{};
    {
        hook_profiler::Scope profile(
            hook_profiler::Point::LrPromptLabelOriginal);
        result = original(label, source, preserveMode);
    }
    g_pendingLabelCapture = previousPending;
    if (!label) return result;

    hook_profiler::Scope bookkeeping(
        hook_profiler::Point::LrPromptLabelBookkeeping);
    const bool captureReady = pending.containsButton && sourceCopyReady;
    void* vtable{};
    if (captureReady) {
        std::memcpy(&vtable, label, sizeof(vtable));
    }

    AcquireSRWLockExclusive(&g_guideLock);
    CapturedGuide* captured = FindGuideLocked(label);
    if (!captureReady || !vtable) {
        if (captured) ForgetGuideLocked(*captured);
        ReleaseSRWLockExclusive(&g_guideLock);
        return result;
    }

    if (!captured) captured = ReserveGuideLocked(label);
    if (captured) {
        const CapturedGuide::Kind kind = captured->kind;
        void* const alignmentContainer = captured->alignmentContainer;
        void* const alignmentReferenceLabel =
            captured->alignmentReferenceLabel;
        const float alignmentBaseX = captured->alignmentBaseX;
        const float alignmentRowStepX = captured->alignmentRowStepX;
        *captured = {};
        captured->label = label;
        captured->vtable = vtable;
        captured->preserveMode = preserveMode;
        captured->byteCount = byteCount;
        captured->kind = kind;
        captured->alignmentContainer = alignmentContainer;
        captured->alignmentReferenceLabel = alignmentReferenceLabel;
        captured->alignmentBaseX = alignmentBaseX;
        captured->alignmentRowStepX = alignmentRowStepX;
        captured->refreshGeneration =
            g_activeRefreshGeneration.load(std::memory_order_acquire);
        std::memcpy(captured->bytes.data(), sourceCopy.data(), byteCount);
    }
    ReleaseSRWLockExclusive(&g_guideLock);
    return result;
}

void ForgetPreviousSnapshotGuides() {
    AcquireSRWLockExclusive(&g_guideLock);
    std::size_t index = 0u;
    while (index < g_guideCount) {
        CapturedGuide& captured = g_guides[index];
        if (captured.kind == CapturedGuide::Kind::Snapshot ||
            captured.kind == CapturedGuide::Kind::StandaloneSnapshotHud) {
            ForgetGuideLocked(captured);
        } else {
            ++index;
        }
    }
    g_snapshotStandaloneLabel = nullptr;
    g_snapshotAlignmentReferenceLabel = nullptr;
    ReleaseSRWLockExclusive(&g_guideLock);
}

void AssignAndCaptureGuide(void* label, const void* stringObject,
                           CapturedGuide::Kind kind) {
    TextLabelWrapperFn original = g_textLabelWrapper;
    if (!original) return;

    original(label, stringObject);

    AcquireSRWLockExclusive(&g_guideLock);
    if (CapturedGuide* captured = FindGuideLocked(label)) {
        captured->kind = kind;
    }
    ReleaseSRWLockExclusive(&g_guideLock);
}

void __fastcall SaveLoadGuideAssignHook(void* label, void*,
                                        const void* stringObject) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.save-load-guide-assign-total");
    AssignAndCaptureGuide(
        label, stringObject, CapturedGuide::Kind::SaveLoad);
}

void __fastcall SnapshotGuideAssignHook(void* label, void*,
                                        const void* stringObject) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-guide-assign-total");
    AssignAndCaptureGuide(
        label, stringObject, CapturedGuide::Kind::Snapshot);
}

bool CreateAndRegisterSnapshotHudGuide(void* camera) {
    AcquireSRWLockShared(&g_guideLock);
    const bool hasCaptureSlot = g_guideCount < g_guides.size();
    ReleaseSRWLockShared(&g_guideLock);
    if (!hasCaptureSlot) {
        Log("LR standalone Snapshot HUD guide rejected: persistent-guide registry is full");
        return false;
    }

    const snapshot_hud_guide::NativeApi api{
        g_layoutNodeFind,
        g_gameAllocate,
        g_textLabelBaseConstructor,
        g_textLabelTailConstructor,
        g_textLabelInitialize,
        g_textLabelRebuild,
        g_textLabelGeometry,
        g_guiComponentConstructor,
        g_guiTextLabelVtable,
        g_gameLanguage,
        g_gameWideToText,
    };
    snapshot_hud_guide::CreatedGuide created;
    if (!snapshot_hud_guide::Create(camera, api, created)) return false;

    CapturedGuide* reserved = nullptr;
    AcquireSRWLockExclusive(&g_guideLock);
    reserved = ReserveGuideLocked(created.label);
    if (reserved) {
        *reserved = {};
        reserved->label = created.label;
    }
    ReleaseSRWLockExclusive(&g_guideLock);
    if (!reserved) {
        Log("LR standalone Snapshot HUD guide rejected: persistent-guide slot disappeared");
        return false;
    }

    AcquireSRWLockExclusive(&g_guideLock);
    if (reserved->label == created.label) {
        reserved->vtable = created.vtable;
        reserved->preserveMode = 0;
        reserved->byteCount = created.sourceByteCount;
        reserved->kind = CapturedGuide::Kind::StandaloneSnapshotHud;
        reserved->alignmentContainer = created.alignmentContainer;
        reserved->alignmentReferenceLabel =
            created.alignmentReferenceLabel;
        reserved->alignmentBaseX = created.alignmentBaseX;
        reserved->alignmentRowStepX = created.alignmentRowStepX;
        reserved->refreshGeneration =
            g_activeRefreshGeneration.load(std::memory_order_acquire);
        std::memcpy(reserved->bytes.data(), created.source.data(),
                    created.sourceByteCount);
        g_snapshotStandaloneLabel = created.label;
        g_snapshotAlignmentReferenceLabel =
            created.alignmentReferenceLabel;
    }
    ReleaseSRWLockExclusive(&g_guideLock);
    return true;
}

int __fastcall SnsCameraModeConstructorHook(void* camera, void*, int owner,
                                            int listener) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-camera-constructor-total");
    SnsCameraModeConstructorFn original = g_originalSnsCameraModeConstructor;
    if (!original) return 0;

    std::scoped_lock nativeLock(g_nativeGuideMutex);
    ForgetPreviousSnapshotGuides();

    const ULONGLONG originalStart = GetTickCount64();
    const int result = original(camera, owner, listener);
    const ULONGLONG originalElapsed = GetTickCount64() - originalStart;
    const ULONGLONG extensionStart = GetTickCount64();
    (void)CreateAndRegisterSnapshotHudGuide(camera);
    const ULONGLONG extensionElapsed = GetTickCount64() - extensionStart;
    if (originalElapsed >= 50u || extensionElapsed >= 10u) {
        Log("LR Snapshot camera construction timing: native=%llums HUD-guide=%llums",
            static_cast<unsigned long long>(originalElapsed),
            static_cast<unsigned long long>(extensionElapsed));
    }
    return result;
}

int __fastcall TextLabelDestructorHook(void* label, void*, int destroyFlags) {
    hook_profiler::Scope totalTiming(
        hook_profiler::Point::LrPromptLabelDestroyOwn);
    // Keep this recursive lock alive across the native destructor call. Its
    // lifetime is part of the original hook's synchronization contract
    std::scoped_lock nativeLock(g_nativeGuideMutex);
    AcquireSRWLockExclusive(&g_guideLock);
    if (CapturedGuide* captured = FindGuideLocked(label)) {
        ForgetGuideLocked(*captured);
    }
    if (g_snapshotAlignmentReferenceLabel == label) {
        if (CapturedGuide* standalone =
                FindGuideLocked(g_snapshotStandaloneLabel)) {
            standalone->alignmentReferenceLabel = nullptr;
        }
        g_snapshotAlignmentReferenceLabel = nullptr;
    }
    if (g_snapshotStandaloneLabel == label) {
        g_snapshotStandaloneLabel = nullptr;
    }
    ReleaseSRWLockExclusive(&g_guideLock);

    TextLabelDestructorFn original = g_originalTextLabelDestructor;
    hook_profiler::Scope profile(
        hook_profiler::Point::LrPromptLabelDestroyOriginal);
    return original ? original(label, destroyFlags) : 0;
}

bool ValidateCallSites(const ImageView& image) {
    for (const GuideAssignCallSite& site : kGuideAssignCalls) {
        const auto range = image.At(site.rva, site.expected.size());
        if (!range ||
            std::memcmp(range->data(), site.expected.data(),
                        site.expected.size()) != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

bool Install(const ImageView& image) {
    const auto textLabelDestructorRange = image.At(
        kTextLabelDestructorRva, kTextLabelDestructorPrefix.size());
    const auto textLabelRebuildRange = image.At(
        kTextLabelRebuildRva, kTextLabelRebuildPrefix.size());
    const auto textLabelWrapperRange = image.At(
        kTextLabelWrapperRva, kTextLabelWrapperPrefix.size());
    const auto buttonGlyphMeasureRange = image.At(
        kButtonGlyphMeasureRva, kButtonGlyphMeasurePrefix.size());
    const auto snsCameraModeConstructorRange = image.At(
        kSnsCameraModeConstructorRva,
        kSnsCameraModeConstructorPrefix.size());
    const auto layoutNodeFindRange = image.At(
        kLayoutNodeFindRva, kLayoutNodeFindPrefix.size());
    const auto gameAllocateRange = image.At(
        kGameAllocateRva, kGameAllocatePrefix.size());
    const auto textLabelBaseConstructorRange = image.At(
        kTextLabelBaseConstructorRva,
        kTextLabelBaseConstructorPrefix.size());
    const auto textLabelTailConstructorRange = image.At(
        kTextLabelTailConstructorRva,
        kTextLabelTailConstructorPrefix.size());
    const auto textLabelInitializeRange = image.At(
        kTextLabelInitializeRva, kTextLabelInitializePrefix.size());
    const auto textLabelGeometryRange = image.At(
        kTextLabelGeometryRva, kTextLabelGeometryPrefix.size());
    const auto guiComponentConstructorRange = image.At(
        kGuiComponentConstructorRva,
        kGuiComponentConstructorPrefix.size());
    const auto guiTextLabelVtable = image.Address32(
        kGuiTextLabelVtableRva, sizeof(void*));
    const auto gameLanguageRange = image.At(
        kGameLanguageRva, kGameLanguagePrefix.size());
    const auto gameWideToTextRange = image.At(
        kGameWideToTextRva, kGameWideToTextPrefix.size());
    if (!ValidateCallSites(image) || !textLabelDestructorRange ||
        !textLabelRebuildRange || !textLabelWrapperRange ||
        !buttonGlyphMeasureRange || !snsCameraModeConstructorRange ||
        !layoutNodeFindRange || !gameAllocateRange ||
        !textLabelBaseConstructorRange || !textLabelTailConstructorRange ||
        !textLabelInitializeRange || !textLabelGeometryRange ||
        !guiComponentConstructorRange || !guiTextLabelVtable ||
        !gameLanguageRange || !gameWideToTextRange) {
        LogWarning("LR dynamic prompts disabled: persistent guide RVA contract is outside the image");
        return false;
    }

    std::byte* const textLabelDestructor =
        const_cast<std::byte*>(textLabelDestructorRange->data());
    std::byte* const textLabelRebuild =
        const_cast<std::byte*>(textLabelRebuildRange->data());
    std::byte* const textLabelWrapper =
        const_cast<std::byte*>(textLabelWrapperRange->data());
    std::byte* const buttonGlyphMeasure =
        const_cast<std::byte*>(buttonGlyphMeasureRange->data());
    std::byte* const snsCameraModeConstructor =
        const_cast<std::byte*>(snsCameraModeConstructorRange->data());
    std::byte* const layoutNodeFind =
        const_cast<std::byte*>(layoutNodeFindRange->data());
    std::byte* const gameAllocate =
        const_cast<std::byte*>(gameAllocateRange->data());
    std::byte* const textLabelBaseConstructor =
        const_cast<std::byte*>(textLabelBaseConstructorRange->data());
    std::byte* const textLabelTailConstructor =
        const_cast<std::byte*>(textLabelTailConstructorRange->data());
    std::byte* const textLabelInitialize =
        const_cast<std::byte*>(textLabelInitializeRange->data());
    std::byte* const textLabelGeometry =
        const_cast<std::byte*>(textLabelGeometryRange->data());
    std::byte* const guiComponentConstructor =
        const_cast<std::byte*>(guiComponentConstructorRange->data());
    std::byte* const gameLanguage =
        const_cast<std::byte*>(gameLanguageRange->data());
    std::byte* const gameWideToText =
        const_cast<std::byte*>(gameWideToTextRange->data());
    void* const guiTextLabelVtableAddress = reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(*guiTextLabelVtable));
    void* guiTextLabelDestructor{};
    std::memcpy(&guiTextLabelDestructor, guiTextLabelVtableAddress,
                sizeof(guiTextLabelDestructor));
    if (std::memcmp(textLabelDestructor,
                    kTextLabelDestructorPrefix.data(),
                    kTextLabelDestructorPrefix.size()) != 0 ||
        std::memcmp(textLabelRebuild, kTextLabelRebuildPrefix.data(),
                    kTextLabelRebuildPrefix.size()) != 0 ||
        std::memcmp(textLabelWrapper, kTextLabelWrapperPrefix.data(),
                    kTextLabelWrapperPrefix.size()) != 0 ||
        std::memcmp(buttonGlyphMeasure,
                    kButtonGlyphMeasurePrefix.data(),
                    kButtonGlyphMeasurePrefix.size()) != 0 ||
        std::memcmp(snsCameraModeConstructor,
                    kSnsCameraModeConstructorPrefix.data(),
                    kSnsCameraModeConstructorPrefix.size()) != 0 ||
        std::memcmp(layoutNodeFind, kLayoutNodeFindPrefix.data(),
                    kLayoutNodeFindPrefix.size()) != 0 ||
        std::memcmp(gameAllocate, kGameAllocatePrefix.data(),
                    kGameAllocatePrefix.size()) != 0 ||
        std::memcmp(textLabelBaseConstructor,
                    kTextLabelBaseConstructorPrefix.data(),
                    kTextLabelBaseConstructorPrefix.size()) != 0 ||
        std::memcmp(textLabelTailConstructor,
                    kTextLabelTailConstructorPrefix.data(),
                    kTextLabelTailConstructorPrefix.size()) != 0 ||
        std::memcmp(textLabelInitialize,
                    kTextLabelInitializePrefix.data(),
                    kTextLabelInitializePrefix.size()) != 0 ||
        std::memcmp(textLabelGeometry, kTextLabelGeometryPrefix.data(),
                    kTextLabelGeometryPrefix.size()) != 0 ||
        std::memcmp(guiComponentConstructor,
                    kGuiComponentConstructorPrefix.data(),
                    kGuiComponentConstructorPrefix.size()) != 0 ||
        std::memcmp(gameLanguage, kGameLanguagePrefix.data(),
                    kGameLanguagePrefix.size()) != 0 ||
        std::memcmp(gameWideToText, kGameWideToTextPrefix.data(),
                    kGameWideToTextPrefix.size()) != 0 ||
        guiTextLabelDestructor != textLabelDestructor) {
        LogWarning("LR dynamic prompts disabled: persistent guide executable contract mismatch");
        return false;
    }

    g_layoutNodeFind = reinterpret_cast<LayoutNodeFindFn>(layoutNodeFind);
    g_gameAllocate = reinterpret_cast<GameAllocateFn>(gameAllocate);
    g_textLabelBaseConstructor =
        reinterpret_cast<TextLabelBaseConstructorFn>(
            textLabelBaseConstructor);
    g_textLabelTailConstructor =
        reinterpret_cast<TextLabelTailConstructorFn>(
            textLabelTailConstructor);
    g_textLabelInitialize =
        reinterpret_cast<TextLabelInitializeFn>(textLabelInitialize);
    g_textLabelGeometry =
        reinterpret_cast<TextLabelGeometryFn>(textLabelGeometry);
    g_guiComponentConstructor =
        reinterpret_cast<GuiComponentConstructorFn>(
            guiComponentConstructor);
    g_guiTextLabelVtable = guiTextLabelVtableAddress;
    g_gameLanguage = reinterpret_cast<GameLanguageFn>(gameLanguage);
    g_gameWideToText = reinterpret_cast<GameWideToTextFn>(gameWideToText);
    g_textLabelWrapper =
        reinterpret_cast<TextLabelWrapperFn>(textLabelWrapper);

    if (!g_buttonGlyphMeasureDetour.Install(
            "lr-button-glyph-observer", buttonGlyphMeasure,
            reinterpret_cast<void*>(&ButtonGlyphMeasureHook),
            std::as_bytes(std::span{kButtonGlyphMeasurePrefix}),
            reinterpret_cast<void**>(&g_originalButtonGlyphMeasure))) {
        Revert();
        LogWarning("LR dynamic prompts disabled: button-glyph observer hook failed");
        return false;
    }

    if (!g_textLabelRebuildDetour.Install(
            "lr-button-label-lifecycle", textLabelRebuild,
            reinterpret_cast<void*>(&TextLabelRebuildHook),
            std::as_bytes(std::span{kTextLabelRebuildPrefix}),
            reinterpret_cast<void**>(&g_textLabelRebuild))) {
        Revert();
        LogWarning("LR dynamic prompts disabled: button-label lifecycle hook failed");
        return false;
    }

    if (!g_textLabelDestructorDetour.Install(
            "lr-button-label-lifetime", textLabelDestructor,
            reinterpret_cast<void*>(&TextLabelDestructorHook),
            std::as_bytes(std::span{kTextLabelDestructorPrefix}),
            reinterpret_cast<void**>(&g_originalTextLabelDestructor))) {
        Revert();
        LogWarning("LR dynamic prompts disabled: persistent guide label lifetime hook failed");
        return false;
    }

    const auto imageBytes = image.Bytes();
    auto guideAssignPatch = std::make_unique<patch::Transaction>(
        "lr-persistent-guide-assignments",
        std::span<std::byte>(const_cast<std::byte*>(imageBytes.data()),
                             imageBytes.size()),
        patch::registry::Kind::Detour);
    bool guideCallsDefined = true;
    for (std::size_t index = 0u; index < kGuideAssignCalls.size(); ++index) {
        const GuideAssignCallSite& site = kGuideAssignCalls[index];
        const auto call = image.At(site.rva, site.expected.size());
        if (!call) {
            guideCallsDefined = false;
            break;
        }
        const auto replacement = patch::EncodeRelativeCall32(
            reinterpret_cast<std::uintptr_t>(call->data()),
            reinterpret_cast<std::uintptr_t>(
                index == 0u ? &SaveLoadGuideAssignHook
                            : &SnapshotGuideAssignHook));
        if (!replacement ||
            !guideAssignPatch->Add(
                site.rva, std::as_bytes(std::span{site.expected}),
                *replacement)) {
            guideCallsDefined = false;
            break;
        }
    }
    if (!guideCallsDefined ||
        guideAssignPatch->Apply() != patch::ApplyStatus::Applied) {
        Revert();
        LogWarning("LR dynamic prompts disabled: persistent guide call-site hooks failed");
        return false;
    }
    g_guideAssignPatch = std::move(guideAssignPatch);

    if (!g_snsCameraModeConstructorDetour.Install(
            "lr-snapshot-native-hud-guide-object",
            snsCameraModeConstructor,
            reinterpret_cast<void*>(&SnsCameraModeConstructorHook),
            std::as_bytes(std::span{kSnsCameraModeConstructorPrefix}),
            reinterpret_cast<void**>(&g_originalSnsCameraModeConstructor))) {
        Revert();
        LogWarning("LR dynamic prompts disabled: Snapshot guide object hook failed");
        return false;
    }

    return true;
}

bool Revert() {
    std::scoped_lock nativeLock(g_nativeGuideMutex);
    if (g_guideAssignPatch) {
        if (!g_guideAssignPatch->Revert()) {
            LogError("LR persistent guide call-site rollback refused: live bindings retained");
            return false;
        }
        g_guideAssignPatch.reset();
    }
    if (!g_snsCameraModeConstructorDetour.RevertAndClear(
            g_originalSnsCameraModeConstructor)) {
        LogError("LR Snapshot guide constructor rollback refused: live bindings retained");
        return false;
    }
    if (!g_textLabelDestructorDetour.RevertAndClear(
            g_originalTextLabelDestructor)) {
        LogError("LR persistent guide lifetime rollback refused: live bindings retained");
        return false;
    }
    if (!g_textLabelRebuildDetour.RevertAndClear(g_textLabelRebuild)) {
        LogError("LR button-label lifecycle rollback refused: live binding retained");
        return false;
    }
    if (!g_buttonGlyphMeasureDetour.RevertAndClear(
            g_originalButtonGlyphMeasure)) {
        LogError("LR button-glyph observer rollback refused: live binding retained");
        return false;
    }

    g_textLabelWrapper = nullptr;
    g_layoutNodeFind = nullptr;
    g_gameAllocate = nullptr;
    g_textLabelBaseConstructor = nullptr;
    g_textLabelTailConstructor = nullptr;
    g_textLabelInitialize = nullptr;
    g_textLabelGeometry = nullptr;
    g_guiComponentConstructor = nullptr;
    g_guiTextLabelVtable = nullptr;
    g_gameLanguage = nullptr;
    g_gameWideToText = nullptr;
    g_activeRefreshGeneration.store(0u, std::memory_order_release);
    AcquireSRWLockExclusive(&g_guideLock);
    g_guides = {};
    g_guideIndex = {};
    g_guideCount = 0u;
    g_snapshotStandaloneLabel = nullptr;
    g_snapshotAlignmentReferenceLabel = nullptr;
    ReleaseSRWLockExclusive(&g_guideLock);
    return true;
}

void BeginRefresh(std::uint64_t generation) {
    g_activeRefreshGeneration.store(generation, std::memory_order_release);
}

std::size_t RebuildMissing(std::uint64_t generation) {
    hook_profiler::Scope profile(
        hook_profiler::Point::LrPromptGuideRebuild);
    TextLabelRebuildFn rebuild = g_textLabelRebuild;
    if (!rebuild) return 0u;

    std::scoped_lock nativeLock(g_nativeGuideMutex);
    std::size_t rebuilt = 0u;
    std::size_t index = 0u;
    for (;;) {
        CapturedGuide captured;
        AcquireSRWLockShared(&g_guideLock);
        if (index >= g_guideCount) {
            ReleaseSRWLockShared(&g_guideLock);
            break;
        }
        captured = g_guides[index];
        ReleaseSRWLockShared(&g_guideLock);
        if (!IsLiveGuide(captured) || captured.byteCount == 0u ||
            captured.refreshGeneration == generation) {
            ++index;
            continue;
        }

        auto* const labelBytes = static_cast<std::byte*>(captured.label);
        void* currentVtable{};
        std::memcpy(&currentVtable, labelBytes, sizeof(currentVtable));
        if (currentVtable != captured.vtable) {
            AcquireSRWLockExclusive(&g_guideLock);
            if (CapturedGuide* stale = FindGuideLocked(captured.label)) {
                ForgetGuideLocked(*stale);
            }
            ReleaseSRWLockExclusive(&g_guideLock);
            continue;
        }

        rebuild(captured.label, captured.bytes.data(), captured.preserveMode);
        if (captured.kind == CapturedGuide::Kind::StandaloneSnapshotHud &&
            captured.alignmentContainer &&
            captured.alignmentReferenceLabel) {
            snapshot_hud_guide::AlignLeftOrigin(
                captured.alignmentContainer, captured.alignmentBaseX,
                captured.alignmentReferenceLabel, captured.label,
                captured.alignmentRowStepX);
        }
        ++rebuilt;
        ++index;
    }
    return rebuilt;
}

} // namespace novafix::game::lr_prompt_guides

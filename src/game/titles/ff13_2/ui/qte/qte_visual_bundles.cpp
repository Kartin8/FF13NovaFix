#include "game/titles/ff13_2/ui/qte/qte_visual_bundles.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace novafix::game::xiii2_qte_visuals {
namespace {

static_assert(sizeof(void*) == 4u);

constexpr std::uint32_t kPayloadDispatchRva = 0x003B5030u;
constexpr std::uint32_t kBuildHighFamilyRva = 0x003B39F0u;
constexpr std::uint32_t kBuildLowFamilyRva = 0x003B3BB0u;
constexpr std::uint32_t kBuildControllerTailRva = 0x003B3CF0u;
constexpr std::uint32_t kCreateResourceRva = 0x00495480u;
constexpr std::uint32_t kAppendChildRva = 0x00386F00u;
constexpr std::uint32_t kRemoveActionRva = 0x00378C70u;
constexpr std::uint32_t kSetActionFlagsRva = 0x00378D60u;
constexpr std::uint32_t kProductManagerGlobalRva = 0x04D4DE0Cu;

constexpr std::size_t kOwnerRootOffset = 0x34u;
constexpr std::size_t kOwnerStateOffset = 0x70u;
constexpr std::size_t kOwnerClosePhaseOffset = 0x74u;
constexpr std::size_t kOwnerCommandMaskOffset = 0xB4u;
constexpr std::size_t kOwnerType2OutcomeOffset = 0xE1u;
constexpr std::size_t kOwnerModeOffset = 0xF8u;
constexpr std::size_t kFactorySelectorOffset = 0x10u;
constexpr std::size_t kProductFactoryOffset = 2152u;
constexpr std::size_t kNodeParentOffset = 0x04u;
constexpr std::size_t kNodeFirstChildOffset = 0x08u;
constexpr std::size_t kNodeLastChildOffset = 0x0Cu;
constexpr std::size_t kNodePreviousOffset = 0x10u;
constexpr std::size_t kNodeNextOffset = 0x14u;
constexpr std::size_t kNodeFlagsOffset = 0xCCu;
constexpr std::uint32_t kNodeTransformDirty = 0x4u;

constexpr std::array<std::size_t, 9> kBundleFieldOffsets{
    0x80u, 0x84u, 0x88u, 0xA4u, 0xA8u,
    0xC4u, 0xC8u, 0xCCu, 0xD0u,
};

constexpr std::array<std::uint8_t, 8> kPayloadDispatchPrefix{
    0x8B, 0x44, 0x24, 0x08, 0x85, 0xC0, 0x74, 0x6C};
constexpr std::array<std::uint8_t, 8> kBuildHighFamilyPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC};
constexpr std::array<std::uint8_t, 8> kBuildLowFamilyPrefix{
    0x51, 0x53, 0x57, 0x8B, 0xF9, 0x8B, 0x97, 0xF8};
constexpr std::array<std::uint8_t, 8> kBuildControllerTailPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC};
constexpr std::array<std::uint8_t, 8> kCreateResourcePrefix{
    0x8B, 0x44, 0x24, 0x04, 0x50, 0x6A, 0xFF, 0xE8};
constexpr std::array<std::uint8_t, 8> kAppendChildPrefix{
    0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0, 0x74, 0x0C};
constexpr std::array<std::uint8_t, 8> kRemoveActionPrefix{
    0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x8B, 0xF1};
constexpr std::array<std::uint8_t, 8> kSetActionFlagsPrefix{
    0x53, 0x8B, 0x5C, 0x24, 0x08, 0x55, 0x8B, 0x6C};

using NativeMode = input::NativePromptMode;
using PayloadDispatchFn = int (__thiscall*)(void*, int, const void*);
using BuildHighFamilyFn = void (__thiscall*)(void*, int);
using BuildLowFamilyFn = void (__thiscall*)(void*, int, int);
using BuildControllerTailFn = void (__thiscall*)(void*);
using CreateResourceFn = void* (__thiscall*)(void*, int);
using AppendChildFn = int (__thiscall*)(void*, void*);
using RemoveActionFn = void (__thiscall*)(void*, int);
using SetActionFlagsFn = void (__thiscall*)(void*, int, int, int);
using SetVisibleFn = void (__thiscall*)(void*, int);
using StartActionFn = void (__thiscall*)(void*, int);

struct Bundle {
    std::array<void*, kBundleFieldOffsets.size()> nodes{};
    std::array<void*, kBundleFieldOffsets.size()> order{};
    std::array<bool, kBundleFieldOffsets.size()> visible{};
    std::size_t count{};
};

struct OwnerRecord {
    void* owner{};
    NativeMode activeMode{NativeMode::Unknown};
    Bundle controller{};
    Bundle keyboard{};
    bool buildAttempted{};
    bool switchable{};
};

enum class PrepareResult {
    Ignored,
    Ready,
    Unsupported,
};

constexpr std::size_t kMaximumOwners = 32u;

patch::Detour32 g_payloadDispatchDetour;
PayloadDispatchFn g_originalPayloadDispatch{};
BuildHighFamilyFn g_buildHighFamily{};
BuildLowFamilyFn g_buildLowFamily{};
BuildControllerTailFn g_buildControllerTail{};
CreateResourceFn g_createResource{};
AppendChildFn g_appendChild{};
RemoveActionFn g_removeAction{};
SetActionFlagsFn g_setActionFlags{};
void* volatile* g_productManagerGlobal{};

SRWLOCK g_ownerLock = SRWLOCK_INIT;
std::array<OwnerRecord, kMaximumOwners> g_owners{};
NativeMode g_lastBlockedMode{NativeMode::Unknown};
std::atomic_uint g_loggedRejectionStages{};
std::atomic_bool g_ownerRegistryFullLogged{};

enum RejectionStage : unsigned {
    ModeRejected = 1u << 0u,
    RootRejected = 1u << 1u,
    AlternateBuildRejected = 1u << 2u,
    ShapeRejected = 1u << 3u,
    NodeRunsRejected = 1u << 4u,
    ParkRejected = 1u << 5u,
};

bool FirstRejectionAt(unsigned stage) {
    return (g_loggedRejectionStages.fetch_or(
                stage, std::memory_order_relaxed) & stage) == 0u;
}

template <typename T>
T ReadAt(const void* object, std::size_t offset) {
    T value{};
    if (object) {
        std::memcpy(&value, static_cast<const std::byte*>(object) + offset,
                    sizeof(value));
    }
    return value;
}

template <typename T>
void WriteAt(void* object, std::size_t offset, const T& value) {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value,
                sizeof(value));
}

std::size_t ModeIndex(NativeMode mode) {
    return mode == NativeMode::Keyboard ? 1u : 0u;
}

const char* ModeName(NativeMode mode) {
    return mode == NativeMode::Keyboard ? "keyboard" : "controller";
}

Bundle& BundleFor(OwnerRecord& record, NativeMode mode) {
    return ModeIndex(mode) ? record.keyboard : record.controller;
}

const Bundle& BundleFor(const OwnerRecord& record, NativeMode mode) {
    return ModeIndex(mode) ? record.keyboard : record.controller;
}

OwnerRecord* FindOwner(void* owner) {
    for (auto& record : g_owners) {
        if (record.owner == owner) return &record;
    }
    return nullptr;
}

OwnerRecord* ReserveOwner(void* owner) {
    if (OwnerRecord* existing = FindOwner(owner)) return existing;
    for (auto& record : g_owners) {
        if (!record.owner) {
            record = {};
            record.owner = owner;
            return &record;
        }
    }
    return nullptr;
}

bool CallSetVisible(void* node, bool visible) {
    if (!node || !patch::memory::IsReadable(node, sizeof(void*))) return false;
    void** table = ReadAt<void**>(node, 0u);
    if (!table || !patch::memory::IsReadable(table + 6, sizeof(void*))) {
        return false;
    }
    const auto function = reinterpret_cast<SetVisibleFn>(table[6]);
    if (!function || !patch::memory::IsExecutable(
            reinterpret_cast<void*>(function), 1u)) {
        return false;
    }
    function(node, visible ? 1 : 0);
    return true;
}

bool HasVisibilitySlot(void* node) {
    if (!node || !patch::memory::IsReadable(
            node, kNodeFlagsOffset + sizeof(std::uint32_t))) {
        return false;
    }
    void** table = ReadAt<void**>(node, 0u);
    return table && patch::memory::IsReadable(table + 6, sizeof(void*)) &&
        table[6] && patch::memory::IsExecutable(table[6], 1u);
}

bool CallStartAction(void* node, int action) {
    if (!node || !patch::memory::IsReadable(node, sizeof(void*))) return false;
    void** table = ReadAt<void**>(node, 0u);
    if (!table || !patch::memory::IsReadable(table + 10, sizeof(void*))) {
        return false;
    }
    const auto function = reinterpret_cast<StartActionFn>(table[10]);
    if (!function || !patch::memory::IsExecutable(
            reinterpret_cast<void*>(function), 1u)) {
        return false;
    }
    function(node, action);
    return true;
}

void AppendTo(void* parent, void* child) {
    g_appendChild(static_cast<std::byte*>(parent) + 4u, child);
}

void Unlink(void* node) {
    void* parent = ReadAt<void*>(node, kNodeParentOffset);
    void* previous = ReadAt<void*>(node, kNodePreviousOffset);
    void* next = ReadAt<void*>(node, kNodeNextOffset);
    if (!parent) return;

    if (previous) {
        WriteAt(previous, kNodeNextOffset, next);
    } else {
        WriteAt(parent, kNodeFirstChildOffset, next);
    }
    if (next) {
        WriteAt(next, kNodePreviousOffset, previous);
    } else {
        WriteAt(parent, kNodeLastChildOffset, previous);
    }
    WriteAt<void*>(node, kNodeParentOffset, nullptr);
    WriteAt<void*>(node, kNodePreviousOffset, nullptr);
    WriteAt<void*>(node, kNodeNextOffset, nullptr);
}

void InsertBetween(void* parent, void* node, void* previous, void* next) {
    WriteAt(node, kNodeParentOffset, parent);
    WriteAt(node, kNodePreviousOffset, previous);
    WriteAt(node, kNodeNextOffset, next);
    if (previous) {
        WriteAt(previous, kNodeNextOffset, node);
    } else {
        WriteAt(parent, kNodeFirstChildOffset, node);
    }
    if (next) {
        WriteAt(next, kNodePreviousOffset, node);
    } else {
        WriteAt(parent, kNodeLastChildOffset, node);
    }
}

Bundle CaptureBundle(void* owner) {
    Bundle bundle{};
    for (std::size_t index = 0; index < kBundleFieldOffsets.size(); ++index) {
        bundle.nodes[index] = ReadAt<void*>(owner, kBundleFieldOffsets[index]);
    }
    return bundle;
}

void WriteBundle(void* owner, const Bundle& bundle) {
    for (std::size_t index = 0; index < kBundleFieldOffsets.size(); ++index) {
        WriteAt(owner, kBundleFieldOffsets[index], bundle.nodes[index]);
    }
}

void ClearBundleFields(void* owner) {
    const Bundle empty{};
    WriteBundle(owner, empty);
}

bool ContainsNode(const Bundle& bundle, void* node) {
    if (!node) return false;
    for (void* candidate : bundle.nodes) {
        if (candidate == node) return true;
    }
    return false;
}

std::size_t UniqueNodeCount(const Bundle& bundle) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < bundle.nodes.size(); ++index) {
        if (!bundle.nodes[index]) continue;
        bool duplicate = false;
        for (std::size_t prior = 0; prior < index; ++prior) {
            duplicate |= bundle.nodes[prior] == bundle.nodes[index];
        }
        if (!duplicate) ++count;
    }
    return count;
}

std::uint32_t PresenceMask(const Bundle& bundle) {
    std::uint32_t mask = 0u;
    for (std::size_t index = 0; index < bundle.nodes.size(); ++index) {
        if (bundle.nodes[index]) mask |= 1u << index;
    }
    return mask;
}

void HideDistinctNodes(const Bundle& bundle, const Bundle& preserve) {
    for (std::size_t index = 0; index < bundle.nodes.size(); ++index) {
        void* node = bundle.nodes[index];
        if (!node || ContainsNode(preserve, node)) continue;
        bool duplicate = false;
        for (std::size_t prior = 0; prior < index; ++prior) {
            duplicate |= bundle.nodes[prior] == node;
        }
        if (!duplicate) CallSetVisible(node, false);
    }
}

bool ShapeMatches(const Bundle& controller, const Bundle& keyboard,
                  bool highFamily) {
    if (!controller.nodes[1] || !keyboard.nodes[1]) return false;
    if (highFamily) {
        if (!controller.nodes[0] || !controller.nodes[2] ||
            keyboard.nodes[0] || keyboard.nodes[2]) {
            return false;
        }
        for (std::size_t index = 3; index < controller.nodes.size(); ++index) {
            if (!!controller.nodes[index] != !!keyboard.nodes[index]) {
                return false;
            }
        }
        return true;
    }
    // Resource 118 at +0x80 is controller-only. sub_7B3BB0 preserves the
    // +0xF8 mode value in EDX across sub_7B2140 and tests it before creating
    // that node; Hex-Rays incorrectly presents the test as a 64-bit return
    if (!controller.nodes[0] || keyboard.nodes[0]) return false;
    for (std::size_t index = 2; index < controller.nodes.size(); ++index) {
        if (!!controller.nodes[index] != !!keyboard.nodes[index]) return false;
    }
    return true;
}

int HighResourceId(std::uint32_t mask) {
    if ((mask & 0x80u) != 0u) return 0;
    if ((mask & 0x20u) != 0u) return 43;
    if ((mask & 0x10u) != 0u) return 50;
    if ((mask & 0x40u) != 0u) return 57;
    return 0;
}

bool BuildType2HighCopies(void* owner, NativeMode mode) {
    void* productManager = g_productManagerGlobal
        ? *g_productManagerGlobal : nullptr;
    void* factory = productManager
        ? ReadAt<void*>(productManager, kProductFactoryOffset) : nullptr;
    void* root = ReadAt<void*>(owner, kOwnerRootOffset);
    if (!factory || !root) return false;

    const std::uint32_t previousSelector =
        ReadAt<std::uint32_t>(factory, kFactorySelectorOffset);
    const std::uint32_t selector =
        mode == NativeMode::Keyboard ? 1u : 0u;
    const int resource = HighResourceId(
        ReadAt<std::uint32_t>(owner, kOwnerCommandMaskOffset));
    bool complete = true;

    for (std::size_t index = 0; index < 4u; ++index) {
        WriteAt(factory, kFactorySelectorOffset, selector);
        void* child = g_createResource(factory, resource);
        WriteAt(factory, kFactorySelectorOffset, previousSelector);
        if (!child) {
            complete = false;
            continue;
        }

        WriteAt(owner, kBundleFieldOffsets[5u + index], child);
        AppendTo(root, child);

        WriteAt<std::uint32_t>(child, 0x98u, 0x42080000u);
        WriteAt<std::uint32_t>(child, 0x9Cu, 0x42080000u);
        WriteAt<std::uint32_t>(child, 0xA0u, 0xC3480000u);
        WriteAt<std::uint32_t>(child, 0xA4u, 0x3F800000u);
        if (index > 1u) {
            WriteAt<std::uint32_t>(child, 0x68u, 0x3F48F5C3u);
            WriteAt<std::uint32_t>(child, 0x6Cu, 0x3F48F5C3u);
            WriteAt<std::uint32_t>(child, 0x70u, 0x3F48F5C3u);
            WriteAt<std::uint32_t>(child, 0x74u, 0x3F800000u);
        }
        complete &= CallSetVisible(child, true);
        WriteAt<std::uint32_t>(child, 0xA8u, 0xC2080000u);
        WriteAt<std::uint32_t>(child, 0xACu, 0xC2080000u);
        WriteAt<std::uint32_t>(child, 0xB0u, 0x80000000u);
        WriteAt<std::uint32_t>(child, 0xB4u, 0xBF800000u);
        const std::uint32_t flags =
            ReadAt<std::uint32_t>(child, kNodeFlagsOffset);
        WriteAt(child, kNodeFlagsOffset, flags | kNodeTransformDirty);
    }
    WriteAt(factory, kFactorySelectorOffset, previousSelector);
    return complete;
}

bool ApplyAlternatePostInit(void* owner, bool highFamily) {
    const std::uint32_t state =
        ReadAt<std::uint32_t>(owner, kOwnerStateOffset);
    bool ok = true;
    if (state == 1u) {
        for (std::size_t index = 0; index < 3u; ++index) {
            void* child = ReadAt<void*>(owner, kBundleFieldOffsets[index]);
            if (child) ok &= CallStartAction(child, 5);
        }
    } else if (state == 3u && highFamily) {
        void* child = ReadAt<void*>(owner, 0x88u);
        if (child) {
            g_removeAction(child, 1);
            ok &= CallStartAction(child, 86);
            g_setActionFlags(child, 86, 1, 1);
        }
    }
    return ok;
}

bool BuildAlternateBranch(void* owner, NativeMode alternateMode,
                          bool highFamily) {
    const std::uint32_t state =
        ReadAt<std::uint32_t>(owner, kOwnerStateOffset);
    if (highFamily) {
        g_buildHighFamily(owner, state == 2u ? 1 : 0);
        g_buildControllerTail(owner);
        if (state == 2u && !BuildType2HighCopies(owner, alternateMode)) {
            return false;
        }
    } else {
        const int activate = state == 5u ? 0 : 1;
        const int resultChildren =
            (state == 1u || state == 3u || state == 6u) ? 1 : 0;
        g_buildLowFamily(owner, activate, resultChildren);
    }
    return ApplyAlternatePostInit(owner, highFamily);
}

bool CaptureNodeRun(void* root, Bundle& bundle,
                    void*& previous, void*& next) {
    bundle.count = 0u;
    bundle.order = {};
    bundle.visible = {};
    previous = nullptr;
    next = nullptr;
    bool inside = false;
    bool finished = false;

    for (void* child = ReadAt<void*>(root, kNodeFirstChildOffset); child;
         child = ReadAt<void*>(child, kNodeNextOffset)) {
        const bool selected = ContainsNode(bundle, child);
        if (selected) {
            if (finished || ReadAt<void*>(child, kNodeParentOffset) != root ||
                bundle.count >= bundle.order.size()) {
                return false;
            }
            if (!inside) {
                inside = true;
                previous = ReadAt<void*>(child, kNodePreviousOffset);
            }
            bundle.order[bundle.count] = child;
            bundle.visible[bundle.count] =
                (ReadAt<std::uint32_t>(child, kNodeFlagsOffset) & 0x2u) != 0u;
            ++bundle.count;
        } else if (inside) {
            finished = true;
            next = child;
        }
    }
    return bundle.count != 0u && bundle.count == UniqueNodeCount(bundle);
}

bool ParkBundle(Bundle& bundle) {
    if (bundle.count == 0u) return false;
    for (std::size_t index = 0; index < bundle.count; ++index) {
        if (!HasVisibilitySlot(bundle.order[index])) return false;
    }
    for (std::size_t index = 0; index < bundle.count; ++index) {
        void* node = bundle.order[index];
        CallSetVisible(node, false);
        Unlink(node);
    }
    return true;
}

PrepareResult PrepareOwner(void* owner) {
    OwnerRecord* record = FindOwner(owner);
    if (!record || record->buildAttempted) return PrepareResult::Ignored;
    record->buildAttempted = true;

    const NativeMode primaryMode = record->activeMode;
    const NativeMode alternateMode = primaryMode == NativeMode::Keyboard
        ? NativeMode::Controller : NativeMode::Keyboard;
    const std::uint32_t capturedMode =
        ReadAt<std::uint32_t>(owner, kOwnerModeOffset);
    if ((capturedMode != 0u) != (primaryMode == NativeMode::Keyboard)) {
        if (FirstRejectionAt(ModeRejected)) {
            LogDebug("XIII-2 QTE bundle rejected: owner=%p stage=mode epoch=%s native=%u",
                     owner, ModeName(primaryMode), capturedMode);
        }
        return PrepareResult::Unsupported;
    }
    void* root = ReadAt<void*>(owner, kOwnerRootOffset);
    if (!root) {
        if (FirstRejectionAt(RootRejected)) {
            LogDebug("XIII-2 QTE bundle rejected: owner=%p stage=root mode=%s",
                     owner, ModeName(primaryMode));
        }
        return PrepareResult::Unsupported;
    }

    Bundle primary = CaptureBundle(owner);

    const std::uint32_t commandMask =
        ReadAt<std::uint32_t>(owner, kOwnerCommandMaskOffset);
    const bool highFamily = (commandMask & 0xF0u) != 0u;

    ClearBundleFields(owner);
    WriteAt<std::uint32_t>(owner, kOwnerModeOffset,
                           alternateMode == NativeMode::Keyboard ? 1u : 0u);
    const bool built = BuildAlternateBranch(owner, alternateMode, highFamily);
    Bundle alternate = CaptureBundle(owner);
    WriteBundle(owner, primary);
    WriteAt<std::uint32_t>(owner, kOwnerModeOffset,
                           primaryMode == NativeMode::Keyboard ? 1u : 0u);

    Bundle controller = primaryMode == NativeMode::Controller
        ? primary : alternate;
    Bundle keyboard = primaryMode == NativeMode::Keyboard
        ? primary : alternate;
    const bool shapeMatches = ShapeMatches(controller, keyboard, highFamily);
    if (!built || !shapeMatches) {
        HideDistinctNodes(alternate, primary);
        const unsigned stage = built
            ? ShapeRejected : AlternateBuildRejected;
        if (FirstRejectionAt(stage)) {
            LogDebug("XIII-2 QTE bundle rejected: owner=%p stage=%s state=%u family=%s mode=%s controller-mask=0x%03X keyboard-mask=0x%03X",
                     owner, built ? "shape" : "alternate-build",
                     ReadAt<std::uint32_t>(owner, kOwnerStateOffset),
                     highFamily ? "high" : "directional",
                     ModeName(primaryMode), PresenceMask(controller),
                     PresenceMask(keyboard));
        }
        return PrepareResult::Unsupported;
    }
    void* primaryPrevious = nullptr;
    void* primaryNext = nullptr;
    void* alternatePrevious = nullptr;
    void* alternateNext = nullptr;
    const bool primaryRun = CaptureNodeRun(
        root, primary, primaryPrevious, primaryNext);
    const bool alternateRun = CaptureNodeRun(
        root, alternate, alternatePrevious, alternateNext);
    if (!primaryRun || !alternateRun) {
        HideDistinctNodes(alternate, primary);
        if (FirstRejectionAt(NodeRunsRejected)) {
            LogDebug("XIII-2 QTE bundle rejected: owner=%p stage=node-runs state=%u family=%s mode=%s primary-mask=0x%03X alternate-mask=0x%03X",
                     owner, ReadAt<std::uint32_t>(owner, kOwnerStateOffset),
                     highFamily ? "high" : "directional",
                     ModeName(primaryMode), PresenceMask(primary),
                     PresenceMask(alternate));
        }
        return PrepareResult::Unsupported;
    }
    if (!ParkBundle(alternate)) {
        HideDistinctNodes(alternate, primary);
        if (FirstRejectionAt(ParkRejected)) {
            LogDebug("XIII-2 QTE bundle rejected: owner=%p stage=park state=%u family=%s mode=%s",
                     owner, ReadAt<std::uint32_t>(owner, kOwnerStateOffset),
                     highFamily ? "high" : "directional",
                     ModeName(primaryMode));
        }
        return PrepareResult::Unsupported;
    }

    if (primaryMode == NativeMode::Controller) {
        record->controller = primary;
        record->keyboard = alternate;
    } else {
        record->keyboard = primary;
        record->controller = alternate;
    }
    record->switchable = true;
    return PrepareResult::Ready;
}

void CopyTransformRange(void* source, void* destination,
                        std::size_t offset, std::size_t bytes) {
    if (!source || !destination) return;
    std::memcpy(static_cast<std::byte*>(destination) + offset,
                static_cast<const std::byte*>(source) + offset, bytes);
    const std::uint32_t flags =
        ReadAt<std::uint32_t>(destination, kNodeFlagsOffset);
    WriteAt(destination, kNodeFlagsOffset, flags | kNodeTransformDirty);
}

void SynchronizeDirectTransforms(void* owner, const Bundle& source,
                                 const Bundle& destination) {
    const std::uint32_t state =
        ReadAt<std::uint32_t>(owner, kOwnerStateOffset);
    if (state == 6u) {
        CopyTransformRange(source.nodes[1], destination.nodes[1], 0x98u, 16u);
    }
    if (state == 2u &&
        (ReadAt<std::uint32_t>(owner, kOwnerCommandMaskOffset) & 0xF0u) != 0u) {
        for (std::size_t index = 5u; index < 9u; ++index) {
            CopyTransformRange(source.nodes[index], destination.nodes[index],
                               0x88u, 16u);
        }
    }
}

bool BundleIsActive(const Bundle& bundle, void* root) {
    if (!root || bundle.count == 0u) return false;
    for (std::size_t index = 0; index < bundle.count; ++index) {
        void* node = bundle.order[index];
        void* expectedPrevious = index == 0u
            ? ReadAt<void*>(node, kNodePreviousOffset)
            : bundle.order[index - 1u];
        void* expectedNext = index + 1u == bundle.count
            ? ReadAt<void*>(node, kNodeNextOffset)
            : bundle.order[index + 1u];
        if (!HasVisibilitySlot(node) ||
            ReadAt<void*>(node, kNodeParentOffset) != root ||
            (index != 0u && ReadAt<void*>(node, kNodePreviousOffset) !=
                expectedPrevious) ||
            (index + 1u != bundle.count &&
             ReadAt<void*>(node, kNodeNextOffset) != expectedNext)) {
            return false;
        }
    }
    return true;
}

bool BundleIsParked(const Bundle& bundle) {
    if (bundle.count == 0u) return false;
    for (std::size_t index = 0; index < bundle.count; ++index) {
        void* node = bundle.order[index];
        if (!HasVisibilitySlot(node) ||
            ReadAt<void*>(node, kNodeParentOffset) ||
            ReadAt<void*>(node, kNodePreviousOffset) ||
            ReadAt<void*>(node, kNodeNextOffset)) {
            return false;
        }
    }
    return true;
}

bool OwnerFieldsMatch(const OwnerRecord& record, const Bundle& bundle) {
    for (std::size_t index = 0; index < bundle.nodes.size(); ++index) {
        if (ReadAt<void*>(record.owner, kBundleFieldOffsets[index]) !=
            bundle.nodes[index]) {
            return false;
        }
    }
    return true;
}

bool OwnerIsTerminal(const OwnerRecord& record) {
    if (ReadAt<std::uint32_t>(record.owner, kOwnerClosePhaseOffset) != 0u) {
        return true;
    }
    return ReadAt<std::uint32_t>(record.owner, kOwnerStateOffset) == 2u &&
        ReadAt<std::uint8_t>(record.owner, kOwnerType2OutcomeOffset) != 0u;
}

bool BundleIsHidden(const Bundle& bundle) {
    if (bundle.count == 0u) return false;
    for (std::size_t index = 0; index < bundle.count; ++index) {
        void* node = bundle.order[index];
        if (!patch::memory::IsReadable(
                node, kNodeFlagsOffset + sizeof(std::uint32_t)) ||
            (ReadAt<std::uint32_t>(node, kNodeFlagsOffset) & 0x2u) != 0u) {
            return false;
        }
    }
    return true;
}

bool OwnerCanBeIgnored(const OwnerRecord& record) {
    return OwnerIsTerminal(record) &&
        BundleIsHidden(BundleFor(record, record.activeMode));
}

bool OwnerCanSwitch(const OwnerRecord& record, NativeMode target) {
    if (!record.owner || !record.buildAttempted || !record.switchable ||
        !patch::memory::IsReadable(record.owner, kOwnerModeOffset + 4u)) {
        return false;
    }
    void* root = ReadAt<void*>(record.owner, kOwnerRootOffset);
    const Bundle& active = BundleFor(record, record.activeMode);
    const Bundle& destination = BundleFor(record, target);
    if (!BundleIsActive(active, root) || !BundleIsParked(destination) ||
        !OwnerFieldsMatch(record, active) ||
        ReadAt<std::uint32_t>(record.owner, kOwnerModeOffset) !=
            (record.activeMode == NativeMode::Keyboard ? 1u : 0u)) {
        return false;
    }
    return !OwnerIsTerminal(record);
}

void SwitchOwner(OwnerRecord& record, NativeMode target) {
    Bundle& source = BundleFor(record, record.activeMode);
    Bundle& destination = BundleFor(record, target);
    void* root = ReadAt<void*>(record.owner, kOwnerRootOffset);
    void* previous = ReadAt<void*>(source.order[0], kNodePreviousOffset);
    void* next = ReadAt<void*>(source.order[source.count - 1u],
                               kNodeNextOffset);
    SynchronizeDirectTransforms(record.owner, source, destination);
    for (std::size_t index = 0; index < source.count; ++index) {
        source.visible[index] =
            (ReadAt<std::uint32_t>(source.order[index], kNodeFlagsOffset) &
             0x2u) != 0u;
        CallSetVisible(source.order[index], false);
        Unlink(source.order[index]);
    }
    void* insertAfter = previous;
    for (std::size_t index = 0; index < destination.count; ++index) {
        void* node = destination.order[index];
        InsertBetween(root, node, insertAfter, next);
        CallSetVisible(node, destination.visible[index]);
        insertAfter = node;
    }
    WriteBundle(record.owner, destination);
    WriteAt<std::uint32_t>(record.owner, kOwnerModeOffset,
                           target == NativeMode::Keyboard ? 1u : 0u);
    record.activeMode = target;
}

int __fastcall PayloadDispatchHook(void* owner, void*, int type,
                                   const void* payload) {
    PayloadDispatchFn original = g_originalPayloadDispatch;
    if (!original) return 0;
    const int result = original(owner, type, payload);

    static const auto buildPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13-2.qte-direct-bundle-build");
    hook_profiler::Scope buildTiming(buildPoint);
    AcquireSRWLockExclusive(&g_ownerLock);
    (void)PrepareOwner(owner);
    ReleaseSRWLockExclusive(&g_ownerLock);
    return result;
}

template <std::size_t Size>
bool Matches(const ImageView& image, std::uint32_t rva,
             const std::array<std::uint8_t, Size>& prefix) {
    const auto range = image.At(rva, prefix.size());
    return range && std::memcmp(range->data(), prefix.data(), prefix.size()) == 0;
}

} // namespace

bool HasExpectedContract(const ImageView& image) {
    return Matches(image, kPayloadDispatchRva, kPayloadDispatchPrefix) &&
        Matches(image, kBuildHighFamilyRva, kBuildHighFamilyPrefix) &&
        Matches(image, kBuildLowFamilyRva, kBuildLowFamilyPrefix) &&
        Matches(image, kBuildControllerTailRva, kBuildControllerTailPrefix) &&
        Matches(image, kCreateResourceRva, kCreateResourcePrefix) &&
        Matches(image, kAppendChildRva, kAppendChildPrefix) &&
        Matches(image, kRemoveActionRva, kRemoveActionPrefix) &&
        Matches(image, kSetActionFlagsRva, kSetActionFlagsPrefix) &&
        image.At(kProductManagerGlobalRva, sizeof(void*));
}

bool Install(const ImageView& image) {
    if (!HasExpectedContract(image)) return false;

    const auto address = [&image](std::uint32_t rva, std::size_t bytes) {
        const auto range = image.At(rva, bytes);
        return range ? const_cast<std::byte*>(range->data()) : nullptr;
    };
    std::byte* dispatch = address(kPayloadDispatchRva,
                                  kPayloadDispatchPrefix.size());
    g_buildHighFamily = reinterpret_cast<BuildHighFamilyFn>(
        address(kBuildHighFamilyRva, 1u));
    g_buildLowFamily = reinterpret_cast<BuildLowFamilyFn>(
        address(kBuildLowFamilyRva, 1u));
    g_buildControllerTail = reinterpret_cast<BuildControllerTailFn>(
        address(kBuildControllerTailRva, 1u));
    g_createResource = reinterpret_cast<CreateResourceFn>(
        address(kCreateResourceRva, 1u));
    g_appendChild = reinterpret_cast<AppendChildFn>(
        address(kAppendChildRva, 1u));
    g_removeAction = reinterpret_cast<RemoveActionFn>(
        address(kRemoveActionRva, 1u));
    g_setActionFlags = reinterpret_cast<SetActionFlagsFn>(
        address(kSetActionFlagsRva, 1u));
    g_productManagerGlobal = reinterpret_cast<void* volatile*>(
        address(kProductManagerGlobalRva, sizeof(void*)));

    if (!dispatch || !g_buildHighFamily || !g_buildLowFamily ||
        !g_buildControllerTail ||
        !g_createResource || !g_appendChild || !g_removeAction ||
        !g_setActionFlags || !g_productManagerGlobal) {
        return false;
    }

    if (g_payloadDispatchDetour.Install(
            "ff13-2-qte-direct-visual-bundles", dispatch,
            reinterpret_cast<void*>(&PayloadDispatchHook),
            std::as_bytes(std::span{kPayloadDispatchPrefix}),
            reinterpret_cast<void**>(&g_originalPayloadDispatch))) {
        return true;
    }
    g_originalPayloadDispatch = nullptr;
    return false;
}

bool Revert() {
    return g_payloadDispatchDetour.RevertAndClear(g_originalPayloadDispatch);
}

void RegisterOwner(void* owner, NativeMode mode) {
    if (!owner || mode == NativeMode::Unknown) return;
    AcquireSRWLockExclusive(&g_ownerLock);
    OwnerRecord* record = ReserveOwner(owner);
    if (record) record->activeMode = mode;
    ReleaseSRWLockExclusive(&g_ownerLock);
    if (record) {
        g_ownerRegistryFullLogged.store(false, std::memory_order_release);
    } else if (!g_ownerRegistryFullLogged.exchange(
                   true, std::memory_order_acq_rel)) {
        LogWarning("XIII-2 QTE owner registry is full: live switching will remain frozen");
    }
}

void UnregisterOwner(void* owner) {
    AcquireSRWLockExclusive(&g_ownerLock);
    if (OwnerRecord* record = FindOwner(owner)) {
        if (record->switchable) {
            void* root = ReadAt<void*>(owner, kOwnerRootOffset);
            Bundle& inactive = BundleFor(
                *record, record->activeMode == NativeMode::Keyboard
                    ? NativeMode::Controller : NativeMode::Keyboard);
            if (root && patch::memory::IsReadable(
                    root, kNodeLastChildOffset + sizeof(void*)) &&
                BundleIsParked(inactive)) {
                for (std::size_t index = 0; index < inactive.count; ++index) {
                    CallSetVisible(inactive.order[index], false);
                    AppendTo(root, inactive.order[index]);
                }
            }
        }
        *record = {};
    }
    ReleaseSRWLockExclusive(&g_ownerLock);
}

SwitchResult SwitchAll(NativeMode mode) {
    if (mode == NativeMode::Unknown) return SwitchResult::Deferred;
    AcquireSRWLockExclusive(&g_ownerLock);

    std::size_t owners = 0u;
    std::size_t unsupportedOwners = 0u;
    std::size_t deferredOwners = 0u;
    bool needsSwitch = false;
    bool unsupported = false;
    bool deferred = false;
    for (const auto& record : g_owners) {
        if (!record.owner) continue;
        ++owners;
        needsSwitch |= record.activeMode != mode;
        if (record.activeMode != mode) {
            if (!record.buildAttempted || !record.switchable) {
                unsupported = true;
                ++unsupportedOwners;
                continue;
            }
            if (!OwnerCanBeIgnored(record) &&
                !OwnerCanSwitch(record, mode)) {
                deferred = true;
                ++deferredOwners;
            }
        }
    }

    if (!owners) {
        g_lastBlockedMode = NativeMode::Unknown;
        ReleaseSRWLockExclusive(&g_ownerLock);
        return SwitchResult::NoOwners;
    }
    if (!needsSwitch) {
        g_lastBlockedMode = NativeMode::Unknown;
        ReleaseSRWLockExclusive(&g_ownerLock);
        return SwitchResult::Unchanged;
    }
    if (unsupported || deferred) {
        if (g_lastBlockedMode != mode) {
            LogDebug("XIII-2 QTE live visual switch deferred: owners=%zu unsupported=%zu terminal-visible=%zu mode=%s",
                     owners, unsupportedOwners, deferredOwners,
                     ModeName(mode));
            g_lastBlockedMode = mode;
        }
        ReleaseSRWLockExclusive(&g_ownerLock);
        return unsupported ? SwitchResult::Unsupported : SwitchResult::Deferred;
    }

    static const auto switchPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13-2.qte-live-bundle-switch");
    hook_profiler::Scope switchTiming(switchPoint);
    for (auto& record : g_owners) {
        if (record.owner && record.activeMode != mode &&
            !OwnerCanBeIgnored(record)) {
            SwitchOwner(record, mode);
        }
    }
    g_lastBlockedMode = NativeMode::Unknown;
    ReleaseSRWLockExclusive(&g_ownerLock);

    return SwitchResult::Switched;
}

} // namespace novafix::game::xiii2_qte_visuals

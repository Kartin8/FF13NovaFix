#include "game/titles/ff13_2/ui/main_menu_frame.h"

#include "diagnostics/log.h"
#include "display/aspect_runtime.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::xiii2_main_menu_frame {
namespace {

constexpr std::uint32_t kMainMenuDrawRva = 0x004424C0u;
constexpr std::array<std::byte, 17> kMainMenuDrawPrefix{
    std::byte{0x81}, std::byte{0xEC}, std::byte{0x90}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x53}, std::byte{0x55},
    std::byte{0x56}, std::byte{0x8D}, std::byte{0x44}, std::byte{0x24},
    std::byte{0x1C}, std::byte{0x57}, std::byte{0x50}, std::byte{0x8B},
    std::byte{0xE9},
};

constexpr std::uint32_t kGuiListUpdateRva = 0x004D3E90u;
constexpr std::array<std::byte, 20> kGuiListUpdatePrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xE4}, std::byte{0xF0}, std::byte{0x83}, std::byte{0xEC},
    std::byte{0x74}, std::byte{0x53}, std::byte{0x8B}, std::byte{0xD9},
    std::byte{0x33}, std::byte{0xC9}, std::byte{0x56}, std::byte{0x57},
    std::byte{0x89}, std::byte{0x4C}, std::byte{0x24}, std::byte{0x10},
};

constexpr std::uint32_t kInventoryConstructRva = 0x004387F0u;
constexpr std::array<std::byte, 19> kInventoryConstructPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xE4}, std::byte{0xF0}, std::byte{0x83}, std::byte{0xEC},
    std::byte{0x54}, std::byte{0x53}, std::byte{0x56}, std::byte{0x57},
    std::byte{0x8B}, std::byte{0xF1}, std::byte{0xE8}, std::byte{0xED},
    std::byte{0xA6}, std::byte{0x05}, std::byte{0x00},
};

constexpr std::uint32_t kMainTopMenuVtableRva = 0x009FD4D8u;
constexpr std::uint32_t kMainTopMenuPartyVtableRva =
    0x00E47F40u - 0x00400000u;
constexpr std::size_t kProductChromeContainerOffset = 0xB4u;
constexpr std::size_t kProductActiveSubproductOffset = 0x84u;
constexpr std::size_t kProductRootOffset = 0x34u;
constexpr std::size_t kFirstChildOffset = 0x08u;
constexpr std::size_t kParentOffset = 0x04u;
constexpr std::size_t kNextSiblingOffset = 0x14u;
constexpr std::size_t kScaleXOffset = 0x68u;
constexpr std::size_t kPositionXOffset = 0xA8u;
constexpr std::size_t kPositionYOffset = 0xACu;
constexpr std::size_t kDirtyFlagsOffset = 0xCCu;
constexpr std::size_t kListMaximumTextWidthOffset = 0x64Cu;
constexpr std::size_t kMainTopMenuPartyRootOffset = 0x78u;
constexpr std::size_t kTopMenuHeaderOffset = 0x90u;
constexpr std::size_t kInventoryPartyRootOffset = 0xA4u;
constexpr std::size_t kInventoryRightEdgeOffset = 0x80u;
constexpr std::uint32_t kTransformDirty = 0x04u;
constexpr std::size_t kChromeRootCount = 2u;
constexpr std::size_t kMaximumDirectChildren = 32u;
constexpr std::size_t kMaximumTrackedLists = 96u;
constexpr unsigned kMaximumParentDepth = 16u;
constexpr DWORD kMainMenuDrawGraceMilliseconds = 250u;
constexpr float kMinimumSafeScaleX = 0.25f;
constexpr float kAuthoredCenterX = 640.0f;
constexpr float kAuthoredCenterY = 360.0f;
constexpr float kHiddenHeaderThresholdX = -640.0f;
constexpr float kLeftListMinimumX = -512.0f;
constexpr float kLeftListMaximumX = 320.0f;
constexpr float kInventoryRightChildMinimumX = 350.0f;
constexpr float kTransformEpsilon = 0.0001f;

using MainMenuDrawFn = int (__thiscall*)(void* product, int layer);
using GuiListUpdateFn = int (__thiscall*)(void* list, float delta);
using InventoryConstructFn = void* (__thiscall*)(void* product);

struct ChildBaseline {
    std::byte* node{};
    float scaleX{1.0f};
    float positionX{};
    float appliedPositionX{};
    bool positionApplied{};
};

struct RootBaseline {
    std::byte* node{};
    float scaleX{1.0f};
    float positionX{};
    float positionY{};
    std::array<ChildBaseline, kMaximumDirectChildren> children{};
    std::size_t childCount{};
    std::array<ChildBaseline, 5> headerContent{};
    bool headerContentValid{};
};

struct FrameBaseline {
    void* product{};
    std::array<RootBaseline, kChromeRootCount> roots{};
    bool valid{};
};

struct ParkingBaseline {
    void* product{};
    std::byte* hiddenHeader{};
    float positionX{};
    bool valid{};
};

struct PartyAnchorValidation {
    void* mainMenu{};
    std::byte* topMenu{};
    std::byte* topMenuRoot{};
    std::byte* partyRoot{};
};

struct ListBaseline {
    std::byte* node{};
    std::byte* parent{};
    float positionX{};
    float maximumTextWidth{};
    float appliedOverscan{};
    bool eligible{};
};

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_drawDetour;
patch::Detour32 g_listUpdateDetour;
patch::Detour32 g_inventoryConstructDetour;
MainMenuDrawFn g_originalDraw{};
GuiListUpdateFn g_originalListUpdate{};
InventoryConstructFn g_originalInventoryConstruct{};
FrameBaseline g_frameBaseline{};
ParkingBaseline g_parkingBaseline{};
PartyAnchorValidation g_partyAnchorValidation{};
std::array<ListBaseline, kMaximumTrackedLists> g_listBaselines{};
std::size_t g_nextListBaseline{};
const std::byte* g_imageBase{};
std::atomic<DWORD> g_lastMainMenuDrawTick{};

template <typename T>
bool ReadAt(const std::byte* object, std::size_t offset, T& value) {
    const auto* address = object + offset;
    if (!patch::memory::IsReadable(address, sizeof(T))) return false;
    std::memcpy(&value, address, sizeof(T));
    return true;
}

template <typename T>
bool WriteAt(std::byte* object, std::size_t offset, const T& value) {
    auto* address = object + offset;
    if (!patch::memory::IsWritable(address, sizeof(T))) return false;
    std::memcpy(address, &value, sizeof(T));
    return true;
}

template <typename T>
T& TrustedField(std::byte* object, std::size_t offset) {
    return *reinterpret_cast<T*>(object + offset);
}

float WideOverscan() {
    const display::AspectRatio aspect = display::CurrentAspectRatio();
    if (!aspect.valid || aspect.safeScaleX < kMinimumSafeScaleX ||
        aspect.safeScaleX >= 1.0f) {
        return 0.0f;
    }
    return kAuthoredCenterX * ((1.0f / aspect.safeScaleX) - 1.0f);
}

float TallOverscan() {
    const display::AspectRatio aspect = display::CurrentAspectRatio();
    if (!aspect.valid || aspect.safeScaleY < kMinimumSafeScaleX ||
        aspect.safeScaleY >= 1.0f) {
        return 0.0f;
    }
    return kAuthoredCenterY * ((1.0f / aspect.safeScaleY) - 1.0f);
}

bool MainMenuWasRecentlyDrawn() {
    const DWORD last = g_lastMainMenuDrawTick.load(std::memory_order_acquire);
    return last != 0u && GetTickCount() - last <= kMainMenuDrawGraceMilliseconds;
}

bool MarkTransformDirty(std::byte* node) {
    std::uint32_t flags{};
    if (!ReadAt(node, kDirtyFlagsOffset, flags)) return false;
    flags |= kTransformDirty;
    return WriteAt(node, kDirtyFlagsOffset, flags);
}

bool WriteFloatIfChanged(std::byte* node, std::size_t offset,
                         float value, bool& changed) {
    float current{};
    if (!std::isfinite(value) || !ReadAt(node, offset, current) ||
        !std::isfinite(current)) {
        return false;
    }
    if (std::fabs(current - value) <= kTransformEpsilon) return true;
    if (!WriteAt(node, offset, value)) return false;
    changed = true;
    return true;
}

// These nodes were validated when FrameBaseline was captured and belong to the menu being drawn.
// Don't VirtualQuery every transform field again on every frame.
bool WriteTrustedFloatIfChanged(std::byte* node, std::size_t offset,
                                float value, bool& changed) {
    if (!node || !std::isfinite(value)) return false;
    float& current = TrustedField<float>(node, offset);
    if (!std::isfinite(current)) return false;
    if (std::fabs(current - value) <= kTransformEpsilon) return true;
    current = value;
    changed = true;
    return true;
}

void MarkTrustedTransformDirty(std::byte* node) {
    TrustedField<std::uint32_t>(node, kDirtyFlagsOffset) |= kTransformDirty;
}

bool SetNodePositionX(std::byte* node, float positionX) {
    bool changed{};
    return node && WriteFloatIfChanged(
                       node, kPositionXOffset, positionX, changed) &&
        (!changed || MarkTransformDirty(node));
}

bool CaptureRoot(std::byte* root, RootBaseline& output) {
    if (!root || !ReadAt(root, kScaleXOffset, output.scaleX) ||
        !ReadAt(root, kPositionXOffset, output.positionX) ||
        !ReadAt(root, kPositionYOffset, output.positionY) ||
        !std::isfinite(output.scaleX) ||
        !std::isfinite(output.positionX) ||
        !std::isfinite(output.positionY) || output.scaleX == 0.0f) {
        return false;
    }

    output.node = root;
    std::byte* child{};
    if (!ReadAt(root, kFirstChildOffset, child)) return false;
    while (child && output.childCount < output.children.size()) {
        std::byte* parent{};
        ChildBaseline snapshot{};
        snapshot.node = child;
        if (!ReadAt(child, kParentOffset, parent) || parent != root ||
            !ReadAt(child, kScaleXOffset, snapshot.scaleX) ||
            !ReadAt(child, kPositionXOffset, snapshot.positionX) ||
            !std::isfinite(snapshot.scaleX) ||
            !std::isfinite(snapshot.positionX)) {
            return false;
        }
        output.children[output.childCount++] = snapshot;
        if (!ReadAt(child, kNextSiblingOffset, child)) return false;
    }
    if (child) return false;
    if (output.childCount < 2u) return true;
    std::byte* content{};
    if (!ReadAt(output.children[1].node, kFirstChildOffset, content)) {
        return false;
    }
    for (ChildBaseline& entry : output.headerContent) {
        std::byte* parent{};
        if (!content || !ReadAt(content, kParentOffset, parent) ||
            parent != output.children[1].node ||
            !ReadAt(content, kPositionXOffset, entry.positionX) ||
            !std::isfinite(entry.positionX)) {
            return true;
        }
        entry.node = content;
        if (!ReadAt(content, kNextSiblingOffset, content)) return false;
    }
    output.headerContentValid = content == nullptr;
    return true;
}

bool CaptureFrameBaseline(void* product) {
    if (!product || !patch::memory::IsReadable(
            product, kProductChromeContainerOffset + sizeof(void*))) {
        return false;
    }

    FrameBaseline next{};
    next.product = product;
    std::byte* container{};
    std::memcpy(&container,
                static_cast<std::byte*>(product) +
                    kProductChromeContainerOffset,
                sizeof(container));
    if (!container || !patch::memory::IsReadable(
            container, kFirstChildOffset + sizeof(void*))) {
        return false;
    }

    std::byte* root{};
    std::memcpy(&root, container + kFirstChildOffset, sizeof(root));
    for (RootBaseline& snapshot : next.roots) {
        if (!CaptureRoot(root, snapshot)) return false;
        std::byte* sibling{};
        if (!ReadAt(root, kNextSiblingOffset, sibling)) return false;
        root = sibling;
    }

    next.valid = true;
    g_frameBaseline = next;
    return true;
}

bool FrameBaselineMatches(void* product) {
    if (!g_frameBaseline.valid || g_frameBaseline.product != product ||
        !product) {
        return false;
    }

    std::byte* container{};
    std::byte* root{};
    // The baseline was validated when captured. While this product is being drawn,
    // just verify the expected node layout instead of VirtualQuerying every pointer again.
    container = TrustedField<std::byte*>(
        static_cast<std::byte*>(product), kProductChromeContainerOffset);
    if (!container) return false;
    root = TrustedField<std::byte*>(container, kFirstChildOffset);
    for (const RootBaseline& baseline : g_frameBaseline.roots) {
        if (!root || root != baseline.node) return false;
        std::byte* child = TrustedField<std::byte*>(
            baseline.node, kFirstChildOffset);
        for (std::size_t index = 0u; index < baseline.childCount; ++index) {
            if (child != baseline.children[index].node) return false;
            child = TrustedField<std::byte*>(child, kNextSiblingOffset);
        }
        if (child) return false;
        if (baseline.headerContentValid) {
            std::byte* content = TrustedField<std::byte*>(
                baseline.children[1].node, kFirstChildOffset);
            for (const ChildBaseline& item : baseline.headerContent) {
                if (content != item.node) return false;
                content = TrustedField<std::byte*>(
                    content, kNextSiblingOffset);
            }
            if (content) return false;
        }
        root = TrustedField<std::byte*>(root, kNextSiblingOffset);
    }
    return true;
}

bool ApplyRoot(RootBaseline& root, float safeScaleX,
               float verticalOffset, bool footer) {
    const float rootScale = root.scaleX / safeScaleX;
    const float rootPosition = root.positionX - root.scaleX *
        kAuthoredCenterX * ((1.0f / safeScaleX) - 1.0f);
    bool rootChanged{};
    if (!root.node ||
        !WriteTrustedFloatIfChanged(root.node, kScaleXOffset,
                                    rootScale, rootChanged) ||
        !WriteTrustedFloatIfChanged(root.node, kPositionXOffset,
                                    rootPosition, rootChanged) ||
        !WriteTrustedFloatIfChanged(root.node, kPositionYOffset,
                                    root.positionY + verticalOffset,
                                    rootChanged)) {
        return false;
    }
    if (rootChanged) MarkTrustedTransformDirty(root.node);

    for (std::size_t index = 0; index < root.childCount; ++index) {
        ChildBaseline& child = root.children[index];
        const bool backing = index == 0u;
        const float currentPosition = TrustedField<float>(
            child.node, kPositionXOffset);
        if (!std::isfinite(currentPosition)) return false;
        if (footer && !backing && child.positionApplied &&
            std::fabs(currentPosition - child.appliedPositionX) >
                kTransformEpsilon) {
            child.positionX = currentPosition;
        }
        const float childScale = backing
            ? child.scaleX : child.scaleX * safeScaleX;
        float childPosition = backing
            ? child.positionX
            : kAuthoredCenterX +
                (child.positionX - kAuthoredCenterX) * safeScaleX;
        if (footer && !backing && index + 1u == root.childCount &&
            TrustedField<const void*>(child.node, 0u) ==
                g_imageBase + 0x009E8F58u) {
            childPosition += kAuthoredCenterX * (1.0f - safeScaleX);
        }
        bool childChanged{};
        if (!WriteTrustedFloatIfChanged(child.node, kScaleXOffset,
                                        childScale, childChanged) ||
            !WriteTrustedFloatIfChanged(child.node, kPositionXOffset,
                                        childPosition, childChanged)) {
            return false;
        }
        if (childChanged) MarkTrustedTransformDirty(child.node);
        child.appliedPositionX = childPosition;
        child.positionApplied = true;
    }
    if (!footer && root.headerContentValid && safeScaleX < 1.0f) {
        const float overscan = kAuthoredCenterX *
            ((1.0f / safeScaleX) - 1.0f);
        for (const ChildBaseline& node : root.headerContent) {
            bool changed{};
            if (!WriteTrustedFloatIfChanged(node.node, kPositionXOffset,
                                            node.positionX - overscan,
                                            changed)) return false;
            if (changed) MarkTrustedTransformDirty(node.node);
        }
    }
    return true;
}

bool CaptureParkingBaseline(void* mainMenu) {
    ParkingBaseline next{};
    std::byte* topMenu{};
    if (!mainMenu || !g_imageBase ||
        !ReadAt(static_cast<std::byte*>(mainMenu),
                kProductActiveSubproductOffset, topMenu) || !topMenu) {
        return false;
    }

    const void* vtable{};
    if (!ReadAt(topMenu, 0u, vtable) ||
        vtable != g_imageBase + kMainTopMenuVtableRva) {
        return false;
    }

    std::byte* header{};
    std::byte* child{};
    if (!ReadAt(topMenu, kTopMenuHeaderOffset, header) || !header ||
        !ReadAt(header, kFirstChildOffset, child)) {
        return false;
    }
    while (child) {
        float positionX{};
        if (!ReadAt(child, kPositionXOffset, positionX)) return false;
        if (positionX <= kHiddenHeaderThresholdX) {
            next.product = topMenu;
            next.hiddenHeader = child;
            next.positionX = positionX;
            next.valid = true;
            g_parkingBaseline = next;
            return true;
        }
        if (!ReadAt(child, kNextSiblingOffset, child)) return false;
    }
    return false;
}

bool ParkingBaselineMatches(std::byte* topMenu, float overscan) {
    if (!g_parkingBaseline.valid ||
        g_parkingBaseline.product != topMenu ||
        !g_parkingBaseline.hiddenHeader) {
        return false;
    }

    std::byte* header{};
    std::byte* parent{};
    float positionX{};
    if (!ReadAt(topMenu, kTopMenuHeaderOffset, header) || !header ||
        !ReadAt(g_parkingBaseline.hiddenHeader, kParentOffset, parent) ||
        parent != header ||
        !ReadAt(g_parkingBaseline.hiddenHeader,
                kPositionXOffset, positionX) ||
        !std::isfinite(positionX)) {
        return false;
    }
    return std::fabs(positionX - g_parkingBaseline.positionX) <=
            kTransformEpsilon ||
        std::fabs(positionX -
                  (g_parkingBaseline.positionX - overscan)) <=
            kTransformEpsilon;
}

void ApplySubmenuParking(void* mainMenu, float overscan) {
    if (overscan <= 0.0f) {
        g_parkingBaseline = {};
        return;
    }

    std::byte* topMenu{};
    if (!ReadAt(static_cast<std::byte*>(mainMenu),
                kProductActiveSubproductOffset, topMenu)) {
        g_parkingBaseline = {};
        return;
    }
    if (!ParkingBaselineMatches(topMenu, overscan)) {
        g_parkingBaseline = {};
        if (!CaptureParkingBaseline(mainMenu)) return;
    }

    if (!SetNodePositionX(g_parkingBaseline.hiddenHeader,
                          g_parkingBaseline.positionX - overscan)) {
        g_parkingBaseline = {};
        return;
    }
}

void ApplyMainTopMenuPartyAnchor(void* mainMenu, float verticalOverscan) {
    std::byte* topMenu{};
    const void* topMenuVtable{};
    std::byte* topMenuRoot{};
    std::byte* partyRoot{};
    const void* partyVtable{};
    std::byte* partyParent{};
    if (!mainMenu || !g_imageBase) return;
    topMenu = TrustedField<std::byte*>(
        static_cast<std::byte*>(mainMenu), kProductActiveSubproductOffset);
    if (!topMenu) {
        g_partyAnchorValidation = {};
        return;
    }
    topMenuVtable = TrustedField<void*>(topMenu, 0u);
    if (topMenuVtable != g_imageBase + kMainTopMenuVtableRva) {
        g_partyAnchorValidation = {};
        return;
    }

    topMenuRoot = TrustedField<std::byte*>(
        topMenu, kProductRootOffset);
    partyRoot = TrustedField<std::byte*>(
        topMenu, kMainTopMenuPartyRootOffset);
    if (!topMenuRoot || !partyRoot) {
        g_partyAnchorValidation = {};
        return;
    }
    const bool previouslyValidated =
        g_partyAnchorValidation.mainMenu == mainMenu &&
        g_partyAnchorValidation.topMenu == topMenu &&
        g_partyAnchorValidation.topMenuRoot == topMenuRoot &&
        g_partyAnchorValidation.partyRoot == partyRoot;
    if (!previouslyValidated &&
        !patch::memory::IsWritable(
            partyRoot, kDirtyFlagsOffset + sizeof(std::uint32_t))) {
        g_partyAnchorValidation = {};
        return;
    }
    partyVtable = TrustedField<void*>(partyRoot, 0u);
    partyParent = TrustedField<std::byte*>(partyRoot, kParentOffset);
    if (partyVtable != g_imageBase + kMainTopMenuPartyVtableRva ||
        partyParent != topMenuRoot) {
        g_partyAnchorValidation = {};
        return;
    }
    if (!previouslyValidated) {
        g_partyAnchorValidation = {
            mainMenu, topMenu, topMenuRoot, partyRoot};
    }

    bool changed{};
    if (!WriteTrustedFloatIfChanged(partyRoot, kPositionYOffset,
                                    verticalOverscan, changed)) {
        return;
    }
    if (changed) MarkTrustedTransformDirty(partyRoot);
}

void ApplyWideFrame(void* product) {
    const display::AspectRatio aspect = display::CurrentAspectRatio();
    if (!aspect.valid || !aspect.correctionRequired) {
        g_frameBaseline = {};
        g_partyAnchorValidation = {};
        return;
    }
    if (!FrameBaselineMatches(product)) {
        g_frameBaseline = {};
        g_partyAnchorValidation = {};
        if (!CaptureFrameBaseline(product)) return;
    }

    const float safeScaleX = aspect.valid &&
            aspect.safeScaleX >= kMinimumSafeScaleX &&
            aspect.safeScaleX < 1.0f
        ? aspect.safeScaleX : 1.0f;
    const float verticalOverscan = TallOverscan();
    for (std::size_t index = 0u;
         index < g_frameBaseline.roots.size(); ++index) {
        const float verticalOffset = index == 0u
            ? -verticalOverscan : verticalOverscan;
        if (!ApplyRoot(g_frameBaseline.roots[index], safeScaleX,
                       verticalOffset, index == 1u)) {
            g_frameBaseline = {};
            g_partyAnchorValidation = {};
            return;
        }
    }
    ApplySubmenuParking(product, WideOverscan());
    ApplyMainTopMenuPartyAnchor(product, verticalOverscan);

}

float ApproximateWorldX(std::byte* node) {
    float positionX{};
    for (unsigned depth = 0u; node && depth < kMaximumParentDepth; ++depth) {
        positionX += TrustedField<float>(node, kPositionXOffset);
        node = TrustedField<std::byte*>(node, kParentOffset);
    }
    return positionX;
}

ListBaseline* FindListBaseline(std::byte* list, std::byte* parent) {
    for (ListBaseline& baseline : g_listBaselines) {
        if (baseline.node == list && baseline.parent == parent) return &baseline;
    }
    return nullptr;
}

bool Matches(float value, float expected) {
    return std::fabs(value - expected) <= kTransformEpsilon;
}

void CaptureListBaseline(ListBaseline& baseline, std::byte* list,
                         std::byte* parent) {
    baseline = {};
    baseline.node = list;
    baseline.parent = parent;
    baseline.positionX = TrustedField<float>(list, kPositionXOffset);
    baseline.maximumTextWidth =
        TrustedField<float>(list, kListMaximumTextWidthOffset);
    const float worldX = ApproximateWorldX(list);
    baseline.eligible = std::isfinite(baseline.positionX) &&
        std::isfinite(baseline.maximumTextWidth) &&
        baseline.maximumTextWidth > 0.0f &&
        worldX >= kLeftListMinimumX && worldX <= kLeftListMaximumX;
}

void ApplyListLayout(std::byte* list) {
    if (!list || !MainMenuWasRecentlyDrawn()) return;

    const float overscan = WideOverscan();
    std::byte* parent = TrustedField<std::byte*>(list, kParentOffset);
    ListBaseline* baseline = FindListBaseline(list, parent);
    if (!baseline) {
        if (overscan <= 0.0f) return;
        baseline = &g_listBaselines[
            g_nextListBaseline++ % g_listBaselines.size()];
        CaptureListBaseline(*baseline, list, parent);
    } else {
        const float positionX = TrustedField<float>(list, kPositionXOffset);
        const float maximumTextWidth =
            TrustedField<float>(list, kListMaximumTextWidthOffset);
        const bool knownPosition = Matches(positionX, baseline->positionX) ||
            Matches(positionX,
                    baseline->positionX - baseline->appliedOverscan);
        const bool knownWidth = Matches(maximumTextWidth,
                                        baseline->maximumTextWidth) ||
            Matches(maximumTextWidth,
                    baseline->maximumTextWidth +
                        baseline->appliedOverscan);
        if (!knownPosition || !knownWidth) {
            CaptureListBaseline(*baseline, list, parent);
        }
    }
    if (!baseline->eligible) return;

    const float correctedPositionX = baseline->positionX - overscan;
    const bool positionChanged = !Matches(
        TrustedField<float>(list, kPositionXOffset), correctedPositionX);
    TrustedField<float>(list, kPositionXOffset) = correctedPositionX;
    TrustedField<float>(list, kListMaximumTextWidthOffset) =
        baseline->maximumTextWidth + overscan;
    if (positionChanged) {
        TrustedField<std::uint32_t>(list, kDirtyFlagsOffset) |= kTransformDirty;
    }
    baseline->appliedOverscan = overscan;

}

void ShiftTrustedNode(std::byte* node, float offset) {
    if (!node || offset == 0.0f) return;
    TrustedField<float>(node, kPositionXOffset) += offset;
    TrustedField<std::uint32_t>(node, kDirtyFlagsOffset) |= kTransformDirty;
}

void ApplyInventoryRightAnchors(void* product) {
    const float overscan = WideOverscan();
    if (!product || overscan <= 0.0f) return;

    auto* bytes = static_cast<std::byte*>(product);
    std::byte* partyRoot =
        TrustedField<std::byte*>(bytes, kInventoryPartyRootOffset);
    std::byte* child = partyRoot
        ? TrustedField<std::byte*>(partyRoot, kFirstChildOffset) : nullptr;
    for (std::size_t count = 0u;
         child && count < kMaximumDirectChildren; ++count) {
        std::byte* next = TrustedField<std::byte*>(child, kNextSiblingOffset);
        if (TrustedField<std::byte*>(child, kParentOffset) == partyRoot &&
            TrustedField<float>(child, kPositionXOffset) >=
                kInventoryRightChildMinimumX) {
            ShiftTrustedNode(child, overscan);
        }
        child = next;
    }

    ShiftTrustedNode(
        TrustedField<std::byte*>(bytes, kInventoryRightEdgeOffset), overscan);
}

int __fastcall MainMenuDrawHook(void* product, void*, int layer) {
    g_lastMainMenuDrawTick.store(GetTickCount(), std::memory_order_release);
    ApplyWideFrame(product);
    return g_originalDraw ? g_originalDraw(product, layer) : 0;
}

int __fastcall GuiListUpdateHook(void* list, void*, float delta) {
    ApplyListLayout(static_cast<std::byte*>(list));
    return g_originalListUpdate ? g_originalListUpdate(list, delta) : 0;
}

void* __fastcall InventoryConstructHook(void* product, void*) {
    void* result = g_originalInventoryConstruct
        ? g_originalInventoryConstruct(product) : product;
    ApplyInventoryRightAnchors(result ? result : product);
    return result;
}

template <std::size_t Size>
bool MatchesContract(const ImageView& image, std::uint32_t rva,
                     const std::array<std::byte, Size>& prefix) {
    const auto bytes = image.At(rva, prefix.size());
    return bytes &&
        std::memcmp(bytes->data(), prefix.data(), prefix.size()) == 0;
}

BOOL CALLBACK InitializeOnce(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2 ||
        !Supports(Capability::ArbitraryAspectRatio)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image ||
        !MatchesContract(*image, kMainMenuDrawRva, kMainMenuDrawPrefix) ||
        !MatchesContract(*image, kGuiListUpdateRva, kGuiListUpdatePrefix) ||
        !MatchesContract(*image, kInventoryConstructRva,
                         kInventoryConstructPrefix)) {
        LogWarning("XIII-2 main-menu wide layout disabled: executable contract mismatch");
        return TRUE;
    }
    g_imageBase = image->Bytes().data();

    const auto target = [&image](std::uint32_t rva) {
        return const_cast<std::byte*>(image->At(rva, 1u)->data());
    };
    const bool drawInstalled = g_drawDetour.Install(
        "ff13-2-shared-main-menu-wide-frame", target(kMainMenuDrawRva),
        reinterpret_cast<void*>(&MainMenuDrawHook), kMainMenuDrawPrefix,
        reinterpret_cast<void**>(&g_originalDraw));
    const bool listInstalled = drawInstalled && g_listUpdateDetour.Install(
        "ff13-2-main-menu-list-layout", target(kGuiListUpdateRva),
        reinterpret_cast<void*>(&GuiListUpdateHook), kGuiListUpdatePrefix,
        reinterpret_cast<void**>(&g_originalListUpdate));
    const bool inventoryInstalled = listInstalled &&
        g_inventoryConstructDetour.Install(
            "ff13-2-inventory-right-anchors",
            target(kInventoryConstructRva),
            reinterpret_cast<void*>(&InventoryConstructHook),
            kInventoryConstructPrefix,
            reinterpret_cast<void**>(&g_originalInventoryConstruct));
    if (!inventoryInstalled) {
        g_inventoryConstructDetour.RevertAndClear(g_originalInventoryConstruct);
        g_listUpdateDetour.RevertAndClear(g_originalListUpdate);
        g_drawDetour.RevertAndClear(g_originalDraw);
        LogWarning("XIII-2 main-menu wide layout disabled: detour installation failed");
        return TRUE;
    }

    return TRUE;
}

} // namespace

void Initialize() {
    InitOnceExecuteOnce(&g_initializeOnce, &InitializeOnce, nullptr, nullptr);
}

} // namespace novafix::game::xiii2_main_menu_frame

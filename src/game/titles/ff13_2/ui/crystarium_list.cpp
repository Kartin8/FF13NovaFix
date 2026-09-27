#include "game/titles/ff13_2/ui/crystarium_list.h"

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

namespace novafix::game::xiii2_crystarium_list {
namespace {

constexpr std::uint32_t kCrystariumUpdateRva = 0x0041A3A0u;
constexpr std::array<std::byte, 7> kCrystariumUpdatePrefix{
    std::byte{0x81}, std::byte{0xEC}, std::byte{0x64}, std::byte{0x01},
    std::byte{0x00}, std::byte{0x00}, std::byte{0xA1},
};
constexpr std::uint32_t kCrystariumVtableRva = 0x009F1DE8u;
constexpr std::uint32_t kListVtableRva = 0x00A49D88u;
constexpr std::uint32_t kListAnchorVtableRva = 0x00A48828u;
constexpr std::uint32_t kRightPanelVtableRva = 0x00A49B08u;
constexpr std::uint32_t kCapacityVtableRva = 0x009F9EB0u;
constexpr std::uint32_t kCapacityNodeId = 1576u;
constexpr std::uint32_t kListAnchorId = 740u;
constexpr std::uint32_t kCharacterListVtableRva = 0x009F1B58u;
constexpr std::uint32_t kCharacterListRootVtableRva = 0x00A47B88u;
constexpr std::uint32_t kCharacterListBodyVtableRva = 0x009F2B58u;
constexpr std::uint32_t kCharacterListBackdropId = 927u;
constexpr float kAuthoredAnchorX = 404.0f;
constexpr float kAuthoredListX = -862.0f;
constexpr float kAuthoredRightPanelX = 959.0f;
constexpr float kAuthoredCapacityX = 882.0f;
constexpr std::size_t kRootOffset = 0x34u;
constexpr std::size_t kListOffset = 0x88u;
constexpr std::size_t kCharacterListOffset = 0x84u;
constexpr std::size_t kCapacityOffset = 0x1F4u;
constexpr std::size_t kMonsterTraitsOffset = 0x154u;
constexpr std::uint32_t kMonsterTraitsNodeVtableRva = 0x009EFA20u;
constexpr float kAuthoredMonsterTraitsX = 852.0f;
constexpr std::size_t kFirstChildOffset = 0x08u;
constexpr std::size_t kParentOffset = 0x04u;
constexpr std::size_t kNextSiblingOffset = 0x14u;
constexpr std::size_t kPositionXOffset = 0xA8u;
constexpr std::size_t kNodeIdOffset = 0xC0u;
constexpr std::size_t kDirtyFlagsOffset = 0xCCu;
constexpr std::uint32_t kTransformDirty = 0x04u;
constexpr float kCenterX = 640.0f;
constexpr float kMinimumSafeScaleX = 0.25f;
constexpr float kEpsilon = 0.0001f;
constexpr DWORD kActiveGraceMs = 250u;

constexpr int kScissorTop = 271;
constexpr int kScissorMaximumRight = 612;
constexpr int kScissorBottom = 634;
constexpr int kInnerScissorRight = 374;
constexpr int kInnerScissorBottom = 255;
constexpr int kReferenceHeight = 720;

using CrystariumUpdateFn = char (__thiscall*)(void* product, float delta);

struct ListBaseline {
    void* product{};
    std::byte* root{};
    std::byte* list{};
    std::byte* anchor{};
    std::byte* rightPanel{};
    std::byte* capacity{};
    std::byte* capacityRoot{};
    std::byte* capacityNode{};
    std::size_t capacityOffset{kCapacityOffset};
    float capacityPositionX{kAuthoredCapacityX};
    std::byte* characterList{};
    std::byte* characterListRoot{};
    std::byte* characterListWrapper{};
    std::byte* characterHeader{};
    std::byte* characterBackdrop{};
    std::byte* characterBody{};
};

INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_updateDetour;
CrystariumUpdateFn g_originalUpdate{};
const std::byte* g_imageBase{};
ListBaseline g_baseline{};
std::atomic<DWORD> g_lastUpdateTick{};

template <typename T>
bool ReadAt(const std::byte* object, std::size_t offset, T& value) {
    if (!object ||
        !patch::memory::IsReadable(object + offset, sizeof(T))) {
        return false;
    }
    std::memcpy(&value, object + offset, sizeof(T));
    return true;
}

bool CaptureList(void* product, std::byte* root, std::byte* list,
                 std::byte* anchor, std::byte* rightPanel,
                 std::byte* capacity, std::byte* capacityRoot,
                 std::byte* capacityNode,
                 std::byte* characterList,
                 std::byte* characterListRoot, std::byte* wrapper,
                 std::byte* characterHeader,
                 std::byte* characterBackdrop,
                 std::byte* characterBody,
                 ListBaseline& output) {
    for (auto* node : {list, anchor, rightPanel, capacityRoot, capacityNode,
                       wrapper, characterHeader,
                       characterBackdrop, characterBody}) {
        float position{};
        if (!patch::memory::IsWritable(
                node + kDirtyFlagsOffset, sizeof(std::uint32_t)) ||
            !ReadAt(node, kPositionXOffset, position) ||
            !std::isfinite(position)) {
            return false;
        }
    }
    output.product = product;
    output.root = root;
    output.list = list;
    output.anchor = anchor;
    output.rightPanel = rightPanel;
    output.capacity = capacity;
    output.capacityRoot = capacityRoot;
    output.capacityNode = capacityNode;
    output.characterList = characterList;
    output.characterListRoot = characterListRoot;
    output.characterListWrapper = wrapper;
    output.characterHeader = characterHeader;
    output.characterBackdrop = characterBackdrop;
    output.characterBody = characterBody;
    return true;
}

void SetPosition(std::byte* node, float target) {
    float& position = *reinterpret_cast<float*>(node + kPositionXOffset);
    if (std::fabs(position - target) <= kEpsilon) return;
    position = target;
    *reinterpret_cast<std::uint32_t*>(node + kDirtyFlagsOffset) |=
        kTransformDirty;
}

template <typename T>
T TrustedAt(const std::byte* object, std::size_t offset) {
    return *reinterpret_cast<const T*>(object + offset);
}

void ApplyPositions(const ListBaseline& nodes,
                    const display::AspectRatio& aspect) {
    const float overscan = kCenterX *
        ((1.0f / aspect.safeScaleX) - 1.0f);
    SetPosition(nodes.list, kAuthoredListX - overscan);
    SetPosition(nodes.anchor, kAuthoredAnchorX - overscan);
    SetPosition(nodes.rightPanel, kAuthoredRightPanelX + overscan);
    if (nodes.capacityOffset == kMonsterTraitsOffset) {
        SetPosition(nodes.capacityRoot, overscan);
    } else {
        SetPosition(nodes.capacityNode, nodes.capacityPositionX + overscan);
    }
    SetPosition(nodes.characterListWrapper, 0.0f);
    SetPosition(nodes.characterHeader, kAuthoredAnchorX - overscan);
    SetPosition(nodes.characterBackdrop, kAuthoredListX - overscan);
    SetPosition(nodes.characterBody, kAuthoredListX - overscan);
}

bool ApplyList(void* product, const display::AspectRatio& aspect) {
    auto* bytes = static_cast<std::byte*>(product);
    if (product == g_baseline.product && g_baseline.root &&
        (g_baseline.capacityOffset == kCapacityOffset ||
         TrustedAt<std::byte*>(bytes, kCapacityOffset) == nullptr) &&
        TrustedAt<const void*>(bytes, 0u) ==
            g_imageBase + kCrystariumVtableRva &&
        TrustedAt<std::byte*>(bytes, kRootOffset) == g_baseline.root &&
        TrustedAt<std::byte*>(bytes, kListOffset) == g_baseline.list &&
        TrustedAt<std::byte*>(bytes, g_baseline.capacityOffset) ==
            g_baseline.capacity &&
        TrustedAt<std::byte*>(g_baseline.capacity, kRootOffset) ==
            g_baseline.capacityRoot &&
        TrustedAt<std::byte*>(g_baseline.capacityRoot,
                              kFirstChildOffset) ==
            g_baseline.capacityNode &&
        TrustedAt<std::byte*>(g_baseline.list,
                              kNextSiblingOffset) ==
            g_baseline.rightPanel &&
        TrustedAt<std::byte*>(bytes, kCharacterListOffset) ==
            g_baseline.characterList &&
        TrustedAt<std::byte*>(g_baseline.root, kFirstChildOffset) ==
            g_baseline.anchor &&
        TrustedAt<std::byte*>(g_baseline.characterList,
                              kRootOffset) ==
            g_baseline.characterListRoot &&
        TrustedAt<std::byte*>(g_baseline.characterListRoot,
                              kFirstChildOffset) ==
            g_baseline.characterListWrapper &&
        TrustedAt<std::byte*>(g_baseline.characterListWrapper,
                              kFirstChildOffset) ==
            g_baseline.characterHeader &&
        TrustedAt<std::byte*>(g_baseline.characterHeader,
                              kNextSiblingOffset) ==
            g_baseline.characterBackdrop &&
        TrustedAt<std::byte*>(g_baseline.characterBackdrop,
                              kNextSiblingOffset) ==
            g_baseline.characterBody) {
        ApplyPositions(g_baseline, aspect);
        return true;
    }

    const void* vtable{};
    std::byte* root{};
    std::byte* list{};
    std::byte* anchor{};
    std::byte* rightPanel{};
    std::byte* capacity{};
    std::byte* capacityRoot{};
    std::byte* capacityNode{};
    std::byte* characterList{};
    std::byte* characterListRoot{};
    std::byte* wrapper{};
    std::byte* characterHeader{};
    std::byte* characterBackdrop{};
    std::byte* characterBody{};
    std::byte* parent{};
    const void* listVtable{};
    const void* anchorVtable{};
    const void* rightPanelVtable{};
    const void* capacityVtable{};
    std::uint32_t capacityNodeId{};
    const void* capacityNodeVtable{};
    std::size_t capacityOffset = kCapacityOffset;
    if (!ReadAt(bytes, kCapacityOffset, capacity)) return false;
    if (!capacity) {
        capacityOffset = kMonsterTraitsOffset;
        if (!ReadAt(bytes, capacityOffset, capacity)) return false;
    }
    const void* characterListVtable{};
    const void* characterListRootVtable{};
    const void* characterBodyVtable{};
    std::uint32_t anchorId{};
    std::uint32_t characterBackdropId{};
    if (!g_imageBase ||
        !ReadAt(bytes, 0u, vtable) ||
        vtable != g_imageBase + kCrystariumVtableRva ||
        !ReadAt(bytes, kRootOffset, root) ||
        !ReadAt(bytes, kListOffset, list) ||
        !ReadAt(capacity, 0u, capacityVtable) ||
        capacityVtable != g_imageBase + kCapacityVtableRva ||
        !ReadAt(capacity, kRootOffset, capacityRoot) ||
        !ReadAt(capacityRoot, kFirstChildOffset, capacityNode) ||
        !ReadAt(capacityNode, kNodeIdOffset, capacityNodeId) ||
        !ReadAt(capacityNode, 0u, capacityNodeVtable) ||
        (capacityOffset == kCapacityOffset
            ? capacityNodeId != kCapacityNodeId
            : capacityNodeVtable !=
                g_imageBase + kMonsterTraitsNodeVtableRva) ||
        !ReadAt(bytes, kCharacterListOffset, characterList) ||
        !ReadAt(characterList, 0u, characterListVtable) ||
        characterListVtable !=
            g_imageBase + kCharacterListVtableRva ||
        !ReadAt(characterList, kRootOffset, characterListRoot) ||
        !ReadAt(characterListRoot, 0u,
                characterListRootVtable) ||
        characterListRootVtable !=
            g_imageBase + kCharacterListRootVtableRva ||
        !ReadAt(characterListRoot, kFirstChildOffset, wrapper) ||
        !ReadAt(wrapper, kFirstChildOffset, characterHeader) ||
        !ReadAt(characterHeader, kNextSiblingOffset,
                characterBackdrop) ||
        !ReadAt(characterBackdrop, kNodeIdOffset,
                characterBackdropId) ||
        characterBackdropId != kCharacterListBackdropId ||
        !ReadAt(characterBackdrop, kNextSiblingOffset,
                characterBody) ||
        !ReadAt(characterBody, 0u, characterBodyVtable) ||
        characterBodyVtable !=
            g_imageBase + kCharacterListBodyVtableRva ||
        !ReadAt(root, kFirstChildOffset, anchor) ||
        !ReadAt(list, 0u, listVtable) ||
        listVtable != g_imageBase + kListVtableRva ||
        !ReadAt(anchor, 0u, anchorVtable) ||
        anchorVtable != g_imageBase + kListAnchorVtableRva ||
        !ReadAt(list, kNextSiblingOffset, rightPanel) ||
        !ReadAt(rightPanel, 0u, rightPanelVtable) ||
        rightPanelVtable != g_imageBase + kRightPanelVtableRva ||
        !ReadAt(rightPanel, kParentOffset, parent) || parent != root ||
        !ReadAt(anchor, 0xC0u, anchorId) ||
        anchorId != kListAnchorId ||
        !ReadAt(list, kParentOffset, parent) || parent != root) {
        return false;
    }

    if (g_baseline.product != product || g_baseline.root != root ||
        g_baseline.list != list || g_baseline.anchor != anchor ||
        g_baseline.rightPanel != rightPanel ||
        g_baseline.capacity != capacity ||
        g_baseline.capacityRoot != capacityRoot ||
        g_baseline.capacityNode != capacityNode ||
        g_baseline.capacityOffset != capacityOffset ||
        g_baseline.characterList != characterList ||
        g_baseline.characterListRoot != characterListRoot ||
        g_baseline.characterListWrapper != wrapper ||
        g_baseline.characterHeader != characterHeader ||
        g_baseline.characterBackdrop != characterBackdrop ||
        g_baseline.characterBody != characterBody) {
        ListBaseline next{};
        if (!CaptureList(product, root, list, anchor, rightPanel,
                         capacity, capacityRoot, capacityNode,
                         characterList,
                         characterListRoot, wrapper,
                         characterHeader, characterBackdrop,
                         characterBody, next)) {
            return false;
        }
        next.capacityOffset = capacityOffset;
        next.capacityPositionX = capacityOffset == kCapacityOffset
            ? kAuthoredCapacityX : kAuthoredMonsterTraitsX;
        g_baseline = next;
    }

    ApplyPositions(g_baseline, aspect);
    return true;
}

char __fastcall CrystariumUpdateHook(void* product, void*, float delta) {
    const auto aspect = display::CurrentAspectRatio();
    if (aspect.valid && aspect.safeScaleX >= kMinimumSafeScaleX &&
        aspect.safeScaleX < 1.0f) {
        g_lastUpdateTick.store(GetTickCount(), std::memory_order_release);
        ApplyList(product, aspect);
    }
    return g_originalUpdate ? g_originalUpdate(product, delta) : 0;
}

BOOL CALLBACK InitializeOnce(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2 ||
        !Supports(Capability::ArbitraryAspectRatio)) {
        return TRUE;
    }
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) return TRUE;
    const auto target = image->At(kCrystariumUpdateRva,
                                  kCrystariumUpdatePrefix.size());
    if (!target || std::memcmp(target->data(),
                               kCrystariumUpdatePrefix.data(),
                               kCrystariumUpdatePrefix.size()) != 0) {
        return TRUE;
    }
    g_imageBase = image->Bytes().data();
    g_updateDetour.Install(
        "ff13-2-crystarium-character-list",
        const_cast<std::byte*>(target->data()),
        reinterpret_cast<void*>(&CrystariumUpdateHook),
        kCrystariumUpdatePrefix,
        reinterpret_cast<void**>(&g_originalUpdate));
    return TRUE;
}

} // namespace

display::ScissorRect CorrectScissor(
    display::ScissorRect source, display::ScissorRect corrected,
    const display::AspectRatio& aspect) {
    if (!aspect.valid || aspect.safeScaleX >= 1.0f ||
        source.left != 0) {
        return corrected;
    }
    const DWORD last = g_lastUpdateTick.load(std::memory_order_acquire);
    if (!last || GetTickCount() - last > kActiveGraceMs) return corrected;

    const double scale = static_cast<double>(aspect.height) /
        kReferenceHeight;
    const auto matches = [scale](int value, int authored) {
        return std::fabs(value - authored * scale) <= 1.5;
    };
    const bool listClip = matches(source.top, kScissorTop) &&
        matches(source.bottom, kScissorBottom) &&
        source.right > 0 &&
        source.right <= kScissorMaximumRight * scale + 1.5;
    const bool innerClip = source.top == 0 &&
        matches(source.right, kInnerScissorRight) &&
        matches(source.bottom, kInnerScissorBottom);
    if (!listClip && !innerClip) {
        return corrected;
    }
    corrected.left = 0;
    return corrected;
}

void Initialize() {
    InitOnceExecuteOnce(&g_once, &InitializeOnce, nullptr, nullptr);
}

} // namespace novafix::game::xiii2_crystarium_list

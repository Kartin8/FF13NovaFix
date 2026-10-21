#include "game/titles/ff13_2/ui/menu_chrome.h"

#include "display/aspect_runtime.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"

#include <windows.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::xiii2_menu_chrome {
namespace {

constexpr std::uint32_t kDetailUpdateRva = 0x0049C5C0u;
constexpr std::uint32_t kGuideUpdateRva = 0x003F0A20u;
constexpr std::uint32_t kHistoriaVtableRva = 0x009EC900u;
constexpr std::uint32_t kGuideTextVtableRva = 0x00A48D58u;
constexpr std::uint32_t kHelpVtableRva = 0xDEBE70u - 0x400000u;
constexpr std::uint32_t kSecondaryGuideVtableRva = 0xDEDA40u - 0x400000u;
constexpr std::uint32_t kSaveLoadVtableRva = 0xE41258u - 0x400000u;
constexpr std::uint32_t kGroupVtableRva = 0xE48828u - 0x400000u;
constexpr std::uint32_t kPersistentTextVtableRva = 0xDE8F58u - 0x400000u;
constexpr std::uint32_t kScrollWindowVtableRva = 0xE41818u - 0x400000u;
constexpr std::uint32_t kScrollWindowDrawRva = 0x89B070u - 0x400000u;
constexpr std::array<std::byte, 13> kScrollWindowDrawPrefix{
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x1C}, std::byte{0x56},
    std::byte{0x8B}, std::byte{0xF1}, std::byte{0xF6}, std::byte{0x86},
    std::byte{0xCC}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x02},
};
constexpr std::array<std::byte, 7> kGuideUpdatePrefix{
    std::byte{0x81}, std::byte{0xEC}, std::byte{0xD8}, std::byte{0x04},
    std::byte{0x00}, std::byte{0x00}, std::byte{0xA1},
};
constexpr std::array<std::byte, 14> kDetailUpdatePrefix{
    std::byte{0x56}, std::byte{0x8B}, std::byte{0xF1}, std::byte{0x8B},
    std::byte{0x86}, std::byte{0x94}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x57}, std::byte{0x85}, std::byte{0xC0},
    std::byte{0x74}, std::byte{0x19},
};
constexpr std::uint32_t kDetailVtableRva = 0x00A41F00u;
constexpr std::uint32_t kHeaderNodeId = 927u;
constexpr std::uint32_t kDetailNodeId = 1264u;
constexpr std::uint32_t kDetailBackingNodeId = 1265u;
constexpr std::uint32_t kHeaderRowNodeId = 1270u;
constexpr std::uint32_t kHeaderRowFillNodeId = 1280u;
constexpr std::uint32_t kHeaderRowBackingNodeId = 1471u;
constexpr std::uint32_t kBottomNodeId = 1291u;
constexpr std::uint32_t kBottomBackingNodeId = 1483u;
constexpr float kCenterX = 640.0f;
constexpr float kMinimumSafeScaleX = 0.25f;
constexpr float kEpsilon = 0.0001f;

constexpr std::size_t kRootOffset = 0x34u;
constexpr std::size_t kFirstChildOffset = 0x08u;
constexpr std::size_t kParentOffset = 0x04u;
constexpr std::size_t kNextSiblingOffset = 0x14u;
constexpr std::size_t kScaleXOffset = 0x68u;
constexpr std::size_t kPositionXOffset = 0xA8u;
constexpr std::size_t kNodeIdOffset = 0xC0u;
constexpr std::size_t kDirtyFlagsOffset = 0xCCu;
constexpr std::uint32_t kTransformDirty = 0x04u;

using DetailUpdateFn = char (__thiscall*)(void* product, float delta);
using GuideUpdateFn = void (__thiscall*)(void* product, float delta);
using ScrollWindowDrawFn = void (__thiscall*)(void* window, int layer);

struct NodeBaseline {
    std::byte* node{};
    float scaleX{1.0f};
    float positionX{};
};

struct BackingBaseline {
    void* product{};
    std::byte* root{};
    NodeBaseline header{};
    std::byte* detail{};
    NodeBaseline detailBacking{};
    NodeBaseline headerRowFill{};
    NodeBaseline headerRowBacking{};
    NodeBaseline bottomBacking{};
};

INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_updateDetour;
patch::Detour32 g_guideDetour;
patch::Detour32 g_scrollDrawDetour;
ScrollWindowDrawFn g_originalScrollDraw{};
void* g_historiaRoot{};
void* g_historiaList{};
thread_local void* g_historiaListDraw{};
GuideUpdateFn g_originalGuideUpdate{};
void* g_guideProduct{};
std::byte* g_guideRoot{};
std::byte* g_guideText{};
DetailUpdateFn g_originalUpdate{};
const std::byte* g_imageBase{};
BackingBaseline g_baseline{};

template <typename T>
T& NativeField(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

template <typename T>
bool ReadAt(const std::byte* object, std::size_t offset, T& value) {
    if (!object ||
        !patch::memory::IsReadable(object + offset, sizeof(T))) {
        return false;
    }
    std::memcpy(&value, object + offset, sizeof(T));
    return true;
}

bool CaptureBacking(void* product, BackingBaseline& baseline) {
    auto* bytes = static_cast<std::byte*>(product);
    std::byte* root{};
    std::byte* header{};
    std::byte* detail{};
    std::byte* detailBacking{};
    std::byte* headerRow{};
    std::byte* headerRowFill{};
    std::byte* headerRowBacking{};
    std::byte* label{};
    std::byte* bottom{};
    std::byte* bottomBacking{};
    std::byte* parent{};
    std::uint32_t headerId{};
    std::uint32_t detailId{};
    std::uint32_t detailBackingId{};
    std::uint32_t headerRowId{};
    std::uint32_t headerRowFillId{};
    std::uint32_t headerRowBackingId{};
    std::uint32_t bottomId{};
    std::uint32_t bottomBackingId{};
    if (!ReadAt(bytes, kRootOffset, root) ||
        !ReadAt(root, kFirstChildOffset, header) ||
        !ReadAt(header, kParentOffset, parent) || parent != root ||
        !ReadAt(header, kNodeIdOffset, headerId) ||
        !ReadAt(header, kNextSiblingOffset, detail) ||
        !ReadAt(detail, kParentOffset, parent) || parent != root ||
        !ReadAt(detail, kNodeIdOffset, detailId) ||
        !ReadAt(detail, kFirstChildOffset, detailBacking) ||
        !ReadAt(detailBacking, kNodeIdOffset, detailBackingId) ||
        !ReadAt(detailBacking, kNextSiblingOffset, headerRow) ||
        !ReadAt(headerRow, kNodeIdOffset, headerRowId) ||
        !ReadAt(headerRow, kFirstChildOffset, headerRowFill) ||
        !ReadAt(headerRowFill, kNodeIdOffset, headerRowFillId) ||
        !ReadAt(headerRowFill, kNextSiblingOffset, headerRowBacking) ||
        !ReadAt(headerRowBacking, kNodeIdOffset, headerRowBackingId) ||
        !ReadAt(headerRow, kNextSiblingOffset, label) ||
        !ReadAt(label, kNextSiblingOffset, bottom) ||
        !ReadAt(bottom, kNodeIdOffset, bottomId) ||
        !ReadAt(bottom, kFirstChildOffset, bottomBacking) ||
        !ReadAt(bottomBacking, kNodeIdOffset, bottomBackingId) ||
        headerId != kHeaderNodeId || detailId != kDetailNodeId ||
        detailBackingId != kDetailBackingNodeId ||
        headerRowId != kHeaderRowNodeId ||
        headerRowFillId != kHeaderRowFillNodeId ||
        headerRowBackingId != kHeaderRowBackingNodeId ||
        bottomId != kBottomNodeId ||
        bottomBackingId != kBottomBackingNodeId ||
        !ReadAt(header, kScaleXOffset, baseline.header.scaleX) ||
        !ReadAt(header, kPositionXOffset, baseline.header.positionX) ||
        !ReadAt(detailBacking, kScaleXOffset,
                baseline.detailBacking.scaleX) ||
        !ReadAt(detailBacking, kPositionXOffset,
                baseline.detailBacking.positionX) ||
        !ReadAt(headerRowFill, kScaleXOffset,
                baseline.headerRowFill.scaleX) ||
        !ReadAt(headerRowFill, kPositionXOffset,
                baseline.headerRowFill.positionX) ||
        !ReadAt(headerRowBacking, kScaleXOffset,
                baseline.headerRowBacking.scaleX) ||
        !ReadAt(headerRowBacking, kPositionXOffset,
                baseline.headerRowBacking.positionX) ||
        !ReadAt(bottomBacking, kScaleXOffset,
                baseline.bottomBacking.scaleX) ||
        !ReadAt(bottomBacking, kPositionXOffset,
                baseline.bottomBacking.positionX) ||
        !std::isfinite(baseline.header.scaleX) ||
        !std::isfinite(baseline.header.positionX) ||
        !std::isfinite(baseline.detailBacking.scaleX) ||
        !std::isfinite(baseline.detailBacking.positionX) ||
        !std::isfinite(baseline.headerRowFill.scaleX) ||
        !std::isfinite(baseline.headerRowFill.positionX) ||
        baseline.headerRowFill.positionX <= 0.0f ||
        !std::isfinite(baseline.headerRowBacking.scaleX) ||
        !std::isfinite(baseline.headerRowBacking.positionX) ||
        !std::isfinite(baseline.bottomBacking.scaleX) ||
        !std::isfinite(baseline.bottomBacking.positionX)) {
        return false;
    }
    for (auto* node : {header, detailBacking, headerRowFill,
                       headerRowBacking, bottomBacking}) {
        if (!patch::memory::IsWritable(node + kScaleXOffset,
                                        sizeof(float)) ||
            !patch::memory::IsWritable(node + kPositionXOffset,
                                        sizeof(float)) ||
            !patch::memory::IsWritable(node + kDirtyFlagsOffset,
                                        sizeof(std::uint32_t))) {
            return false;
        }
    }
    baseline.product = product;
    baseline.root = root;
    baseline.header.node = header;
    baseline.detail = detail;
    baseline.detailBacking.node = detailBacking;
    baseline.headerRowFill.node = headerRowFill;
    baseline.headerRowBacking.node = headerRowBacking;
    baseline.bottomBacking.node = bottomBacking;
    return true;
}

void ApplyNode(const NodeBaseline& baseline, float scaleX,
               float positionX) {
    auto* node = baseline.node;
    float& currentScale = *reinterpret_cast<float*>(node + kScaleXOffset);
    float& currentPosition = *reinterpret_cast<float*>(
        node + kPositionXOffset);
    if (std::fabs(currentScale - scaleX) <= kEpsilon &&
        std::fabs(currentPosition - positionX) <= kEpsilon) {
        return;
    }
    currentScale = scaleX;
    currentPosition = positionX;
    *reinterpret_cast<std::uint32_t*>(node + kDirtyFlagsOffset) |=
        kTransformDirty;
}

void ApplyBacking(void* product) {
    if (!product || !g_imageBase) return;
    auto* bytes = static_cast<std::byte*>(product);
    const void* vtable{};
    if (!ReadAt(bytes, 0u, vtable) ||
        vtable != g_imageBase + kDetailVtableRva) {
        return;
    }

    std::byte* root{};
    std::byte* header{};
    std::byte* detail{};
    if (!ReadAt(bytes, kRootOffset, root) ||
        !ReadAt(root, kFirstChildOffset, header) ||
        !ReadAt(header, kNextSiblingOffset, detail)) {
        return;
    }
    if (g_baseline.product != product || g_baseline.root != root ||
        g_baseline.header.node != header ||
        g_baseline.detail != detail) {
        BackingBaseline next{};
        if (!CaptureBacking(product, next)) return;
        g_baseline = next;
    }

    const auto aspect = display::CurrentAspectRatio();
    const float safeScaleX = aspect.valid &&
            aspect.safeScaleX >= kMinimumSafeScaleX &&
            aspect.safeScaleX < 1.0f
        ? aspect.safeScaleX : 1.0f;
    const float overscan = kCenterX *
        ((1.0f / safeScaleX) - 1.0f);
    ApplyNode(g_baseline.header,
              g_baseline.header.scaleX / safeScaleX,
              g_baseline.header.positionX - overscan);
    ApplyNode(g_baseline.detailBacking,
              g_baseline.detailBacking.scaleX / safeScaleX,
              g_baseline.detailBacking.positionX - overscan);
    ApplyNode(g_baseline.headerRowFill,
              g_baseline.headerRowFill.scaleX + overscan,
              g_baseline.headerRowFill.positionX);
    ApplyNode(g_baseline.headerRowBacking,
              g_baseline.headerRowBacking.scaleX *
                  (1.0f + overscan /
                      g_baseline.headerRowFill.positionX),
              g_baseline.headerRowBacking.positionX - overscan);
    ApplyNode(g_baseline.bottomBacking,
              g_baseline.bottomBacking.scaleX / safeScaleX,
              g_baseline.bottomBacking.positionX - overscan);
}

char __fastcall DetailUpdateHook(void* product, void*, float delta) {
    ApplyBacking(product);
    return g_originalUpdate ? g_originalUpdate(product, delta) : 0;
}

void __fastcall GuideUpdateHook(void* product, void*, float delta) {
    if (g_originalGuideUpdate) g_originalGuideUpdate(product, delta);
    const auto anchors = CaptureVerticalAnchors(
        product, reinterpret_cast<std::uintptr_t>(g_imageBase));
    g_historiaRoot = anchors.historiaRoot;
    g_historiaList = anchors.historiaRoot ? anchors.header : nullptr;
    const auto aspect = display::CurrentAspectRatio();
    if (!aspect.valid || aspect.safeScaleX >= 1.0f ||
        aspect.safeScaleX < kMinimumSafeScaleX || !product) return;
    auto* bytes = static_cast<std::byte*>(product);
    auto* root = *reinterpret_cast<std::byte**>(bytes + kRootOffset);
    if (!root) return;
    if (g_guideProduct != product || g_guideRoot != root) {
        g_guideText = nullptr;
        const void* vtable{};
        if (!ReadAt(bytes, 0u, vtable) ||
            vtable != g_imageBase + kHistoriaVtableRva) return;
        std::byte* child{};
        if (!ReadAt(root, kFirstChildOffset, child)) return;
        for (unsigned count = 0; child && count < 32; ++count) {
            const void* childVtable{};
            std::byte* parent{};
            float y{};
            if (!ReadAt(child, 0u, childVtable) ||
                !ReadAt(child, kParentOffset, parent) ||
                !ReadAt(child, 0xACu, y)) return;
            if (parent == root &&
                childVtable == g_imageBase + kGuideTextVtableRva &&
                std::fabs(y - 642.0f) < kEpsilon &&
                patch::memory::IsWritable(
                    child, kDirtyFlagsOffset + sizeof(std::uint32_t))) {
                g_guideText = child;
            }
            if (!ReadAt(child, kNextSiblingOffset, child)) return;
        }
        if (!g_guideText) return;
        g_guideProduct = product;
        g_guideRoot = root;
    }
    const float target = 544.0f + kCenterX *
        ((1.0f / aspect.safeScaleX) - 1.0f);
    ApplyNode({g_guideText, 1.0f, 544.0f},
              *reinterpret_cast<float*>(g_guideText + kScaleXOffset), target);
}

void __fastcall ScrollWindowDrawHook(void* window, void*, int layer) {
    struct RestoreDrawContext {
        void* previous{g_historiaListDraw};
        ~RestoreDrawContext() { g_historiaListDraw = previous; }
    } restore;
    g_historiaListDraw = nullptr;
    if (window && window == g_historiaList && g_historiaRoot &&
        NativeField<void*>(window, kParentOffset) == g_historiaRoot &&
        NativeField<const void*>(window, 0u) == g_imageBase + kScrollWindowVtableRva &&
        NativeField<const void*>(g_historiaRoot, 0u) == g_imageBase + kGroupVtableRva) {
        // Compare only live native objects. The leading footer container and
        // resource-740 title also reject an address recycled by another menu.
        void* const footer = NativeField<void*>(g_historiaRoot, kFirstChildOffset);
        void* const title = footer
            ? NativeField<void*>(footer, kNextSiblingOffset) : nullptr;
        void* const counter = title
            ? NativeField<void*>(title, kNextSiblingOffset) : nullptr;
        const auto counterId = counter
            ? NativeField<std::uint32_t>(counter, kNodeIdOffset) : 0u;
        if (title && NativeField<std::uint32_t>(title, kNodeIdOffset) == 740u &&
            NativeField<void*>(title, kParentOffset) == g_historiaRoot &&
            (counterId == 1583u || counterId == 1442u) &&
            NativeField<void*>(counter, kParentOffset) == g_historiaRoot) {
            g_historiaListDraw = window;
        }
    }
    if (g_originalScrollDraw) g_originalScrollDraw(window, layer);
}

BOOL CALLBACK InitializeOnce(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2 ||
        !Supports(Capability::ArbitraryAspectRatio)) {
        return TRUE;
    }
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) return TRUE;
    const auto target = image->At(kDetailUpdateRva,
                                  kDetailUpdatePrefix.size());
    if (!target || std::memcmp(target->data(), kDetailUpdatePrefix.data(),
                                kDetailUpdatePrefix.size()) != 0) {
        return TRUE;
    }
    g_imageBase = image->Bytes().data();
    g_updateDetour.Install(
        "ff13-2-menu-detail-backing",
        const_cast<std::byte*>(target->data()),
        reinterpret_cast<void*>(&DetailUpdateHook), kDetailUpdatePrefix,
        reinterpret_cast<void**>(&g_originalUpdate));
    const auto guide = image->At(kGuideUpdateRva, kGuideUpdatePrefix.size());
    if (guide && std::memcmp(guide->data(), kGuideUpdatePrefix.data(),
                             kGuideUpdatePrefix.size()) == 0) {
        g_guideDetour.Install(
            "ff13-2-shared-menu-footer-guide",
            const_cast<std::byte*>(guide->data()),
            reinterpret_cast<void*>(&GuideUpdateHook), kGuideUpdatePrefix,
            reinterpret_cast<void**>(&g_originalGuideUpdate));
    }
    const auto scroll = image->At(kScrollWindowDrawRva, kScrollWindowDrawPrefix.size());
    if (g_originalGuideUpdate && scroll &&
        std::memcmp(scroll->data(), kScrollWindowDrawPrefix.data(),
                    kScrollWindowDrawPrefix.size()) == 0) {
        g_scrollDrawDetour.Install(
            "ff13-2-historia-list-clip", const_cast<std::byte*>(scroll->data()),
            reinterpret_cast<void*>(&ScrollWindowDrawHook), kScrollWindowDrawPrefix,
            reinterpret_cast<void**>(&g_originalScrollDraw));
    }
    return TRUE;
}

} // namespace

VerticalAnchors CaptureVerticalAnchors(void* product, std::uintptr_t imageBase) {
    VerticalAnchors anchors{};
    if (!product || !imageBase) return anchors;
    // These are engine-owned objects inside their native update, rather than
    // cached pointers to a menu that may have been destroyed/recreated.
    const auto vtable = [](void* object) {
        return reinterpret_cast<std::uintptr_t>(NativeField<void*>(object, 0u));
    };
    const auto productVtable = vtable(product);
    const bool help = productVtable == imageBase + kHelpVtableRva;
    const bool guide = productVtable == imageBase + kSecondaryGuideVtableRva;
    const bool historia = productVtable == imageBase + kHistoriaVtableRva;
    const bool saveLoad = productVtable == imageBase + kSaveLoadVtableRva;
    if (!help && !guide && !saveLoad && !historia) return anchors;
    void* const root = NativeField<void*>(product, kRootOffset);
    if (!root || vtable(root) != imageBase + kGroupVtableRva ||
        NativeField<void*>(root, kParentOffset)) return anchors;
    if (help) {
        anchors.header = root;
    } else if (guide) {
        anchors.footers[0] = root;
    } else if (historia) {
        // 7EB6E0 mixes upper panels with a separate lower-info container.
        // Never translate the common root or the independent gate effects.
        anchors.historiaRoot = root;
        void* const location = NativeField<void*>(product, 0x14Cu);
        void* const panel = location
            ? NativeField<void*>(location, kParentOffset) : nullptr;
        void* const footer = panel
            ? NativeField<void*>(panel, kParentOffset) : nullptr;
        if (panel && footer && vtable(panel) == imageBase + kGroupVtableRva &&
            NativeField<std::uint32_t>(panel, kNodeIdOffset) == 717u &&
            vtable(footer) == imageBase + kGroupVtableRva &&
            NativeField<void*>(footer, kParentOffset) == root) {
            anchors.footers[0] = footer;
        }
        void* const buttons = NativeField<void*>(product, 0x168u);
        if (buttons && vtable(buttons) == imageBase + kGuideTextVtableRva &&
            NativeField<void*>(buttons, kParentOffset) == root) {
            anchors.footers[1] = buttons;
        }
        void* const list = NativeField<void*>(product, 0x164u);
        if (g_originalScrollDraw && list &&
            vtable(list) == imageBase + kScrollWindowVtableRva &&
            NativeField<void*>(list, kParentOffset) == root) {
            anchors.header = list;
        }
    } else {
        // 8962B0 stores the Load/Save label at +0x51C and the button guide
        // at +0x534. The label's resource-740 parent owns the whole header.
        void* const label = NativeField<void*>(product, 0x51Cu);
        void* const header = label
            ? NativeField<void*>(label, kParentOffset) : nullptr;
        if (header && vtable(header) == imageBase + kGroupVtableRva &&
            NativeField<void*>(header, kParentOffset) == root &&
            NativeField<std::uint32_t>(header, kNodeIdOffset) == 740u) {
            anchors.header = header;
        }
        void* const content = NativeField<void*>(product, 0x70u);
        void* const bottom = content
            ? NativeField<void*>(content, kNextSiblingOffset) : nullptr;
        if (bottom && vtable(bottom) == imageBase + kGroupVtableRva &&
            NativeField<void*>(bottom, kParentOffset) == root &&
            NativeField<std::uint32_t>(bottom, kNodeIdOffset) == 60u) {
            anchors.footers[0] = bottom;
        }
        void* const buttons = NativeField<void*>(product, 0x534u);
        if (buttons && vtable(buttons) == imageBase + kPersistentTextVtableRva &&
            NativeField<void*>(buttons, kParentOffset) == root) {
            anchors.footers[1] = buttons;
        }
    }
    return anchors;
}

bool IsHeader(void* node, const VerticalAnchors& anchors, std::uintptr_t imageBase) {
    if (!node) return false;
    if (node == anchors.header) return true;
    if (!anchors.historiaRoot ||
        NativeField<void*>(node, kParentOffset) != anchors.historiaRoot ||
        reinterpret_cast<std::uintptr_t>(NativeField<void*>(node, 0u)) !=
            imageBase + kGroupVtableRva) return false;
    // Native templates in 7EB6E0, including alternate counter/map modes.
    switch (NativeField<std::uint32_t>(node, kNodeIdOffset)) {
    case 740u: case 1583u: case 1442u: // Title and total fragments
    case 755u: case 1687u: case 1574u: case 1711u: // Gate Matrix panels
        return true;
    default:
        return false;
    }
}

display::ScissorRect CorrectScissor(
    display::ScissorRect source, display::ScissorRect corrected,
    const display::AspectRatio& aspect) {
    if (!g_historiaListDraw || !aspect.valid || !aspect.correctionRequired ||
        aspect.safeScaleY <= 0.0f || aspect.safeScaleY >= 1.0f) return corrected;
    void* const list = g_historiaListDraw;
    const double x = NativeField<float>(list, kPositionXOffset) - 4.0;
    const double y = NativeField<float>(list, 0xACu);
    const double width = NativeField<int>(list, 276u);
    const double height = NativeField<int>(list, 280u);
    const auto matches = [&](double scaleX, double scaleY) {
        const double actualWidth = static_cast<double>(source.right) - source.left;
        return std::fabs(source.left - x * scaleX) <= 1.5 &&
            std::fabs(source.top - y * scaleY) <= 1.5 &&
            std::fabs(source.bottom - (y + height) * scaleY) <= 2.0 &&
            actualWidth >= (width + 8.0) * scaleX - 1.5 &&
            actualWidth <= (width + 16.0) * scaleX + 1.5;
    };
    // 89B070 clips from local +A8, while its children use the world matrix.
    // 7A4830 may first scale the authored rectangle to the back buffer.
    if (!matches(1.0, 1.0) &&
        !matches(aspect.width / 1280.0, aspect.height / 720.0)) return corrected;
    const double gutter = (aspect.height - aspect.width * (9.0 / 16.0)) * 0.5;
    corrected.top = static_cast<std::int32_t>(std::floor(corrected.top - gutter));
    corrected.bottom = static_cast<std::int32_t>(std::ceil(corrected.bottom - gutter));
    return corrected;
}

void Initialize() {
    InitOnceExecuteOnce(&g_once, &InitializeOnce, nullptr, nullptr);
}

} // namespace novafix::game::xiii2_menu_chrome

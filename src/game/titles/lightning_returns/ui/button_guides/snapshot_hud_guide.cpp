#include "game/titles/lightning_returns/ui/button_guides/snapshot_hud_guide.h"

#include "diagnostics/log.h"
#include "patch/memory_access.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::lr_prompt_guides::snapshot_hud_guide {
namespace {

constexpr std::size_t kGuiProductParentOffset = 0x04u;
constexpr std::size_t kGuiProductFirstChildOffset = 0x08u;
constexpr std::size_t kGuiProductLastChildOffset = 0x0Cu;
constexpr std::size_t kGuiProductPreviousSiblingOffset = 0x10u;
constexpr std::size_t kGuiProductNextSiblingOffset = 0x14u;
constexpr std::size_t kGuiProductTransformOffset = 0x58u;
constexpr std::size_t kGuiProductTransformBytes = 0x40u;
constexpr std::size_t kGuiProductPositionXOffset = 0x58u;
constexpr std::size_t kGuiProductPositionYOffset = 0x5Cu;
constexpr std::size_t kGuiProductDirtyFlagsOffset = 0xA4u;
constexpr std::size_t kTextLabelInnerPositionXOffset = 0x11Cu;
constexpr std::size_t kTextLabelMeasuredWidthOffset = 0x1E0u;
constexpr std::size_t kTextLabelFontConfigOffset = 0x1FCu;
constexpr std::size_t kFontPixelSizeOffset = 0x08u;
constexpr std::size_t kTextLabelInitWidthOffset = 0x200u;
constexpr std::size_t kTextLabelInitHeightOffset = 0x202u;
constexpr std::size_t kTextLabelFlagsOffset = 0x210u;
constexpr std::size_t kTextLabelPrimaryColorOffset = 0x358u;
constexpr std::size_t kTextLabelSecondaryColorOffset = 0x35Cu;
constexpr std::size_t kTextLabelTertiaryColorOffset = 0x360u;
constexpr std::size_t kTextLabelAlignmentWidthOffset = 0x36Cu;
constexpr std::size_t kTextLabelAlignmentHeightOffset = 0x36Eu;
constexpr std::size_t kTextLabelTailOffset = 0x370u;
constexpr std::size_t kTextLabelObjectBytes = 0x37Cu;
constexpr std::size_t kGuiComponentObjectBytes = 0xC4u;
constexpr unsigned int kButtonCode = 0xF143u;
constexpr int kLightningGuideNodeId = 3046;
constexpr int kTakeGuideNodeId = 3094;
constexpr std::size_t kCameraLayoutRootOffset = 0x9Cu;
constexpr int kRightTextAlignment = 4;
constexpr float kAuthoredOffsetX = 15.0f;
constexpr short kMissingGameGlyph = 0x25A1;

struct LocalizedGuideText {
    int language;
    const wchar_t* text;
    const char* name;
};

constexpr std::array<LocalizedGuideText, 8> kLocalizedTexts{{
    {0, L" HUD\x306E\x8868\x793A/\x975E\x8868\x793A", "Japanese"},
    {1, L" Show/hide HUD", "English"},
    {3, L" Mostra/nascondi HUD", "Italian"},
    {4, L" HUD ein-/ausblenden", "German"},
    {5, L" Afficher/masquer l'ATH", "French"},
    {6, L" Mostrar/ocultar HUD", "Spanish"},
    {8, L" HUD \xD45C\xC2DC/\xC228\xAE30\xAE30", "Korean"},
    {10, L" \x986F\x793A/\x96B1\x85CF HUD", "TraditionalChinese"},
}};

using VirtualNoArgumentFn = void* (__thiscall*)(void*);
using VirtualDestructorFn = int (__thiscall*)(void*, int);

template <typename T>
bool ReadObjectField(void* object, std::size_t offset, T& value) {
    if (!object || !patch::memory::IsReadable(
                       static_cast<std::byte*>(object) + offset,
                       sizeof(value))) {
        return false;
    }
    std::memcpy(&value, static_cast<std::byte*>(object) + offset,
                sizeof(value));
    return true;
}

void DestroyUnattachedGuiProduct(void* product) {
    if (!product || !patch::memory::IsReadable(product, sizeof(void*))) return;

    void* vtable{};
    std::memcpy(&vtable, product, sizeof(vtable));
    if (!vtable || !patch::memory::IsReadable(
                       vtable, sizeof(VirtualDestructorFn))) {
        return;
    }

    VirtualDestructorFn destructor{};
    std::memcpy(&destructor, vtable, sizeof(destructor));
    if (destructor) destructor(product, 1);
}

bool AttachOwnedChild(void* parent, void* child) {
    if (!parent || !child ||
        !patch::memory::IsReadable(
            parent, kGuiProductNextSiblingOffset + sizeof(void*))) {
        return false;
    }

    void* previousSibling{};
    std::memcpy(&previousSibling,
                static_cast<std::byte*>(parent) +
                    kGuiProductLastChildOffset,
                sizeof(previousSibling));
    if (previousSibling &&
        !patch::memory::IsReadable(
            static_cast<std::byte*>(previousSibling) +
                kGuiProductNextSiblingOffset,
            sizeof(void*))) {
        return false;
    }

    auto* const childBytes = static_cast<std::byte*>(child);
    std::memcpy(childBytes + kGuiProductPreviousSiblingOffset,
                &previousSibling, sizeof(previousSibling));
    std::memcpy(childBytes + kGuiProductParentOffset, &parent, sizeof(parent));
    if (previousSibling) {
        std::memcpy(static_cast<std::byte*>(previousSibling) +
                        kGuiProductNextSiblingOffset,
                    &child, sizeof(child));
    } else {
        std::memcpy(static_cast<std::byte*>(parent) +
                        kGuiProductFirstChildOffset,
                    &child, sizeof(child));
    }
    std::memcpy(static_cast<std::byte*>(parent) +
                    kGuiProductLastChildOffset,
                &child, sizeof(child));
    return true;
}

void* CallVirtualNoArgument(void* object, std::size_t methodOffset) {
    if (!object || !patch::memory::IsReadable(object, sizeof(void*))) {
        return nullptr;
    }

    void* vtable{};
    std::memcpy(&vtable, object, sizeof(vtable));
    if (!vtable || !patch::memory::IsReadable(
                       static_cast<std::byte*>(vtable) + methodOffset,
                       sizeof(VirtualNoArgumentFn))) {
        return nullptr;
    }

    VirtualNoArgumentFn method{};
    std::memcpy(&method, static_cast<std::byte*>(vtable) + methodOffset,
                sizeof(method));
    return method ? method(object) : nullptr;
}

float NativeGuideRowAdvance(void* lightningNode, void* takeNode,
                            void* originalLabel) {
    if (lightningNode && takeNode) {
        float lightningY{};
        float takeY{};
        if (ReadObjectField(lightningNode, kGuiProductPositionYOffset,
                            lightningY) &&
            ReadObjectField(takeNode, kGuiProductPositionYOffset, takeY)) {
            const float authoredAdvance = takeY - lightningY;
            if (authoredAdvance >= 8.0f && authoredAdvance <= 96.0f) {
                return authoredAdvance;
            }
        }
    }

    void* fontConfig{};
    std::int32_t fontSize{};
    if (ReadObjectField(originalLabel, kTextLabelFontConfigOffset,
                        fontConfig) &&
        fontConfig &&
        ReadObjectField(fontConfig, kFontPixelSizeOffset, fontSize) &&
        fontSize > 0 && fontSize <= 128) {
        return static_cast<float>(fontSize + 4);
    }

    short initialHeight{};
    if (ReadObjectField(originalLabel, kTextLabelInitHeightOffset,
                        initialHeight) &&
        initialHeight > 0 && initialHeight <= 128) {
        return static_cast<float>(initialHeight + 4);
    }
    return 32.0f;
}

float NativeGuideRowStepX(void* lightningLabel, void* takeLabel) {
    float lightningInnerX{};
    float takeInnerX{};
    if (!ReadObjectField(lightningLabel, kTextLabelInnerPositionXOffset,
                         lightningInnerX) ||
        !ReadObjectField(takeLabel, kTextLabelInnerPositionXOffset,
                         takeInnerX)) {
        return 0.0f;
    }

    const float step = lightningInnerX - takeInnerX;
    return step >= -96.0f && step <= 96.0f ? step : 0.0f;
}

const LocalizedGuideText& CurrentGuideText(const NativeApi& api) {
    const int language = api.gameLanguage ? api.gameLanguage() : 1;
    for (const auto& candidate : kLocalizedTexts) {
        if (candidate.language == language) return candidate;
    }
    return kLocalizedTexts[1];
}

bool BuildSource(const NativeApi& api, CreatedGuide& created,
                 const LocalizedGuideText*& localized) {
    localized = &CurrentGuideText(api);
    created.source = {};
    created.sourceByteCount = 0u;
    if (!api.wideToText || !localized->text) return false;

    const int textBytes =
        api.wideToText(nullptr, localized->text, kMissingGameGlyph);
    if (textBytes <= 0 ||
        static_cast<std::size_t>(textBytes) + 3u > created.source.size()) {
        return false;
    }

    created.source[0] = std::byte{static_cast<unsigned char>(
        kButtonCode >> 8u)};
    created.source[1] = std::byte{static_cast<unsigned char>(
        kButtonCode & 0xFFu)};
    const int converted = api.wideToText(
        reinterpret_cast<char*>(created.source.data() + 2u),
        localized->text, kMissingGameGlyph);
    if (converted != textBytes) {
        created.source = {};
        return false;
    }

    created.sourceByteCount = static_cast<std::uint16_t>(textBytes + 3u);
    return true;
}

} // namespace

float AlignLeftOrigin(void* container, float baseX, void* referenceLabel,
                      void* standaloneLabel, float authoredRowStepX) {
    float referenceInnerX{};
    float standaloneInnerX{};
    if (!ReadObjectField(referenceLabel, kTextLabelInnerPositionXOffset,
                         referenceInnerX) ||
        !ReadObjectField(standaloneLabel, kTextLabelInnerPositionXOffset,
                         standaloneInnerX)) {
        return 0.0f;
    }

    const float adjustment =
        referenceInnerX - standaloneInnerX + authoredRowStepX +
        kAuthoredOffsetX;
    if (adjustment < -512.0f || adjustment > 512.0f) return 0.0f;

    const float alignedX = baseX + adjustment;
    std::memcpy(static_cast<std::byte*>(container) +
                    kGuiProductPositionXOffset,
                &alignedX, sizeof(alignedX));
    auto* const dirtyFlags = reinterpret_cast<std::uint32_t*>(
        static_cast<std::byte*>(container) + kGuiProductDirtyFlagsOffset);
    *dirtyFlags |= 4u;
    return adjustment;
}

bool Create(void* camera, const NativeApi& api, CreatedGuide& created) {
    created = {};
    const auto reject = [](const char* reason) {
        Log("LR standalone Snapshot HUD guide rejected: %s",
            reason);
        return false;
    };
    if (!camera || !api.layoutNodeFind || !api.allocate ||
        !api.guiComponentConstructor || !api.textLabelBaseConstructor ||
        !api.textLabelTailConstructor || !api.textLabelInitialize ||
        !api.textLabelRebuild || !api.textLabelGeometry ||
        !api.gameLanguage || !api.wideToText ||
        !patch::memory::IsReadable(
            static_cast<std::byte*>(camera) + kCameraLayoutRootOffset,
            sizeof(void*))) {
        return reject("constructor functions unavailable");
    }

    void* layoutRoot{};
    std::memcpy(&layoutRoot,
                static_cast<std::byte*>(camera) + kCameraLayoutRootOffset,
                sizeof(layoutRoot));
    void* const layoutNode =
        api.layoutNodeFind(layoutRoot, kLightningGuideNodeId);
    void* const takeLayoutNode =
        api.layoutNodeFind(layoutRoot, kTakeGuideNodeId);
    void* layoutParent{};
    ReadObjectField(layoutNode, kGuiProductParentOffset, layoutParent);
    void* const nodePayload = CallVirtualNoArgument(layoutNode, 16u);
    void* const originalLabel = CallVirtualNoArgument(nodePayload, 72u);
    void* const takeNodePayload =
        CallVirtualNoArgument(takeLayoutNode, 16u);
    void* const takeLabel = CallVirtualNoArgument(takeNodePayload, 72u);
    if (!layoutNode || !layoutParent || !originalLabel ||
        !patch::memory::IsReadable(
            layoutNode,
            kGuiProductTransformOffset + kGuiProductTransformBytes) ||
        !patch::memory::IsReadable(originalLabel, kTextLabelObjectBytes)) {
        return reject("authored guide node, parent, or source label unavailable");
    }

    const LocalizedGuideText* localized{};
    if (!BuildSource(api, created, localized)) {
        return reject("localized native text conversion failed");
    }

    void* originalVtable{};
    void* fontConfig{};
    short initialWidth{};
    short initialHeight{};
    short textFlags{};
    short alignmentWidth{};
    short alignmentHeight{};
    std::uint32_t primaryColor{};
    std::uint32_t secondaryColor{};
    std::uint32_t tertiaryColor{};
    if (!ReadObjectField(originalLabel, 0u, originalVtable) ||
        !originalVtable ||
        !ReadObjectField(originalLabel, kTextLabelFontConfigOffset,
                         fontConfig) ||
        !ReadObjectField(originalLabel, kTextLabelInitWidthOffset,
                         initialWidth) ||
        !ReadObjectField(originalLabel, kTextLabelInitHeightOffset,
                         initialHeight) ||
        !ReadObjectField(originalLabel, kTextLabelFlagsOffset, textFlags) ||
        !ReadObjectField(originalLabel, kTextLabelAlignmentWidthOffset,
                         alignmentWidth) ||
        !ReadObjectField(originalLabel, kTextLabelAlignmentHeightOffset,
                         alignmentHeight) ||
        !ReadObjectField(originalLabel, kTextLabelPrimaryColorOffset,
                         primaryColor) ||
        !ReadObjectField(originalLabel, kTextLabelSecondaryColorOffset,
                         secondaryColor) ||
        !ReadObjectField(originalLabel, kTextLabelTertiaryColorOffset,
                         tertiaryColor) ||
        !fontConfig) {
        return reject("source label appearance contract unavailable");
    }
    if (!api.guiTextLabelVtable) {
        return reject("GuiTextLabel vtable unavailable");
    }

    void* const container = api.allocate(kGuiComponentObjectBytes);
    if (!container) return reject("container allocation failed");
    api.guiComponentConstructor(container);
    auto* const containerBytes = static_cast<std::byte*>(container);
    std::memcpy(containerBytes + kGuiProductTransformOffset,
                static_cast<std::byte*>(layoutNode) +
                    kGuiProductTransformOffset,
                kGuiProductTransformBytes);
    float containerY{};
    if (!ReadObjectField(container, kGuiProductPositionYOffset, containerY)) {
        DestroyUnattachedGuiProduct(container);
        return reject("container transform unavailable");
    }
    const float rowAdvance =
        NativeGuideRowAdvance(layoutNode, takeLayoutNode, originalLabel);
    const float rowStepX = NativeGuideRowStepX(originalLabel, takeLabel);
    containerY -= rowAdvance;
    std::memcpy(containerBytes + kGuiProductPositionYOffset, &containerY,
                sizeof(containerY));
    auto* const containerDirtyFlags = reinterpret_cast<std::uint32_t*>(
        containerBytes + kGuiProductDirtyFlagsOffset);
    *containerDirtyFlags |= 4u;

    void* const newLabel = api.allocate(kTextLabelObjectBytes);
    if (!newLabel) {
        DestroyUnattachedGuiProduct(container);
        return reject("label allocation failed");
    }
    api.textLabelBaseConstructor(newLabel);
    std::memcpy(newLabel, &api.guiTextLabelVtable,
                sizeof(api.guiTextLabelVtable));
    api.textLabelTailConstructor(
        static_cast<std::byte*>(newLabel) + kTextLabelTailOffset);
    api.textLabelInitialize(
        newLabel, fontConfig, initialWidth, initialHeight, 12, 1,
        static_cast<std::byte*>(newLabel) + kTextLabelTailOffset);

    auto* const originalBytes = static_cast<std::byte*>(originalLabel);
    auto* const newBytes = static_cast<std::byte*>(newLabel);
    std::memcpy(newBytes + kGuiProductTransformOffset,
                originalBytes + kGuiProductTransformOffset,
                kGuiProductTransformBytes);
    std::memcpy(newBytes + kTextLabelFlagsOffset, &textFlags,
                sizeof(textFlags));
    std::memcpy(newBytes + kTextLabelPrimaryColorOffset, &primaryColor,
                sizeof(primaryColor));
    std::memcpy(newBytes + kTextLabelSecondaryColorOffset, &secondaryColor,
                sizeof(secondaryColor));
    std::memcpy(newBytes + kTextLabelTertiaryColorOffset, &tertiaryColor,
                sizeof(tertiaryColor));

    api.textLabelRebuild(newLabel, created.source.data(), 0);
    int nativeWidth = alignmentWidth;
    if (nativeWidth <= 0) {
        float innerPositionX{};
        float measuredWidth{};
        float horizontalScale{};
        if (ReadObjectField(originalLabel, kTextLabelInnerPositionXOffset,
                            innerPositionX) &&
            ReadObjectField(originalLabel, kTextLabelMeasuredWidthOffset,
                            measuredWidth) &&
            ReadObjectField(originalLabel, 0x78u, horizontalScale) &&
            horizontalScale > 0.0f) {
            nativeWidth = static_cast<int>(
                innerPositionX + measuredWidth * horizontalScale + 0.5f);
        }
    }
    if (nativeWidth <= 0) nativeWidth = 384;
    int nativeHeight = alignmentHeight;
    if (nativeHeight <= 0) {
        nativeHeight = static_cast<int>(rowAdvance - 4.0f);
    }
    api.textLabelGeometry(
        newLabel, kRightTextAlignment, nativeWidth, nativeHeight);

    float containerX{};
    ReadObjectField(container, kGuiProductPositionXOffset, containerX);
    (void)AlignLeftOrigin(
        container, containerX, originalLabel, newLabel, rowStepX);
    auto* const dirtyFlags = reinterpret_cast<std::uint32_t*>(
        newBytes + kGuiProductDirtyFlagsOffset);
    *dirtyFlags |= 4u;

    if (!AttachOwnedChild(container, newLabel)) {
        DestroyUnattachedGuiProduct(newLabel);
        DestroyUnattachedGuiProduct(container);
        return reject("container rejected label child");
    }
    if (!AttachOwnedChild(layoutParent, container)) {
        DestroyUnattachedGuiProduct(container);
        return reject("authored guide parent rejected container");
    }

    created.label = newLabel;
    created.vtable = api.guiTextLabelVtable;
    created.alignmentContainer = container;
    created.alignmentReferenceLabel = originalLabel;
    created.alignmentBaseX = containerX;
    created.alignmentRowStepX = rowStepX;
    return true;
}

} // namespace novafix::game::lr_prompt_guides::snapshot_hud_guide

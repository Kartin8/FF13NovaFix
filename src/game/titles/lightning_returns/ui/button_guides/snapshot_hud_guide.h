#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace novafix::game::lr_prompt_guides::snapshot_hud_guide {

constexpr std::size_t kMaximumSourceBytes = 256u;

using LayoutNodeFindFn = void* (__cdecl*)(void*, int);
using GameAllocateFn = void* (__cdecl*)(std::size_t);
using TextLabelBaseConstructorFn = void* (__thiscall*)(void*);
using TextLabelTailConstructorFn = void (__thiscall*)(void*);
using TextLabelInitializeFn = int (__thiscall*)(
    void*, void*, short, short, int, int, const void*);
using TextLabelRebuildFn = int (__thiscall*)(void*, const void*, int);
using TextLabelGeometryFn = void (__thiscall*)(void*, int, int, int);
using GuiComponentConstructorFn = void* (__thiscall*)(void*);
using GameLanguageFn = int (__cdecl*)();
using GameWideToTextFn = int (__cdecl*)(char*, const wchar_t*, short);

struct NativeApi {
    LayoutNodeFindFn layoutNodeFind{};
    GameAllocateFn allocate{};
    TextLabelBaseConstructorFn textLabelBaseConstructor{};
    TextLabelTailConstructorFn textLabelTailConstructor{};
    TextLabelInitializeFn textLabelInitialize{};
    TextLabelRebuildFn textLabelRebuild{};
    TextLabelGeometryFn textLabelGeometry{};
    GuiComponentConstructorFn guiComponentConstructor{};
    void* guiTextLabelVtable{};
    GameLanguageFn gameLanguage{};
    GameWideToTextFn wideToText{};
};

struct CreatedGuide {
    void* label{};
    void* vtable{};
    void* alignmentContainer{};
    void* alignmentReferenceLabel{};
    float alignmentBaseX{};
    float alignmentRowStepX{};
    std::uint16_t sourceByteCount{};
    std::array<std::byte, kMaximumSourceBytes> source{};
};

bool Create(void* camera, const NativeApi& api, CreatedGuide& created);

float AlignLeftOrigin(void* container, float baseX, void* referenceLabel,
                      void* standaloneLabel, float authoredRowStepX);

} // namespace novafix::game::lr_prompt_guides::snapshot_hud_guide

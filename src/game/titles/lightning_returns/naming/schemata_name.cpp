#include "game/titles/lightning_returns/naming/schemata_name.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/titles/lightning_returns/ui/text_editor/editor.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace novafix::game::lr_schemata_rename {
namespace {

// Restore selection of StyleMenu's retained name state 5. Use the White editor;
// keep native confirmation, SetStyleName and save storage
constexpr std::uint32_t kStyleMenuUpdateRva = 0x004764A0u;
constexpr std::uint32_t kStyleMenuConfirmRva = 0x00477490u;
constexpr std::uint32_t kStyleMenuNavigateRva = 0x00470C00u;
constexpr std::uint32_t kSetStyleNameRva = 0x00525CA0u;
constexpr std::uint32_t kGetStyleNameRva = 0x00525CB0u;
constexpr std::uint32_t kRefreshStyleMenuRva = 0x00471980u;
constexpr std::uint32_t kSetListSelectionRva = 0x00492700u;
constexpr std::uint32_t kRefreshItemSelectionRva = 0x0046FC70u;
constexpr std::uint32_t kMaximumNameCharactersRva = 0x0046D930u;
constexpr std::uint32_t kPlayGuiSoundRva = 0x003A6D50u;
constexpr std::uint32_t kGuiManagerSlotRva = 0x04CF19F4u;

constexpr std::array<std::uint8_t, 16> kStyleMenuUpdatePrefix{
    0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4,
    0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04};
// Stop before the loader-relocated SEH handler operand
constexpr std::array<std::uint8_t, 5> kStyleMenuConfirmPrefix{
    0x55, 0x8B, 0xEC, 0x6A, 0xFF};
constexpr std::array<std::uint8_t, 14> kStyleMenuNavigatePrefix{
    0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x8B,
    0x8E, 0x60, 0x02, 0x00, 0x00, 0x85, 0xC9};
constexpr std::array<std::uint8_t, 5> kSetStyleNamePrefix{
    0x55, 0x8B, 0xEC, 0x5D, 0xE9};
constexpr std::array<std::uint8_t, 5> kGetStyleNamePrefix{
    0x55, 0x8B, 0xEC, 0x5D, 0xE9};
constexpr std::array<std::uint8_t, 10> kRefreshStyleMenuPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1, 0x8B};
constexpr std::array<std::uint8_t, 10> kSetListSelectionPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xB9, 0x5C, 0x04, 0x00, 0x00, 0x02};
constexpr std::array<std::uint8_t, 6> kRefreshItemSelectionPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14};
constexpr std::array<std::uint8_t, 16> kMaximumNameCharactersPrefix{
    0xE8, 0xCB, 0x64, 0xEE, 0xFF, 0x85, 0xC0, 0x74,
    0x10, 0x83, 0xF8, 0x07, 0x7E, 0x05, 0x83, 0xF8};
constexpr std::array<std::uint8_t, 6> kPlayGuiSoundPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C};

constexpr std::size_t kStyleMenuMinimumSize = 712u;
constexpr std::size_t kButtonEventSubobjectOffset = 128u;
constexpr std::size_t kNameWidgetOffset = 404u;
constexpr std::size_t kOptionalBottomWidgetOffset = 412u;
constexpr std::size_t kStateOffset = 344u;
constexpr std::size_t kSelectedStyleOffset = 588u;
constexpr std::size_t kEquipmentListOffset = 608u;
constexpr std::size_t kEquipmentListSelectionOffset = 1384u;
constexpr int kNameFieldState = 5;
constexpr int kSchemaEditorState = 7;
constexpr int kMaximumStyleCount = 9;
constexpr int kFirstEquipmentSelection = 0;
constexpr int kFirstAbilitySelection = 6;
constexpr int kLastAbilitySelection = 9;
constexpr int kNoEquipmentSelection = 10;

using StyleMenuUpdateFn = void (__thiscall*)(void*, float);
using StyleMenuConfirmFn = void (__thiscall*)(void*);
using StyleMenuNavigateFn = void (__thiscall*)(void*, char, char);
using SetStyleNameFn = int (__cdecl*)(int, const char*);
using GetStyleNameFn = int (__cdecl*)(int, char*, int);
using RefreshStyleMenuFn = int (__thiscall*)(void*);
using SetListSelectionFn = int (__thiscall*)(void*, int);
using RefreshItemSelectionFn = void (__thiscall*)(void*, int);
using MaximumNameCharactersFn = int (__cdecl*)();
using PlayGuiSoundFn = int (__thiscall*)(void*, int);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_styleMenuUpdateDetour;
patch::Detour32 g_styleMenuConfirmDetour;
patch::Detour32 g_styleMenuNavigateDetour;
StyleMenuUpdateFn g_originalStyleMenuUpdate{};
StyleMenuConfirmFn g_originalStyleMenuConfirm{};
StyleMenuNavigateFn g_originalStyleMenuNavigate{};
SetStyleNameFn g_setStyleName{};
GetStyleNameFn g_getStyleName{};
RefreshStyleMenuFn g_refreshStyleMenu{};
SetListSelectionFn g_setListSelection{};
RefreshItemSelectionFn g_refreshItemSelection{};
MaximumNameCharactersFn g_maximumNameCharacters{};
PlayGuiSoundFn g_playGuiSound{};
void** g_guiManagerSlot{};

struct RenameSession {
    void* menu{};
    int style{-1};
    std::array<char, 64> originalName{};
    bool active{};
};

RenameSession g_session;

template <typename Value>
Value& Field(void* object, std::size_t offset) {
    return *reinterpret_cast<Value*>(
        static_cast<std::byte*>(object) + offset);
}

bool Matches(
    const std::span<const std::byte>& bytes,
    std::span<const std::byte> expected) {
    return bytes.size() == expected.size() &&
        std::memcmp(bytes.data(), expected.data(), expected.size()) == 0;
}

template <std::size_t Size>
bool MatchesAt(
    const ImageView& image, std::uint32_t rva,
    const std::array<std::uint8_t, Size>& expected) {
    const auto bytes = image.At(rva, expected.size());
    return bytes && Matches(*bytes, std::as_bytes(std::span{expected}));
}

void ReadStyleName(int style, std::array<char, 64>& name) {
    name.fill('\0');
    if (g_getStyleName && style >= 0 && style < kMaximumStyleCount) {
        g_getStyleName(style, name.data(), static_cast<int>(name.size()));
        name.back() = '\0';
    }
}

void ResetSession() {
    g_session = {};
}

void PlayNavigationSound(char guideInput) {
    if (!g_playGuiSound || !g_guiManagerSlot ||
        !patch::memory::IsReadable(g_guiManagerSlot, sizeof(void*))) {
        return;
    }
    void* const manager = *g_guiManagerSlot;
    if (manager && patch::memory::IsReadable(manager, sizeof(void*))) {
        g_playGuiSound(manager, guideInput ? 29 : 5);
    }
}

bool SelectNameField(void* menu, char guideInput) {
    if (!menu || !g_setListSelection || !g_refreshItemSelection ||
        !patch::memory::IsReadable(menu, kStyleMenuMinimumSize)) {
        return false;
    }
    void* const nameWidget = Field<void*>(menu, kNameWidgetOffset);
    if (!nameWidget || !patch::memory::IsReadable(nameWidget, sizeof(void*))) {
        return false;
    }

    // Match Xbox's selectNameField: select name-widget index 0
    // PC's 0x491C30 clears a different list and leaves this row unfocused
    g_setListSelection(nameWidget, 0);
    g_refreshItemSelection(menu, kNoEquipmentSelection);
    Field<int>(menu, kStateOffset) = kNameFieldState;
    PlayNavigationSound(guideInput);
    return true;
}

bool BeginRename(void* menu) {
    if (!menu || g_session.active || lr_text_editor::IsOpen() ||
        !patch::memory::IsReadable(menu, kStyleMenuMinimumSize)) {
        return false;
    }
    const int style = Field<int>(menu, kSelectedStyleOffset);
    if (style < 0 || style >= kMaximumStyleCount) return false;

    RenameSession pending;
    pending.menu = menu;
    pending.style = style;
    pending.active = true;
    ReadStyleName(style, pending.originalName);
    const int maximumCharacters = g_maximumNameCharacters
        ? g_maximumNameCharacters() : 18;
    if (maximumCharacters <= 0 || maximumCharacters > 36 ||
        !lr_text_editor::OpenFromGameText(
            pending.originalName.data(),
            static_cast<std::size_t>(maximumCharacters))) {
        return false;
    }

    g_session = pending;
    return true;
}

void FinishRenameIfReady(void* menu) {
    if (!g_session.active || g_session.menu != menu ||
        !lr_text_editor::ResultReady()) {
        return;
    }
    if (!patch::memory::IsReadable(menu, kStyleMenuMinimumSize)) {
        return;
    }

    std::string utf8;
    bool submitted = false;
    if (!lr_text_editor::TakeResult(utf8, submitted)) return;

    if (submitted && g_setStyleName && g_refreshStyleMenu) {
        std::string gameText;
        if (lr_text_editor::ConvertUtf8ToGameText(utf8, gameText)) {
            g_setStyleName(g_session.style, gameText.c_str());
            g_refreshStyleMenu(menu);
        } else {
            LogWarning("LR Schemata name conversion failed: style=%d",
                       g_session.style);
        }
    }

    // RefreshStyleMenu can rebuild the list widgets. Re-select the native name
    // field after it completes so normal Up/Down navigation resumes there
    SelectNameField(menu, 0);
    ResetSession();
}

void __fastcall StyleMenuUpdateHook(
    void* menu, void*, float deltaSeconds) {
    hook_profiler::Scope totalTiming(
        hook_profiler::Point::LrSchemataUpdateOwn);
    const StyleMenuUpdateFn original = g_originalStyleMenuUpdate;
    if (!original) return;
    {
        hook_profiler::Scope originalTiming(
            hook_profiler::Point::LrSchemataUpdateOriginal);
        original(menu, deltaSeconds);
    }
    FinishRenameIfReady(menu);
}

void __fastcall StyleMenuConfirmHook(void* eventSubobject, void*) {
    hook_profiler::Scope totalTiming(
        hook_profiler::Point::LrSchemataConfirmOwn);
    const StyleMenuConfirmFn original = g_originalStyleMenuConfirm;
    if (!original) return;
    if (!eventSubobject || !patch::memory::IsReadable(
            eventSubobject, kStyleMenuMinimumSize -
                kButtonEventSubobjectOffset)) {
        hook_profiler::Scope originalTiming(
            hook_profiler::Point::LrSchemataConfirmOriginal);
        original(eventSubobject);
        return;
    }

    void* const menu = static_cast<std::byte*>(eventSubobject) -
        kButtonEventSubobjectOffset;
    if (Field<int>(menu, kStateOffset) == kNameFieldState) {
        if (g_session.active || BeginRename(menu)) return;
    }
    hook_profiler::Scope originalTiming(
        hook_profiler::Point::LrSchemataConfirmOriginal);
    original(eventSubobject);
}

void __fastcall StyleMenuNavigateHook(
    void* menu, void*, char direction, char guideInput) {
    hook_profiler::Scope totalTiming(
        hook_profiler::Point::LrSchemataNavigateOwn);
    const StyleMenuNavigateFn original = g_originalStyleMenuNavigate;
    if (!original) return;
    if (!menu || !patch::memory::IsReadable(
            menu, kStyleMenuMinimumSize) ||
        Field<int>(menu, kStateOffset) != kSchemaEditorState) {
        hook_profiler::Scope originalTiming(
            hook_profiler::Point::LrSchemataNavigateOriginal);
        original(menu, direction, guideInput);
        return;
    }

    void* const list = Field<void*>(menu, kEquipmentListOffset);
    if (!list || !patch::memory::IsReadable(
            list, kEquipmentListSelectionOffset + sizeof(int))) {
        hook_profiler::Scope originalTiming(
            hook_profiler::Point::LrSchemataNavigateOriginal);
        original(menu, direction, guideInput);
        return;
    }
    const int selection = Field<int>(list, kEquipmentListSelectionOffset);
    const bool movingUpFromTop =
        !direction && selection == kFirstEquipmentSelection;
    const bool movingDownFromBottom =
        direction && selection >= kFirstAbilitySelection &&
        selection <= kLastAbilitySelection &&
        !Field<void*>(menu, kOptionalBottomWidgetOffset);
    if (movingUpFromTop || movingDownFromBottom) {
        if (SelectNameField(menu, guideInput)) return;
    }
    hook_profiler::Scope originalTiming(
        hook_profiler::Point::LrSchemataNavigateOriginal);
    original(menu, direction, guideInput);
}

BOOL CALLBACK InitializeFeature(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::LightningReturns ||
        !Supports(Capability::SchemataRename)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) {
        LogWarning("LR Schemata rename disabled: invalid executable image");
        return TRUE;
    }

    const bool updateMatches = MatchesAt(
        *image, kStyleMenuUpdateRva, kStyleMenuUpdatePrefix);
    const bool confirmMatches = MatchesAt(
        *image, kStyleMenuConfirmRva, kStyleMenuConfirmPrefix);
    const bool navigateMatches = MatchesAt(
        *image, kStyleMenuNavigateRva, kStyleMenuNavigatePrefix);
    const bool helpersMatch =
        MatchesAt(*image, kSetStyleNameRva, kSetStyleNamePrefix) &&
        MatchesAt(*image, kGetStyleNameRva, kGetStyleNamePrefix) &&
        MatchesAt(*image, kRefreshStyleMenuRva, kRefreshStyleMenuPrefix) &&
        MatchesAt(*image, kSetListSelectionRva,
                  kSetListSelectionPrefix) &&
        MatchesAt(*image, kRefreshItemSelectionRva,
                  kRefreshItemSelectionPrefix) &&
        MatchesAt(*image, kMaximumNameCharactersRva,
                  kMaximumNameCharactersPrefix) &&
        MatchesAt(*image, kPlayGuiSoundRva, kPlayGuiSoundPrefix);
    const auto update = image->At(kStyleMenuUpdateRva, 1u);
    const auto confirm = image->At(kStyleMenuConfirmRva, 1u);
    const auto navigate = image->At(kStyleMenuNavigateRva, 1u);
    const auto setName = image->At(kSetStyleNameRva, 1u);
    const auto getName = image->At(kGetStyleNameRva, 1u);
    const auto refresh = image->At(kRefreshStyleMenuRva, 1u);
    const auto setListSelection = image->At(kSetListSelectionRva, 1u);
    const auto refreshItem = image->At(kRefreshItemSelectionRva, 1u);
    const auto maximumName = image->At(kMaximumNameCharactersRva, 1u);
    const auto playSound = image->At(kPlayGuiSoundRva, 1u);
    const auto guiManagerSlot = image->Address32(
        kGuiManagerSlotRva, sizeof(void*));
    if (!updateMatches || !confirmMatches || !navigateMatches ||
        !helpersMatch || !update || !confirm || !navigate || !setName ||
        !getName || !refresh || !setListSelection || !refreshItem ||
        !maximumName || !playSound || !guiManagerSlot) {
        LogWarning("LR Schemata rename disabled: native contract mismatch update=%d confirm=%d navigate=%d helpers=%d",
                   updateMatches ? 1 : 0, confirmMatches ? 1 : 0,
                   navigateMatches ? 1 : 0, helpersMatch ? 1 : 0);
        return TRUE;
    }

    g_setStyleName = reinterpret_cast<SetStyleNameFn>(
        const_cast<std::byte*>(setName->data()));
    g_getStyleName = reinterpret_cast<GetStyleNameFn>(
        const_cast<std::byte*>(getName->data()));
    g_refreshStyleMenu = reinterpret_cast<RefreshStyleMenuFn>(
        const_cast<std::byte*>(refresh->data()));
    g_setListSelection = reinterpret_cast<SetListSelectionFn>(
        const_cast<std::byte*>(setListSelection->data()));
    g_refreshItemSelection = reinterpret_cast<RefreshItemSelectionFn>(
        const_cast<std::byte*>(refreshItem->data()));
    g_maximumNameCharacters = reinterpret_cast<MaximumNameCharactersFn>(
        const_cast<std::byte*>(maximumName->data()));
    g_playGuiSound = reinterpret_cast<PlayGuiSoundFn>(
        const_cast<std::byte*>(playSound->data()));
    g_guiManagerSlot = reinterpret_cast<void**>(*guiManagerSlot);

    if (!g_styleMenuNavigateDetour.Install(
            "lr-native-schemata-name-focus",
            const_cast<std::byte*>(navigate->data()),
            reinterpret_cast<void*>(&StyleMenuNavigateHook),
            std::as_bytes(std::span{kStyleMenuNavigatePrefix}),
            reinterpret_cast<void**>(&g_originalStyleMenuNavigate))) {
        LogWarning("LR Schemata rename disabled: name-focus detour failed");
        return TRUE;
    }
    if (!g_styleMenuConfirmDetour.Install(
            "lr-native-schemata-name-confirm",
            const_cast<std::byte*>(confirm->data()),
            reinterpret_cast<void*>(&StyleMenuConfirmHook),
            std::as_bytes(std::span{kStyleMenuConfirmPrefix}),
            reinterpret_cast<void**>(&g_originalStyleMenuConfirm))) {
        g_styleMenuNavigateDetour.RevertAndClear(
            g_originalStyleMenuNavigate);
        LogWarning("LR Schemata rename disabled: name-confirm detour failed");
        return TRUE;
    }
    if (!g_styleMenuUpdateDetour.Install(
            "lr-native-schemata-name-result",
            const_cast<std::byte*>(update->data()),
            reinterpret_cast<void*>(&StyleMenuUpdateHook),
            std::as_bytes(std::span{kStyleMenuUpdatePrefix}),
            reinterpret_cast<void**>(&g_originalStyleMenuUpdate))) {
        g_styleMenuConfirmDetour.RevertAndClear(
            g_originalStyleMenuConfirm);
        g_styleMenuNavigateDetour.RevertAndClear(
            g_originalStyleMenuNavigate);
        LogWarning("LR Schemata rename disabled: result detour failed");
        return TRUE;
    }

    return TRUE;
}

} // namespace

void Initialize() {
    InitOnceExecuteOnce(
        &g_initializeOnce, &InitializeFeature, nullptr, nullptr);
}

} // namespace novafix::game::lr_schemata_rename

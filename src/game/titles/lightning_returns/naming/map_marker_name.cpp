#include "game/titles/lightning_returns/naming/map_marker_name.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/titles/lightning_returns/ui/text_editor/editor.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>

namespace novafix::game::lr_map_marker_rename {
namespace {

// Restore the missing rename row and keyboard completion. PC retains the
// 64-byte name field, bounded setter, save state and rendering
constexpr std::uint32_t kCreateSelectionWindowRva = 0x003A55E0u;
constexpr std::uint32_t kMarkerCommandListCallRva = 0x0043B81Eu;
constexpr std::uint32_t kMarkerCommandListenerRva = 0x0043DA30u;
constexpr std::uint32_t kResolveSelectionInputRva = 0x0043B130u;
constexpr std::uint32_t kOpenIconSelectionRva = 0x0043C3D0u;
constexpr std::uint32_t kOpenMarkerActionRva = 0x0043D990u;
constexpr std::uint32_t kDeleteMarkerRva = 0x0043B6F0u;
constexpr std::uint32_t kSetCommandHelpRva = 0x0043EA30u;
constexpr std::uint32_t kMarkerRecordByIndexRva = 0x000376C0u;
constexpr std::uint32_t kSetMarkerNameRva = 0x000373D0u;
constexpr std::uint32_t kRefreshMapProductRva = 0x00437430u;
constexpr std::uint32_t kPlayGuiSoundRva = 0x003A6D50u;
constexpr std::uint32_t kCloseGuiProductRva = 0x003AE780u;
constexpr std::uint32_t kMarkerStorageSlotRva = 0x01F509A8u;
constexpr std::uint32_t kGuiManagerSlotRva = 0x04CF19F4u;

constexpr std::array<std::uint8_t, 8> kCreateSelectionWindowPrefix{
    0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x8B, 0x44};
constexpr std::array<std::uint8_t, 12> kMarkerCommandListenerPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C,
    0x53, 0x56, 0x8B, 0x75, 0x0C, 0x57};
constexpr std::array<std::uint8_t, 12> kResolveSelectionInputPrefix{
    0x8B, 0x01, 0x8B, 0x50, 0x04, 0x56,
    0xFF, 0xD2, 0x8B, 0xF0, 0x85, 0xF6};
constexpr std::array<std::uint8_t, 8> kOpenIconSelectionPrefix{
    0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x2E, 0x1B};
constexpr std::array<std::uint8_t, 12> kOpenMarkerActionPrefix{
    0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1,
    0x8B, 0x8E, 0x90, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 10> kDeleteMarkerPrefix{
    0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0x90, 0x00, 0x00, 0x00, 0x85};
constexpr std::array<std::uint8_t, 10> kMarkerRecordByIndexPrefix{
    0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08, 0x69, 0xF6, 0xF0};
constexpr std::array<std::uint8_t, 12> kSetMarkerNamePrefix{
    0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D,
    0x08, 0x56, 0x57, 0x53, 0x8B, 0xF9};
// Stop before the loader-relocated SEH handler operand
constexpr std::array<std::uint8_t, 5> kRefreshMapProductPrefix{
    0x55, 0x8B, 0xEC, 0x6A, 0xFF};
constexpr std::array<std::uint8_t, 6> kPlayGuiSoundPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C};
constexpr std::array<std::uint8_t, 5> kMarkerCommandListCall{
    0xE8, 0xBD, 0x9D, 0xF6, 0xFF};

constexpr std::size_t kMenuMapProductOffset = 144u;
constexpr std::size_t kMenuSelectedMarkerOffset = 152u;
constexpr std::size_t kMenuCommandWindowOffset = 180u;
constexpr std::size_t kMapGroupOffset = 384u;
constexpr std::size_t kSelectionIndexOffset = 144u;
constexpr std::size_t kProductHandleOffset = 68u;
constexpr std::size_t kMarkerActiveOffset = 160u;
constexpr std::size_t kMarkerNameOffset = 172u;
constexpr std::size_t kMarkerRecordSize = 240u;
constexpr std::size_t kMarkerStorageDirtyOffset = 84492u;
constexpr std::size_t kMaximumMarkerNameCharacters = 20u;

using CreateSelectionWindowFn = int (__thiscall*)(
    void*, int, int, const char*, const char* const*, int, int, int, int,
    int, int, int, int, int);
using MarkerCommandListenerFn = void (__thiscall*)(void*, void*, void*);
using ResolveSelectionInputFn = int (__cdecl*)();
using OpenIconSelectionFn = void (__thiscall*)(void*, int);
using OpenMarkerActionFn = void (__thiscall*)(void*, int);
using DeleteMarkerFn = void (__thiscall*)(void*);
using SetCommandHelpFn = void (__thiscall*)(void*, const char*);
using MarkerRecordByIndexFn = void* (__thiscall*)(void*, int);
using SetMarkerNameFn = void (__thiscall*)(void*, const char*);
using RefreshMapProductFn = void (__thiscall*)(void*, int);
using PlayGuiSoundFn = int (__thiscall*)(void*, int);
using CloseGuiProductFn = void (__thiscall*)(void*, int, int);
using ProductObjectFn = void* (__thiscall*)(void*);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_markerCommandListenerDetour;
std::unique_ptr<patch::Transaction> g_markerCommandListCallPatch;
CreateSelectionWindowFn g_originalCreateSelectionWindow{};
MarkerCommandListenerFn g_originalMarkerCommandListener{};
ResolveSelectionInputFn g_resolveSelectionInput{};
OpenIconSelectionFn g_openIconSelection{};
OpenMarkerActionFn g_openMarkerAction{};
DeleteMarkerFn g_deleteMarker{};
SetCommandHelpFn g_setCommandHelp{};
MarkerRecordByIndexFn g_markerRecordByIndex{};
SetMarkerNameFn g_setMarkerName{};
RefreshMapProductFn g_refreshMapProduct{};
PlayGuiSoundFn g_playGuiSound{};
CloseGuiProductFn g_closeGuiProduct{};
void** g_markerStorageSlot{};
void** g_guiManagerSlot{};
std::string g_renameCommandLabel;

struct RenameSession {
    void* menu{};
    void* mapProduct{};
    void* markerRecord{};
    void* markerStorage{};
    bool active{};
};

RenameSession g_session;

template <typename Value>
Value& Field(void* object, std::size_t offset) {
    return *reinterpret_cast<Value*>(
        static_cast<std::byte*>(object) + offset);
}

template <std::size_t Size>
bool MatchesAt(const ImageView& image, std::uint32_t rva,
               const std::array<std::uint8_t, Size>& expected) {
    const auto bytes = image.At(rva, expected.size());
    return bytes && std::memcmp(bytes->data(), expected.data(),
                                expected.size()) == 0;
}

void* SelectionObject(void* selectionEvent) {
    if (!selectionEvent || !patch::memory::IsReadable(
            selectionEvent, sizeof(void*))) {
        return nullptr;
    }
    void** const vtable = *static_cast<void***>(selectionEvent);
    if (!vtable || !patch::memory::IsReadable(
            vtable, 2u * sizeof(void*))) {
        return nullptr;
    }
    const auto object = reinterpret_cast<ProductObjectFn>(vtable[1]);
    if (!object || !patch::memory::IsExecutable(
            reinterpret_cast<const void*>(object), 1u)) {
        return nullptr;
    }
    return object(selectionEvent);
}

int ResolveSelectionInput(void* selectionEvent, void* input) {
#if defined(_M_IX86) || defined(__i386__)
    int result = -1;
    ResolveSelectionInputFn function = g_resolveSelectionInput;
    __asm {
        push edi
        mov ecx, selectionEvent
        mov edi, input
        call dword ptr [function]
        mov result, eax
        pop edi
    }
    return result;
#else
    (void)selectionEvent;
    (void)input;
    return -1;
#endif
}

void CloseCommandWindow(void* menu) {
    if (!menu || !g_closeGuiProduct || !g_guiManagerSlot ||
        !patch::memory::IsReadable(
            menu, kMenuCommandWindowOffset + sizeof(void*)) ||
        !patch::memory::IsReadable(g_guiManagerSlot, sizeof(void*))) {
        return;
    }
    void* const window = Field<void*>(menu, kMenuCommandWindowOffset);
    void* const manager = *g_guiManagerSlot;
    if (!window || !manager || !patch::memory::IsReadable(
            window, kProductHandleOffset + sizeof(int))) {
        return;
    }
    const int handle = Field<int>(window, kProductHandleOffset);
    g_closeGuiProduct(manager, handle, 0);
    // Keep +0xB4 until native state-3 teardown restores the cursor, selection
    // and listener; clearing the product here skips that cleanup
}

void PlayReturnSound() {
    if (!g_playGuiSound || !g_guiManagerSlot ||
        !patch::memory::IsReadable(g_guiManagerSlot, sizeof(void*))) {
        return;
    }
    void* const manager = *g_guiManagerSlot;
    if (manager && patch::memory::IsReadable(manager, sizeof(void*))) {
        // Reuse Schemata's native focus-return sound
        g_playGuiSound(manager, 5);
    }
}

bool ResolveSelectedMarker(void* menu, RenameSession& session,
                           std::array<char, 64>& currentName) {
    currentName.fill('\0');
    if (!menu || !g_markerStorageSlot || !g_markerRecordByIndex ||
        !patch::memory::IsReadable(
            menu, kMenuSelectedMarkerOffset + sizeof(int)) ||
        !patch::memory::IsReadable(g_markerStorageSlot, sizeof(void*))) {
        return false;
    }
    void* const mapProduct = Field<void*>(menu, kMenuMapProductOffset);
    const int selected = Field<int>(menu, kMenuSelectedMarkerOffset);
    void* const storage = *g_markerStorageSlot;
    if (!mapProduct || !storage || selected < 0 || selected >= 10 ||
        !patch::memory::IsReadable(
            mapProduct, kMapGroupOffset + sizeof(int)) ||
        !patch::memory::IsReadable(
            storage, kMarkerStorageDirtyOffset + sizeof(int))) {
        return false;
    }
    const int group = Field<int>(mapProduct, kMapGroupOffset);
    if (group < 0 || group >= 32) return false;
    const int markerIndex = group * 10 + selected;
    void* const record = g_markerRecordByIndex(storage, markerIndex);
    if (!record || !patch::memory::IsReadable(record, kMarkerRecordSize) ||
        Field<int>(record, kMarkerActiveOffset) == -1) {
        return false;
    }

    const char* const source = static_cast<char*>(record) + kMarkerNameOffset;
    for (std::size_t index = 0u; index + 1u < currentName.size(); ++index) {
        if (!patch::memory::IsReadable(source + index, 1u)) return false;
        currentName[index] = source[index];
        if (source[index] == '\0') break;
    }
    currentName.back() = '\0';
    session = {menu, mapProduct, record, storage, true};
    return true;
}

bool BeginRename(void* menu) {
    if (g_session.active || lr_text_editor::IsOpen()) return false;
    RenameSession pending;
    std::array<char, 64> currentName{};
    if (!ResolveSelectedMarker(menu, pending, currentName) ||
        !lr_text_editor::OpenFromGameText(
            currentName.data(), kMaximumMarkerNameCharacters)) {
        return false;
    }
    g_session = pending;
    CloseCommandWindow(menu);
    return true;
}

int __fastcall CreateSelectionWindowHook(
    void* manager, void*, int a2, int a3, const char* title,
    const char* const* items, int count, int a7, int a8, int a9,
    int a10, int a11, int a12, int a13, int a14) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        timing, "hook.lr.map-marker-command-list-total");
    const CreateSelectionWindowFn original =
        g_originalCreateSelectionWindow;
    if (!original) return 0;
    // Hook only the map menu's command-list call, not every selection window
    if (items && count == 3 && !g_renameCommandLabel.empty()) {
        const std::array<const char*, 4> restoredItems{
            g_renameCommandLabel.c_str(), items[0], items[1], items[2],
        };
        return original(manager, a2, a3, title, restoredItems.data(),
                        static_cast<int>(restoredItems.size()), a7, a8, a9,
                        a10, a11, a12, a13, a14);
    }
    return original(manager, a2, a3, title, items, count, a7, a8, a9,
                    a10, a11, a12, a13, a14);
}

void UpdateCommandHelp(void* selectionEvent) {
    if (!g_setCommandHelp) return;
    void* const selection = SelectionObject(selectionEvent);
    if (!selection || !patch::memory::IsReadable(
            selection, kSelectionIndexOffset + sizeof(int))) {
        return;
    }
    const int index = Field<int>(selection, kSelectionIndexOffset);
    // Rename help is absent on PC. Reuse edit help without indexing past
    // the original three-entry table
    constexpr std::array<const char*, 4> kHelpKeys{
        "$m_res_fl_wmh06", "$m_res_fl_wmh06",
        "$m_res_fl_wmh07", "$m_res_fl_wmh08",
    };
    if (index >= 0 && index < static_cast<int>(kHelpKeys.size())) {
        // Pass the event's selection product, not its owning map menu:
        // this method writes a White string at +0x1F0
        g_setCommandHelp(selection,
                         kHelpKeys[static_cast<std::size_t>(index)]);
    }
}

void __fastcall MarkerCommandListenerHook(
    void* listener, void*, void* input, void* selectionEvent) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        timing, "hook.lr.map-marker-command-listener-total");
    if (!listener || !input || !selectionEvent ||
        !patch::memory::IsReadable(listener, 2u * sizeof(void*))) {
        if (g_originalMarkerCommandListener) {
            g_originalMarkerCommandListener(listener, input, selectionEvent);
        }
        return;
    }
    void* const menu = Field<void*>(listener, sizeof(void*));
    if (!menu || !patch::memory::IsReadable(
            menu, kMenuCommandWindowOffset + sizeof(void*))) {
        if (g_originalMarkerCommandListener) {
            g_originalMarkerCommandListener(listener, input, selectionEvent);
        }
        return;
    }

    const int action = ResolveSelectionInput(selectionEvent, input);
    if (action == -1) {
        UpdateCommandHelp(selectionEvent);
        return;
    }
    if (action < 0) return;

    switch (action) {
    case 0:
        BeginRename(menu);
        return;
    case 1:
        g_openIconSelection(menu, 1);
        return;
    case 2:
        g_openMarkerAction(menu, 1);
        return;
    case 3:
        g_deleteMarker(menu);
        return;
    default:
        LogWarning("LR map-marker command ignored: unexpected index=%d",
                   action);
        return;
    }
}

BOOL CALLBACK InitializeFeature(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::LightningReturns ||
        !Supports(Capability::MapMarkerRename)) {
        return TRUE;
    }
#if !defined(_M_IX86) && !defined(__i386__)
    LogWarning("LR map-marker rename disabled: x86 ABI required");
    return TRUE;
#else
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) {
        LogWarning("LR map-marker rename disabled: invalid executable image");
        return TRUE;
    }
    const bool contractMatches =
        MatchesAt(*image, kCreateSelectionWindowRva,
                  kCreateSelectionWindowPrefix) &&
        MatchesAt(*image, kMarkerCommandListenerRva,
                  kMarkerCommandListenerPrefix) &&
        MatchesAt(*image, kResolveSelectionInputRva,
                  kResolveSelectionInputPrefix) &&
        MatchesAt(*image, kOpenIconSelectionRva,
                  kOpenIconSelectionPrefix) &&
        MatchesAt(*image, kOpenMarkerActionRva,
                  kOpenMarkerActionPrefix) &&
        MatchesAt(*image, kDeleteMarkerRva, kDeleteMarkerPrefix) &&
        MatchesAt(*image, kMarkerRecordByIndexRva,
                  kMarkerRecordByIndexPrefix) &&
        MatchesAt(*image, kSetMarkerNameRva, kSetMarkerNamePrefix) &&
        MatchesAt(*image, kRefreshMapProductRva,
                  kRefreshMapProductPrefix) &&
        MatchesAt(*image, kPlayGuiSoundRva, kPlayGuiSoundPrefix) &&
        MatchesAt(*image, kMarkerCommandListCallRva,
                  kMarkerCommandListCall);
    if (!contractMatches) {
        LogWarning("LR map-marker rename disabled: native contract mismatch");
        return TRUE;
    }
    // ASCII needs no conversion. Native language tables are not ready during
    // feature initialization; calling the wide-text converter would assert
    g_renameCommandLabel = "Rename marker";

    const auto createSelection = image->At(kCreateSelectionWindowRva, 1u);
    const auto commandListener = image->At(kMarkerCommandListenerRva, 1u);
    const auto resolveInput = image->At(kResolveSelectionInputRva, 1u);
    const auto openIcons = image->At(kOpenIconSelectionRva, 1u);
    const auto openAction = image->At(kOpenMarkerActionRva, 1u);
    const auto deleteMarker = image->At(kDeleteMarkerRva, 1u);
    const auto setHelp = image->At(kSetCommandHelpRva, 1u);
    const auto recordByIndex = image->At(kMarkerRecordByIndexRva, 1u);
    const auto setName = image->At(kSetMarkerNameRva, 1u);
    const auto refreshMap = image->At(kRefreshMapProductRva, 1u);
    const auto playSound = image->At(kPlayGuiSoundRva, 1u);
    const auto closeProduct = image->At(kCloseGuiProductRva, 1u);
    const auto listCall = image->At(
        kMarkerCommandListCallRva, kMarkerCommandListCall.size());
    const auto markerStorageSlot = image->Address32(
        kMarkerStorageSlotRva, sizeof(void*));
    const auto guiManagerSlot = image->Address32(
        kGuiManagerSlotRva, sizeof(void*));
    if (!createSelection || !commandListener || !resolveInput ||
        !openIcons || !openAction || !deleteMarker || !setHelp ||
        !recordByIndex || !setName || !refreshMap || !playSound ||
        !closeProduct ||
        !listCall || !markerStorageSlot || !guiManagerSlot) {
        LogWarning("LR map-marker rename disabled: native address resolution failed");
        return TRUE;
    }

    g_resolveSelectionInput = reinterpret_cast<ResolveSelectionInputFn>(
        const_cast<std::byte*>(resolveInput->data()));
    g_openIconSelection = reinterpret_cast<OpenIconSelectionFn>(
        const_cast<std::byte*>(openIcons->data()));
    g_openMarkerAction = reinterpret_cast<OpenMarkerActionFn>(
        const_cast<std::byte*>(openAction->data()));
    g_deleteMarker = reinterpret_cast<DeleteMarkerFn>(
        const_cast<std::byte*>(deleteMarker->data()));
    g_setCommandHelp = reinterpret_cast<SetCommandHelpFn>(
        const_cast<std::byte*>(setHelp->data()));
    g_markerRecordByIndex = reinterpret_cast<MarkerRecordByIndexFn>(
        const_cast<std::byte*>(recordByIndex->data()));
    g_setMarkerName = reinterpret_cast<SetMarkerNameFn>(
        const_cast<std::byte*>(setName->data()));
    g_refreshMapProduct = reinterpret_cast<RefreshMapProductFn>(
        const_cast<std::byte*>(refreshMap->data()));
    g_playGuiSound = reinterpret_cast<PlayGuiSoundFn>(
        const_cast<std::byte*>(playSound->data()));
    g_closeGuiProduct = reinterpret_cast<CloseGuiProductFn>(
        const_cast<std::byte*>(closeProduct->data()));
    g_markerStorageSlot = reinterpret_cast<void**>(*markerStorageSlot);
    g_guiManagerSlot = reinterpret_cast<void**>(*guiManagerSlot);
    g_originalCreateSelectionWindow =
        reinterpret_cast<CreateSelectionWindowFn>(
            const_cast<std::byte*>(createSelection->data()));

    std::byte* const callAddress =
        const_cast<std::byte*>(listCall->data());
    const auto replacementCall = patch::EncodeRelativeCall32(
        reinterpret_cast<std::uintptr_t>(callAddress),
        reinterpret_cast<std::uintptr_t>(&CreateSelectionWindowHook));
    auto callPatch = std::make_unique<patch::Transaction>(
        "lr-map-marker-command-list-call",
        std::span<std::byte>(callAddress, kMarkerCommandListCall.size()),
        patch::registry::Kind::Detour);
    const bool callInstalled = replacementCall &&
        callPatch->Add(0u, std::as_bytes(std::span{kMarkerCommandListCall}),
                       *replacementCall) &&
        callPatch->Apply() == patch::ApplyStatus::Applied;
    if (callInstalled) {
        g_markerCommandListCallPatch = std::move(callPatch);
    }
    const bool listenerInstalled = callInstalled &&
        g_markerCommandListenerDetour.Install(
            "lr-map-marker-command-listener",
            const_cast<std::byte*>(commandListener->data()),
            reinterpret_cast<void*>(&MarkerCommandListenerHook),
            std::as_bytes(std::span{kMarkerCommandListenerPrefix}),
            reinterpret_cast<void**>(&g_originalMarkerCommandListener));
    if (!listenerInstalled) {
        if (g_markerCommandListCallPatch) {
            g_markerCommandListCallPatch->Revert();
            g_markerCommandListCallPatch.reset();
        }
        g_originalCreateSelectionWindow = nullptr;
        LogWarning("LR map-marker rename disabled: detour installation failed");
        return TRUE;
    }

    return TRUE;
#endif
}

} // namespace

void Initialize() {
    InitOnceExecuteOnce(
        &g_initializeOnce, &InitializeFeature, nullptr, nullptr);
}

void Pump() {
    if (!g_session.active) return;
    std::string utf8;
    bool submitted = false;
    if (!lr_text_editor::TakeResult(utf8, submitted)) return;

    bool stored = false;
    if (submitted && g_session.markerRecord && g_session.markerStorage &&
        patch::memory::IsReadable(g_session.markerRecord,
                                  kMarkerRecordSize) &&
        patch::memory::IsReadable(
            g_session.markerStorage,
            kMarkerStorageDirtyOffset + sizeof(int))) {
        std::string gameText;
        if (lr_text_editor::ConvertUtf8ToGameText(utf8, gameText) &&
            !gameText.empty()) {
            g_setMarkerName(g_session.markerRecord, gameText.c_str());
            Field<int>(g_session.markerStorage,
                       kMarkerStorageDirtyOffset) = 1;
            stored = true;
        }
    }
    if (stored && g_refreshMapProduct && g_session.menu &&
        g_session.mapProduct && patch::memory::IsReadable(
            g_session.menu, kMenuMapProductOffset + sizeof(void*)) &&
        Field<void*>(g_session.menu, kMenuMapProductOffset) ==
            g_session.mapProduct &&
        patch::memory::IsReadable(g_session.mapProduct,
                                  kMapGroupOffset + sizeof(int))) {
        g_refreshMapProduct(g_session.mapProduct, 0);
        PlayReturnSound();
    }
    g_session = {};
}

} // namespace novafix::game::lr_map_marker_rename

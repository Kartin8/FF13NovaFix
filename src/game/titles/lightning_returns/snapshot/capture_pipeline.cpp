#include "game/titles/lightning_returns/snapshot/capture_pipeline.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/titles/lightning_returns/snapshot/image_capture.h"
#include "game/titles/lightning_returns/snapshot/outerworld_menu.h"
#include "input/xinput_backend.h"
#include "overlay/overlay.h"
#include "patch/detour32.h"

#include <windows.h>
#include <d3d9.h>
#include <xinput.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <system_error>

namespace novafix::game::lr_snapshot::capture_pipeline {
namespace {

constexpr std::uint32_t kCameraAcceptRva = 0x00047EC0u;
constexpr std::uint32_t kCameraCancelRva = 0x00047F10u;
constexpr std::uint32_t kCameraUpdateRva = 0x00047E30u;
constexpr std::uint32_t kGuiDrawRva = 0x003934B0u;
constexpr std::uint32_t kTextureStreamReadRva = 0x00089A50u;
constexpr std::size_t kNativeSnapshotWidth = 1280u;
constexpr std::size_t kNativeSnapshotHeight = 720u;
constexpr std::size_t kNativeSnapshotBytes =
    kNativeSnapshotWidth * kNativeSnapshotHeight * 4u;

constexpr std::array<std::uint8_t, 16> kCameraAcceptPrefix{
    0x56, 0x8B, 0xF1, 0x83, 0xBE, 0x90, 0x00, 0x00,
    0x00, 0x00, 0x75, 0x3F, 0x6A, 0xFF, 0x6A, 0x00};
constexpr std::array<std::uint8_t, 15> kCameraCancelPrefix{
    0x83, 0xB9, 0x90, 0x00, 0x00, 0x00, 0x00, 0x75,
    0x2B, 0x8B, 0x81, 0x88, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 16> kCameraUpdatePrefix{
    0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x8B, 0x86,
    0x90, 0x00, 0x00, 0x00, 0x83, 0xE8, 0x02, 0x75};
constexpr std::array<std::uint8_t, 16> kGuiDrawPrefix{
    0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08, 0x57,
    0x8B, 0xF9, 0x0F, 0xBF, 0x87, 0xA0, 0x00, 0x00};
constexpr std::array<std::uint8_t, 15> kTextureStreamReadPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20, 0x53, 0x56,
    0x8B, 0x75, 0x0C, 0x57, 0x8B, 0x7D, 0x08};

using CameraAcceptFn = void (__thiscall*)(void*);
using CameraCancelFn = int (__thiscall*)(void*);
using CameraUpdateFn = void (__thiscall*)(void*, float);
using TextureStreamReadFn = int (__thiscall*)(void*, void*, std::size_t);

patch::Detour32 g_cameraAcceptDetour;
patch::Detour32 g_cameraCancelDetour;
patch::Detour32 g_cameraUpdateDetour;
patch::Detour32 g_guiDrawDetour;
patch::Detour32 g_textureStreamReadDetour;
CameraAcceptFn g_originalCameraAccept{};
CameraCancelFn g_originalCameraCancel{};
CameraUpdateFn g_originalCameraUpdate{};
void* g_originalGuiDraw{};
TextureStreamReadFn g_originalTextureStreamRead{};

std::atomic_bool g_nativeCaptureRequested{false};
std::atomic_bool g_highResolutionCaptureRequested{false};
std::atomic_bool g_highResolutionCaptureSucceeded{false};
constexpr LONG kGuiSuppressionCapture = 1 << 0;
constexpr LONG kGuiSuppressionPhotoModeHud = 1 << 1;
volatile LONG g_guiSuppressionReasons{};
volatile LONG g_suppressedGuiDrawObserved{};
volatile LONG g_guiSuppressionUpdateBudget{};
std::atomic_bool g_photoModeHudHidden{false};
bool g_photoModeYWasDown{};
std::mutex g_captureStateMutex;
std::string g_lastSavedRelativePath;
std::filesystem::path g_nativeFallbackPath;

void ResetPhotoModeHud() {
    g_photoModeYWasDown = false;
    g_photoModeHudHidden.store(false, std::memory_order_release);
    InterlockedAnd(&g_guiSuppressionReasons,
                   ~kGuiSuppressionPhotoModeHud);
}

bool IsPhotoModeYDown() {
    const HWND gameWindow = overlay::PresentationWindow();
    if (!gameWindow || GetForegroundWindow() != gameWindow ||
        IsIconic(gameWindow)) {
        return false;
    }

    if ((GetAsyncKeyState(VK_OEM_PERIOD) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_DECIMAL) & 0x8000) != 0) {
        return true;
    }
    for (unsigned index = 0; index < 4u; ++index) {
        std::uint16_t buttons{};
        if (input::xinput::GetButtons(index, buttons) &&
            (buttons & XINPUT_GAMEPAD_Y) != 0) {
            return true;
        }
    }
    return false;
}

void UpdatePhotoModeHudToggle() {
    const bool yDown = IsPhotoModeYDown();
    if (yDown && !g_photoModeYWasDown) {
        const bool hidden =
            !g_photoModeHudHidden.load(std::memory_order_acquire);
        g_photoModeHudHidden.store(hidden, std::memory_order_release);
        if (hidden) {
            InterlockedOr(&g_guiSuppressionReasons,
                          kGuiSuppressionPhotoModeHud);
        } else {
            InterlockedAnd(&g_guiSuppressionReasons,
                           ~kGuiSuppressionPhotoModeHud);
        }
    }
    g_photoModeYWasDown = yDown;
}

// GuiActionObject::draw uses ECX for the object, ESI for inherited render
// state, and one stack argument. A naked gate preserves that non-standard
// native ABI exactly while omitting the whole White GUI traversal for the
// capture frame or while the player has hidden the photo-mode HUD
void __declspec(naked) GuiDrawHook() {
    __asm {
        cmp dword ptr [g_guiSuppressionReasons], 0
        je forward_to_game
        mov dword ptr [g_suppressedGuiDrawObserved], 1
        xor eax, eax
        ret 4
    forward_to_game:
        jmp dword ptr [g_originalGuiDraw]
    }
}

void __fastcall CameraAcceptHook(void* camera, void*) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-camera-accept-total");
    const CameraAcceptFn original = g_originalCameraAccept;
    if (!original) return;

    const bool idle = camera &&
        *reinterpret_cast<const std::int32_t*>(
            static_cast<const std::byte*>(camera) + 144u) == 0;
    original(camera);
    if (idle && outerworld_menu::HasOfflineSnapshotMenu()) {
        {
            std::scoped_lock lock(g_captureStateMutex);
            g_nativeFallbackPath.clear();
        }
        g_highResolutionCaptureSucceeded.store(false,
                                                std::memory_order_release);
        g_nativeCaptureRequested.store(true, std::memory_order_release);
        InterlockedExchange(&g_suppressedGuiDrawObserved, 0);
        InterlockedExchange(&g_guiSuppressionUpdateBudget, 4);
        g_highResolutionCaptureRequested.store(true,
                                               std::memory_order_release);
        InterlockedOr(&g_guiSuppressionReasons, kGuiSuppressionCapture);
    }
}

int __fastcall CameraCancelHook(void* camera, void*) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-camera-cancel-total");
    ResetPhotoModeHud();
    return g_originalCameraCancel ? g_originalCameraCancel(camera) : 0;
}

void __fastcall CameraUpdateHook(void* camera, void*,
                                 float deltaSeconds) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-camera-update-total");
    const CameraUpdateFn original = g_originalCameraUpdate;
    if (!original) return;

    const std::int32_t cameraState = camera
        ? *reinterpret_cast<const std::int32_t*>(
              static_cast<const std::byte*>(camera) + 144u)
        : 3;
    if (cameraState < 3) {
        if (cameraState == 0) UpdatePhotoModeHudToggle();
    } else {
        ResetPhotoModeHud();
    }

    const bool nativeCaptureCompleted = cameraState == 2;
    if (nativeCaptureCompleted &&
        g_highResolutionCaptureRequested.load(
            std::memory_order_acquire)) {
        // Do not let SnsCameraMode reactivate the Submit menu before a frame
        // has actually passed through the GUI suppression gate
        if (InterlockedDecrement(&g_guiSuppressionUpdateBudget) >= 0) return;

        g_highResolutionCaptureRequested.store(false,
                                                std::memory_order_release);
        InterlockedAnd(&g_guiSuppressionReasons,
                       ~kGuiSuppressionCapture);
        InterlockedExchange(&g_suppressedGuiDrawObserved, 0);
        Log("LR GUI-less capture window expired: native 1280x720 fallback retained");
    }
    original(camera, deltaSeconds);
    if (camera &&
        *reinterpret_cast<const std::int32_t*>(
            static_cast<const std::byte*>(camera) + 144u) >= 3) {
        ResetPhotoModeHud();
    }
}

int __fastcall TextureStreamReadHook(void* stream, void*, void* listener,
                                     std::size_t size) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-texture-read-total");
    const TextureStreamReadFn original = g_originalTextureStreamRead;
    if (!original) return -1;

    const int result = original(stream, listener, size);
    if (result < 0 || size != kNativeSnapshotBytes ||
        !g_nativeCaptureRequested.exchange(false,
                                            std::memory_order_acq_rel)) {
        return result;
    }

    std::filesystem::path savedPath;
    if (!capture::SaveNative(listener, savedPath)) {
        LogError("LR could not save the native #backup pixel buffer");
        overlay::ShowNotification(
            "Snapshot could not be saved\nSee NovaFix.log for details.");
        return result;
    }

    const std::string relative =
        "snapshots\\" + savedPath.filename().string();
    if (g_highResolutionCaptureSucceeded.load(
            std::memory_order_acquire)) {
        std::error_code ignored;
        std::filesystem::remove(savedPath, ignored);
        return result;
    }
    {
        std::scoped_lock lock(g_captureStateMutex);
        g_lastSavedRelativePath = relative;
        g_nativeFallbackPath = savedPath;
    }
    Log("LR native Snapshot saved: %ls", savedPath.c_str());
    overlay::ShowNotification("Snapshot saved\n" + relative);
    return result;
}

} // namespace

bool HasExpectedContract(const ImageView& image) {
    return image.At(kCameraAcceptRva, kCameraAcceptPrefix.size()) &&
        image.At(kCameraCancelRva, kCameraCancelPrefix.size()) &&
        image.At(kCameraUpdateRva, kCameraUpdatePrefix.size()) &&
        image.At(kGuiDrawRva, kGuiDrawPrefix.size()) &&
        image.At(kTextureStreamReadRva,
                 kTextureStreamReadPrefix.size());
}

bool InstallHooks(const ImageView& image) {
    const auto cameraAccept =
        image.At(kCameraAcceptRva, kCameraAcceptPrefix.size());
    const auto cameraCancel =
        image.At(kCameraCancelRva, kCameraCancelPrefix.size());
    const auto cameraUpdate =
        image.At(kCameraUpdateRva, kCameraUpdatePrefix.size());
    const auto guiDraw = image.At(kGuiDrawRva, kGuiDrawPrefix.size());
    const auto textureStreamRead = image.At(
        kTextureStreamReadRva, kTextureStreamReadPrefix.size());
    if (!cameraAccept || !cameraCancel || !cameraUpdate || !guiDraw ||
        !textureStreamRead) {
        return false;
    }

    if (!g_cameraAcceptDetour.Install(
            "lr-offline-snapshot-camera-accept",
            const_cast<std::byte*>(cameraAccept->data()),
            reinterpret_cast<void*>(&CameraAcceptHook),
            std::as_bytes(std::span{kCameraAcceptPrefix}),
            reinterpret_cast<void**>(&g_originalCameraAccept))) {
        LogWarning("LR Snapshot disabled: camera accept detour failed");
        return false;
    }
    if (!g_cameraCancelDetour.Install(
            "lr-offline-snapshot-camera-cancel",
            const_cast<std::byte*>(cameraCancel->data()),
            reinterpret_cast<void*>(&CameraCancelHook),
            std::as_bytes(std::span{kCameraCancelPrefix}),
            reinterpret_cast<void**>(&g_originalCameraCancel))) {
        LogWarning("LR Snapshot disabled: camera cancel detour failed");
        return false;
    }
    if (!g_textureStreamReadDetour.Install(
            "lr-offline-snapshot-native-pixels",
            const_cast<std::byte*>(textureStreamRead->data()),
            reinterpret_cast<void*>(&TextureStreamReadHook),
            std::as_bytes(std::span{kTextureStreamReadPrefix}),
            reinterpret_cast<void**>(&g_originalTextureStreamRead))) {
        LogWarning("LR Snapshot disabled: native pixel stream detour failed");
        return false;
    }
    if (!g_cameraUpdateDetour.Install(
            "lr-offline-snapshot-camera-complete",
            const_cast<std::byte*>(cameraUpdate->data()),
            reinterpret_cast<void*>(&CameraUpdateHook),
            std::as_bytes(std::span{kCameraUpdatePrefix}),
            reinterpret_cast<void**>(&g_originalCameraUpdate))) {
        LogWarning("LR Snapshot disabled: camera completion detour failed");
        return false;
    }
    if (!g_guiDrawDetour.Install(
            "lr-offline-snapshot-one-frame-gui-suppression",
            const_cast<std::byte*>(guiDraw->data()),
            reinterpret_cast<void*>(&GuiDrawHook),
            std::as_bytes(std::span{kGuiDrawPrefix}),
            &g_originalGuiDraw)) {
        LogWarning("LR Snapshot disabled: one-frame GUI suppression detour failed");
        return false;
    }
    return true;
}

bool RevertHooks() {
    ResetPhotoModeHud();
    InterlockedExchange(&g_guiSuppressionReasons, 0);
    bool clean = true;
    clean = g_guiDrawDetour.RevertAndClear(g_originalGuiDraw) && clean;
    clean = g_cameraUpdateDetour.RevertAndClear(
                g_originalCameraUpdate) && clean;
    clean = g_textureStreamReadDetour.RevertAndClear(
                g_originalTextureStreamRead) && clean;
    clean = g_cameraCancelDetour.RevertAndClear(
                g_originalCameraCancel) && clean;
    clean = g_cameraAcceptDetour.RevertAndClear(
                g_originalCameraAccept) && clean;
    return clean;
}

void OnPresent(IDirect3DDevice9* device) {
    if (CurrentProfile().title != Title::LightningReturns) return;

    if (!g_highResolutionCaptureRequested.load(
            std::memory_order_acquire) ||
        InterlockedCompareExchange(
            &g_suppressedGuiDrawObserved, 0, 0) == 0) {
        return;
    }
    if (!g_highResolutionCaptureRequested.exchange(
            false, std::memory_order_acq_rel)) {
        return;
    }
    InterlockedAnd(&g_guiSuppressionReasons, ~kGuiSuppressionCapture);
    InterlockedExchange(&g_suppressedGuiDrawObserved, 0);

    std::filesystem::path savedPath;
    if (!capture::SaveBackBuffer(device, savedPath)) {
        Log("LR retained the native 1280x720 Snapshot fallback");
        return;
    }

    g_highResolutionCaptureSucceeded.store(true,
                                            std::memory_order_release);
    g_nativeCaptureRequested.store(false, std::memory_order_release);
    const std::string relative =
        "snapshots\\" + savedPath.filename().string();
    std::filesystem::path fallbackPath;
    {
        std::scoped_lock lock(g_captureStateMutex);
        fallbackPath = std::move(g_nativeFallbackPath);
        g_nativeFallbackPath.clear();
        g_lastSavedRelativePath = relative;
    }
    if (!fallbackPath.empty() && fallbackPath != savedPath) {
        std::error_code ignored;
        std::filesystem::remove(fallbackPath, ignored);
    }

    Log("LR full-resolution Snapshot saved: %ls",
        savedPath.c_str());
    overlay::ShowNotification("Snapshot saved\n" + relative);
}

} // namespace novafix::game::lr_snapshot::capture_pipeline

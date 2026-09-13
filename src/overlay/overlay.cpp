#include "overlay/overlay.h"
#include "overlay/overlay_style.h"

#include "diagnostics/log.h"
#include "compat/compatibility.h"
#include "game/core/game_profile.h"
#include "game/shared/ui/scissor_fix.h"
#include "overlay/input/overlay_gamepad.h"
#include "overlay/input/overlay_hotkey.h"
#include "input/win32_input.h"
#include "patch/registry.h"
#include "settings/performance_settings.h"
#include "settings/input_settings.h"
#include "overlay/settings_window.h"

#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace novafix::overlay {
namespace {

HWND g_window = nullptr;
IDirect3DDevice9* g_device = nullptr;
std::atomic_bool g_open{false};
std::atomic<ULONGLONG> g_renderSuppressedUntil{};
// ImGui is shared by Present/input/Reset/Release. Recursive because DX9
// shutdown releases the device and re-enters the Release hook
std::recursive_mutex g_contextMutex;
bool g_imguiReady = false;
enum class InitializationFailure {
    None,
    Context,
    Win32,
    Direct3D9,
};
InitializationFailure g_initializationFailure{InitializationFailure::None};
IDirect3DDevice9* g_initializationFailureDevice{};
struct HotkeyState {
    settings::OverlayHotkey binding{settings::OverlayHotkey::Disabled};
    bool wasDown{};
};

HotkeyState g_primaryHotkey;
HotkeyState g_alternateHotkey;
std::atomic_bool g_navigationFocusRequested{false};
bool g_mainPanelVisible = false;
float g_uiScale = 1.0f;
std::mutex g_textNotificationMutex;
std::string g_textNotification;
ULONGLONG g_textNotificationExpires{};
std::atomic_bool g_textNotificationActive{};

bool IsWindowActive() {
    return g_window && GetForegroundWindow() == g_window && !IsIconic(g_window);
}

bool ShouldLogInitializationFailure(
    InitializationFailure failure, IDirect3DDevice9* device) {
    if (g_initializationFailure == failure &&
        g_initializationFailureDevice == device) {
        return false;
    }
    g_initializationFailure = failure;
    g_initializationFailureDevice = device;
    return true;
}

bool Initialize(IDirect3DDevice9* device) {
    if (g_imguiReady) return device == g_device;
    if (!device || !g_window) return false;

    IMGUI_CHECKVERSION();
    if (!ImGui::CreateContext()) {
        if (ShouldLogInitializationFailure(
                InitializationFailure::Context, device)) {
            LogError("Dear ImGui context creation failed for device=%p; repeats suppressed",
                     device);
        }
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    g_uiScale = style::ResolveUiScale(g_window);
    style::ConfigureFonts(io, g_uiScale);
    style::ConfigureStyle(g_uiScale);
    if (!ImGui_ImplWin32_Init(g_window)) {
        ImGui::DestroyContext();
        if (ShouldLogInitializationFailure(
                InitializationFailure::Win32, device)) {
            LogError("Dear ImGui Win32 initialization failed for device=%p; repeats suppressed",
                     device);
        }
        return false;
    }
    if (!ImGui_ImplDX9_Init(device)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (ShouldLogInitializationFailure(
                InitializationFailure::Direct3D9, device)) {
            LogError("Dear ImGui Direct3D 9 initialization failed for device=%p; repeats suppressed",
                     device);
        }
        return false;
    }

    g_device = device;
    g_imguiReady = true;
    g_initializationFailure = InitializationFailure::None;
    g_initializationFailureDevice = nullptr;
    return true;
}

bool UpdateHotkey(settings::OverlayHotkey binding, HotkeyState& state) {
    const int virtualKey = input::OverlayHotkeyVirtualKey(binding);
    const bool down = virtualKey != 0 &&
                      (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    if (state.binding != binding) {
        state.binding = binding;
        state.wasDown = down;
        return false;
    }
    const bool pressed = down && !state.wasDown;
    state.wasDown = down;
    return pressed;
}

void ToggleIfRequested() {
    const settings::InputSettings settings = settings::ActiveInput();
    const bool primaryPressed =
        UpdateHotkey(settings.overlayHotkey, g_primaryHotkey);
    const bool alternatePressed =
        UpdateHotkey(settings.alternateOverlayHotkey, g_alternateHotkey);
    const bool gamepadPressed = input::overlay_gamepad::PollToggle(
        settings.overlayGamepadHotkey, IsOpen());
    if ((!primaryPressed && !alternatePressed && !gamepadPressed) ||
        !IsWindowActive()) {
        return;
    }
    const bool open = !g_open.load(std::memory_order_relaxed);
    if (!open) {
        std::scoped_lock contextLock(g_contextMutex);
        ui::FinishSettingsEdits();
    }
    g_open.store(open, std::memory_order_relaxed);
    g_mainPanelVisible = open;
    g_navigationFocusRequested.store(
        open && gamepadPressed, std::memory_order_release);
    if (!open) input::overlay_gamepad::HoldUntilReleased();
    if (open) {
        ClipCursor(nullptr);
        ReleaseCapture();
    }
}

void ClampCurrentWindow() {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 size = ImGui::GetWindowSize();
    size.x = std::min(size.x, std::max(1.0f, io.DisplaySize.x));
    size.y = std::min(size.y, std::max(1.0f, io.DisplaySize.y));
    ImGui::SetWindowSize(size, ImGuiCond_Always);

    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 clamped{
        std::clamp(position.x, 0.0f, std::max(0.0f, io.DisplaySize.x - size.x)),
        std::clamp(position.y, 0.0f, std::max(0.0f, io.DisplaySize.y - size.y))};
    if (clamped.x != position.x || clamped.y != position.y) {
        ImGui::SetWindowPos(clamped, ImGuiCond_Always);
    }
}

void DrawFpsCounter() {
    static float fps{};
    static float elapsed{};
    static unsigned frames{};

    elapsed += ImGui::GetIO().DeltaTime;
    ++frames;

    if (elapsed >= 0.1f) {
        fps = static_cast<float>(frames) / elapsed;
        frames = 0;
        elapsed = 0.0f;
    }

    if (fps <= 0.0f) return;

    ImGui::SetNextWindowPos(
        ImVec2(12.0f * g_uiScale, 12.0f * g_uiScale),
        ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);

    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(4.0f * g_uiScale, 2.0f * g_uiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs;

    ImGui::Begin("###NovaFixFpsCounter", nullptr, flags);
    ImGui::Text("FPS  %.1f", fps);
    ImGui::End();

    ImGui::PopStyleVar(2);
}

void DrawTextNotification(std::string_view text) {
    if (text.empty()) return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float margin = 16.0f * g_uiScale;
    ImGui::SetNextWindowPos(
        ImVec2(margin, std::max(0.0f, display.y - margin)),
        ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.88f);
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.07f, 0.08f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.42f, 0.42f, 0.44f, 0.75f));
    ImGui::Begin("###NovaFixTextNotification", nullptr, flags);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::End();
    ImGui::PopStyleColor(2);
}

void DrawPanelWindow() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 preferred{760.0f * g_uiScale, 540.0f * g_uiScale};
    const ImVec2 initial{std::min(preferred.x, display.x),
                         std::min(preferred.y, display.y)};
    const ImVec2 minimum{std::min(680.0f * g_uiScale, display.x),
                         std::min(360.0f * g_uiScale, display.y)};
    ImGui::SetNextWindowSize(initial, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(std::max(0.0f, (display.x - initial.x) * 0.5f),
                                  std::max(0.0f, (display.y - initial.y) * 0.5f)),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        minimum,
        ImVec2(std::max(minimum.x, display.x), std::max(minimum.y, display.y)));

    const bool requestNavigationFocus =
        g_navigationFocusRequested.exchange(false, std::memory_order_acq_rel);
    if (requestNavigationFocus) ImGui::SetNextWindowFocus();
    ImGui::Begin("NovaFix###NovaFixMain", &g_mainPanelVisible,
                 ImGuiWindowFlags_NoCollapse);
    if (requestNavigationFocus) ImGui::SetNavCursorVisible(true);
    ui::DrawSettingsWindow(g_window);
    ClampCurrentWindow();
    ImGui::End();
    if (!g_mainPanelVisible) {
        ui::FinishSettingsEdits();
        g_open.store(false, std::memory_order_release);
    }
}

void DrawOverlay(bool drawPanel, bool drawCounter,
                 std::string_view textNotification) {
    if (drawPanel) input::FeedOverlayMouse();
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    if (drawPanel) input::overlay_gamepad::FeedNavigation();
    ImGui::NewFrame();

    const bool anyPopupOpen = ImGui::IsPopupOpen(
        nullptr, ImGuiPopupFlags_AnyPopupId |
                 ImGuiPopupFlags_AnyPopupLevel);
    if (drawPanel && !anyPopupOpen && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        ui::FinishSettingsEdits();
        g_open.store(false, std::memory_order_release);
        g_mainPanelVisible = false;
        g_navigationFocusRequested.store(false, std::memory_order_release);
        input::overlay_gamepad::HoldUntilReleased();
        input::SetOverlayNativeCursorVisible(false);
        drawPanel = false;
    }

    if (drawCounter) DrawFpsCounter();
    if (!textNotification.empty()) DrawTextNotification(textNotification);
    if (drawPanel) {
        if (g_mainPanelVisible) DrawPanelWindow();
    }
    ImGui::Render();
    const game::scissor_fix::ScopedBypass preserveNativeOverlayClipRects;
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

} // namespace

void SelectPresentationWindow(HWND window) {
    if (!window || !IsWindow(window)) return;
    g_window = window;
    Log("Presentation window selected without WndProc subclass: hwnd=%p", window);
}

HWND PresentationWindow() {
    return g_window;
}

void ShowNotification(std::string_view text, ULONGLONG durationMs) {
    std::scoped_lock lock(g_textNotificationMutex);
    g_textNotification.assign(text);
    g_textNotificationExpires = GetTickCount64() + durationMs;
    g_textNotificationActive.store(!text.empty(), std::memory_order_release);
}

bool IsOpen() {
    return g_open.load(std::memory_order_relaxed);
}

bool WantsInput() {
    return IsOpen();
}

bool CapturesInput() {
    return WantsInput() && IsWindowActive();
}

bool CapturesControllerInput() {
    return IsWindowActive() &&
        (WantsInput() || input::overlay_gamepad::CapturesInput());
}

void CloseForFocusLoss() {
    input::overlay_gamepad::ClearCapture();
    g_navigationFocusRequested.store(false, std::memory_order_release);
    (void)g_open.exchange(false, std::memory_order_relaxed);
}

void PrepareForApplicationClose() {
    // Do not lock ImGui here: Present may be waiting on this window thread
    // Suppress new frames and let the current frame finish before D3D teardown
    const ULONGLONG deadline = GetTickCount64() + 2000u;
    ULONGLONG current = g_renderSuppressedUntil.load(std::memory_order_relaxed);
    while (current < deadline &&
           !g_renderSuppressedUntil.compare_exchange_weak(
               current, deadline, std::memory_order_release,
               std::memory_order_relaxed)) {}

    (void)g_open.exchange(false, std::memory_order_acq_rel);
    input::overlay_gamepad::ClearCapture();
    g_navigationFocusRequested.store(false, std::memory_order_release);
}

bool ForwardWin32InputMessage(HWND window, UINT message, WPARAM wParam,
                              LPARAM lParam) {
    std::scoped_lock contextLock(g_contextMutex);
    const bool topologyMessage = message == WM_DEVICECHANGE;
    if (!g_imguiReady || !ImGui::GetCurrentContext() ||
        (!CapturesInput() && !topologyMessage)) {
        return false;
    }
    if (!window) window = g_window;
    if (!window) return false;
    ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
    return true;
}

void OnPresent(IDirect3DDevice9* device) {
    if (!IsOpen() && g_mainPanelVisible) {
        std::scoped_lock contextLock(g_contextMutex);
        ui::FinishSettingsEdits();
        g_mainPanelVisible = false;
    }
    ULONGLONG suppressionUntil =
        g_renderSuppressedUntil.load(std::memory_order_acquire);
    if (suppressionUntil != 0) {
        const ULONGLONG now = GetTickCount64();
        if (now < suppressionUntil) {
            input::SetOverlayNativeCursorVisible(false);
            return;
        }
        g_renderSuppressedUntil.compare_exchange_strong(
            suppressionUntil, 0, std::memory_order_acq_rel,
            std::memory_order_relaxed);
    }

    // Hotkeys touch only atomics/Win32 state; avoid the ImGui lock while idle
    ToggleIfRequested();
    const bool drawPanel = IsOpen();
    const bool windowActive = IsWindowActive();
    input::SetOverlayNativeCursorVisible(windowActive && drawPanel);
    const bool drawCounter =
        settings::ActivePerformance().showFpsCounter;
    const bool notificationActive =
        g_textNotificationActive.load(std::memory_order_acquire);
    if (!drawPanel && !drawCounter && !notificationActive) {
        return;
    }

    std::scoped_lock contextLock(g_contextMutex);
    // A close request may have arrived between the fast path and acquiring the
    // context. Never begin a late ImGui frame during application shutdown
    suppressionUntil =
        g_renderSuppressedUntil.load(std::memory_order_acquire);
    if (suppressionUntil != 0 && GetTickCount64() < suppressionUntil) return;

    std::string textNotification;
    if (notificationActive) {
        const ULONGLONG now = GetTickCount64();
        std::scoped_lock lock(g_textNotificationMutex);
        if (g_textNotificationExpires != 0 && now < g_textNotificationExpires) {
            textNotification = g_textNotification;
        } else if (g_textNotificationExpires != 0) {
            g_textNotification.clear();
            g_textNotificationExpires = 0;
            g_textNotificationActive.store(false, std::memory_order_release);
        }
    }
    if ((!drawPanel && !drawCounter && textNotification.empty()) ||
        !windowActive) return;
    if (!g_device && !Initialize(device)) return;
    if (device != g_device || !g_imguiReady) return;
    // Use the Win32 cursor so its movement is not tied to frame rate
    ImGui::GetIO().MouseDrawCursor = false;
    if (device->TestCooperativeLevel() != D3D_OK) return;

    if (drawPanel) ClipCursor(nullptr);
    DrawOverlay(drawPanel, drawCounter, textNotification);
}

bool OwnsDevice(IDirect3DDevice9* device) {
    std::scoped_lock contextLock(g_contextMutex);
    return g_imguiReady && g_device == device;
}

void DetachDevice(IDirect3DDevice9* device) {
    std::scoped_lock contextLock(g_contextMutex);
    if (!g_imguiReady || g_device != device) return;
    ui::FinishSettingsEdits();

    // Mark detached before ImGui's Release re-enters the hook, avoiding a second detach
    g_imguiReady = false;
    g_device = nullptr;
    g_open.store(false, std::memory_order_release);
    input::overlay_gamepad::ClearCapture();
    g_navigationFocusRequested.store(false, std::memory_order_release);
    input::SetOverlayNativeCursorVisible(false);
    ImGui::GetIO().MouseDrawCursor = false;
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

void BeforeDeviceReset(IDirect3DDevice9* device) {
    std::scoped_lock contextLock(g_contextMutex);
    if (g_imguiReady && device == g_device) {
        ui::FinishSettingsEdits();
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
}

void AfterDeviceReset(IDirect3DDevice9* device, HRESULT result) {
    std::scoped_lock contextLock(g_contextMutex);
    if (SUCCEEDED(result) && g_imguiReady && device == g_device) ImGui_ImplDX9_CreateDeviceObjects();
}

} // namespace novafix::overlay

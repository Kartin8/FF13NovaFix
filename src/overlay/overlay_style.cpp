#include "overlay/overlay_style.h"

#include "compat/platform.h"
#include "diagnostics/log.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <string_view>

#include "imgui.h"

namespace novafix::overlay::style {
namespace {

constexpr int kEmbeddedUiFontResource = 101;
constexpr ImWchar kUiGlyphRanges[]{
    0x0020, 0x00FF,
    0x0400, 0x052F, // cyrillic
    0x25A2, 0x25A2, // square
    0x25B3, 0x25B3, // triangle
    0x25CB, 0x25CB, // circle
    0,
};
std::atomic_bool g_fontFallbackLogged{};

std::string Utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), required, nullptr, nullptr);
    return result;
}

bool AddSystemUiFont(ImGuiIO& io, float scale) {
    wchar_t windowsDirectory[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    const std::wstring fontsDirectory =
        std::wstring(windowsDirectory, length) + L"\\Fonts\\";
    constexpr const wchar_t* candidates[]{
        L"segoeui.ttf",
        L"tahoma.ttf",
        L"arial.ttf",
    };
    for (const wchar_t* candidate : candidates) {
        const std::wstring path = fontsDirectory + candidate;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        const std::string utf8Path = Utf8(path);
        if (!utf8Path.empty() &&
            io.Fonts->AddFontFromFileTTF(utf8Path.c_str(), 15.0f * scale,
                                         nullptr, kUiGlyphRanges)) {
            return true;
        }
    }
    return false;
}

bool AddEmbeddedUiFont(ImGuiIO& io, float scale) {
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(&kEmbeddedUiFontResource, &memory, sizeof(memory)) ||
        !memory.AllocationBase) {
        return false;
    }
    const HMODULE module = static_cast<HMODULE>(memory.AllocationBase);
    const HRSRC resource = FindResourceW(
        module, MAKEINTRESOURCEW(kEmbeddedUiFontResource), RT_RCDATA);
    if (!resource) return false;
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD size = SizeofResource(module, resource);
    void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || !size) return false;

    ImFontConfig config{};
    // Resource data is owned by the module; ImGui must not free it
    config.FontDataOwnedByAtlas = false;
    return io.Fonts->AddFontFromMemoryTTF(
               data, static_cast<int>(size), 15.0f * scale, &config,
               kUiGlyphRanges) != nullptr;
}

} // namespace

float ResolveUiScale(HWND window) {
    using GetDpiForWindowFn = UINT (WINAPI*)(HWND);
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto getDpiForWindow = user32
        ? reinterpret_cast<GetDpiForWindowFn>(
              GetProcAddress(user32, "GetDpiForWindow"))
        : nullptr;
    if (getDpiForWindow && window) {
        const UINT reported = getDpiForWindow(window);
        if (reported) dpi = reported;
    } else {
        HDC deviceContext = GetDC(window);
        if (deviceContext) {
            const int reported = GetDeviceCaps(deviceContext, LOGPIXELSX);
            if (reported > 0) dpi = static_cast<UINT>(reported);
            ReleaseDC(window, deviceContext);
        }
    }
    return std::clamp(static_cast<float>(dpi) /
                          static_cast<float>(USER_DEFAULT_SCREEN_DPI),
                      1.0f, 2.5f);
}

void ConfigureFonts(ImGuiIO& io, float scale) {
    const bool underWine =
        compat::CurrentEnvironment().host != compat::Host::Windows;
    const bool embeddedFont = underWine && AddEmbeddedUiFont(io, scale);
    const bool systemFont = !embeddedFont && AddSystemUiFont(io, scale);
    const bool lateEmbeddedFont =
        !embeddedFont && !systemFont && AddEmbeddedUiFont(io, scale);
    if (!embeddedFont && !systemFont && !lateEmbeddedFont) {
        ImFontConfig fontConfig{};
        fontConfig.SizePixels = 15.0f * scale;
        io.Fonts->AddFontDefault(&fontConfig);
        if (!g_fontFallbackLogged.exchange(
                true, std::memory_order_acq_rel)) {
            LogWarning("Overlay font fallback: Dear ImGui bitmap font");
        }
    }
}

void ConfigureStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.WindowRounding = 5.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 0.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.045f, 0.060f, 0.96f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.085f, 0.145f, 0.235f, 0.90f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.11f, 0.22f, 0.36f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.13f, 0.27f, 0.44f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.10f, 0.24f, 0.40f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.14f, 0.34f, 0.56f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.10f, 0.29f, 0.49f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.10f, 0.23f, 0.38f, 0.90f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.14f, 0.32f, 0.52f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.13f, 0.28f, 0.47f, 1.0f);
    colors[ImGuiCol_Tab] = ImVec4(0.07f, 0.13f, 0.21f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.14f, 0.32f, 0.52f, 1.0f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.11f, 0.26f, 0.43f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.68f, 1.0f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.60f, 0.92f, 1.0f);
    style.ScaleAllSizes(scale);
}

} // namespace novafix::overlay::style

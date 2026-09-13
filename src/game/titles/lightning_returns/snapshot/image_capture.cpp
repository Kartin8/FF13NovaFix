#include "game/titles/lightning_returns/snapshot/image_capture.h"

#include "diagnostics/log.h"
#include "patch/memory_access.h"

#include <windows.h>
#include <wincodec.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <system_error>
#include <vector>

namespace novafix::game::lr_snapshot::capture {
namespace {

constexpr std::size_t kNativeWidth = 1280u;
constexpr std::size_t kNativeHeight = 720u;
constexpr std::size_t kNativeBytes =
    kNativeWidth * kNativeHeight * sizeof(std::uint32_t);

template <typename T>
void ReleaseCom(T*& object) {
    if (!object) return;
    object->Release();
    object = nullptr;
}

std::filesystem::path ExecutableDirectory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    return std::filesystem::path(
               path.data(), path.data() + length).parent_path();
}

std::filesystem::path MakePath() {
    const std::filesystem::path directory =
        ExecutableDirectory() / L"snapshots";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return {};

    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    if (localtime_s(&local, &time) != 0) return {};
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

    wchar_t name[96]{};
    _snwprintf_s(name, _countof(name), _TRUNCATE,
                 L"LR_Snapshot_%04d-%02d-%02d_%02d-%02d-%02d-%03lld.png",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                 local.tm_hour, local.tm_min, local.tm_sec,
                 static_cast<long long>(milliseconds.count()));
    std::filesystem::path result = directory / name;
    for (unsigned suffix = 1;
         std::filesystem::exists(result, error) && !error; ++suffix) {
        wchar_t uniqueName[112]{};
        _snwprintf_s(
            uniqueName, _countof(uniqueName), _TRUNCATE,
            L"LR_Snapshot_%04d-%02d-%02d_%02d-%02d-%02d-%03lld_%u.png",
            local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
            local.tm_hour, local.tm_min, local.tm_sec,
            static_cast<long long>(milliseconds.count()), suffix);
        result = directory / uniqueName;
    }
    return error ? std::filesystem::path{} : result;
}

bool EncodePng(const std::filesystem::path& path, UINT width, UINT height,
               const std::vector<std::uint8_t>& pixels) {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(apartment);
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) return false;

    IWICImagingFactory* factory{};
    IWICStream* stream{};
    IWICBitmapEncoder* encoder{};
    IWICBitmapFrameEncode* frame{};
    IPropertyBag2* options{};
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
    if (SUCCEEDED(result)) {
        result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    }
    if (SUCCEEDED(result)) {
        result = factory->CreateEncoder(
            GUID_ContainerFormatPng, nullptr, &encoder);
    }
    if (SUCCEEDED(result)) {
        result = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(result)) {
        result = encoder->CreateNewFrame(&frame, &options);
    }
    if (SUCCEEDED(result)) result = frame->Initialize(options);
    if (SUCCEEDED(result)) result = frame->SetSize(width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(result)) result = frame->SetPixelFormat(&format);
    if (SUCCEEDED(result) &&
        !IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
        result = E_FAIL;
    }
    const UINT stride = width * 4u;
    if (SUCCEEDED(result) && pixels.size() <= UINT_MAX) {
        result = frame->WritePixels(
            height, stride, static_cast<UINT>(pixels.size()),
            const_cast<BYTE*>(pixels.data()));
    } else if (SUCCEEDED(result)) {
        result = E_INVALIDARG;
    }
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    const bool saved = SUCCEEDED(result);

    ReleaseCom(options);
    ReleaseCom(frame);
    ReleaseCom(encoder);
    ReleaseCom(stream);
    ReleaseCom(factory);
    if (uninitialize) CoUninitialize();
    return saved;
}

} // namespace

bool SaveNative(void* listener, std::filesystem::path& savedPath) {
    if (!listener || !patch::memory::IsReadable(listener, 24u)) return false;

    auto* listenerBytes = static_cast<std::byte*>(listener);
    void* const buffer =
        *reinterpret_cast<void**>(listenerBytes + sizeof(void*));
    const std::uint32_t bufferSize =
        *reinterpret_cast<const std::uint32_t*>(listenerBytes + 20u);
    if (!buffer || bufferSize != kNativeBytes ||
        !patch::memory::IsReadable(buffer, sizeof(void*))) {
        return false;
    }

    void** const vtable = *reinterpret_cast<void***>(buffer);
    if (!vtable ||
        !patch::memory::IsReadable(vtable, 2u * sizeof(void*)) ||
        !patch::memory::IsExecutable(vtable[1], 1u)) {
        return false;
    }
    using BufferDataFn = void* (__thiscall*)(void*);
    const auto data = reinterpret_cast<BufferDataFn>(vtable[1])(buffer);
    if (!data || !patch::memory::IsReadable(data, kNativeBytes)) {
        return false;
    }

    std::vector<std::uint8_t> pixels(kNativeBytes);
    std::memcpy(pixels.data(), data, pixels.size());
    for (std::size_t offset = 3u; offset < pixels.size(); offset += 4u) {
        pixels[offset] = 0xFFu;
    }

    savedPath = MakePath();
    const bool saved = !savedPath.empty() && EncodePng(
        savedPath, static_cast<UINT>(kNativeWidth),
        static_cast<UINT>(kNativeHeight), pixels);
    if (!saved && !savedPath.empty()) {
        std::error_code ignored;
        std::filesystem::remove(savedPath, ignored);
    }
    return saved;
}

bool SaveBackBuffer(IDirect3DDevice9* device,
                    std::filesystem::path& savedPath) {
    if (!device) return false;

    IDirect3DSurface9* backBuffer{};
    IDirect3DSurface9* resolved{};
    IDirect3DSurface9* systemMemory{};
    bool saved = false;

    HRESULT result = device->GetBackBuffer(
        0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    D3DSURFACE_DESC description{};
    if (SUCCEEDED(result) && backBuffer) {
        result = backBuffer->GetDesc(&description);
    }
    if (SUCCEEDED(result) &&
        description.Format != D3DFMT_A8R8G8B8 &&
        description.Format != D3DFMT_X8R8G8B8) {
        LogError("LR full-resolution Snapshot rejected back-buffer format %u",
            static_cast<unsigned>(description.Format));
        result = D3DERR_INVALIDCALL;
    }

    IDirect3DSurface9* gpuSource = backBuffer;
    if (SUCCEEDED(result) &&
        description.MultiSampleType != D3DMULTISAMPLE_NONE) {
        result = device->CreateRenderTarget(
            description.Width, description.Height, description.Format,
            D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr);
        if (SUCCEEDED(result)) {
            result = device->StretchRect(
                backBuffer, nullptr, resolved, nullptr, D3DTEXF_NONE);
            gpuSource = resolved;
        }
    }
    if (SUCCEEDED(result)) {
        result = device->CreateOffscreenPlainSurface(
            description.Width, description.Height, description.Format,
            D3DPOOL_SYSTEMMEM, &systemMemory, nullptr);
    }
    if (SUCCEEDED(result)) {
        result = device->GetRenderTargetData(gpuSource, systemMemory);
    }

    D3DLOCKED_RECT locked{};
    bool isLocked = false;
    std::vector<std::uint8_t> pixels;
    if (SUCCEEDED(result)) {
        result = systemMemory->LockRect(&locked, nullptr, D3DLOCK_READONLY);
        isLocked = SUCCEEDED(result);
    }
    if (SUCCEEDED(result)) {
        const std::size_t rowBytes =
            static_cast<std::size_t>(description.Width) * 4u;
        pixels.resize(rowBytes * description.Height);
        for (UINT y = 0; y < description.Height; ++y) {
            const auto* source =
                static_cast<const std::uint8_t*>(locked.pBits) +
                static_cast<std::ptrdiff_t>(y) * locked.Pitch;
            auto* destination = pixels.data() +
                static_cast<std::size_t>(y) * rowBytes;
            std::memcpy(destination, source, rowBytes);
            for (std::size_t x = 3u; x < rowBytes; x += 4u) {
                destination[x] = 0xFFu;
            }
        }
        savedPath = MakePath();
        saved = !savedPath.empty() && EncodePng(
            savedPath, description.Width, description.Height, pixels);
    }
    if (isLocked) systemMemory->UnlockRect();
    ReleaseCom(systemMemory);
    ReleaseCom(resolved);
    ReleaseCom(backBuffer);

    if (!saved) {
        LogError("LR full-resolution back-buffer capture failed: 0x%08lX",
            static_cast<unsigned long>(result));
        if (!savedPath.empty()) {
            std::error_code ignored;
            std::filesystem::remove(savedPath, ignored);
        }
    }
    return saved;
}

} // namespace novafix::game::lr_snapshot::capture

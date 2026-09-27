#include "d3d9/device/device_ui_hooks.h"

#include "compat/compatibility.h"
#include "display/scissor_transform.h"
#include "display/texture_policy.h"
#include "display/ui_render_policy.h"
#include "game/core/game_profile.h"
#include "game/shared/ui/scissor_fix.h"
#include "game/titles/ff13/camera/aspect_ratio.h"
#include "game/titles/ff13/ui/menu/menu_aspect.h"
#include "settings/graphics_settings.h"

#include <cstdint>

namespace novafix::d3d9::device_ui_hooks {

HRESULT STDMETHODCALLTYPE SetScissorRectHook(IDirect3DDevice9* self, const RECT* rectangle,
    const Originals& functions,
    DeviceExtent (*findExtent)(IDirect3DDevice9*)) {
    if (!functions.setScissorRect) return D3DERR_INVALIDCALL;
    if (!rectangle) {
        return functions.setScissorRect(self, rectangle);
    }
    if (!game::scissor_fix::ShouldTransformCurrentThread()) {
        return functions.setScissorRect(self, rectangle);
    }
    const bool xiiiAspect = game::ff13_aspect_ratio::IsActive();
    if (xiiiAspect && !game::ff13_aspect_ratio::IsDrawingGui()) {
        return functions.setScissorRect(self, rectangle);
    }
    const DeviceExtent record = findExtent(self);
    UINT targetWidth = record.backBufferWidth;
    UINT targetHeight = record.backBufferHeight;
    if (game::CurrentProfile().title == game::Title::FinalFantasyXIII) {
        // GUI passes can use a viewport smaller than the presentation buffer.
        D3DVIEWPORT9 viewport{};
        if (SUCCEEDED(self->GetViewport(&viewport)) &&
            viewport.X == 0u && viewport.Y == 0u &&
            viewport.Width >= 640u && viewport.Height >= 480u) {
            targetWidth = viewport.Width;
            targetHeight = viewport.Height;
        }
    }
    if (targetWidth < 640u || targetHeight < 480u) {
        return functions.setScissorRect(self, rectangle);
    }
    const display::ScissorRect source{
        rectangle->left, rectangle->top, rectangle->right, rectangle->bottom};
    display::ScissorRect menuListRect{};
    if (xiiiAspect && targetWidth == record.backBufferWidth &&
        targetHeight == record.backBufferHeight &&
        game::ff13_ui::menu_aspect::ActiveGuiListScissorRect(
            source, menuListRect)) {
        const RECT output{
            menuListRect.left, menuListRect.top,
            menuListRect.right, menuListRect.bottom};
        return functions.setScissorRect(self, &output);
    }
    const display::ScissorRect scaled = xiiiAspect
        ? display::FitAuthoredScissorRectToViewport(
            source, display::ResolveAspectRatio(
                targetWidth, targetHeight))
        : display::ScaleScissorRect(
            source, targetWidth, targetHeight);
    const RECT output{scaled.left, scaled.top, scaled.right, scaled.bottom};
    return functions.setScissorRect(self, &output);
}

HRESULT STDMETHODCALLTYPE CreateVertexBufferHook(
    IDirect3DDevice9* self, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
    IDirect3DVertexBuffer9** output, HANDLE* shared, const Originals& functions) {
    if (!functions.createVertexBuffer) return D3DERR_INVALIDCALL;
    const bool optimize = compat::compatibility::AllowsNovaFixChanges() &&
        game::Supports(game::Capability::UiVertexBufferOptimization) &&
        settings::Active().optimizeUiVertexBuffer &&
        display::IsNativeUiVertexBuffer(
            {length, usage, fvf, static_cast<std::uint32_t>(pool),
             shared != nullptr});
    HRESULT result = D3DERR_INVALIDCALL;
    if (optimize) {
        result = functions.createVertexBuffer(
            self, length, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, fvf,
            D3DPOOL_SYSTEMMEM, output, shared);
        if (FAILED(result)) {
            result = functions.createVertexBuffer(self, length, usage, fvf, pool, output, shared);
        }
    } else {
        result = functions.createVertexBuffer(self, length, usage, fvf, pool, output, shared);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE DrawPrimitiveUPHook(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE primitiveType, UINT primitiveCount,
    const void* vertexData, UINT stride, const Originals& functions) {
    if (!functions.drawPrimitiveUP) return D3DERR_INVALIDCALL;
    const auto submit = [&](const void* submittedVertices) {
        const HRESULT result = functions.drawPrimitiveUP(
            self, primitiveType, primitiveCount, submittedVertices, stride);
        return result;
    };
    if (primitiveType != D3DPT_TRIANGLEFAN || primitiveCount != 2 ||
        !settings::CorrectScreenSpaceAlignmentEnabled()) {
        return submit(vertexData);
    }
    if (!display::IsNativeScreenQuad(
            static_cast<const float*>(vertexData), stride)) {
        return submit(vertexData);
    }

    D3DVIEWPORT9 viewport{};
    display::ScreenQuad corrected{};
    if (SUCCEEDED(self->GetViewport(&viewport)) &&
        display::CorrectNativeScreenQuad(static_cast<const float*>(vertexData), stride,
                                         viewport.Width, viewport.Height, corrected)) {
        return submit(corrected.data());
    }
    return submit(vertexData);
}

} // namespace novafix::d3d9::device_ui_hooks

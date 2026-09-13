#pragma once

#include "game/titles/lightning_returns/ui/text_editor/native_api.h"
#include "game/titles/lightning_returns/ui/text_editor/session_context.h"

namespace novafix::game::lr_text_editor::detail {

bool CreateProduct(const NativeApi& api, EditorState& state);
bool RefreshProduct(const NativeApi& api, const EditorState& state);
bool SuppressProductVisuals(EditorState& state, bool suppress);
void CloseProduct(void* product);

} // namespace novafix::game::lr_text_editor::detail

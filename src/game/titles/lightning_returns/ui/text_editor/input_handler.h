#pragma once

#include "game/titles/lightning_returns/ui/text_editor/session_context.h"

#include <windows.h>

#include <cstdint>

namespace novafix::game::lr_text_editor::detail {

inline constexpr ULONGLONG kCursorBlinkIntervalMs = 500u;

void InitializeInput(EditorState& state);
void ResetCursorBlink(EditorState& state);

bool RecordControllerState(
    unsigned userIndex, bool connected, std::uint16_t buttons);
std::uint16_t CurrentControllerButtons();
void SetPreviousControllerButtons(std::uint16_t buttons);
void HandleControllerInput(
    EditorState& state, bool focused, bool connected,
    std::uint16_t buttons, bool oppositeConfirm);

void BeginInputRelease(const EditorState& state);
void UpdateInputReleaseLatch();
bool InputReleasePending();
bool ConsumeReleasedKeyboardMessage(const MSG& message);

bool HandleKeyboardMessage(
    EditorState& state, const MSG& message, bool unicodeMessage);

} // namespace novafix::game::lr_text_editor::detail

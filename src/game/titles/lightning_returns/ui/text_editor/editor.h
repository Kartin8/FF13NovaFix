#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace novafix::game::lr_text_editor {

bool Available();
bool OpenFromGameText(
    const char* initialText, std::size_t maximumCharacters);
bool TakeResult(std::string& text, bool& submitted);
bool ResultReady();
bool ConvertUtf8ToGameText(std::string_view source, std::string& text);
void RefreshPromptGlyphs();
void Pump();
bool IsOpen();
bool CapturesInput();
bool HandleInputMessage(const MSG& message, bool unicodeMessage);
bool HandleInputLanguageMessage(const MSG& message);
void ObserveControllerState(
    unsigned userIndex, bool connected, std::uint16_t buttons);

} // namespace novafix::game::lr_text_editor

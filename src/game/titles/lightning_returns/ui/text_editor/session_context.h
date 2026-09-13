#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace novafix::game::lr_text_editor::detail {

struct EditorState {
    bool open{};
    bool resultReady{};
    bool submitted{};
    void* product{};
    std::wstring text;
    std::size_t cursor{};
    std::size_t maximumCharacters{};
    std::uint16_t reservedInputRows{};
    std::uint16_t wrapWidth{};
    std::uint16_t cursorWidth{};
    float visibleScaleX{};
    float visibleScaleY{};
    bool visualsSuppressed{};
    std::string resultUtf8;
    wchar_t pendingHighSurrogate{};
    char pendingAnsiLeadByte{};
    bool returnKeyHeldOnOpen{};
    bool escapeKeyHeldOnOpen{};
    bool refreshRequested{};
    bool closeRequested{};
    bool submitRequested{};
    unsigned releaseKeyboardKey{};
    std::uint16_t releaseControllerButtons{};
    bool cursorVisible{true};
    ULONGLONG nextCursorBlink{};
};

} // namespace novafix::game::lr_text_editor::detail

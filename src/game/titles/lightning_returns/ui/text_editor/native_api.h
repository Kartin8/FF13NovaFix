#pragma once

#include <windows.h>

namespace novafix::game::lr_text_editor {

using GameTextToWideFn = unsigned int (__cdecl*)(wchar_t*, const char*);
using CreateSystemMessageFn = void* (__cdecl*)(const char*, void*, int, int);
using SetGuiLayerFn = void (__thiscall*)(void*, int);
using GameLanguageFn = int (__cdecl*)();
using GameWideToTextFn = int (__cdecl*)(char*, const wchar_t*, short);
using GameStringAssignFn = void (__thiscall*)(void*, const char*);
using TextLabelWrapperFn = void (__thiscall*)(void*, const void*);

struct NativeApi {
    GameTextToWideFn gameTextToWide{};
    CreateSystemMessageFn createSystemMessage{};
    SetGuiLayerFn setGuiLayer{};
    GameLanguageFn gameLanguage{};
    GameWideToTextFn gameWideToText{};
    GameStringAssignFn gameStringAssign{};
    TextLabelWrapperFn textLabelWrapper{};
};

class BackendRedirectScope {
public:
    BackendRedirectScope();
    BackendRedirectScope(const BackendRedirectScope&) = delete;
    BackendRedirectScope& operator=(const BackendRedirectScope&) = delete;
    ~BackendRedirectScope();
};

void Configure(const NativeApi& api);
bool BackendRedirectRequested();

} // namespace novafix::game::lr_text_editor

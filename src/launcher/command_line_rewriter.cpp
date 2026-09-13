#include "launcher/command_line_rewriter.h"

#include <cctype>
#include <cwctype>
#include <string>

namespace novafix::launcher {
namespace {

template <typename Char>
bool IsSpace(Char value);

template <>
bool IsSpace(char value) {
    return std::isspace(static_cast<unsigned char>(value)) != 0;
}

template <>
bool IsSpace(wchar_t value) {
    return std::iswspace(value) != 0;
}

template <typename Char>
Char Lower(Char value);

template <>
char Lower(char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

template <>
wchar_t Lower(wchar_t value) {
    return static_cast<wchar_t>(std::towlower(value));
}

template <typename Char>
bool EqualInsensitive(std::basic_string_view<Char> left, std::basic_string_view<Char> right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (Lower(left[i]) != Lower(right[i])) return false;
    }
    return true;
}

template <typename Char>
void ReplaceOrAppend(std::basic_string<Char>& commandLine,
                     std::basic_string_view<Char> prefix,
                     std::basic_string_view<Char> value) {
    for (size_t position = 0; position + prefix.size() <= commandLine.size(); ++position) {
        if (position != 0 && !IsSpace(commandLine[position - 1])) continue;
        if (!EqualInsensitive<Char>(std::basic_string_view<Char>(commandLine).substr(position, prefix.size()), prefix)) {
            continue;
        }
        size_t end = position + prefix.size();
        while (end < commandLine.size() && !IsSpace(commandLine[end])) ++end;
        commandLine.replace(position, end - position, std::basic_string<Char>(prefix) + std::basic_string<Char>(value));
        return;
    }

    if (!commandLine.empty() && !IsSpace(commandLine.back())) commandLine.push_back(static_cast<Char>(' '));
    commandLine.append(prefix);
    commandLine.append(value);
}

template <typename Char>
std::basic_string<Char> Number(unsigned value);

template <>
std::string Number(unsigned value) {
    return std::to_string(value);
}

template <>
std::wstring Number(unsigned value) {
    return std::to_wstring(value);
}

template <typename Char>
std::basic_string<Char> Rewrite(std::basic_string_view<Char> original,
                                const settings::GraphicsSettings& graphics,
                                unsigned fullscreenWidth,
                                unsigned fullscreenHeight,
                                std::basic_string_view<Char> fullscreenPrefix,
                                std::basic_string_view<Char> widthPrefix,
                                std::basic_string_view<Char> heightPrefix,
                                std::basic_string_view<Char> shadowPrefix,
                                std::basic_string_view<Char> msaaPrefix,
                                std::basic_string_view<Char> disable) {
    std::basic_string<Char> rewritten(original);
    const bool borderlessFullscreen =
        graphics.displayMode == settings::DisplayMode::BorderlessFullscreen;
    const unsigned width = borderlessFullscreen && fullscreenWidth ? fullscreenWidth : graphics.width;
    const unsigned height = borderlessFullscreen && fullscreenHeight ? fullscreenHeight : graphics.height;
    ReplaceOrAppend(rewritten, fullscreenPrefix, disable);
    ReplaceOrAppend(rewritten, widthPrefix, std::basic_string_view<Char>(Number<Char>(width)));
    ReplaceOrAppend(rewritten, heightPrefix, std::basic_string_view<Char>(Number<Char>(height)));
    ReplaceOrAppend(rewritten, shadowPrefix, std::basic_string_view<Char>(Number<Char>(graphics.shadowResolution)));
    ReplaceOrAppend(rewritten, msaaPrefix, std::basic_string_view<Char>(Number<Char>(graphics.msaaSamples)));
    return rewritten;
}

} // namespace

std::string RewriteGraphicsOptions(std::string_view commandLine,
                                   const settings::GraphicsSettings& graphics,
                                   unsigned fullscreenWidth,
                                   unsigned fullscreenHeight) {
    return Rewrite<char>(commandLine, graphics, fullscreenWidth, fullscreenHeight,
                         "-FullScreenMode=", "-Width=", "-Height=", "-Shadow=", "-MSAA=",
                         "Disable");
}

std::wstring RewriteGraphicsOptions(std::wstring_view commandLine,
                                    const settings::GraphicsSettings& graphics,
                                    unsigned fullscreenWidth,
                                    unsigned fullscreenHeight) {
    return Rewrite<wchar_t>(commandLine, graphics, fullscreenWidth, fullscreenHeight,
                            L"-FullScreenMode=", L"-Width=", L"-Height=", L"-Shadow=", L"-MSAA=",
                            L"Disable");
}

} // namespace novafix::launcher

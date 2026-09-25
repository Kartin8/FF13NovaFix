#include "launcher/command_line_rewriter.h"

#include <cctype>
#include <cwctype>
#include <span>
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
bool HasToken(std::basic_string_view<Char> commandLine,
              std::basic_string_view<Char> token) {
    for (size_t position = 0; position + token.size() <= commandLine.size();
         ++position) {
        if (position != 0 && !IsSpace(commandLine[position - 1])) continue;
        const size_t end = position + token.size();
        if (end != commandLine.size() && !IsSpace(commandLine[end])) continue;
        if (EqualInsensitive(commandLine.substr(position, token.size()),
                             token)) {
            return true;
        }
    }
    return false;
}

template <typename Char>
void RemoveToken(std::basic_string<Char>& commandLine,
                 std::basic_string_view<Char> token) {
    for (size_t position = 0; position + token.size() <= commandLine.size();
         ++position) {
        if (position != 0 && !IsSpace(commandLine[position - 1])) continue;
        const size_t end = position + token.size();
        if (end != commandLine.size() && !IsSpace(commandLine[end])) continue;
        if (!EqualInsensitive(
                std::basic_string_view<Char>(commandLine).substr(
                    position, token.size()), token)) {
            continue;
        }
        size_t eraseBegin = position;
        size_t eraseEnd = end;
        if (eraseBegin != 0 && IsSpace(commandLine[eraseBegin - 1])) {
            --eraseBegin;
        } else {
            while (eraseEnd < commandLine.size() &&
                   IsSpace(commandLine[eraseEnd])) {
                ++eraseEnd;
            }
        }
        commandLine.erase(eraseBegin, eraseEnd - eraseBegin);
        return;
    }
}

template <typename Char>
void EnsureToken(std::basic_string<Char>& commandLine,
                 std::basic_string_view<Char> token) {
    if (HasToken<Char>(commandLine, token)) return;
    if (!commandLine.empty() && !IsSpace(commandLine.back())) {
        commandLine.push_back(static_cast<Char>(' '));
    }
    commandLine.append(token);
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
                                const settings::GameSettings& gameSettings,
                                unsigned fullscreenWidth,
                                unsigned fullscreenHeight,
                                bool rewriteGraphics,
                                bool rewriteLanguages,
                                std::basic_string_view<Char> fullscreenPrefix,
                                std::basic_string_view<Char> widthPrefix,
                                std::basic_string_view<Char> heightPrefix,
                                std::basic_string_view<Char> shadowPrefix,
                                std::basic_string_view<Char> msaaPrefix,
                                std::basic_string_view<Char> disable,
                                std::basic_string_view<Char> languagePrefix,
                                std::basic_string_view<Char> areaPrefix,
                                std::basic_string_view<Char> voiceFlag,
                                std::span<const std::basic_string_view<Char>>
                                    languageCodes,
                                std::span<const std::basic_string_view<Char>>
                                    areaCodes) {
    std::basic_string<Char> rewritten(original);
    if (rewriteGraphics) {
        const bool borderlessFullscreen =
            graphics.displayMode == settings::DisplayMode::BorderlessFullscreen;
        const unsigned width = borderlessFullscreen && fullscreenWidth
            ? fullscreenWidth : graphics.width;
        const unsigned height = borderlessFullscreen && fullscreenHeight
            ? fullscreenHeight : graphics.height;
        ReplaceOrAppend(rewritten, fullscreenPrefix, disable);
        ReplaceOrAppend(rewritten, widthPrefix,
                        std::basic_string_view<Char>(Number<Char>(width)));
        ReplaceOrAppend(rewritten, heightPrefix,
                        std::basic_string_view<Char>(Number<Char>(height)));
        ReplaceOrAppend(
            rewritten, shadowPrefix,
            std::basic_string_view<Char>(Number<Char>(
                graphics.shadowResolution)));
        ReplaceOrAppend(
            rewritten, msaaPrefix,
            std::basic_string_view<Char>(Number<Char>(graphics.msaaSamples)));
    }
    if (rewriteLanguages) {
        const unsigned interfaceLanguage =
            static_cast<unsigned>(gameSettings.interfaceLanguage);
        if (interfaceLanguage < languageCodes.size() &&
            !languageCodes[interfaceLanguage].empty()) {
            ReplaceOrAppend(
                rewritten, languagePrefix, languageCodes[interfaceLanguage]);
            ReplaceOrAppend(
                rewritten, areaPrefix, areaCodes[interfaceLanguage]);
        }
        if (gameSettings.voiceLanguage == settings::VoiceLanguage::English) {
            RemoveToken<Char>(rewritten, voiceFlag);
        } else {
            EnsureToken<Char>(rewritten, voiceFlag);
        }
    }
    return rewritten;
}

} // namespace

std::string RewriteLaunchOptions(
    std::string_view commandLine,
    const settings::GraphicsSettings& graphics,
    const settings::GameSettings& gameSettings,
    unsigned fullscreenWidth, unsigned fullscreenHeight,
    bool rewriteGraphics, bool rewriteLanguages) {
    constexpr std::string_view languageCodes[]{
        "US", "JP", "GR", "FR", "IT", "SP", "KR", "CH"};
    constexpr std::string_view areaCodes[]{
        "US", "JP", "INTER", "INTER", "INTER", "INTER", "CH", "CH"};
    return Rewrite<char>(
        commandLine, graphics, gameSettings, fullscreenWidth,
        fullscreenHeight, rewriteGraphics, rewriteLanguages,
        "-FullScreenMode=", "-Width=", "-Height=", "-Shadow=", "-MSAA=",
        "Disable", "-Language=", "-Area=", "-VoiceJPMode", languageCodes,
        areaCodes);
}

std::wstring RewriteLaunchOptions(
    std::wstring_view commandLine,
    const settings::GraphicsSettings& graphics,
    const settings::GameSettings& gameSettings,
    unsigned fullscreenWidth, unsigned fullscreenHeight,
    bool rewriteGraphics, bool rewriteLanguages) {
    constexpr std::wstring_view languageCodes[]{
        L"US", L"JP", L"GR", L"FR", L"IT", L"SP", L"KR", L"CH"};
    constexpr std::wstring_view areaCodes[]{
        L"US", L"JP", L"INTER", L"INTER", L"INTER", L"INTER",
        L"CH", L"CH"};
    return Rewrite<wchar_t>(
        commandLine, graphics, gameSettings, fullscreenWidth,
        fullscreenHeight, rewriteGraphics, rewriteLanguages,
        L"-FullScreenMode=", L"-Width=", L"-Height=", L"-Shadow=", L"-MSAA=",
        L"Disable", L"-Language=", L"-Area=", L"-VoiceJPMode",
        languageCodes, areaCodes);
}

std::string RewriteGraphicsOptions(std::string_view commandLine,
                                   const settings::GraphicsSettings& graphics,
                                   unsigned fullscreenWidth,
                                   unsigned fullscreenHeight) {
    return RewriteLaunchOptions(
        commandLine, graphics, {}, fullscreenWidth, fullscreenHeight,
        true, false);
}

std::wstring RewriteGraphicsOptions(std::wstring_view commandLine,
                                    const settings::GraphicsSettings& graphics,
                                    unsigned fullscreenWidth,
                                    unsigned fullscreenHeight) {
    return RewriteLaunchOptions(
        commandLine, graphics, {}, fullscreenWidth, fullscreenHeight,
        true, false);
}

} // namespace novafix::launcher

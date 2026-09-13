#include "game/titles/lightning_returns/ui/text_editor/message_window_adapter.h"

#include "diagnostics/log.h"
#include "game/titles/lightning_returns/ui/prompt_mode_manager.h"
#include "game/titles/lightning_returns/ui/text_editor/game_text_converter.h"
#include "patch/memory_access.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace novafix::game::lr_text_editor::detail {
namespace {

constexpr std::array<unsigned char, 2> kConfirmGlyph{{0xF1u, 0x40u}};
constexpr std::array<unsigned char, 2> kCancelGlyph{{0xF1u, 0x41u}};
constexpr std::array<unsigned char, 2> kPauseGlyph{{0xF1u, 0x44u}};
constexpr std::size_t kMessageWindowLabelOffset = 204u;
constexpr std::size_t kMessageWindowTextOffset = 216u;
constexpr std::size_t kGuiProductRootOffset = 52u;
constexpr std::size_t kGuiComponentScaleXOffset = 120u;
constexpr std::size_t kGuiComponentScaleYOffset = 124u;
constexpr std::size_t kGuiComponentDirtyFlagsOffset = 164u;
constexpr std::size_t kTextLayoutBaseOffset = 0xC4u;
constexpr std::size_t kTextLayoutWrapWidthOffset = 214u;
constexpr std::size_t kTextLayoutLineCountOffset = 334u;
constexpr std::size_t kTextLayoutSourceOffset = 336u;
constexpr std::size_t kTextLayoutLinesOffset = 340u;
constexpr std::size_t kTextLayoutLineBytes = 16u;
constexpr std::size_t kTextLayoutLineLengthOffset = 4u;
constexpr std::size_t kTextLayoutLineWidthOffset = 10u;
constexpr std::uint16_t kMaximumNativeTextRows = 20u;
constexpr std::size_t kLongEditorSizingColumns = 30u;
constexpr unsigned kMaximumEmergencyBreaks = 8u;

struct OverlongLine {
    std::size_t sourceOffset{};
    std::uint16_t byteCount{};
    std::uint16_t width{};
};

enum class LayoutMeasure { Size, FirstLine, Overflow };

struct InputLayout {
    std::uint16_t rows{};
    std::uint16_t maximumWidth{};
    std::uint16_t firstLineWidth{};
    OverlongLine overlong;
};

struct LocalizedText {
    int language;
    const wchar_t* done;
    const wchar_t* cancel;
};

constexpr std::array<LocalizedText, 8> kLocalizedText{{
    {0, L"\x5B8C\x4E86", L"\x30AD\x30E3\x30F3\x30BB\x30EB"},
    {1, L"Done", L"Cancel"},
    {3, L"Fine", L"Annulla"},
    {4, L"Fertig", L"Abbrechen"},
    {5, L"Terminer", L"Annuler"},
    {6, L"Listo", L"Cancelar"},
    {8, L"\xC644\xB8CC", L"\xCDE8\xC18C"},
    {10, L"\x5B8C\x6210", L"\x53D6\x6D88"},
}};

const LocalizedText& CurrentLocalizedText(const NativeApi& api) {
    const int language = api.gameLanguage ? api.gameLanguage() : 1;
    for (const LocalizedText& candidate : kLocalizedText) {
        if (candidate.language == language) return candidate;
    }
    return kLocalizedText[1];
}

std::wstring SizingInputText(std::size_t maximumCharacters) {
    std::wstring sizing;
    sizing.reserve(maximumCharacters + 2u);
    for (std::size_t index = 0u; index < maximumCharacters; ++index) {
        if (index && maximumCharacters > kLongEditorSizingColumns &&
            index % kLongEditorSizingColumns == 0u) {
            sizing += L'\n';
        }
        sizing += L'W';
    }
    return sizing;
}

bool AppendNativeText(
    const NativeApi& api, std::vector<char>& destination,
    std::wstring_view source) {
    std::vector<char> converted;
    if (!BuildNativeText(api, source, converted) || converted.empty()) {
        return false;
    }
    destination.insert(destination.end(), converted.begin(),
                       converted.end() - 1u);
    return true;
}

void AppendControlGlyph(
    std::vector<char>& destination,
    const std::array<unsigned char, 2>& glyph) {
    destination.push_back(static_cast<char>(glyph[0]));
    destination.push_back(static_cast<char>(glyph[1]));
}

bool BuildEditorNativeText(
    const NativeApi& api, std::size_t currentCharacters,
    std::size_t maximumCharacters,
    const std::vector<char>& inputText, std::uint16_t paddingRows,
    std::vector<char>& text, std::size_t& footerOffset) {
    text.clear();
    footerOffset = 0u;
    if (inputText.empty()) return false;
    text.insert(text.end(), inputText.begin(), inputText.end() - 1u);
    text.insert(text.end(), paddingRows, '\n');
    text.push_back('\n');
    text.push_back('\n');
    footerOffset = text.size();

    const LocalizedText& localized = CurrentLocalizedText(api);
    const input::NativePromptMode promptMode = lr_prompts::CurrentMode();
    AppendControlGlyph(text, kConfirmGlyph);
    std::wstring done = L" ";
    done += localized.done;
    done += L"     ";
    if (!AppendNativeText(api, text, done)) return false;

    std::wstring cancel = L" ";
    AppendControlGlyph(
        text, promptMode == input::NativePromptMode::Controller
                  ? kCancelGlyph
                  : kPauseGlyph);
    cancel += localized.cancel;
    cancel += L"\n";
    cancel += std::to_wstring(currentCharacters);
    cancel += L" / ";
    cancel += std::to_wstring(maximumCharacters);
    if (!AppendNativeText(api, text, cancel)) return false;

    text.push_back('\0');
    return true;
}

bool ProductLabel(void* product, void*& label) {
    label = nullptr;
    if (!product || !patch::memory::IsReadable(
                        product, kMessageWindowTextOffset + sizeof(void*))) {
        return false;
    }
    std::memcpy(&label,
                static_cast<std::byte*>(product) +
                    kMessageWindowLabelOffset,
                sizeof(label));
    return label && patch::memory::IsReadable(
                        label, kTextLayoutBaseOffset +
                                   kTextLayoutLinesOffset +
                                   kMaximumNativeTextRows *
                                       kTextLayoutLineBytes);
}

bool AssignProductText(const NativeApi& api, void* product, void* label,
                       const std::vector<char>& text) {
    if (!api.gameStringAssign || !api.textLabelWrapper || text.empty()) {
        return false;
    }

    auto* const productBytes = static_cast<std::byte*>(product);
    api.gameStringAssign(productBytes + kMessageWindowTextOffset,
                         text.data());
    api.textLabelWrapper(label,
                         productBytes + kMessageWindowTextOffset);
    return true;
}

bool ReadInputLayout(void* label, std::size_t footerOffset,
                     LayoutMeasure measure, InputLayout& result,
                     std::uint16_t wrapWidth = 0u) {
    result = {};
    auto* const layout =
        static_cast<std::byte*>(label) + kTextLayoutBaseOffset;
    std::uint16_t lineCount{};
    const std::byte* source{};
    std::memcpy(&lineCount, layout + kTextLayoutLineCountOffset,
                sizeof(lineCount));
    std::memcpy(&source, layout + kTextLayoutSourceOffset,
                sizeof(source));
    if (!source || lineCount == 0u ||
        lineCount > kMaximumNativeTextRows ||
        footerOffset > 8192u) {
        return false;
    }

    const std::byte* const footer = source + footerOffset;
    for (std::uint16_t line = 0u; line < lineCount; ++line) {
        const std::byte* lineStart{};
        std::memcpy(&lineStart,
                    layout + kTextLayoutLinesOffset +
                        static_cast<std::size_t>(line) *
                            kTextLayoutLineBytes,
                    sizeof(lineStart));
        if (lineStart == footer) {
            // The editor deliberately leaves one empty native row between
            // the input and its footer controls
            if (line < 2u) return false;
            result.rows = static_cast<std::uint16_t>(line - 1u);
            if (measure == LayoutMeasure::Size) {
                // The wrap limit also governs Done/Cancel; include its localized width
                for (std::uint16_t layoutLine = 0u;
                     layoutLine <= line; ++layoutLine) {
                    std::uint16_t width{};
                    std::memcpy(
                        &width,
                        layout + kTextLayoutLinesOffset +
                            static_cast<std::size_t>(layoutLine) *
                                kTextLayoutLineBytes +
                            kTextLayoutLineWidthOffset,
                        sizeof(width));
                    if (width > result.maximumWidth) {
                        result.maximumWidth = width;
                    }
                }
                if (!result.maximumWidth) return false;
            }
            if (measure != LayoutMeasure::Size) {
                for (std::uint16_t inputLine = 0u;
                     inputLine < result.rows; ++inputLine) {
                    const std::byte* inputStart{};
                    std::uint16_t byteCount{};
                    std::uint16_t width{};
                    const std::byte* const descriptor =
                        layout + kTextLayoutLinesOffset +
                        static_cast<std::size_t>(inputLine) *
                            kTextLayoutLineBytes;
                    std::memcpy(&inputStart, descriptor,
                                sizeof(inputStart));
                    std::memcpy(
                        &byteCount,
                        descriptor + kTextLayoutLineLengthOffset,
                        sizeof(byteCount));
                    std::memcpy(
                        &width,
                        descriptor + kTextLayoutLineWidthOffset,
                        sizeof(width));
                    if (!inputStart || inputStart < source ||
                        inputStart > footer ||
                        static_cast<std::size_t>(footer - inputStart) <
                            byteCount) {
                        return false;
                    }
                    if (inputLine == 0u) {
                        result.firstLineWidth = width;
                    }
                    if (measure == LayoutMeasure::Overflow && wrapWidth &&
                        width > wrapWidth && !result.overlong.width) {
                        result.overlong.sourceOffset =
                            static_cast<std::size_t>(inputStart - source);
                        result.overlong.byteCount = byteCount;
                        result.overlong.width = width;
                    }
                }
            }
            return result.rows != 0u;
        }
    }
    return false;
}

bool SetNativeWrapWidth(void* label, std::uint16_t width) {
    if (!label || !width) return false;
    auto* const wrapWidth =
        static_cast<std::byte*>(label) + kTextLayoutBaseOffset +
        kTextLayoutWrapWidthOffset;
    if (!patch::memory::IsReadable(wrapWidth, sizeof(width))) {
        return false;
    }
    std::memcpy(wrapWidth, &width, sizeof(width));
    return true;
}

bool MeasureInputWidth(
    const NativeApi& api, void* product, void* label,
    std::size_t currentCharacters, std::size_t maximumCharacters,
    const std::vector<char>& inputText, std::size_t begin,
    std::size_t end, std::uint16_t wrapWidth,
    std::uint16_t& width) {
    width = 0u;
    if (begin >= end || end > inputText.size()) return false;

    std::vector<char> sample(
        inputText.begin() + static_cast<std::ptrdiff_t>(begin),
        inputText.begin() + static_cast<std::ptrdiff_t>(end));
    sample.push_back('\0');
    std::vector<char> editorText;
    std::size_t footerOffset{};
    if (!SetNativeWrapWidth(label, UINT16_MAX) ||
        !BuildEditorNativeText(
            api, currentCharacters, maximumCharacters, sample, 0u,
            editorText, footerOffset) ||
        !AssignProductText(api, product, label, editorText)) {
        SetNativeWrapWidth(label, wrapWidth);
        return false;
    }
    InputLayout measurement;
    const bool measured = ReadInputLayout(
        label, footerOffset, LayoutMeasure::FirstLine, measurement);
    width = measurement.firstLineWidth;
    return SetNativeWrapWidth(label, wrapWidth) && measured && width != 0u;
}

bool InsertEmergencyBreak(
    const NativeApi& api, const EditorState& state, void* label,
    std::size_t currentCharacters, std::vector<char>& inputText,
    const OverlongLine& line, std::size_t& cursorOffset) {
    const std::uint16_t wrapWidth = state.wrapWidth;
    if (!line.byteCount || line.width <= wrapWidth ||
        line.sourceOffset >= inputText.size()) {
        return false;
    }
    const std::size_t lineEnd = line.sourceOffset + line.byteCount;
    if (lineEnd >= inputText.size()) return false;

    std::vector<std::size_t> glyphs;
    for (std::size_t offset = line.sourceOffset; offset < lineEnd;) {
        const unsigned char first =
            static_cast<unsigned char>(inputText[offset]);
        if (first == '\n' || first == '\0') break;
        glyphs.push_back(offset);
        offset += (first & 0x80u) != 0u ? 2u : 1u;
        if (offset > lineEnd) return false;
    }
    if (glyphs.size() < 2u) return false;

    std::size_t first = 1u;
    std::size_t last = glyphs.size() - 1u;
    std::size_t splitGlyph = 1u;
    while (first <= last) {
        const std::size_t candidate = first + (last - first) / 2u;
        std::uint16_t width{};
        if (!MeasureInputWidth(
                api, state.product, label, currentCharacters,
                state.maximumCharacters, inputText, line.sourceOffset,
                glyphs[candidate], wrapWidth, width)) {
            return false;
        }
        if (width <= wrapWidth) {
            splitGlyph = candidate;
            first = candidate + 1u;
        } else {
            if (candidate == 1u) break;
            last = candidate - 1u;
        }
    }
    const std::size_t splitOffset = glyphs[splitGlyph];
    inputText.insert(
        inputText.begin() + static_cast<std::ptrdiff_t>(splitOffset), '\n');
    if (splitOffset <= cursorOffset) ++cursorOffset;
    return true;
}

bool InsertCursor(const NativeApi& api, const EditorState& state,
                  std::vector<char>& inputText,
                  std::size_t cursorOffset) {
    if (UnicodeCharacterCount(state.text) >= state.maximumCharacters) {
        return true;
    }
    std::vector<char> cursor;
    if (!BuildNativeText(
            api, state.cursorVisible ? L"_" : L" ", cursor) ||
        cursor.empty() || cursorOffset >= inputText.size()) {
        return false;
    }
    inputText.insert(
        inputText.begin() + static_cast<std::ptrdiff_t>(cursorOffset),
        cursor.begin(), cursor.end() - 1u);
    return true;
}

} // namespace

bool RefreshProduct(const NativeApi& api, const EditorState& state) {
    if (!state.product || !api.gameStringAssign ||
        !api.textLabelWrapper ||
        !state.reservedInputRows) {
        return false;
    }

    void* label{};
    if (!ProductLabel(state.product, label) ||
        !SetNativeWrapWidth(label, state.wrapWidth)) {
        return false;
    }

    std::vector<char> inputText;
    const std::size_t currentCharacters =
        UnicodeCharacterCount(state.text);
    if (!BuildNativeText(api, state.text, inputText)) {
        return false;
    }
    std::vector<char> cursorPrefix;
    if (!BuildNativeText(
            api, std::wstring_view(state.text).substr(0u, state.cursor),
            cursorPrefix) || cursorPrefix.empty()) {
        return false;
    }
    std::size_t cursorOffset = cursorPrefix.size() - 1u;
    std::vector<char> editorText;
    std::size_t footerOffset{};
    InputLayout layout;
    unsigned emergencyBreaks{};
    for (;;) {
        if (!BuildEditorNativeText(
                api, currentCharacters, state.maximumCharacters,
                inputText, 0u, editorText,
                footerOffset) ||
            !AssignProductText(api, state.product, label, editorText)) {
            return false;
        }

        if (!ReadInputLayout(
                label, footerOffset, LayoutMeasure::Overflow, layout,
                state.wrapWidth)) {
            return false;
        }
        if (!layout.overlong.width) break;
        if (emergencyBreaks++ >= kMaximumEmergencyBreaks ||
            !InsertEmergencyBreak(
                api, state, label, currentCharacters, inputText,
                layout.overlong, cursorOffset)) {
            return false;
        }
    }

    if (!InsertCursor(api, state, inputText, cursorOffset)) {
        return false;
    }
    const std::uint32_t visualWrapWidth =
        static_cast<std::uint32_t>(state.wrapWidth) +
        (currentCharacters < state.maximumCharacters
             ? state.cursorWidth
             : 0u);
    if (visualWrapWidth > UINT16_MAX ||
        !SetNativeWrapWidth(
            label, static_cast<std::uint16_t>(visualWrapWidth))) {
        return false;
    }

    const std::uint16_t paddingRows =
        layout.rows < state.reservedInputRows
            ? static_cast<std::uint16_t>(
                  state.reservedInputRows - layout.rows)
            : 0u;
    return BuildEditorNativeText(
               api, currentCharacters, state.maximumCharacters,
               inputText, paddingRows, editorText,
               footerOffset) &&
        AssignProductText(api, state.product, label, editorText);
}

bool SuppressProductVisuals(EditorState& state, bool suppress) {
    if (!state.product) return false;
    if (state.visualsSuppressed == suppress) return true;

    auto* const product = static_cast<std::byte*>(state.product);
    if (!patch::memory::IsReadable(
            product + kGuiProductRootOffset, sizeof(void*))) {
        return false;
    }
    void* root{};
    std::memcpy(&root, product + kGuiProductRootOffset, sizeof(root));
    if (!root || !patch::memory::IsReadable(
                     root, kGuiComponentDirtyFlagsOffset +
                               sizeof(std::uint32_t))) {
        return false;
    }

    auto* const component = static_cast<std::byte*>(root);
    if (suppress) {
        std::memcpy(&state.visibleScaleX,
                    component + kGuiComponentScaleXOffset,
                    sizeof(state.visibleScaleX));
        std::memcpy(&state.visibleScaleY,
                    component + kGuiComponentScaleYOffset,
                    sizeof(state.visibleScaleY));
        const float hiddenScale = 0.0f;
        std::memcpy(component + kGuiComponentScaleXOffset,
                    &hiddenScale, sizeof(hiddenScale));
        std::memcpy(component + kGuiComponentScaleYOffset,
                    &hiddenScale, sizeof(hiddenScale));
    } else {
        std::memcpy(component + kGuiComponentScaleXOffset,
                    &state.visibleScaleX, sizeof(state.visibleScaleX));
        std::memcpy(component + kGuiComponentScaleYOffset,
                    &state.visibleScaleY, sizeof(state.visibleScaleY));
    }
    auto* const dirtyFlags = reinterpret_cast<std::uint32_t*>(
        component + kGuiComponentDirtyFlagsOffset);
    *dirtyFlags |= 4u;
    state.visualsSuppressed = suppress;
    return true;
}

void CloseProduct(void* product) {
    if (!product || !patch::memory::IsReadable(
                        product, 81u * sizeof(void*))) {
        return;
    }
    void** const vtable = *static_cast<void***>(product);
    if (!vtable || !patch::memory::IsReadable(
                       vtable, 81u * sizeof(void*))) {
        return;
    }
    using CloseGuiProductFn = int (__thiscall*)(void*, int);
    const auto close = reinterpret_cast<CloseGuiProductFn>(vtable[80]);
    if (close) close(product, 0);
}

bool CreateProduct(const NativeApi& api, EditorState& state) {
    // Size SystemMessage once with fixed rows of wide glyphs; use the measured
    // width as the live text's native wrap limit
    const std::wstring sizingText =
        SizingInputText(state.maximumCharacters);
    std::vector<char> sizingInput;
    if (!BuildNativeText(api, sizingText, sizingInput)) {
        return false;
    }
    std::vector<char> native;
    std::size_t footerOffset{};
    if (!BuildEditorNativeText(
            api, state.maximumCharacters, state.maximumCharacters,
            sizingInput, 0u, native,
            footerOffset)) {
        return false;
    }

    state.product = api.createSystemMessage(native.data(), nullptr, 0, 0);
    if (!state.product || !patch::memory::IsReadable(
            state.product, 81u * sizeof(void*))) {
        CloseProduct(state.product);
        state.product = nullptr;
        return false;
    }

    auto* const product = static_cast<std::byte*>(state.product);
    void* label{};
    InputLayout sizingLayout;
    if (!ProductLabel(state.product, label) ||
        !ReadInputLayout(label, footerOffset, LayoutMeasure::Size, sizingLayout) ||
        !SetNativeWrapWidth(label, sizingLayout.maximumWidth)) {
        CloseProduct(state.product);
        state.product = nullptr;
        return false;
    }
    state.reservedInputRows = sizingLayout.rows;
    state.wrapWidth = sizingLayout.maximumWidth;
    std::vector<char> cursorInput;
    std::vector<char> cursorNative;
    std::size_t cursorFooterOffset{};
    InputLayout cursorLayout;
    if (!BuildNativeText(api, L"_", cursorInput) ||
        !BuildEditorNativeText(
            api, 0u, state.maximumCharacters, cursorInput, 0u,
            cursorNative, cursorFooterOffset) ||
        !AssignProductText(
            api, state.product, label, cursorNative) ||
        !ReadInputLayout(
            label, cursorFooterOffset, LayoutMeasure::FirstLine, cursorLayout) ||
        !cursorLayout.firstLineWidth) {
        CloseProduct(state.product);
        state.product = nullptr;
        return false;
    }
    state.cursorWidth = cursorLayout.firstLineWidth;
    LogDebug(
        "LR text editor native layout reserved %u input rows at %u pixels for %zu characters cursor=%u",
        static_cast<unsigned>(state.reservedInputRows),
        static_cast<unsigned>(state.wrapWidth),
        state.maximumCharacters,
        static_cast<unsigned>(state.cursorWidth));
    if (!RefreshProduct(api, state)) {
        CloseProduct(state.product);
        state.product = nullptr;
        return false;
    }

    auto* const flags = reinterpret_cast<std::uint32_t*>(product + 56u);
    *flags &= ~8u;
    api.setGuiLayer(state.product, 2);
    void** const vtable = *static_cast<void***>(state.product);
    if (!vtable || !patch::memory::IsReadable(
            vtable, 80u * sizeof(void*))) {
        CloseProduct(state.product);
        state.product = nullptr;
        return false;
    }

    using OpenGuiProductFn = void (__thiscall*)(void*, int, int);
    const auto open = reinterpret_cast<OpenGuiProductFn>(vtable[79]);
    if (!open) {
        CloseProduct(state.product);
        state.product = nullptr;
        return false;
    }

    *flags |= 0x80u;
    open(state.product, 0, 1);
    return true;
}

} // namespace novafix::game::lr_text_editor::detail

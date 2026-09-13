#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace novafix::game::steam::keyboard {

enum class Result {
    None,
    Submitted,
    Canceled,
    Failed,
};

bool Open(std::string_view initialText, std::size_t maximumCharacters);
bool IsOpen();
Result TakeResult(std::string& text);

} // namespace novafix::game::steam::keyboard

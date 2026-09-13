#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace novafix::game {
class ImageView;
}

namespace novafix::game::lr_message_window {

bool Bind(const ImageView& image);
bool Available();
int Language();

bool AppendText(std::vector<char>& destination, std::wstring_view source,
                std::size_t maximumBytes = 4096u);
bool ConvertText(std::wstring_view source, std::vector<char>& text);

void* Open(const char* message, const char* purpose,
           bool logFailure = true);
bool Close(void* product);

} // namespace novafix::game::lr_message_window

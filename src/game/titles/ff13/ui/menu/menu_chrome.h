#pragma once

namespace novafix::game {
class ImageView;
}

namespace novafix::game::ff13_ui::menu_chrome {

struct Context {
    void* header{};
    void* footer{};
    float offset{};
    void* headerBacking{};
    void* footerBacking{};
    float widthScale{1.0f};
    void* backdrop{};
    float heightScale{1.0f};
    void* footerTime{};
    void* footerGil{};
    float footerValueOffset{};
    void* footerGuide{};
    float footerGuideOffset{};
};

const Context& Current();
void Prepare(const ImageView& image);
bool InstallDetour(const ImageView& image);
void Revert();

} // namespace novafix::game::ff13_ui::menu_chrome

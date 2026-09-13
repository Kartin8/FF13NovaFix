#pragma once

namespace novafix::ui::widgets {

bool BeginSettingsTable(const char* identifier, float labelWidth = 205.0f);
void EndSettingsTable();
void SettingRow(const char* label, const char* help = nullptr);
void HelpMarker(const char* text);

} // namespace novafix::ui::widgets

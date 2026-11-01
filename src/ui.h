#pragma once

namespace ui {
void begin();
// Full-screen message used while booting (SD card, Wi-Fi).
void message(const char *line1, const char *line2 = "");
// Re-reads the browser listing, e.g. after files changed from the web page.
void refresh();
void loop();
}

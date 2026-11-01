#pragma once
#include <Arduino.h>

namespace web {
// Joins Wi-Fi (or starts the access point) and starts the web server on its own task.
void begin();
// Short network description for the status bar, e.g. "192.168.1.20".
String label();
}

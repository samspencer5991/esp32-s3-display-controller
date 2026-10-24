// Wi-Fi station + MQTT with Home Assistant discovery.
//
// Credentials live in NVS (namespace "net") and are set from the USB console
// (`wifi`, `mqtt`, `name`); Kconfig values are used until then.
#pragma once

#include <cstddef>

namespace app {
class App;
}

namespace net {

void start(app::App *app);

void set_wifi(const char *ssid, const char *password);
void set_mqtt(const char *uri, const char *username, const char *password);
void set_device_name(const char *name);
void forget();  // erase stored credentials (falls back to Kconfig defaults)

// Multi-line status for the console.
void describe(char *buf, size_t len);

}  // namespace net

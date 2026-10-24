#include "console.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "app/app.h"
#include "board/board.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"
#include "lvgl_port.h"
#include "net.h"
#include "platform.h"
#include "ui/ui.h"

namespace console {

namespace {

app::App *s_app = nullptr;

void write_base64(const uint8_t *data, size_t len)
{
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[121];
    size_t pos = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (i + 1 < len ? data[i + 1] << 8 : 0) |
                           (i + 2 < len ? data[i + 2] : 0);
        line[pos++] = kTable[(n >> 18) & 63];
        line[pos++] = kTable[(n >> 12) & 63];
        line[pos++] = i + 1 < len ? kTable[(n >> 6) & 63] : '=';
        line[pos++] = i + 2 < len ? kTable[n & 63] : '=';
        if (pos == 120) {
            line[pos] = '\0';
            printf("%s\n", line);
            pos = 0;
        }
    }
    if (pos > 0) {
        line[pos] = '\0';
        printf("%s\n", line);
    }
}

int cmd_shot(int, char **)
{
    const size_t len = lvgl_port::kWidth * lvgl_port::kHeight * 2;
    auto *buf = static_cast<uint8_t *>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        printf("ERR no memory\n");
        return 1;
    }
    lvgl_port::snapshot(buf);
    printf("\nSHOT-BEGIN %d %d RGB565LE %u\n", lvgl_port::kWidth, lvgl_port::kHeight, static_cast<unsigned>(len));
    write_base64(buf, len);
    printf("SHOT-END %08lx\n", static_cast<unsigned long>(esp_rom_crc32_le(0, buf, len)));
    fflush(stdout);
    heap_caps_free(buf);
    return 0;
}

int cmd_tap(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: tap <x> <y> [hold_ms]\n");
        return 1;
    }
    const int x = atoi(argv[1]), y = atoi(argv[2]);
    const int ms = argc > 3 ? atoi(argv[3]) : 80;
    lvgl_port::inject_touch(true, x, y);
    vTaskDelay(pdMS_TO_TICKS(ms));
    lvgl_port::inject_touch(false, x, y);
    printf("OK tap %d,%d %dms\n", x, y, ms);
    return 0;
}

int cmd_drag(int argc, char **argv)
{
    if (argc < 6) {
        printf("usage: drag <x1> <y1> <x2> <y2> <ms>\n");
        return 1;
    }
    const int x1 = atoi(argv[1]), y1 = atoi(argv[2]), x2 = atoi(argv[3]), y2 = atoi(argv[4]);
    const int ms = atoi(argv[5]);
    const int steps = ms / 20 > 1 ? ms / 20 : 1;
    for (int i = 0; i <= steps; i++) {
        lvgl_port::inject_touch(true, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    lvgl_port::inject_touch(false, x2, y2);
    printf("OK drag\n");
    return 0;
}

int cmd_stats(int, char **)
{
    const lvgl_port::Stats st = lvgl_port::stats(true);
    uint8_t addrs[16];
    const size_t n = board::i2c_scan_results(addrs, sizeof(addrs));
    printf("uptime_s=%lld\n", esp_timer_get_time() / 1000000);
    printf("heap_internal_free=%u psram_free=%u min_internal_free=%u\n",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    printf("max_handler_ms=%lu\n", static_cast<unsigned long>(st.max_handler_ms));
    printf("lvgl_frames=%lu rows=%lu wait_ms=%lu convert_ms=%lu transfer_ms=%lu since_last_frame_ms=%lu\n",
           static_cast<unsigned long>(st.frames), static_cast<unsigned long>(st.last_rows),
           static_cast<unsigned long>(st.last_wait_ms), static_cast<unsigned long>(st.last_convert_ms),
           static_cast<unsigned long>(st.last_transfer_ms), static_cast<unsigned long>(st.last_render_ms));
    const board::TouchChip chip = board::touch_chip();
    printf("touch_rejected=%lu\n", static_cast<unsigned long>(board::touch_rejected_count()));
    printf("touch=%s pmu=%d i2c=", chip == board::TouchChip::Axs15231b ? "AXS15231B"
                                    : chip == board::TouchChip::Cst3530 ? "CST3530"
                                                                        : "none",
           board::pmu_present());
    for (size_t i = 0; i < n; i++) {
        printf("%s0x%02X", i ? "," : "", addrs[i]);
    }
    printf("\n");
    return 0;
}

int cmd_touch(int argc, char **argv)
{
    // Streams raw touch samples for N seconds (default 10) - for calibration.
    const int seconds = argc > 1 ? atoi(argv[1]) : 10;
    const int64_t end = esp_timer_get_time() + seconds * 1000000LL;
    bool was = false;
    while (esp_timer_get_time() < end) {
        board::TouchPoint p = {};
        if (lvgl_port::lock(50)) {  // the LVGL task also reads the controller
            board::touch_read(p);
            lvgl_port::unlock();
        }
        if (p.pressed || was) {
            printf("touch %s x=%u y=%u\n", p.pressed ? "down" : "up", p.x, p.y);
        }
        was = p.pressed;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return 0;
}

int cmd_reg(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: reg <hex register>\n");
        return 1;
    }
    const uint8_t reg = static_cast<uint8_t>(strtol(argv[1], nullptr, 16));
    lvgl_port::lock();
    const uint8_t v = board::display_read_register(reg);
    lvgl_port::unlock();
    printf("reg 0x%02X = 0x%02X\n", reg, v);
    return 0;
}

int cmd_backlight(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: backlight <0-100>\n");
        return 1;
    }
    board::backlight_set(static_cast<uint8_t>(atoi(argv[1])));
    return 0;
}

int cmd_reboot(int, char **)
{
    printf("restarting...\n");
    fflush(stdout);
    platform::safe_restart(s_app);
    return 0;
}

// --- Application commands ------------------------------------------------------------------

const char *motion_name(app::Motion m) { return m == app::Motion::Up ? "up" : m == app::Motion::Down ? "down" : "idle"; }

int cmd_state(int, char **)
{
    app::State s;
    s_app->snapshot(s);
    const app::DeskState &d = s.desk;
    static const char *const kLinks[] = {"offline", "online", "demo"};
    static const char *const kBox[] = {"normal", "locked", "save_mode", "reset", "error"};
    printf("desk: link=%s height=%s%.1f motion=%s target=%d at_preset=%d box=%s display=\"%s\"\n",
           kLinks[static_cast<int>(d.link)], d.height_valid ? "" : "(invalid) ", static_cast<double>(d.height_cm),
           motion_name(d.motion), d.target_preset + 1, d.at_preset + 1, kBox[static_cast<int>(d.box)], d.display);
    printf("frames: rx=%lu tx=%lu errors=%lu last=%s\n", static_cast<unsigned long>(d.rx_frames),
           static_cast<unsigned long>(d.tx_frames), static_cast<unsigned long>(d.rx_errors), d.last_frame);
    const app::Settings &st = s.settings;
    printf("settings: theme=%d brightness=%d dim=%us sleep=%us rotate=%d units=%s demo=%d lock=%d reminder=%umin\n",
           st.theme, st.brightness, st.dim_after_s, st.sleep_after_s, st.rotate_180,
           st.units == app::Units::Metric ? "cm" : "in", st.demo_mode, st.child_lock, st.reminder_min);
    for (int i = 0; i < app::kPresetCount; i++) {
        printf("preset %d: name=\"%s\" icon=%d height=%.1f\n", i + 1, st.presets[i].name,
               static_cast<int>(st.presets[i].icon), static_cast<double>(st.presets[i].height_cm));
    }
    printf("lights: on=%d brightness=%d rgb=%02X%02X%02X effect=%d\n", st.lights.on, st.lights.brightness,
           st.lights.r, st.lights.g, st.lights.b, static_cast<int>(st.lights.effect));
    printf("reminder: due=%d standing=%d elapsed=%lus  uptime=%lus fw=%s\n", s.reminder.due, s.reminder.standing,
           static_cast<unsigned long>(s.reminder.elapsed_s), static_cast<unsigned long>(s.uptime_s),
           s.firmware_version);
    return 0;
}

int cmd_desk(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: desk up|down [ms] | stop | preset <1-4> | save <1-4> | boxlock | wake | keys <hex> [ms] | demo on|off\n");
        return 1;
    }
    const std::string_view sub = argv[1];
    if (sub == "up" || sub == "down") {
        const int ms = argc > 2 ? atoi(argv[2]) : 1000;
        const app::Motion dir = sub == "up" ? app::Motion::Up : app::Motion::Down;
        for (int t = 0; t < ms; t += 100) {
            if (!s_app->move(dir)) {
                printf("blocked (child lock)\n");
                return 1;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_app->move(app::Motion::Idle);
    } else if (sub == "stop") {
        s_app->stop();
    } else if ((sub == "preset" || sub == "save") && argc > 2) {
        const int i = atoi(argv[2]) - 1;
        const bool ok = sub == "preset" ? s_app->goto_preset(i) : s_app->save_preset(i);
        printf("%s\n", ok ? "OK" : "blocked/invalid");
    } else if (sub == "boxlock") {
        printf(s_app->toggle_box_lock() ? "holding M for 5.5 s\n" : "refused: stop the desk first\n");
    } else if (sub == "wake") {
        s_app->wake_desk();
    } else if (sub == "keys" && argc > 2) {
        const uint16_t keys = static_cast<uint16_t>(strtol(argv[2], nullptr, 16));
        const uint32_t ms = argc > 3 ? static_cast<uint32_t>(atoi(argv[3])) : 300;
        s_app->send_raw_keys(keys, ms);
        printf("sending keys 0x%04X for %lu ms\n", keys, static_cast<unsigned long>(ms));
    } else if (sub == "demo" && argc > 2) {
        const bool on = strcmp(argv[2], "on") == 0;
        s_app->modify_settings([on](app::Settings &st) { st.demo_mode = on; });
        if (s_app->settings().demo_mode != on) {
            printf("refused: stop the desk first\n");
            return 1;
        }
    } else {
        printf("unknown desk command\n");
        return 1;
    }
    return 0;
}

// Frames captured by the controller tap; drained by the console task.
struct TapRecord {
    uint8_t tx;
    uint8_t len;
    uint8_t data[desk::kMaxFrame];
    uint32_t ms;
};
// A message buffer keeps records whole: when it is full a record is dropped,
// never split (a split record would misalign every later read).
MessageBufferHandle_t s_tap_messages = nullptr;

void tap_cb(void *, bool tx, const uint8_t *data, size_t len)
{
    TapRecord r = {};
    r.tx = tx;
    r.len = static_cast<uint8_t>(len > sizeof(r.data) ? sizeof(r.data) : len);
    memcpy(r.data, data, r.len);
    r.ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    xMessageBufferSend(s_tap_messages, &r, sizeof(r), 0);
}

int cmd_sniff(int argc, char **argv)
{
    // sniff [seconds] [all]: prints frames; repeats of the same frame are
    // collapsed unless "all" is given.
    const int seconds = argc > 1 ? atoi(argv[1]) : 10;
    const bool all = argc > 2 && strcmp(argv[2], "all") == 0;
    if (s_tap_messages == nullptr) {
        s_tap_messages = xMessageBufferCreate((sizeof(TapRecord) + sizeof(size_t)) * 64);
    }
    xMessageBufferReset(s_tap_messages);
    s_app->set_frame_tap(tap_cb, nullptr);
    const int64_t end = esp_timer_get_time() + seconds * 1000000LL;
    TapRecord last[2] = {};
    uint32_t repeats[2] = {0, 0};
    while (esp_timer_get_time() < end) {
        TapRecord r;
        if (xMessageBufferReceive(s_tap_messages, &r, sizeof(r), pdMS_TO_TICKS(50)) != sizeof(r) || r.tx > 1 ||
            r.len > sizeof(r.data)) {
            continue;
        }
        if (!all && r.len == last[r.tx].len && memcmp(r.data, last[r.tx].data, r.len) == 0) {
            repeats[r.tx]++;
            continue;
        }
        if (repeats[r.tx] > 0) {
            printf("        (previous %s frame repeated %lu more times)\n", r.tx ? "TX" : "RX",
                   static_cast<unsigned long>(repeats[r.tx]));
            repeats[r.tx] = 0;
        }
        last[r.tx] = r;
        printf("%8lu %s", static_cast<unsigned long>(r.ms), r.tx ? "TX" : "RX");
        for (int i = 0; i < r.len; i++) {
            printf(" %02X", r.data[i]);
        }
        if (!r.tx && r.len >= 9 && r.data[2] == desk::kTypeDisplay) {
            printf("   display \"%s\"", desk::decode_display(r.data + 3).text);
        }
        printf("\n");
    }
    s_app->set_frame_tap(nullptr, nullptr);
    return 0;
}

int cmd_lock(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: lock on|off\n");
        return 1;
    }
    s_app->set_child_lock(strcmp(argv[1], "on") == 0);
    return 0;
}

int cmd_set(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: set theme|brightness|dim|sleep|rotate|units|reminder <value>\n");
        return 1;
    }
    const std::string key = argv[1];
    const std::string arg = argv[2];
    const int v = atoi(argv[2]);
    bool known = true;
    s_app->modify_settings([&](app::Settings &st) {
        if (key == "theme") st.theme = static_cast<uint8_t>(v);
        else if (key == "brightness") st.brightness = static_cast<uint8_t>(v);
        else if (key == "dim") st.dim_after_s = static_cast<uint16_t>(v);
        else if (key == "sleep") st.sleep_after_s = static_cast<uint16_t>(v);
        else if (key == "rotate") st.rotate_180 = v != 0;
        else if (key == "units") st.units = arg == "in" ? app::Units::Imperial : app::Units::Metric;
        else if (key == "reminder") st.reminder_min = static_cast<uint16_t>(v);
        else known = false;
    });
    if (!known) {
        printf("unknown setting\n");
        return 1;
    }
    return 0;
}

int cmd_preset(int argc, char **argv)
{
    if (argc < 4) {
        printf("usage: preset <1-4> name <text> | preset <1-4> icon <0=number 1=sit 2=stand 3=walk 4=focus>\n");
        return 1;
    }
    const int i = atoi(argv[1]) - 1;
    if (strcmp(argv[2], "name") == 0) {
        std::string name = argv[3];
        for (int a = 4; a < argc; a++) {
            name += " ";
            name += argv[a];
        }
        s_app->set_preset_meta(i, name.c_str(), app::PresetIcon::Count);
    } else {
        s_app->set_preset_meta(i, nullptr, static_cast<app::PresetIcon>(atoi(argv[3])));
    }
    return 0;
}

int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: wifi <ssid> [password]\n");
        return 1;
    }
    net::set_wifi(argv[1], argc > 2 ? argv[2] : "");
    printf("saved; connecting to %s\n", argv[1]);
    return 0;
}

int cmd_mqtt(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: mqtt <uri> [username] [password]   e.g. mqtt mqtt://192.168.1.10 mqttuser secret\n");
        return 1;
    }
    net::set_mqtt(argv[1], argc > 2 ? argv[2] : "", argc > 3 ? argv[3] : "");
    printf("saved\n");
    return 0;
}

int cmd_name(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: name <device name>\n");
        return 1;
    }
    std::string name = argv[1];
    for (int a = 2; a < argc; a++) {
        name += " ";
        name += argv[a];
    }
    net::set_device_name(name.c_str());
    return 0;
}

int cmd_net(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "forget") == 0) {
        net::forget();
        printf("stored network credentials erased\n");
        return 0;
    }
    char buf[512];
    net::describe(buf, sizeof(buf));
    printf("%s\n", buf);
    return 0;
}

int cmd_page(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: page home|settings|lights\n");
        return 1;
    }
    const std::string_view p = argv[1];
    lvgl_port::lock();
    ui::show(p == "settings" ? ui::Page::Settings : p == "lights" ? ui::Page::Lights : ui::Page::Home);
    lvgl_port::unlock();
    return 0;
}

void register_cmd(const char *name, const char *help, esp_console_cmd_func_t fn)
{
    esp_console_cmd_t cmd = {};
    cmd.command = name;
    cmd.help = help;
    cmd.func = fn;
    esp_console_cmd_register(&cmd);
}

}  // namespace

void start(app::App *app)
{
    s_app = app;

    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "desk>";
    repl_cfg.max_cmdline_length = 256;
    repl_cfg.task_stack_size = 6144;
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_t *repl = nullptr;
    if (esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl) != ESP_OK) {
        ESP_LOGE("console", "failed to start console");
        return;
    }
    esp_console_register_help_command();

    register_cmd("stats", "Memory, LVGL, touch and I2C status", cmd_stats);
    register_cmd("shot", "Dump the screen as base64 RGB565 (tools/devtool.py shot)", cmd_shot);
    register_cmd("tap", "tap <x> <y> [ms] - inject a touch (landscape coords)", cmd_tap);
    register_cmd("drag", "drag <x1> <y1> <x2> <y2> <ms> - inject a drag", cmd_drag);
    register_cmd("touch", "touch [seconds] - print raw touch samples", cmd_touch);
    register_cmd("reg", "reg <hex> - read a display controller register", cmd_reg);
    register_cmd("backlight", "backlight <0-100>", cmd_backlight);
    register_cmd("reboot", "Restart the controller (stops the desk first)", cmd_reboot);

    if (app != nullptr) {
        register_cmd("state", "Print desk, settings and network state", cmd_state);
        register_cmd("desk", "desk up|down [ms] | stop | preset N | save N | boxlock | wake | keys <hex> [ms] | demo on|off",
                     cmd_desk);
        register_cmd("sniff", "sniff [seconds] [all] - print desk protocol frames", cmd_sniff);
        register_cmd("lock", "lock on|off - UI child lock", cmd_lock);
        register_cmd("set", "set theme|brightness|dim|sleep|rotate|units|reminder <value>", cmd_set);
        register_cmd("preset", "preset <1-4> name <text> | icon <0-4>", cmd_preset);
        register_cmd("wifi", "wifi <ssid> [password] - save and connect", cmd_wifi);
        register_cmd("mqtt", "mqtt <uri> [user] [password] - Home Assistant broker", cmd_mqtt);
        register_cmd("name", "name <device name> - shown in Home Assistant", cmd_name);
        register_cmd("net", "net [forget] - network status / erase credentials", cmd_net);
        register_cmd("page", "page home|settings|lights", cmd_page);
    }

    esp_console_start_repl(repl);
}

}  // namespace console

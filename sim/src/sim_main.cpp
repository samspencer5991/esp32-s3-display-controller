// Desktop simulator for the desk controller UI.
//
// Runs the real UI, app core, desk controller and simulated control box on
// the host. Two modes:
//   * interactive: SDL window (mouse = finger), real time
//   * headless:    --script file drives touches on a virtual clock and saves
//                  PNG screenshots; `expect` lines make it a UI test
//
//   desk_sim [--script path] [--out dir] [--scale N] [--theme N] [--settings file]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <zlib.h>

#include "app/app.h"
#include "lvgl.h"
#include "theme.h"
#include "ui/ui.h"

#ifdef SIM_HAVE_SDL
#include <SDL.h>
#endif

namespace {

constexpr int kW = 640;
constexpr int kH = 180;

// --- Time ----------------------------------------------------------------------------

bool g_virtual_time = true;
uint32_t g_virtual_ms = 1000;

uint32_t now_ms()
{
#ifdef SIM_HAVE_SDL
    if (!g_virtual_time) {
        return SDL_GetTicks() + 1000;
    }
#endif
    return g_virtual_ms;
}

// --- Platform / HAL ---------------------------------------------------------------------

class SimPlatform : public app::Platform {
public:
    std::string path;
    uint32_t now_ms() override { return ::now_ms(); }
    bool load_settings(app::Settings &out) override
    {
        if (path.empty()) return false;
        std::ifstream f(path, std::ios::binary);
        uint16_t version = 0;
        if (!f.read(reinterpret_cast<char *>(&version), sizeof(version)) || version != app::kSettingsVersion) {
            return false;
        }
        return static_cast<bool>(f.read(reinterpret_cast<char *>(&out), sizeof(out)));
    }
    void save_settings(const app::Settings &s) override
    {
        if (path.empty()) return;
        std::ofstream f(path, std::ios::binary);
        const uint16_t version = app::kSettingsVersion;
        f.write(reinterpret_cast<const char *>(&version), sizeof(version));
        f.write(reinterpret_cast<const char *>(&s), sizeof(s));
    }
    const char *firmware_version() override { return "sim"; }
};

class SimHal : public ui::Hal {
public:
    uint8_t backlight = 100;
    bool rotated = false;
    void set_backlight(uint8_t percent) override { backlight = percent; }
    void set_rotation_180(bool r) override { rotated = r; }
    void restart() override { std::printf("[sim] restart requested\n"); }
    void describe_system(char *buf, size_t len) override
    {
        snprintf(buf, len, "Desktop simulator  -  LVGL %d.%d.%d  -  640x180 RGB565", LVGL_VERSION_MAJOR,
                 LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    }
};

// --- Display & input -------------------------------------------------------------------------

uint16_t g_fb[kW * kH];
bool g_fb_dirty = true;

struct Pointer {
    bool pressed = false;
    int x = 0, y = 0;
} g_pointer;

void flush_cb(lv_display_t *disp, const lv_area_t *, uint8_t *)
{
    g_fb_dirty = true;
    lv_display_flush_ready(disp);
}

void read_cb(lv_indev_t *, lv_indev_data_t *data)
{
    bool pressed = g_pointer.pressed;
    if (!ui::touch_gate(pressed)) {
        pressed = false;
    }
    data->point.x = g_pointer.x;
    data->point.y = g_pointer.y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void to_rgb888(std::vector<uint8_t> &rgb, uint8_t backlight = 100)
{
    rgb.resize(kW * kH * 3);
    for (int i = 0; i < kW * kH; i++) {
        const uint16_t v = g_fb[i];
        const int r = (((v >> 11) & 0x1F) * 527 + 23) >> 6;
        const int g = (((v >> 5) & 0x3F) * 259 + 33) >> 6;
        const int b = ((v & 0x1F) * 527 + 23) >> 6;
        rgb[i * 3] = static_cast<uint8_t>(r * backlight / 100);
        rgb[i * 3 + 1] = static_cast<uint8_t>(g * backlight / 100);
        rgb[i * 3 + 2] = static_cast<uint8_t>(b * backlight / 100);
    }
}

bool write_png(const std::string &path, const std::vector<uint8_t> &rgb, int w, int h)
{
    std::vector<uint8_t> raw;
    raw.reserve((w * 3 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + y * w * 3, rgb.begin() + (y + 1) * w * 3);
    }
    uLongf bound = compressBound(raw.size());
    std::vector<uint8_t> z(bound);
    if (compress2(z.data(), &bound, raw.data(), raw.size(), 9) != Z_OK) return false;
    z.resize(bound);

    std::ofstream f(path, std::ios::binary);
    auto be32 = [&](uint32_t v) {
        const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        f.write(reinterpret_cast<const char *>(b), 4);
    };
    auto chunk = [&](const char *tag, const std::vector<uint8_t> &data) {
        be32(static_cast<uint32_t>(data.size()));
        std::vector<uint8_t> crc_buf(tag, tag + 4);
        crc_buf.insert(crc_buf.end(), data.begin(), data.end());
        f.write(reinterpret_cast<const char *>(crc_buf.data()), crc_buf.size());
        be32(static_cast<uint32_t>(crc32(0, crc_buf.data(), crc_buf.size())));
    };
    f.write("\x89PNG\r\n\x1a\n", 8);
    std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                                 uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h),
                                 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return static_cast<bool>(f);
}

// --- Simulation loop -------------------------------------------------------------------------

app::App *g_app = nullptr;
SimHal g_hal;
uint32_t g_last_app_tick = 0;

// Advance the whole system by `ms` of virtual time.
void step(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 5) {
        g_virtual_ms += 5;
        if (g_virtual_ms - g_last_app_tick >= 10) {
            g_last_app_tick = g_virtual_ms;
            g_app->tick();
        }
        lv_timer_handler();
    }
}

// --- Script -------------------------------------------------------------------------------------

int g_failures = 0;
std::string g_out_dir = "sim-out";

bool check(bool ok, int line, const std::string &what)
{
    if (!ok) {
        g_failures++;
        std::printf("  FAIL line %d: %s\n", line, what.c_str());
    }
    return ok;
}

bool run_script(const std::string &path)
{
    std::ifstream in(path);
    if (!in) {
        std::printf("cannot open script %s\n", path.c_str());
        return false;
    }
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        lineno++;
        const auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        std::istringstream ss(line);
        std::string cmd;
        if (!(ss >> cmd)) continue;

        if (cmd == "wait") {
            uint32_t ms;
            ss >> ms;
            step(ms);
        } else if (cmd == "tap") {
            int x = 0, y = 0, ms = 80;
            ss >> x >> y;
            if (!(ss >> ms)) ms = 80;
            g_pointer = {true, x, y};
            step(ms);
            g_pointer.pressed = false;
            step(60);
        } else if (cmd == "press") {
            ss >> g_pointer.x >> g_pointer.y;
            g_pointer.pressed = true;
            step(20);
        } else if (cmd == "move") {
            ss >> g_pointer.x >> g_pointer.y;
            step(20);
        } else if (cmd == "release") {
            g_pointer.pressed = false;
            step(40);
        } else if (cmd == "drag") {
            int x1, y1, x2, y2, ms;
            ss >> x1 >> y1 >> x2 >> y2 >> ms;
            const int steps = std::max(1, ms / 20);
            for (int i = 0; i <= steps; i++) {
                g_pointer = {true, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps};
                step(20);
            }
            g_pointer.pressed = false;
            step(40);
        } else if (cmd == "shot") {
            std::string name;
            ss >> name;
            step(20);
            std::vector<uint8_t> rgb;
            to_rgb888(rgb);
            const std::string file = g_out_dir + "/" + name + ".png";
            write_png(file, rgb, kW, kH);
            std::printf("  shot %s\n", file.c_str());
        } else if (cmd == "theme") {
            int idx;
            ss >> idx;
            g_app->modify_settings([idx](app::Settings &s) { s.theme = static_cast<uint8_t>(idx); });
            step(100);
        } else if (cmd == "set") {
            std::string key;
            int value = 0;
            ss >> key >> value;
            bool known = true;
            g_app->modify_settings([&](app::Settings &s) {
                if (key == "units") s.units = static_cast<app::Units>(value);
                else if (key == "reminder") s.reminder_min = static_cast<uint16_t>(value);
                else if (key == "demo") s.demo_mode = value != 0;
                else if (key == "lock") s.child_lock = value != 0;
                else if (key == "dim") s.dim_after_s = static_cast<uint16_t>(value);
                else if (key == "sleep") s.sleep_after_s = static_cast<uint16_t>(value);
                else if (key == "brightness") s.brightness = static_cast<uint8_t>(value);
                else known = false;
            });
            check(known, lineno, "unknown setting " + key);
            step(60);
        } else if (cmd == "net") {
            int wifi, mqtt;
            ss >> wifi >> mqtt;
            app::NetState n{};
            n.wifi = static_cast<app::WifiStatus>(wifi);
            n.rssi = -58;
            snprintf(n.ssid, sizeof(n.ssid), "HomeNet");
            snprintf(n.ip, sizeof(n.ip), "192.168.1.42");
            n.mqtt_enabled = mqtt > 0;
            n.mqtt_connected = mqtt > 1;
            g_app->set_net_state(n);
            step(60);
        } else if (cmd == "desk_height") {
            float h;
            ss >> h;
            g_app->sim_box().set_height(h);
            step(300);
        } else if (cmd == "desk_text") {
            std::string text;
            uint32_t ms;
            ss >> text >> ms;
            g_app->sim_box().show_text(text.c_str(), ms);
            step(300);
        } else if (cmd == "unplug") {
            g_app->sim_box().set_unplugged(true);
        } else if (cmd == "plug") {
            g_app->sim_box().set_unplugged(false);
        } else if (cmd == "page") {
            std::string p;
            ss >> p;
            ui::show(p == "settings" ? ui::Page::Settings : p == "lights" ? ui::Page::Lights : ui::Page::Home);
            step(400);
        } else if (cmd == "expect") {
            std::string what;
            ss >> what;
            app::State s;
            g_app->snapshot(s);
            if (what == "height") {
                float v = 0, tol = 0.2f;
                ss >> v;
                if (!(ss >> tol)) tol = 0.2f;
                check(s.desk.height_valid && std::fabs(s.desk.height_cm - v) <= tol, lineno,
                      "height " + std::to_string(s.desk.height_cm) + " != " + std::to_string(v));
            } else if (what == "height_above" || what == "height_below") {
                float v;
                ss >> v;
                const bool ok = what == "height_above" ? s.desk.height_cm > v : s.desk.height_cm < v;
                check(ok, lineno, what + " " + std::to_string(v) + " (was " + std::to_string(s.desk.height_cm) + ")");
            } else if (what == "locked") {
                int v;
                ss >> v;
                check(s.settings.child_lock == (v != 0), lineno, "child_lock");
            } else if (what == "target") {
                int v;
                ss >> v;
                check(s.desk.target_preset == v, lineno, "target_preset " + std::to_string(s.desk.target_preset));
            } else if (what == "at_preset") {
                int v;
                ss >> v;
                check(s.desk.at_preset == v, lineno, "at_preset " + std::to_string(s.desk.at_preset));
            } else if (what == "moving") {
                int v;
                ss >> v;
                check((s.desk.motion != app::Motion::Idle) == (v != 0), lineno, "moving");
            } else if (what == "preset_height") {
                int i = 0;
                float v = 0, tol = 0.2f;
                ss >> i >> v;
                if (!(ss >> tol)) tol = 0.2f;
                check(std::fabs(s.settings.presets[i].height_cm - v) <= tol, lineno,
                      "preset " + std::to_string(i) + " height " + std::to_string(s.settings.presets[i].height_cm));
            } else if (what == "preset_at_height") {
                int i = 0;
                ss >> i;
                check(std::fabs(s.settings.presets[i].height_cm - s.desk.height_cm) <= 0.2f, lineno,
                      "preset " + std::to_string(i) + " at " + std::to_string(s.settings.presets[i].height_cm) +
                          ", desk at " + std::to_string(s.desk.height_cm));
            } else if (what == "reminder_due") {
                int v = 0;
                ss >> v;
                check(s.reminder.due == (v != 0), lineno, "reminder_due");
            } else if (what == "theme") {
                int v;
                ss >> v;
                check(s.settings.theme == v && ui::theme::current_index() == v, lineno, "theme");
            } else if (what == "backlight") {
                int v;
                ss >> v;
                check(g_hal.backlight == v, lineno, "backlight " + std::to_string(g_hal.backlight));
            } else if (what == "setting") {
                std::string key;
                int v;
                ss >> key >> v;
                int actual = -999;
                if (key == "units") actual = static_cast<int>(s.settings.units);
                if (key == "reminder") actual = s.settings.reminder_min;
                if (key == "demo") actual = s.settings.demo_mode;
                if (key == "dim") actual = s.settings.dim_after_s;
                if (key == "brightness") actual = s.settings.brightness;
                if (key == "lights_on") actual = s.settings.lights.on;
                if (key == "lights_effect") actual = static_cast<int>(s.settings.lights.effect);
                if (key == "rotate") actual = s.settings.rotate_180;
                if (key == "preset_icon0") actual = static_cast<int>(s.settings.presets[0].icon);
                check(actual == v, lineno, "setting " + key + " = " + std::to_string(actual));
            } else if (what == "link") {
                int v;
                ss >> v;
                check(static_cast<int>(s.desk.link) == v, lineno, "link " + std::to_string(int(s.desk.link)));
            } else if (what == "box") {
                int v;
                ss >> v;
                check(static_cast<int>(s.desk.box) == v, lineno, "box " + std::to_string(int(s.desk.box)));
            } else {
                check(false, lineno, "unknown expect " + what);
            }
        } else if (cmd == "log") {
            std::string rest;
            std::getline(ss, rest);
            std::printf("  --%s\n", rest.c_str());
        } else {
            check(false, lineno, "unknown command " + cmd);
        }
    }
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    std::string script, settings_path;
    int scale = 2;
    int theme = -1;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--script") script = next();
        else if (a == "--out") g_out_dir = next();
        else if (a == "--scale") scale = std::atoi(next().c_str());
        else if (a == "--theme") theme = std::atoi(next().c_str());
        else if (a == "--settings") settings_path = next();
        else {
            std::printf("usage: desk_sim [--script file] [--out dir] [--scale n] [--theme n] [--settings file]\n");
            return 2;
        }
    }

    const bool headless = !script.empty();
#ifndef SIM_HAVE_SDL
    if (!headless) {
        std::printf("built without SDL2: use --script\n");
        return 2;
    }
#endif
    g_virtual_time = headless;
    mkdir(g_out_dir.c_str(), 0755);

    lv_init();
    lv_tick_set_cb(now_ms);
    lv_display_t *disp = lv_display_create(kW, kH);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, g_fb, nullptr, sizeof(g_fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_cb);
    lv_indev_set_long_press_time(indev, 500);
    lv_timer_set_period(lv_indev_get_read_timer(indev), 20);

    SimPlatform platform;
    platform.path = settings_path;
    app::App app(platform, nullptr);
    g_app = &app;
    app.begin();
    if (theme >= 0) {
        app::Settings s = app.settings();
        s.theme = static_cast<uint8_t>(theme);
        app.update_settings(s);
    }
    ui::init(disp, app, g_hal);

    if (headless) {
        step(1500);  // let the simulated desk come online
        run_script(script);
        std::printf("%s: %s\n", script.c_str(), g_failures == 0 ? "PASS" : "FAIL");
        return g_failures == 0 ? 0 : 1;
    }

#ifdef SIM_HAVE_SDL
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *win = SDL_CreateWindow("Desk controller simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       kW * scale, kH * scale, SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, kW, kH);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, kW, kH);
    std::vector<uint8_t> rgb;
    bool running = true;
    uint32_t last_tick = now_ms();
    int shot_index = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                g_pointer.pressed = e.type == SDL_MOUSEBUTTONDOWN;
                g_pointer.x = e.button.x;
                g_pointer.y = e.button.y;
                break;
            case SDL_MOUSEMOTION:
                g_pointer.x = e.motion.x;
                g_pointer.y = e.motion.y;
                break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_s) {  // screenshot
                    to_rgb888(rgb);
                    const std::string file = g_out_dir + "/interactive-" + std::to_string(shot_index++) + ".png";
                    write_png(file, rgb, kW, kH);
                    std::printf("saved %s\n", file.c_str());
                } else if (e.key.keysym.sym == SDLK_t) {  // cycle theme
                    app::Settings s = app.settings();
                    s.theme = static_cast<uint8_t>((s.theme + 1) % ui::palette_count());
                    app.update_settings(s);
                } else if (e.key.keysym.sym == SDLK_ESCAPE) {
                    running = false;
                }
                break;
            }
        }
        const uint32_t t = now_ms();
        if (t - last_tick >= 10) {
            last_tick = t;
            app.tick();
        }
        lv_timer_handler();
        if (g_fb_dirty) {
            g_fb_dirty = false;
            to_rgb888(rgb, std::max<uint8_t>(g_hal.backlight, 8));
            SDL_UpdateTexture(tex, nullptr, rgb.data(), kW * 3);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, nullptr, nullptr);
            SDL_RenderPresent(ren);
        }
        SDL_Delay(5);
    }
    SDL_Quit();
#endif
    return 0;
}

#include "leds.h"

#include <algorithm>
#include <cmath>

#include "app/app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_LED_STRIP_ENABLED
#include "led_strip.h"
#endif

namespace leds {

#if CONFIG_LED_STRIP_ENABLED

namespace {

const char *TAG = "leds";
constexpr int kFrameMs = 20;           // 50 fps
constexpr float kFadeSpeed = 0.08f;    // per frame towards target brightness

struct Rgb {
    float r, g, b;
};

Rgb hsv(float h, float s, float v)
{
    h = std::fmod(h, 1.0f) * 6.0f;
    const int i = static_cast<int>(h);
    const float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i % 6) {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
    }
}

uint8_t gamma8(float linear)
{
    // Perceptual correction; LEDs look far too bright at low values otherwise.
    const float c = std::clamp(linear, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(std::pow(c, 2.2f) * 255.0f));
}

void task(void *arg)
{
    auto *app = static_cast<app::App *>(arg);

    led_strip_config_t cfg = {};
    cfg.strip_gpio_num = CONFIG_LED_STRIP_GPIO;
    cfg.max_leds = CONFIG_LED_STRIP_COUNT;
#if CONFIG_LED_STRIP_MODEL_SK6812
    cfg.led_model = LED_MODEL_SK6812;
#else
    cfg.led_model = LED_MODEL_WS2812;
#endif
    cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rmt = {};
    rmt.resolution_hz = 10 * 1000 * 1000;
    rmt.flags.with_dma = false;
    led_strip_handle_t strip = nullptr;
    if (led_strip_new_rmt_device(&cfg, &rmt, &strip) != ESP_OK) {
        ESP_LOGE(TAG, "LED strip init failed");
        vTaskDelete(nullptr);
        return;
    }
    led_strip_clear(strip);
    ESP_LOGI(TAG, "%d LEDs on GPIO%d", CONFIG_LED_STRIP_COUNT, CONFIG_LED_STRIP_GPIO);

    const int n = CONFIG_LED_STRIP_COUNT;
    const float cap = CONFIG_LED_STRIP_MAX_BRIGHTNESS / 100.0f;
    app::State state;
    uint32_t version = 0;
    float level = 0;  // smoothed master brightness 0..1
    float phase = 0;
    bool cleared = true;

    while (true) {
        const uint32_t v = app->version();
        if (v != version) {
            version = v;
            app->snapshot(state);
        }
        const app::LightSettings &l = state.settings.lights;
        const float target = l.on ? (l.brightness / 100.0f) * cap : 0.0f;
        level += (target - level) * kFadeSpeed;
        if (std::fabs(target - level) < 0.002f) {
            level = target;
        }
        // Every effect period divides 100 s, so wrapping there is seamless and
        // keeps float precision (an ever-growing phase freezes after days).
        phase = std::fmod(phase + kFrameMs / 1000.0f, 100.0f);

        if (level <= 0.0f) {
            if (!cleared) {
                led_strip_clear(strip);
                cleared = true;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        cleared = false;

        const Rgb base = {l.r / 255.0f, l.g / 255.0f, l.b / 255.0f};
        for (int i = 0; i < n; i++) {
            Rgb c = base;
            float k = 1.0f;
            const float pos = static_cast<float>(i) / std::max(1, n - 1);
            switch (l.effect) {
            case app::LightEffect::Solid:
                break;
            case app::LightEffect::Breathe:
                k = 0.25f + 0.75f * (0.5f + 0.5f * std::sin(phase * 2.0f * M_PI / 4.0f));
                break;
            case app::LightEffect::Rainbow:
                c = hsv(pos + phase * 0.08f, 1.0f, 1.0f);
                break;
            case app::LightEffect::DeskStatus: {
                const app::DeskState &d = state.desk;
                if (d.link == app::DeskLink::Offline) {
                    c = {1.0f, 0.1f, 0.05f};
                    k = 0.3f;
                } else if (d.motion != app::Motion::Idle || d.target_preset >= 0) {
                    // A bright band travelling in the direction of motion.
                    const float dir = d.motion == app::Motion::Down ? -1.0f : 1.0f;
                    float wave = pos - dir * phase * 0.9f;
                    wave -= std::floor(wave);  // 0..1 for either direction
                    c = base;
                    k = 0.2f + 0.8f * std::pow(1.0f - std::fabs(wave - 0.5f) * 2.0f, 3.0f);
                } else if (state.reminder.due) {
                    c = {1.0f, 0.65f, 0.1f};
                    k = 0.3f + 0.7f * (0.5f + 0.5f * std::sin(phase * 2.0f * M_PI));
                } else {
                    c = state.reminder.standing ? Rgb{0.25f, 0.9f, 0.55f} : base;
                }
                break;
            }
            default:
                break;
            }
            const float s = level * k;
            led_strip_set_pixel(strip, i, gamma8(c.r * s), gamma8(c.g * s), gamma8(c.b * s));
        }
        led_strip_refresh(strip);
        vTaskDelay(pdMS_TO_TICKS(kFrameMs));
    }
}

}  // namespace

void start(app::App *app) { xTaskCreatePinnedToCore(task, "leds", 4096, app, 3, nullptr, 0); }

#else

void start(app::App *) {}

#endif

}  // namespace leds

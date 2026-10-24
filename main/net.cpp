#include "net.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <tuple>

#include "app/app.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "theme.h"

namespace net {

namespace {

const char *TAG = "net";

constexpr const char *kReminderOptions[] = {"Off", "30 min", "45 min", "60 min", "90 min"};
constexpr uint16_t kReminderMinutes[] = {0, 30, 45, 60, 90};
constexpr const char *kEffects[] = {"Solid", "Breathe", "Rainbow", "Desk status"};

struct Credentials {
    char ssid[33];
    char password[65];
    char mqtt_uri[128];
    char mqtt_user[65];
    char mqtt_password[65];
    char name[40];
};

app::App *s_app = nullptr;
Credentials s_creds{};
char s_id[16] = {};          // e.g. "4d1714"
char s_base[40] = {};        // "smartdesk/4d1714"
esp_netif_t *s_netif = nullptr;
esp_mqtt_client_handle_t s_mqtt = nullptr;
bool s_wifi_started = false;
bool s_have_ip = false;
bool s_mqtt_connected = false;
int s_retry = 0;
app::NetState s_net{};

// Guards the client pointer against mqtt_stop() (console task) racing
// publish() (state task / MQTT task). Never held across blocking calls.
std::mutex s_mqtt_mu;
std::atomic<bool> s_mqtt_start_requested{false};

// Set by the MQTT task when everything must be resent (connect, HA restart);
// the cache below is only ever touched by state_task.
std::atomic<bool> s_republish{false};
std::mutex s_net_mu;  // guards s_net (Wi-Fi events vs. state task)

// Last published payload per state topic, to publish only changes.
struct Published {
    std::string height, motion, link, attributes, child_lock, demo, light, theme, brightness, reminder, standing,
        rssi;
} s_published;

// --- Credentials ----------------------------------------------------------------------------

void load_str(nvs_handle_t h, const char *key, char *out, size_t len, const char *fallback)
{
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) {
        snprintf(out, len, "%s", fallback);
    }
}

void load_credentials()
{
    nvs_handle_t h;
    const bool open = nvs_open("net", NVS_READONLY, &h) == ESP_OK;
    auto get = [&](const char *key, char *out, size_t len, const char *fallback) {
        if (open) {
            load_str(h, key, out, len, fallback);
        } else {
            snprintf(out, len, "%s", fallback);
        }
    };
    get("ssid", s_creds.ssid, sizeof(s_creds.ssid), CONFIG_DESK_WIFI_SSID);
    get("pass", s_creds.password, sizeof(s_creds.password), CONFIG_DESK_WIFI_PASSWORD);
    get("mqtt_uri", s_creds.mqtt_uri, sizeof(s_creds.mqtt_uri), CONFIG_DESK_MQTT_URI);
    get("mqtt_user", s_creds.mqtt_user, sizeof(s_creds.mqtt_user), CONFIG_DESK_MQTT_USERNAME);
    get("mqtt_pass", s_creds.mqtt_password, sizeof(s_creds.mqtt_password), CONFIG_DESK_MQTT_PASSWORD);
    get("name", s_creds.name, sizeof(s_creds.name), CONFIG_DESK_DEVICE_NAME);
    if (open) {
        nvs_close(h);
    }
}

void save_str(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open("net", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, value != nullptr ? value : "");
        nvs_commit(h);
        nvs_close(h);
    }
}

void push_state()
{
    app::NetState copy;
    {
        std::lock_guard<std::mutex> lock(s_net_mu);
        s_net.mqtt_enabled = s_creds.mqtt_uri[0] != '\0';
        s_net.mqtt_connected = s_mqtt_connected;
        snprintf(s_net.ssid, sizeof(s_net.ssid), "%s", s_creds.ssid);
        copy = s_net;
    }
    if (s_app != nullptr) {
        s_app->set_net_state(copy);
    }
}

// Applies `fn` to the shared network state under its lock, then publishes it.
template <typename Fn>
void update_net(Fn fn)
{
    {
        std::lock_guard<std::mutex> lock(s_net_mu);
        fn(s_net);
    }
    push_state();
}

// --- MQTT / Home Assistant ----------------------------------------------------------------------

std::string topic(const char *suffix) { return std::string(s_base) + "/" + suffix; }

void publish(const std::string &t, const std::string &payload, bool retain = true)
{
    std::lock_guard<std::mutex> lock(s_mqtt_mu);
    if (s_mqtt != nullptr && s_mqtt_connected) {
        // Enqueue only (non-blocking); the MQTT task sends.
        esp_mqtt_client_enqueue(s_mqtt, t.c_str(), payload.c_str(), static_cast<int>(payload.size()), 1, retain, true);
    }
}

// cJSON print that never hands a NULL to std::string (out of memory).
std::string print_json(cJSON *item)
{
    char *json = cJSON_PrintUnformatted(item);
    if (json == nullptr) {
        return {};
    }
    std::string out(json);
    cJSON_free(json);
    return out;
}

cJSON *device_block()
{
    cJSON *dev = cJSON_CreateObject();
    cJSON *ids = cJSON_AddArrayToObject(dev, "ids");
    char id[32];
    snprintf(id, sizeof(id), "smartdesk_%s", s_id);
    cJSON_AddItemToArray(ids, cJSON_CreateString(id));
    cJSON_AddStringToObject(dev, "name", s_creds.name);
    cJSON_AddStringToObject(dev, "mf", "DIY");
    cJSON_AddStringToObject(dev, "mdl", "Flexispot desk controller (T-Display-S3-Long)");
    cJSON_AddStringToObject(dev, "sw", esp_app_get_description()->version);
    return dev;
}

// Publishes one discovery config; `fill` adds the entity specific fields.
template <typename Fill>
void discover(const char *component, const char *object_id, const char *name, Fill fill)
{
    cJSON *cfg = cJSON_CreateObject();
    char uid[48];
    snprintf(uid, sizeof(uid), "smartdesk_%s_%s", s_id, object_id);
    cJSON_AddStringToObject(cfg, "name", name);
    cJSON_AddStringToObject(cfg, "uniq_id", uid);
    cJSON_AddStringToObject(cfg, "obj_id", uid);
    cJSON_AddStringToObject(cfg, "avty_t", topic("availability").c_str());
    cJSON_AddItemToObject(cfg, "dev", device_block());
    fill(cfg);
    const std::string json = print_json(cfg);
    cJSON_Delete(cfg);
    if (json.empty()) {
        return;
    }
    char t[128];
    snprintf(t, sizeof(t), "homeassistant/%s/smartdesk_%s/%s/config", component, s_id, object_id);
    publish(t, json);
}

void add(cJSON *o, const char *k, const std::string &v) { cJSON_AddStringToObject(o, k, v.c_str()); }

void publish_discovery(const app::Settings &settings)
{
    discover("sensor", "height", "Height", [](cJSON *c) {
        add(c, "stat_t", topic("height"));
        add(c, "json_attr_t", topic("attributes"));
        cJSON_AddStringToObject(c, "unit_of_meas", "cm");
        cJSON_AddStringToObject(c, "dev_cla", "distance");
        cJSON_AddStringToObject(c, "stat_cla", "measurement");
        cJSON_AddNumberToObject(c, "sug_dsp_prc", 1);
        cJSON_AddStringToObject(c, "ic", "mdi:human-male-height-variant");
    });
    discover("sensor", "motion", "Motion", [](cJSON *c) {
        add(c, "stat_t", topic("motion"));
        cJSON_AddStringToObject(c, "dev_cla", "enum");
        cJSON *opts = cJSON_AddArrayToObject(c, "options");
        for (const char *o : {"idle", "up", "down"}) cJSON_AddItemToArray(opts, cJSON_CreateString(o));
        cJSON_AddStringToObject(c, "ic", "mdi:swap-vertical");
    });
    discover("binary_sensor", "standing", "Standing", [](cJSON *c) {
        add(c, "stat_t", topic("standing"));
        cJSON_AddStringToObject(c, "ic", "mdi:human-handsup");
    });
    discover("sensor", "link", "Desk link", [](cJSON *c) {
        add(c, "stat_t", topic("link"));
        cJSON_AddStringToObject(c, "dev_cla", "enum");
        cJSON *opts = cJSON_AddArrayToObject(c, "options");
        for (const char *o : {"online", "offline", "demo"}) cJSON_AddItemToArray(opts, cJSON_CreateString(o));
        cJSON_AddStringToObject(c, "ent_cat", "diagnostic");
    });
    discover("sensor", "rssi", "Wi-Fi signal", [](cJSON *c) {
        add(c, "stat_t", topic("rssi"));
        cJSON_AddStringToObject(c, "unit_of_meas", "dBm");
        cJSON_AddStringToObject(c, "dev_cla", "signal_strength");
        cJSON_AddStringToObject(c, "ent_cat", "diagnostic");
    });

    for (int i = 0; i < app::kPresetCount; i++) {
        char object_id[16], name[48], press[16];
        snprintf(object_id, sizeof(object_id), "preset_%d", i + 1);
        snprintf(name, sizeof(name), "Preset %d (%s)", i + 1, settings.presets[i].name);
        snprintf(press, sizeof(press), "preset%d", i + 1);
        std::string press_s = press;
        discover("button", object_id, name, [&](cJSON *c) {
            add(c, "cmd_t", topic("command"));
            add(c, "pl_prs", press_s);
            char icon[32];
            snprintf(icon, sizeof(icon), "mdi:numeric-%d-box", i + 1);
            cJSON_AddStringToObject(c, "ic", icon);
        });
    }
    for (auto [id, name, payload, icon] :
         {std::tuple{"stop", "Stop", "stop", "mdi:stop"}, std::tuple{"nudge_up", "Nudge up", "nudge_up", "mdi:arrow-up"},
          std::tuple{"nudge_down", "Nudge down", "nudge_down", "mdi:arrow-down"}}) {
        discover("button", id, name, [&](cJSON *c) {
            add(c, "cmd_t", topic("command"));
            cJSON_AddStringToObject(c, "pl_prs", payload);
            cJSON_AddStringToObject(c, "ic", icon);
        });
    }

    discover("switch", "child_lock", "Child lock", [](cJSON *c) {
        add(c, "stat_t", topic("child_lock"));
        add(c, "cmd_t", topic("child_lock/set"));
        cJSON_AddStringToObject(c, "ic", "mdi:lock");
    });
    discover("light", "lights", "Lights", [](cJSON *c) {
        cJSON_AddStringToObject(c, "schema", "json");
        add(c, "stat_t", topic("light"));
        add(c, "cmd_t", topic("light/set"));
        cJSON_AddBoolToObject(c, "brightness", true);
        cJSON *modes = cJSON_AddArrayToObject(c, "supported_color_modes");
        cJSON_AddItemToArray(modes, cJSON_CreateString("rgb"));
        cJSON_AddBoolToObject(c, "effect", true);
        cJSON *effects = cJSON_AddArrayToObject(c, "effect_list");
        for (const char *e : kEffects) cJSON_AddItemToArray(effects, cJSON_CreateString(e));
    });
    discover("select", "theme", "Display theme", [](cJSON *c) {
        add(c, "stat_t", topic("theme"));
        add(c, "cmd_t", topic("theme/set"));
        cJSON *opts = cJSON_AddArrayToObject(c, "options");
        for (int i = 0; i < ui::palette_count(); i++) cJSON_AddItemToArray(opts, cJSON_CreateString(ui::palette(i).name));
        cJSON_AddStringToObject(c, "ent_cat", "config");
        cJSON_AddStringToObject(c, "ic", "mdi:palette");
    });
    discover("number", "display_brightness", "Display brightness", [](cJSON *c) {
        add(c, "stat_t", topic("brightness"));
        add(c, "cmd_t", topic("brightness/set"));
        cJSON_AddNumberToObject(c, "min", 5);
        cJSON_AddNumberToObject(c, "max", 100);
        cJSON_AddStringToObject(c, "unit_of_meas", "%");
        cJSON_AddStringToObject(c, "ent_cat", "config");
        cJSON_AddStringToObject(c, "ic", "mdi:brightness-6");
    });
    discover("select", "reminder", "Sit/stand reminder", [](cJSON *c) {
        add(c, "stat_t", topic("reminder"));
        add(c, "cmd_t", topic("reminder/set"));
        cJSON *opts = cJSON_AddArrayToObject(c, "options");
        for (const char *o : kReminderOptions) cJSON_AddItemToArray(opts, cJSON_CreateString(o));
        cJSON_AddStringToObject(c, "ent_cat", "config");
        cJSON_AddStringToObject(c, "ic", "mdi:timer-outline");
    });
    discover("switch", "demo_mode", "Demo mode", [](cJSON *c) {
        add(c, "stat_t", topic("demo_mode"));
        add(c, "cmd_t", topic("demo_mode/set"));
        cJSON_AddStringToObject(c, "ent_cat", "config");
        cJSON_AddStringToObject(c, "ic", "mdi:test-tube");
    });
}

void publish_if_changed(std::string &cache, const char *suffix, const std::string &payload)
{
    if (payload != cache) {
        cache = payload;
        publish(topic(suffix), payload);
    }
}

void publish_state(const app::State &s, bool moving_throttle_ok)
{
    char buf[64];
    const app::DeskState &d = s.desk;
    if (d.height_valid && (d.motion == app::Motion::Idle || moving_throttle_ok)) {
        snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(d.height_cm));
        publish_if_changed(s_published.height, "height", buf);
    }
    publish_if_changed(s_published.motion, "motion",
                       d.motion == app::Motion::Up ? "up" : d.motion == app::Motion::Down ? "down" : "idle");
    publish_if_changed(s_published.link, "link",
                       d.link == app::DeskLink::Online ? "online" : d.link == app::DeskLink::Demo ? "demo" : "offline");
    publish_if_changed(s_published.standing, "standing", s.reminder.standing ? "ON" : "OFF");
    publish_if_changed(s_published.child_lock, "child_lock", s.settings.child_lock ? "ON" : "OFF");
    publish_if_changed(s_published.demo, "demo_mode", s.settings.demo_mode ? "ON" : "OFF");
    publish_if_changed(s_published.theme, "theme", ui::palette(s.settings.theme).name);
    snprintf(buf, sizeof(buf), "%d", s.settings.brightness);
    publish_if_changed(s_published.brightness, "brightness", buf);
    const char *reminder = "Off";
    for (size_t i = 0; i < sizeof(kReminderMinutes) / sizeof(kReminderMinutes[0]); i++) {
        if (kReminderMinutes[i] == s.settings.reminder_min) reminder = kReminderOptions[i];
    }
    publish_if_changed(s_published.reminder, "reminder", reminder);
    snprintf(buf, sizeof(buf), "%d", s.net.rssi);
    publish_if_changed(s_published.rssi, "rssi", buf);

    const app::LightSettings &l = s.settings.lights;
    cJSON *light = cJSON_CreateObject();
    cJSON_AddStringToObject(light, "state", l.on ? "ON" : "OFF");
    cJSON_AddNumberToObject(light, "brightness", (l.brightness * 255 + 50) / 100);
    cJSON_AddStringToObject(light, "color_mode", "rgb");
    cJSON *color = cJSON_AddObjectToObject(light, "color");
    cJSON_AddNumberToObject(color, "r", l.r);
    cJSON_AddNumberToObject(color, "g", l.g);
    cJSON_AddNumberToObject(color, "b", l.b);
    cJSON_AddStringToObject(light, "effect", kEffects[static_cast<int>(l.effect) % 4]);
    std::string json = print_json(light);
    cJSON_Delete(light);
    if (!json.empty()) {
        publish_if_changed(s_published.light, "light", json);
    }

    cJSON *attrs = cJSON_CreateObject();
    cJSON *presets = cJSON_AddArrayToObject(attrs, "presets");
    for (const app::Preset &p : s.settings.presets) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", p.name);
        if (p.height_cm > 0) {
            cJSON_AddNumberToObject(item, "height_cm", std::round(p.height_cm * 10) / 10);
        } else {
            cJSON_AddNullToObject(item, "height_cm");
        }
        cJSON_AddItemToArray(presets, item);
    }
    cJSON_AddStringToObject(attrs, "control_box_display", d.display);
    cJSON_AddNumberToObject(attrs, "at_preset", d.at_preset >= 0 ? d.at_preset + 1 : 0);
    cJSON_AddNumberToObject(attrs, "minutes_in_posture", s.reminder.elapsed_s / 60);
    json = print_json(attrs);
    cJSON_Delete(attrs);
    if (!json.empty()) {
        publish_if_changed(s_published.attributes, "attributes", json);
    }
}

void handle_command(esp_mqtt_event_handle_t e)
{
    const std::string t(e->topic, e->topic_len);
    const std::string payload(e->data, e->data_len);
    ESP_LOGI(TAG, "cmd %s = %s", t.c_str(), payload.c_str());

    if (t == "homeassistant/status") {
        if (payload == "online") {
            publish_discovery(s_app->settings());
            s_republish = true;  // HA restarted: resend all states
        }
        return;
    }
    if (t == topic("command")) {
        for (int i = 0; i < app::kPresetCount; i++) {
            char p[16];
            snprintf(p, sizeof(p), "preset%d", i + 1);
            if (payload == p) {
                s_app->goto_preset(i);
            }
        }
        if (payload == "stop") {
            s_app->stop();
        } else if (payload == "nudge_up" || payload == "nudge_down") {
            s_app->nudge(payload == "nudge_up" ? app::Motion::Up : app::Motion::Down, 1000);
        }
        return;
    }
    if (t == topic("child_lock/set")) {
        s_app->set_child_lock(payload == "ON");
    } else if (t == topic("demo_mode/set")) {
        const bool on = payload == "ON";
        s_app->modify_settings([on](app::Settings &s) { s.demo_mode = on; });
    } else if (t == topic("theme/set")) {
        for (int i = 0; i < ui::palette_count(); i++) {
            if (payload == ui::palette(i).name) {
                s_app->modify_settings([i](app::Settings &s) { s.theme = static_cast<uint8_t>(i); });
            }
        }
    } else if (t == topic("brightness/set")) {
        const int v = atoi(payload.c_str());
        const auto level = static_cast<uint8_t>(v < 5 ? 5 : v > 100 ? 100 : v);
        s_app->modify_settings([level](app::Settings &s) { s.brightness = level; });
    } else if (t == topic("reminder/set")) {
        for (size_t i = 0; i < sizeof(kReminderOptions) / sizeof(kReminderOptions[0]); i++) {
            if (payload == kReminderOptions[i]) {
                const uint16_t minutes = kReminderMinutes[i];
                s_app->modify_settings([minutes](app::Settings &s) { s.reminder_min = minutes; });
            }
        }
    } else if (t == topic("light/set")) {
        cJSON *root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            return;
        }
        app::LightSettings l = s_app->settings().lights;
        if (cJSON *state = cJSON_GetObjectItem(root, "state"); cJSON_IsString(state)) {
            l.on = strcmp(state->valuestring, "ON") == 0;
        }
        if (cJSON *b = cJSON_GetObjectItem(root, "brightness"); cJSON_IsNumber(b)) {
            l.brightness = static_cast<uint8_t>((b->valueint * 100 + 127) / 255);
        }
        if (cJSON *c = cJSON_GetObjectItem(root, "color"); cJSON_IsObject(c)) {
            auto ch = [&](const char *k, uint8_t cur) {
                cJSON *v = cJSON_GetObjectItem(c, k);
                return cJSON_IsNumber(v) ? static_cast<uint8_t>(v->valueint) : cur;
            };
            l.r = ch("r", l.r);
            l.g = ch("g", l.g);
            l.b = ch("b", l.b);
        }
        if (cJSON *fx = cJSON_GetObjectItem(root, "effect"); cJSON_IsString(fx)) {
            for (int i = 0; i < 4; i++) {
                if (strcmp(fx->valuestring, kEffects[i]) == 0) {
                    l.effect = static_cast<app::LightEffect>(i);
                }
            }
        }
        cJSON_Delete(root);
        s_app->set_lights(l);
    }
}

void mqtt_event(void *, esp_event_base_t, int32_t id, void *data)
{
    auto *e = static_cast<esp_mqtt_event_handle_t>(data);
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_mqtt_connected = true;
        s_republish = true;
        publish(topic("availability"), "online");
        publish_discovery(s_app->settings());
        esp_mqtt_client_subscribe(e->client, topic("command").c_str(), 1);
        esp_mqtt_client_subscribe(e->client, topic("+/set").c_str(), 1);
        esp_mqtt_client_subscribe(e->client, "homeassistant/status", 1);
        push_state();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_mqtt_connected = false;
        push_state();
        break;
    case MQTT_EVENT_DATA:
        if (e->current_data_offset != 0 || e->data_len != e->total_data_len) {
            break;  // fragmented payloads are never valid commands
        }
        // A retained command would move the desk on every reconnect with
        // nobody there; only HA's birth message is legitimately retained.
        if (e->retain && strncmp(e->topic, "homeassistant/status", e->topic_len) != 0) {
            ESP_LOGW(TAG, "ignoring retained command on %.*s", e->topic_len, e->topic);
            break;
        }
        handle_command(e);
        break;
    default:
        break;
    }
}

void mqtt_start()
{
    if (s_creds.mqtt_uri[0] == '\0') {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_mqtt_mu);
        if (s_mqtt != nullptr) {
            return;
        }
    }
    static std::string lwt_topic;
    lwt_topic = topic("availability");
    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = s_creds.mqtt_uri;
    cfg.credentials.username = s_creds.mqtt_user[0] ? s_creds.mqtt_user : nullptr;
    cfg.credentials.authentication.password = s_creds.mqtt_password[0] ? s_creds.mqtt_password : nullptr;
    cfg.session.last_will.topic = lwt_topic.c_str();
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;
    cfg.buffer.size = 2048;
    cfg.buffer.out_size = 4096;
    cfg.outbox.limit = 64 * 1024;
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGE(TAG, "MQTT client init failed");
        return;
    }
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event, nullptr);
    {
        std::lock_guard<std::mutex> lock(s_mqtt_mu);
        s_mqtt = client;
    }
    esp_mqtt_client_start(client);
    ESP_LOGI(TAG, "MQTT connecting to %s", s_creds.mqtt_uri);
}

void mqtt_stop()
{
    esp_mqtt_client_handle_t client = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_mqtt_mu);
        client = s_mqtt;
        s_mqtt = nullptr;
        s_mqtt_connected = false;
    }
    if (client != nullptr) {
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
    }
}

// --- Wi-Fi -------------------------------------------------------------------------------------

esp_timer_handle_t s_retry_timer = nullptr;

void wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_have_ip = false;
        update_net([](app::NetState &n) {
            n.ip[0] = '\0';
            n.wifi = s_retry >= 5 ? app::WifiStatus::Failed : app::WifiStatus::Connecting;
        });
        // Back off up to 30 s between attempts; keep trying forever.
        const uint64_t delay_ms = std::min(30000, 1000 * (1 << std::min(s_retry, 5)));
        s_retry++;
        if (s_retry_timer == nullptr) {
            esp_timer_create_args_t args = {};
            args.callback = [](void *) {
                if (s_wifi_started) {
                    esp_wifi_connect();
                }
            };
            args.name = "wifi_retry";
            esp_timer_create(&args, &s_retry_timer);
        }
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, delay_ms * 1000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = static_cast<ip_event_got_ip_t *>(data);
        s_have_ip = true;
        s_retry = 0;
        update_net([ev](app::NetState &n) {
            snprintf(n.ip, sizeof(n.ip), IPSTR, IP2STR(&ev->ip_info.ip));
            n.wifi = app::WifiStatus::Connected;
        });
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR, IP2STR(&ev->ip_info.ip));
        s_mqtt_start_requested = true;  // started from state_task (small event task stack)
    }
}

void wifi_start()
{
    if (s_creds.ssid[0] == '\0') {
        update_net([](app::NetState &n) { n.wifi = app::WifiStatus::Disabled; });
        ESP_LOGI(TAG, "Wi-Fi not configured (console: wifi <ssid> <password>)");
        return;
    }
    wifi_config_t cfg = {};
    // SSID (32) and passphrase (64) fields need not be NUL terminated when full.
    memcpy(cfg.sta.ssid, s_creds.ssid, std::min(sizeof(cfg.sta.ssid), strlen(s_creds.ssid)));
    memcpy(cfg.sta.password, s_creds.password, std::min(sizeof(cfg.sta.password), strlen(s_creds.password)));
    cfg.sta.threshold.authmode = s_creds.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    update_net([](app::NetState &n) { n.wifi = app::WifiStatus::Connecting; });
    s_wifi_started = true;
    esp_wifi_start();
    ESP_LOGI(TAG, "Wi-Fi connecting to %s", s_creds.ssid);
}

void wifi_restart()
{
    mqtt_stop();
    if (s_wifi_started) {
        s_wifi_started = false;
        esp_wifi_stop();
    }
    s_retry = 0;
    wifi_start();
}

void state_task(void *)
{
    uint32_t version = 0;
    int64_t last_moving_publish = 0;
    int64_t last_rssi = 0;
    app::State state;
    while (true) {
        const int64_t now = esp_timer_get_time();

        if (s_mqtt_start_requested.exchange(false) && s_have_ip) {
            mqtt_start();
        }

        if (s_have_ip && now - last_rssi > 15 * 1000 * 1000) {
            last_rssi = now;
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                update_net([&ap](app::NetState &n) { n.rssi = ap.rssi; });
            }
        }

        if (s_mqtt_connected) {
            if (s_republish.exchange(false)) {
                s_published = Published();
                version = 0;
            }
            const uint32_t v = s_app->version();
            if (v != version) {
                version = v;
                s_app->snapshot(state);
                const bool throttle_ok = now - last_moving_publish > 1000 * 1000;
                if (throttle_ok) {
                    last_moving_publish = now;
                }
                publish_state(state, throttle_ok);
            }
        } else {
            version = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

}  // namespace

void start(app::App *app)
{
    s_app = app;
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_id, sizeof(s_id), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_base, sizeof(s_base), "smartdesk/%s", s_id);
    load_credentials();

    esp_netif_init();
    esp_event_loop_create_default();
    s_netif = esp_netif_create_default_wifi_sta();
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "smart-desk-%s", s_id);
    esp_netif_set_hostname(s_netif, hostname);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr);

    wifi_start();
    xTaskCreate(state_task, "net_state", 6144, nullptr, 3, nullptr);
}

void set_wifi(const char *ssid, const char *password)
{
    save_str("ssid", ssid);
    save_str("pass", password);
    load_credentials();
    wifi_restart();
}

void set_mqtt(const char *uri, const char *username, const char *password)
{
    save_str("mqtt_uri", uri);
    save_str("mqtt_user", username);
    save_str("mqtt_pass", password);
    load_credentials();
    mqtt_stop();
    push_state();
    s_mqtt_start_requested = s_have_ip;
}

void set_device_name(const char *name)
{
    save_str("name", name);
    load_credentials();
    if (s_mqtt_connected) {
        publish_discovery(s_app->settings());
    }
}

void forget()
{
    nvs_handle_t h;
    if (nvs_open("net", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    load_credentials();
    wifi_restart();
}

void describe(char *buf, size_t len)
{
    std::lock_guard<std::mutex> lock(s_net_mu);
    snprintf(buf, len,
             "device_id=%s name=\"%s\"\nwifi ssid=\"%s\" status=%d ip=%s rssi=%d\nmqtt uri=\"%s\" user=\"%s\" connected=%d\n"
             "topics: %s/...  discovery: homeassistant/<component>/smartdesk_%s/<entity>/config",
             s_id, s_creds.name, s_creds.ssid, static_cast<int>(s_net.wifi), s_net.ip[0] ? s_net.ip : "-", s_net.rssi,
             s_creds.mqtt_uri, s_creds.mqtt_user, s_mqtt_connected, s_base, s_id);
}

}  // namespace net

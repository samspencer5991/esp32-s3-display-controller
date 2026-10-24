// Host unit tests for the desk protocol, controller and simulated control box.
// Build & run: sim/run_tests.sh (target `desk_tests`).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/app.h"
#include "desk/controller.h"
#include "desk/protocol.h"
#include "desk/sim_control_box.h"

using namespace desk;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))

static std::vector<uint8_t> hex(const char *s)
{
    std::vector<uint8_t> out;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        unsigned v;
        std::sscanf(s, "%2x", &v);
        out.push_back(static_cast<uint8_t>(v));
        s += 2;
    }
    return out;
}

// --- Protocol ------------------------------------------------------------------

static void test_crc_and_encoding()
{
    std::printf("crc / key frame encoding\n");
    struct {
        uint16_t keys;
        const char *frame;
    } known[] = {
        {KeyNone, "9B 06 02 00 00 6C A1 9D"},    {KeyUp, "9B 06 02 01 00 FC A0 9D"},
        {KeyDown, "9B 06 02 02 00 0C A0 9D"},    {KeyPreset1, "9B 06 02 04 00 AC A3 9D"},
        {KeyPreset2, "9B 06 02 08 00 AC A6 9D"}, {KeyPreset3, "9B 06 02 10 00 AC AC 9D"},
        {KeyPreset4, "9B 06 02 00 01 AC 60 9D"}, {KeyMemory, "9B 06 02 20 00 AC B8 9D"},
        {KeyAlarm, "9B 06 02 40 00 AC 90 9D"},
    };
    for (auto &k : known) {
        uint8_t buf[kMaxFrame];
        size_t n = encode_keys(k.keys, buf);
        auto expected = hex(k.frame);
        CHECK(n == expected.size());
        CHECK(std::memcmp(buf, expected.data(), n) == 0);
    }
    // Box -> handset frames from captures.
    const uint8_t poll[] = {0x04, 0x11};
    CHECK(crc16_modbus(poll, 2) == 0x7CC3);
    const uint8_t keepalive[] = {0x04, 0x15};
    CHECK(crc16_modbus(keepalive, 2) == 0xBFC2);
    const uint8_t sleep[] = {0x04, 0x13};
    CHECK(crc16_modbus(sleep, 2) == 0xBD42);
}

static std::vector<Frame> parse_all(const std::vector<uint8_t> &bytes, int *errors = nullptr)
{
    FrameParser p;
    std::vector<Frame> frames;
    int errs = 0;
    for (uint8_t b : bytes) {
        auto r = p.feed(b);
        if (r == FrameParser::Result::Frame) frames.push_back(p.frame());
        if (r == FrameParser::Result::Error) errs++;
    }
    if (errors) *errors = errs;
    return frames;
}

static void test_parser()
{
    std::printf("frame parser\n");
    // A captured display frame: blank display.
    auto frames = parse_all(hex("9B 07 12 00 00 00 B8 94 9D"));
    CHECK(frames.size() == 1);
    CHECK(frames[0].type == kTypeDisplay && frames[0].len == 3);

    // Garbage, a poll, a corrupted frame, then a valid key frame.
    int errors = 0;
    frames = parse_all(hex("00 FE 9B 04 11 7C C3 9D 9B 06 02 01 00 FC A1 9D 9B 06 02 01 00 FC A0 9D"), &errors);
    CHECK(frames.size() == 2);
    CHECK(frames.size() == 2 && frames[0].type == kTypePoll && frames[1].type == kTypeKeys);
    CHECK(errors == 1);

    // Truncated frame followed immediately by a good one (resync on 9B).
    frames = parse_all(hex("9B 07 12 06 FD 9B 04 15 BF C2 9D"), &errors);
    CHECK(frames.size() == 1 && frames[0].type == kTypeKeepAlive);

    // Bad length byte.
    frames = parse_all(hex("9B 01 9B 04 13 BD 42 9D"), &errors);
    CHECK(frames.size() == 1 && frames[0].type == kTypeSleep);
}

static void test_display_decoding()
{
    std::printf("7-segment decoding\n");
    uint8_t seg[3];

    // 72.5 -> "7" "2." "5"
    seg[0] = 0x07; seg[1] = 0x5B | kSegDecimalPoint; seg[2] = 0x6D;
    auto r = decode_display(seg);
    CHECK(r.kind == DisplayKind::Height);
    CHECK_NEAR(r.value, 72.5f, 0.001f);
    CHECK(std::strcmp(r.text, "72.5") == 0);

    // 112 -> "1" "1" "2"
    seg[0] = 0x06; seg[1] = 0x06; seg[2] = 0x5B;
    r = decode_display(seg);
    CHECK(r.kind == DisplayKind::Height && r.value == 112.0f);

    seg[0] = 0; seg[1] = 0; seg[2] = 0;
    CHECK(decode_display(seg).kind == DisplayKind::Blank);

    seg[0] = 0x38; seg[1] = 0x5C; seg[2] = 0x39;
    CHECK(decode_display(seg).kind == DisplayKind::Locked);

    seg[0] = 0x6D; seg[1] = 0x40; seg[2] = 0x5B;  // S-2
    r = decode_display(seg);
    CHECK(r.kind == DisplayKind::SaveMode && r.arg == 2);

    seg[0] = 0x77; seg[1] = 0x6D; seg[2] = 0x31;  // ASr
    CHECK(decode_display(seg).kind == DisplayKind::Reset);

    seg[0] = 0x79; seg[1] = 0x3F; seg[2] = 0x06;  // E01
    r = decode_display(seg);
    CHECK(r.kind == DisplayKind::Error);
    CHECK(std::strcmp(r.text, "E01") == 0);

    // Round trip through the encoder used by the simulator.
    for (float h : {63.5f, 72.4f, 99.9f, 100.0f, 118.0f, 128.0f}) {
        encode_height_display(h, seg);
        r = decode_display(seg);
        CHECK(r.kind == DisplayKind::Height);
        CHECK_NEAR(r.value, h < 100 ? h : std::round(h), 0.051f);
    }
    encode_text_display("LoC", seg);
    CHECK(decode_display(seg).kind == DisplayKind::Locked);
    encode_text_display("S- ", seg);
    CHECK(decode_display(seg).kind == DisplayKind::SaveMode);
}

// --- Controller + simulated box ----------------------------------------------------

struct Rig {
    SimControlBox box;
    Controller ctl{nullptr};
    uint32_t now = 1000;

    Rig() { ctl.set_transport(&box, now); }

    void run(uint32_t ms, uint32_t step = 5)
    {
        for (uint32_t t = 0; t < ms; t += step) {
            now += step;
            box.tick(now);
            ctl.poll(now);
        }
    }
};

// Transport wrapper that records frames written by the controller.
struct Recorder : Transport {
    Transport *inner;
    std::vector<uint16_t> keys;
    FrameParser parser;
    explicit Recorder(Transport *t) : inner(t) {}
    size_t read(uint8_t *b, size_t m) override { return inner->read(b, m); }
    void write(const uint8_t *d, size_t n) override
    {
        for (size_t i = 0; i < n; i++) {
            if (parser.feed(d[i]) == FrameParser::Result::Frame) {
                keys.push_back(static_cast<uint16_t>(parser.frame().payload[0] | parser.frame().payload[1] << 8));
            }
        }
        inner->write(d, n);
    }
    void set_wake_pin(bool h) override { inner->set_wake_pin(h); }
};

static void test_link_and_height()
{
    std::printf("controller: link + height\n");
    Rig rig;
    rig.run(1000);
    CHECK(rig.ctl.status().online);
    CHECK(rig.ctl.status().polled);
    CHECK(rig.ctl.status().height_valid);
    CHECK_NEAR(rig.ctl.status().height, 74.0f, 0.05f);
}

static void test_hold_moves_and_fails_safe()
{
    std::printf("controller: hold up / fail-safe release\n");
    Rig rig;
    rig.run(500);
    const float start = rig.box.height();
    for (int i = 0; i < 20; i++) {  // UI refreshes the hold every 100 ms for 2 s
        rig.ctl.hold(KeyUp, rig.now);
        rig.run(100);
    }
    CHECK(rig.box.height() > start + 5.0f);
    CHECK(rig.ctl.status().moving == Direction::Up);

    // UI "hangs": no more refreshes. Desk must stop by itself.
    rig.run(2000);
    const float h = rig.box.height();
    rig.run(1000);
    CHECK_NEAR(rig.box.height(), h, 0.01f);
    CHECK(!rig.box.moving());
    CHECK(rig.ctl.status().moving == Direction::None);
}

static void test_preset_and_stop()
{
    std::printf("controller: preset travel, stop tap\n");
    Rig rig;
    rig.run(500);
    rig.ctl.press(KeyPreset2, 500, rig.now);  // 112.0 stored in the sim
    rig.run(20000);
    CHECK_NEAR(rig.box.height(), 112.0f, 0.01f);
    CHECK_NEAR(rig.ctl.status().height, 112.0f, 0.01f);

    rig.ctl.press(KeyPreset1, 500, rig.now);  // back down towards 72.5
    rig.run(3000);
    CHECK(rig.box.moving());
    rig.ctl.press(KeyUp, 80, rig.now);  // brake
    rig.run(3000);
    CHECK(!rig.box.moving());
    CHECK(rig.box.height() > 80.0f);
}

static void test_save_preset_sequence()
{
    std::printf("controller: M + preset stores height\n");
    Rig rig;
    rig.run(500);
    rig.box.set_height(95.0f);
    rig.ctl.press_sequence(KeyMemory, 300, KeyPreset3, 300, rig.now);
    rig.run(3000);
    CHECK_NEAR(rig.box.preset(2), 95.0f, 0.01f);
    CHECK(rig.ctl.status().display.kind == DisplayKind::SaveMode || rig.ctl.status().display.kind == DisplayKind::Height);
}

static void test_box_lock()
{
    std::printf("controller: control box lock (hold M)\n");
    Rig rig;
    rig.run(500);
    rig.ctl.press(KeyMemory, 5500, rig.now);
    rig.run(6000);
    CHECK(rig.box.locked());
    CHECK(rig.ctl.status().box_locked);

    const float h = rig.box.height();
    for (int i = 0; i < 10; i++) {
        rig.ctl.hold(KeyUp, rig.now);
        rig.run(100);
    }
    CHECK_NEAR(rig.box.height(), h, 0.01f);

    rig.ctl.press(KeyMemory, 5500, rig.now);
    rig.run(6000);
    CHECK(!rig.box.locked());
}

static void test_unsolicited_mode()
{
    std::printf("controller: box that does not poll\n");
    SimControlBox box;
    box.set_poll_enabled(false);
    Recorder rec(&box);
    Controller ctl(nullptr);
    uint32_t now = 1000;
    ctl.set_transport(&rec, now);
    auto run = [&](uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 5) {
            now += 5;
            box.tick(now);
            ctl.poll(now);
        }
    };
    run(500);
    CHECK(!ctl.status().polled);
    rec.keys.clear();
    for (int i = 0; i < 10; i++) {
        ctl.hold(KeyDown, now);
        run(100);
    }
    // ~1 s of holding at 108 ms -> ~9-10 frames, all "down".
    CHECK(rec.keys.size() >= 8 && rec.keys.size() <= 12);
    CHECK(box.height() < 74.0f);
    run(1000);
    CHECK(!rec.keys.empty() && rec.keys.back() == KeyNone);  // explicit release frame
}

static void test_offline_detection()
{
    std::printf("controller: offline detection\n");
    Rig rig;
    rig.run(500);
    CHECK(rig.ctl.status().online);
    rig.box.set_unplugged(true);
    rig.run(3000);
    CHECK(!rig.ctl.status().online);
    rig.box.set_unplugged(false);
    rig.run(500);
    CHECK(rig.ctl.status().online);
}

// --- App -----------------------------------------------------------------------------

struct FakePlatform : app::Platform {
    uint32_t t = 1000;
    int saves = 0;
    app::Settings saved{};
    uint32_t now_ms() override { return t; }
    bool load_settings(app::Settings &) override { return false; }
    void save_settings(const app::Settings &s) override
    {
        saved = s;
        saves++;
    }
};

static void test_app_flow()
{
    std::printf("app: presets, child lock, persistence\n");
    FakePlatform pf;
    app::App a(pf, nullptr);
    a.begin();
    auto run = [&](uint32_t ms) {
        for (uint32_t i = 0; i < ms; i += 10) {
            pf.t += 10;
            a.tick();
        }
    };
    run(1000);
    app::State s;
    a.snapshot(s);
    CHECK(s.desk.link == app::DeskLink::Demo);
    CHECK(s.desk.height_valid);

    CHECK(a.goto_preset(1));
    run(500);
    a.snapshot(s);
    CHECK(s.desk.target_preset == 1);
    run(20000);
    a.snapshot(s);
    CHECK(s.desk.target_preset == -1);
    CHECK_NEAR(s.desk.height_cm, 112.0f, 0.01f);
    CHECK(s.desk.at_preset == 1);
    CHECK_NEAR(s.settings.presets[1].height_cm, 112.0f, 0.01f);  // learned on arrival

    a.set_child_lock(true);
    CHECK(!a.move(app::Motion::Down));
    CHECK(!a.goto_preset(0));
    app::Event ev;
    bool saw_block = false;
    while (a.pop_event(ev)) saw_block |= ev.type == app::EventType::BlockedByLock;
    CHECK(saw_block);
    run(3000);
    CHECK(pf.saves > 0 && pf.saved.child_lock);

    a.set_child_lock(false);
    for (int i = 0; i < 15; i++) {
        CHECK(a.move(app::Motion::Down));
        run(100);
    }
    a.move(app::Motion::Idle);
    run(2000);
    a.snapshot(s);
    CHECK(s.desk.height_cm < 108.0f);
    CHECK(s.desk.motion == app::Motion::Idle);

    CHECK(a.save_preset(3));
    run(3000);
    a.snapshot(s);
    CHECK_NEAR(s.settings.presets[3].height_cm, s.desk.height_cm, 0.01f);
    CHECK_NEAR(a.sim_box().preset(3), a.sim_box().height(), 0.01f);  // stored in the box too
}

static void test_app_reminder()
{
    std::printf("app: sit/stand reminder\n");
    FakePlatform pf;
    app::App a(pf, nullptr);
    a.begin();
    app::Settings st = a.settings();
    st.reminder_min = 1;
    a.update_settings(st);
    bool due = false;
    app::Event ev;
    for (int i = 0; i < 6500 && !due; i++) {
        pf.t += 10;
        a.tick();
        while (a.pop_event(ev)) due |= ev.type == app::EventType::ReminderDue;
    }
    CHECK(due);
    a.acknowledge_reminder(true);
    app::State s;
    a.snapshot(s);
    CHECK(!s.reminder.due);
}


// --- Regression tests from code review ------------------------------------------------

struct KeyCounter {
    int up = 0, down = 0, memory = 0, preset = 0;
    static void tap(void *ctx, bool tx, const uint8_t *d, size_t n)
    {
        if (!tx || n < 8) return;
        auto *c = static_cast<KeyCounter *>(ctx);
        const uint16_t k = static_cast<uint16_t>(d[3] | (d[4] << 8));
        if (k & KeyUp) c->up++;
        if (k & KeyDown) c->down++;
        if (k & KeyMemory) c->memory++;
        if (k & (KeyPreset1 | KeyPreset2 | KeyPreset3 | KeyPreset4)) c->preset++;
    }
};

struct AppRig {
    FakePlatform pf;
    app::App app;
    explicit AppRig(desk::Transport *uart = nullptr, app::AppConfig cfg = app::AppConfig())
        : app(pf, uart, cfg)
    {
        app.begin();
    }
    void run(uint32_t ms)
    {
        for (uint32_t i = 0; i < ms; i += 5) {
            pf.t += 5;
            app.tick();
        }
    }
    bool saw_event(app::EventType type, int arg = -1)
    {
        app::Event ev;
        bool found = false;
        while (app.pop_event(ev)) found |= ev.type == type && (arg < 0 || ev.arg == arg);
        return found;
    }
};

static void test_stalls_do_not_skip_keys()
{
    std::printf("review: task stalls never skip timed keys\n");
    {
        Rig rig;
        rig.run(1000);
        const float h0 = rig.box.height();
        rig.ctl.press_sequence(KeyMemory, 300, KeyPreset2, 300, rig.now);
        rig.now += 350;  // desk task stalled (e.g. flash erase) right after queuing
        rig.box.tick(rig.now);
        rig.run(6000);
        CHECK_NEAR(rig.box.preset(1), h0, 0.05f);  // stored, not driven to
        CHECK_NEAR(rig.box.height(), h0, 0.05f);
    }
    {
        Rig rig;
        rig.run(1000);
        rig.ctl.press(KeyPreset2, 500, rig.now);
        rig.run(2500);
        rig.ctl.press(KeyDown, 80, rig.now);  // stop tap...
        rig.now += 100;                       // ...followed by a stall
        rig.box.tick(rig.now);
        rig.run(8000);
        CHECK(!rig.box.moving());
        CHECK(rig.box.height() < 105.0f);
    }
}

static void test_no_preset_overwrite_after_memory()
{
    std::printf("review: preset keys refused while M may be armed\n");
    {
        AppRig r;
        r.run(1000);
        const float h0 = r.app.sim_box().height();
        CHECK(r.app.save_preset(0));
        r.run(350);
        CHECK(!r.app.goto_preset(1));  // would store preset 2 instead of moving
        CHECK(r.saw_event(app::EventType::Busy, app::BusySavingPreset));
        r.run(8000);
        CHECK_NEAR(r.app.sim_box().preset(1), 112.0f, 0.01f);
        CHECK_NEAR(r.app.sim_box().preset(0), h0, 0.05f);
        CHECK_NEAR(r.app.sim_box().height(), h0, 0.05f);
        // Once the save has completed the preset works normally.
        CHECK(r.app.goto_preset(1));
        r.run(15000);
        CHECK_NEAR(r.app.sim_box().height(), 112.0f, 0.05f);
    }
    {
        AppRig r;
        r.run(1000);
        CHECK(r.app.toggle_box_lock());
        r.run(1500);
        CHECK(!r.app.goto_preset(1));
        r.run(8000);
        CHECK_NEAR(r.app.sim_box().preset(1), 112.0f, 0.01f);
        CHECK(r.app.sim_box().locked());
    }
}

static void test_preset_during_travel_stops()
{
    std::printf("review: preset request during travel stops, learns nothing\n");
    AppRig r;
    r.run(1000);
    CHECK(r.app.goto_preset(1));
    r.run(4000);
    CHECK(!r.app.goto_preset(0));  // HA / console: second preset while moving
    r.run(6000);
    app::State s;
    r.app.snapshot(s);
    CHECK(!r.app.sim_box().moving());
    CHECK(s.desk.target_preset == -1);
    CHECK(s.settings.presets[0].height_cm <= 0);  // not learned from the stop height
    CHECK(s.settings.presets[1].height_cm <= 0);
}

static void test_stop_semantics()
{
    std::printf("review: stop only taps when an automatic move is under way\n");
    {
        AppRig r;
        r.run(1000);
        for (int i = 0; i < 10; i++) {
            r.app.move(app::Motion::Up);
            r.run(100);
        }
        r.app.move(app::Motion::Idle);
        r.run(200);
        KeyCounter c;
        r.app.set_frame_tap(KeyCounter::tap, &c);
        r.app.stop();  // desk is only coasting after a manual move
        r.run(1000);
        CHECK(c.down == 0 && c.up == 0);
    }
    {
        AppRig r;
        r.run(14000);  // simulated box display has gone to sleep (12 s)
        KeyCounter c;
        r.app.set_frame_tap(KeyCounter::tap, &c);
        CHECK(r.app.goto_preset(1));
        r.run(200);  // still in the wake settle, preset key not sent yet
        r.app.stop();
        r.run(3000);
        CHECK(c.preset == 0);
        CHECK(c.down == 0 && c.up == 0);
        CHECK(!r.app.sim_box().moving());
    }
    {
        AppRig r;
        r.run(1000);
        const float h0 = r.app.sim_box().height();
        CHECK(r.app.nudge(app::Motion::Up, 1000));
        r.run(300);
        r.app.stop();
        r.run(2000);
        CHECK(!r.app.sim_box().moving());
        CHECK(r.app.sim_box().height() < h0 + 1.5f);
    }
}

static void test_demo_switch_refused_mid_move()
{
    std::printf("review: demo mode cannot be switched while the desk moves\n");
    SimControlBox real;
    struct Clocked : Transport {
        SimControlBox *box;
        size_t read(uint8_t *b, size_t m) override { return box->read(b, m); }
        void write(const uint8_t *d, size_t n) override { box->write(d, n); }
        void set_wake_pin(bool h) override { box->set_wake_pin(h); }
    } uart;
    uart.box = &real;
    app::AppConfig cfg;
    cfg.demo_mode_default = false;
    AppRig r(&uart, cfg);
    auto run = [&](uint32_t ms) {
        for (uint32_t i = 0; i < ms; i += 5) {
            r.pf.t += 5;
            real.tick(r.pf.t);
            r.app.tick();
        }
    };
    run(1000);
    CHECK(r.app.goto_preset(1));
    run(3000);
    r.app.modify_settings([](app::Settings &s) { s.demo_mode = true; });
    CHECK(!r.app.settings().demo_mode);
    CHECK(r.saw_event(app::EventType::Busy, app::BusyDeskMoving));
    r.app.stop();
    run(4000);
    CHECK(!real.moving());
    CHECK(real.height() < 110.0f);
}

static void test_child_lock_via_settings_stops()
{
    std::printf("review: child lock set through settings also stops the desk\n");
    AppRig r;
    r.run(1000);
    CHECK(r.app.goto_preset(1));
    r.run(2500);
    r.app.modify_settings([](app::Settings &s) { s.child_lock = true; });
    r.run(3000);
    CHECK(!r.app.sim_box().moving());
    CHECK(r.app.settings().child_lock);
}

struct PinRecorder : Transport {
    bool pin = true;
    size_t read(uint8_t *, size_t) override { return 0; }
    void write(const uint8_t *, size_t) override {}
    void set_wake_pin(bool high) override { pin = high; }
};

static void test_wake_pin_clock_wrap()
{
    std::printf("review: wake pulse survives the 32-bit millisecond wrap\n");
    PinRecorder pr;
    Controller ctl(nullptr);
    uint32_t now = 0xFFFFFF68u;
    ctl.set_transport(&pr, now);
    for (int i = 0; i < 400; i++) {
        now += 7;
        ctl.poll(now);
    }
    CHECK(pr.pin);
}

int main()
{
    test_crc_and_encoding();
    test_parser();
    test_display_decoding();
    test_link_and_height();
    test_hold_moves_and_fails_safe();
    test_preset_and_stop();
    test_save_preset_sequence();
    test_box_lock();
    test_unsolicited_mode();
    test_offline_detection();
    test_app_flow();
    test_app_reminder();
    test_stalls_do_not_skip_keys();
    test_no_preset_overwrite_after_memory();
    test_preset_during_travel_stops();
    test_stop_semantics();
    test_demo_switch_refused_mid_move();
    test_child_lock_via_settings_stops();
    test_wake_pin_clock_wrap();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

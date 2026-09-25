// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 lowgui contributors
// Clean-room reimplementation of OpenCV highgui (see README.md).
#include "test_precomp.hpp"
#include <opencv2/lowgui/window_manager.hpp>
#include "lowgui_input.hpp"

namespace opencv_test {
namespace {
using namespace cv::lowgui;
using namespace cv::lowgui::detail;

using K = cv::v4d::event::Keyboard::Key;

// ---------- keyToCode table (§4.1 / §7) ----------

TEST(KeyToCodeTest, ascii_letters_are_lowercase) {
    EXPECT_EQ(keyToCode(K::A), 'a');
    EXPECT_EQ(keyToCode(K::Z), 'z');
    EXPECT_EQ(keyToCode(K::M), 'm');
    EXPECT_EQ(keyToCode(K::L), 'l');
}

TEST(KeyToCodeTest, ascii_digits_match_symbols) {
    EXPECT_EQ(keyToCode(K::N0), '0');
    EXPECT_EQ(keyToCode(K::N9), '9');
    EXPECT_EQ(keyToCode(K::N7), '7');
}

TEST(KeyToCodeTest, ascii_punctuation_and_control) {
    EXPECT_EQ(keyToCode(K::SPACE), 32);
    EXPECT_EQ(keyToCode(K::ENTER), 13);
    EXPECT_EQ(keyToCode(K::BACKSPACE), 8);
    EXPECT_EQ(keyToCode(K::TAB), 9);
    EXPECT_EQ(keyToCode(K::ESCAPE), 27);
    EXPECT_EQ(keyToCode(K::APOSTROPHE), '\'');
    EXPECT_EQ(keyToCode(K::COMMA), ',');
    EXPECT_EQ(keyToCode(K::MINUS), '-');
    EXPECT_EQ(keyToCode(K::PERIOD), '.');
    EXPECT_EQ(keyToCode(K::SLASH), '/');
    EXPECT_EQ(keyToCode(K::SEMICOLON), ';');
    EXPECT_EQ(keyToCode(K::EQUAL), '=');
    EXPECT_EQ(keyToCode(K::LEFT_BRACKET), '[');
    EXPECT_EQ(keyToCode(K::BACKSLASH), '\\');
    EXPECT_EQ(keyToCode(K::RIGHT_BRACKET), ']');
    EXPECT_EQ(keyToCode(K::GRAVE_ACCENT), '`');
}

TEST(KeyToCodeTest, navigation_keys_map_to_x11_keysyms) {
    EXPECT_EQ(keyToCode(K::LEFT), 65361);
    EXPECT_EQ(keyToCode(K::RIGHT), 65363);
    EXPECT_EQ(keyToCode(K::UP), 65362);
    EXPECT_EQ(keyToCode(K::DOWN), 65364);
    EXPECT_EQ(keyToCode(K::HOME), 65360);
    EXPECT_EQ(keyToCode(K::END), 65367);
    EXPECT_EQ(keyToCode(K::PAGE_UP), 65365);
    EXPECT_EQ(keyToCode(K::PAGE_DOWN), 65366);
    EXPECT_EQ(keyToCode(K::INSERT), 65379);
    EXPECT_EQ(keyToCode(K::DELETE), 65535);
}

TEST(KeyToCodeTest, function_keys_map_to_x11_keysyms) {
    EXPECT_EQ(keyToCode(K::F1), 65470);
    EXPECT_EQ(keyToCode(K::F3), 65472);
    EXPECT_EQ(keyToCode(K::F12), 65481);
}

TEST(KeyToCodeTest, numpad_keys_map_to_x11_keysyms) {
    EXPECT_EQ(keyToCode(K::KP_0), 65456);
    EXPECT_EQ(keyToCode(K::KP_5), 65461);
    EXPECT_EQ(keyToCode(K::KP_9), 65465);
    EXPECT_EQ(keyToCode(K::KP_DIVIDE), 65455);
    EXPECT_EQ(keyToCode(K::KP_MULTIPLY), 65450);
    EXPECT_EQ(keyToCode(K::KP_ENTER), 65421);
    EXPECT_EQ(keyToCode(K::KP_DECIMAL), 65454);
    // §4.1 (L1) fix: KP_EQUAL is 65469, not 65461 (which collides with KP_5).
    EXPECT_EQ(keyToCode(K::KP_EQUAL), 65469);
    // Real X11 keysyms (GTK parity): XK_KP_Add = 0xffab = 65451,
    // XK_KP_Subtract = 0xffad = 65453.
    EXPECT_EQ(keyToCode(K::KP_ADD), 65451);
    EXPECT_EQ(keyToCode(K::KP_SUBTRACT), 65453);
}

TEST(KeyToCodeTest, unmapped_keys_return_minus_one) {
    EXPECT_EQ(keyToCode(K::F13), -1);
    EXPECT_EQ(keyToCode(K::CAPS_LOCK), -1);
    EXPECT_EQ(keyToCode(K::PAUSE), -1);
}

// ---------- KeyQueue (§7) ----------

TEST(KeyQueueTest, push_poll_is_fifo) {
    KeyQueue q;
    q.push(1);
    q.push(2);
    EXPECT_EQ(q.poll(), 1);
    EXPECT_EQ(q.poll(), 2);
    EXPECT_EQ(q.poll(), -1);
}

TEST(KeyQueueTest, wait_timeout_returns_minus_one) {
    KeyQueue q;
    EXPECT_EQ(q.wait(5), -1);
}

TEST(KeyQueueTest, wait_returns_pending_key_without_blocking) {
    KeyQueue q;
    q.push(65450);
    EXPECT_EQ(q.wait(0), 65450);
    EXPECT_EQ(q.wait(1), -1);
}

TEST(KeyQueueTest, wait_wakes_up_on_push) {
    KeyQueue q;
    std::thread pusher([&q]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        q.push(13);
    });
    auto start = std::chrono::steady_clock::now();
    int code = q.wait(0);
    auto elapsed = std::chrono::steady_clock::now() - start;
    pusher.join();
    EXPECT_EQ(code, 13);
    EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(KeyQueueTest, notify_wakes_blocked_waiter_and_returns_minus_one) {
    KeyQueue q;
    std::thread closer([&q]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        q.notify();
    });
    int code = q.wait(0);
    closer.join();
    EXPECT_EQ(code, -1);
}

TEST(KeyQueueTest, clear_drops_pending_keys) {
    KeyQueue q;
    q.push(1);
    q.push(2);
    q.clear();
    EXPECT_EQ(q.poll(), -1);
}

TEST(KeyQueueTest, interrupt_wakes_blocked_waiter_and_returns_minus_one) {
    KeyQueue q;
    std::thread interrupter([&q]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        q.interrupt();
    });
    int code = q.wait(0);
    interrupter.join();
    EXPECT_EQ(code, -1);
    // The interrupt is transient (unlike notify): the queue still delivers keys.
    q.push(42);
    EXPECT_EQ(q.poll(), 42);
}

TEST(KeyQueueTest, clearInterrupt_restores_blocking) {
    KeyQueue q;
    q.interrupt();
    q.clearInterrupt();
    // A cleared interrupt must not leave the queue permanently unblocked: a
    // bounded wait now actually waits for the timeout instead of returning
    // immediately.
    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(q.wait(50), -1);
    auto elapsed = std::chrono::steady_clock::now() - start;
    // wait_for returns at least 50ms; allow a generous clock/scheduling margin
    // (30ms) so loaded CI boxes never flake on a sub-millisecond deadline.
    EXPECT_GE(elapsed, std::chrono::milliseconds(30));
    // Pushed keys are still delivered after an interrupt + clear cycle.
    q.push(7);
    EXPECT_EQ(q.wait(0), 7);
}

// ---------- Trackbar registry (§7 / §3 M4) ----------

namespace {
void trackbarCb(int, void* ud) {
    if (ud) *static_cast<int*>(ud) += 1;
}
void buttonCb(int, void* ud) {
    if (ud) *static_cast<int*>(ud) += 1;
}
}

class TrackbarRegistryTest : public testing::Test {
protected:
    void SetUp() override {
        WindowManager::instance().destroyAllWindows();
        // (counters are local per-test via userdata)
    }
    void TearDown() override {
        WindowManager::instance().destroyAllWindows();
    }
};

TEST_F(TrackbarRegistryTest, createTrackbar_dedupes_by_name_and_window) {
    WindowManager::instance().createWindow("tb_win", 0);
    int v1 = 10;
    int v2 = 20;
    EXPECT_TRUE(WindowManager::instance().createTrackbar("gamma", "tb_win", &v1, 100, trackbarCb, nullptr));
    EXPECT_TRUE(WindowManager::instance().createTrackbar("gamma", "tb_win", &v2, 100, trackbarCb, nullptr));
    auto bars = WindowManager::instance().getWindowTrackbars("tb_win");
    EXPECT_EQ(bars.size(), 1u);
    // The second registration re-binds the external value pointer.
    EXPECT_EQ(v2, bars[0].pos);
}

TEST_F(TrackbarRegistryTest, createTrackbar_syncs_external_value_clamped) {
    WindowManager::instance().createWindow("tb_win", 0);
    int v = 150;  // above max: must clamp to count
    EXPECT_TRUE(WindowManager::instance().createTrackbar("gamma", "tb_win", &v, 100, trackbarCb, nullptr));
    EXPECT_EQ(v, 100);
    Trackbar t;
    ASSERT_TRUE(WindowManager::instance().getTrackbar("gamma", "tb_win", t));
    EXPECT_EQ(t.pos, 100);
    EXPECT_EQ(t.min, 0);
    EXPECT_EQ(t.max, 100);

    int low = -5;
    EXPECT_TRUE(WindowManager::instance().createTrackbar("gamma", "tb_win", &low, 100, trackbarCb, nullptr));
    // Qt semantics: re-creating an existing trackbar keeps the current slider
    // position and re-binds the external value pointer to it (does not clamp the
    // new pointer's stale value down to min).
    EXPECT_EQ(low, 100);
}

TEST_F(TrackbarRegistryTest, setTrackbarPos_fires_callback_only_on_change) {
    WindowManager::instance().createWindow("tb_win", 0);
    int cb = 0;
    int v = 0;
    WindowManager::instance().createTrackbar("gamma", "tb_win", &v, 100, trackbarCb, &cb);
    EXPECT_FALSE(WindowManager::instance().setTrackbarPos("gamma", "tb_win", 0));  // no change
    EXPECT_EQ(cb, 0);
    EXPECT_TRUE(WindowManager::instance().setTrackbarPos("gamma", "tb_win", 42));
    EXPECT_EQ(cb, 1);
    EXPECT_EQ(v, 42);
    EXPECT_EQ(WindowManager::instance().getTrackbarPos("gamma", "tb_win"), 42);
    // Out-of-range clamps and still counts as a change.
    EXPECT_TRUE(WindowManager::instance().setTrackbarPos("gamma", "tb_win", 500));
    EXPECT_EQ(cb, 2);
    EXPECT_EQ(WindowManager::instance().getTrackbarPos("gamma", "tb_win"), 100);
}

TEST_F(TrackbarRegistryTest, updateTrackbarPos_returns_updated_clamped_pos) {
    WindowManager::instance().createWindow("tb_win", 0);
    int cb = 0;
    int v = 0;
    WindowManager::instance().createTrackbar("gamma", "tb_win", &v, 100, trackbarCb, &cb);
    EXPECT_EQ(WindowManager::instance().updateTrackbarPos("gamma", "tb_win", 500), 100);
    EXPECT_EQ(cb, 1);
    EXPECT_EQ(v, 100);
    // Unchanged set returns the current position without dispatching.
    EXPECT_EQ(WindowManager::instance().updateTrackbarPos("gamma", "tb_win", 100), 100);
    EXPECT_EQ(cb, 1);
}

TEST_F(TrackbarRegistryTest, setTrackbarMinMax_clamp_and_fire) {
    WindowManager::instance().createWindow("tb_win", 0);
    int cb = 0;
    int v = 0;
    WindowManager::instance().createTrackbar("gamma", "tb_win", &v, 100, trackbarCb, &cb);
    WindowManager::instance().setTrackbarPos("gamma", "tb_win", 50);
    cb = 0;

    WindowManager::instance().setTrackbarMax("gamma", "tb_win", 30);
    EXPECT_EQ(WindowManager::instance().getTrackbarPos("gamma", "tb_win"), 30);
    EXPECT_EQ(v, 30);
    EXPECT_EQ(cb, 1);

    WindowManager::instance().setTrackbarMin("gamma", "tb_win", 40);
    EXPECT_EQ(WindowManager::instance().getTrackbarPos("gamma", "tb_win"), 40);
    EXPECT_EQ(v, 40);
    EXPECT_EQ(cb, 2);
}

TEST_F(TrackbarRegistryTest, unknown_trackbars_return_safe_defaults) {
    WindowManager::instance().createWindow("tb_win", 0);
    EXPECT_EQ(WindowManager::instance().getTrackbarPos("missing", "tb_win"), -1);
    EXPECT_EQ(WindowManager::instance().updateTrackbarPos("missing", "tb_win", 5), -1);
    EXPECT_FALSE(WindowManager::instance().setTrackbarPos("missing", "tb_win", 5));
}

TEST_F(TrackbarRegistryTest, control_panel_trackbar_uses_empty_winname) {
    int cb = 0;
    int v = 0;
    EXPECT_TRUE(WindowManager::instance().createTrackbar("global_gamma", "", &v, 100, trackbarCb, &cb));
    EXPECT_TRUE(WindowManager::instance().hasControlPanelContent());
    EXPECT_EQ(WindowManager::instance().getControlTrackbars().size(), 1u);

    EXPECT_TRUE(WindowManager::instance().setTrackbarPos("global_gamma", "", 7));
    EXPECT_EQ(v, 7);
    EXPECT_EQ(cb, 1);
    EXPECT_EQ(WindowManager::instance().getTrackbarPos("global_gamma", ""), 7);
}

// ---------- Control-panel / button registries (§7 / §3 M4 / L6) ----------

class ControlRegistryTest : public testing::Test {
protected:
    void SetUp() override {
        WindowManager::instance().destroyAllWindows();
        // (counters are local per-test via userdata)
    }
    void TearDown() override {
        WindowManager::instance().destroyAllWindows();
    }
};

TEST_F(ControlRegistryTest, createButton_new_buttonbar_starts_fresh_group) {
    int cb = 0;
    auto id1 = WindowManager::instance().createButton("one", buttonCb, &cb,
                                                      cv::lowgui::QT_RADIOBOX | cv::lowgui::QT_NEW_BUTTONBAR, true);
    auto id2 = WindowManager::instance().createButton("two", buttonCb, &cb,
                                                      cv::lowgui::QT_RADIOBOX | cv::lowgui::QT_NEW_BUTTONBAR, false);
    auto id3 = WindowManager::instance().createButton("three", buttonCb, &cb,
                                                      cv::lowgui::QT_PUSH_BUTTON, false);
    EXPECT_NE(id1, id2);      // each QT_NEW_BUTTONBAR opens a fresh group
    EXPECT_EQ(id2, id3);      // a plain button joins the current group
    EXPECT_NE(id1, 0);
    auto btns = WindowManager::instance().getControlButtons();
    ASSERT_EQ(btns.size(), 3u);
    EXPECT_EQ(btns[0].name, "one");
    EXPECT_EQ(btns[0].barId, id1);
    EXPECT_EQ(btns[1].barId, id2);
    EXPECT_EQ(btns[2].barId, id3);
    EXPECT_EQ(btns[0].state, 1);  // initial true
    EXPECT_EQ(btns[1].state, 0);  // initial false
    EXPECT_EQ(btns[2].state, -1); // push button resting state
    EXPECT_TRUE(WindowManager::instance().hasControlPanelContent());
}

TEST_F(ControlRegistryTest, setButtonState_returns_previous_state) {
    int cb = 0;
    WindowManager::instance().createButton("chk", buttonCb, &cb, cv::lowgui::QT_CHECKBOX, false);
    // Push-free path: default checkbox state is 0, so this is unambiguous.
    EXPECT_EQ(WindowManager::instance().setButtonState("chk", 1), 0);
    EXPECT_EQ(WindowManager::instance().setButtonState("chk", 0), 1);
}

TEST_F(ControlRegistryTest, setButtonState_sentinel_ambiguity_is_documented) {
    // L7: setButtonState returns -1 both for "button not found" and for the
    // previous state of a push button (whose resting state is -1). The return
    // value alone cannot distinguish the two; callers must check existence via
    // getControlButtons if it matters. Pin the behavior here so it does not
    // regress silently.
    int cb = 0;
    WindowManager::instance().createButton("push", buttonCb, &cb, cv::lowgui::QT_PUSH_BUTTON, false);
    // Existing push button: previous state is -1 (ambiguously "not found").
    EXPECT_EQ(WindowManager::instance().setButtonState("push", 0), -1);
    EXPECT_EQ(WindowManager::instance().setButtonState("push", 1), 0);
    // Truly missing button: also -1.
    EXPECT_EQ(WindowManager::instance().setButtonState("nope", 1), -1);
    // The stored state is visible through getControlButtons either way.
    auto btns = WindowManager::instance().getControlButtons();
    ASSERT_EQ(btns.size(), 1u);
    EXPECT_EQ(btns[0].name, "push");
    EXPECT_EQ(btns[0].state, 1);
}

TEST_F(ControlRegistryTest, destroyAllWindows_resets_control_registry) {
    // L6: destroyAllWindows clears control-panel trackbars/buttons and resets
    // barId so a recreated control panel starts from a clean registry.
    int cb = 0;
    int v = 0;
    WindowManager::instance().createTrackbar("global_gamma", "", &v, 100, trackbarCb, &cb);
    WindowManager::instance().createButton("push", buttonCb, &cb,
                                           cv::lowgui::QT_PUSH_BUTTON | cv::lowgui::QT_NEW_BUTTONBAR, false);
    EXPECT_TRUE(WindowManager::instance().hasControlPanelContent());

    WindowManager::instance().destroyAllWindows();

    EXPECT_FALSE(WindowManager::instance().hasControlPanelContent());
    EXPECT_TRUE(WindowManager::instance().getControlTrackbars().empty());
    EXPECT_TRUE(WindowManager::instance().getControlButtons().empty());
    EXPECT_EQ(WindowManager::instance().getTrackbarPos("global_gamma", ""), -1);

    // nextBarId_ was reset: a fresh buttonbar starts back at 1.
    auto id = WindowManager::instance().createButton("fresh", buttonCb, &cb,
                                                     cv::lowgui::QT_PUSH_BUTTON | cv::lowgui::QT_NEW_BUTTONBAR, false);
    EXPECT_EQ(id, 1);
}

} // namespace
} // namespace opencv_test
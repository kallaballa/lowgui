#include "test_precomp.hpp"
#include <opencv2/lowgui/lowgui.hpp>
#include <opencv2/lowgui/window_manager.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/v4d/v4d.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <thread>

namespace opencv_test {
namespace {

using namespace cv::lowgui;

static cv::UMat renderHeadless(const std::string& winname, const cv::Mat& img) {
    Lowgui::namedWindow(winname);
    Lowgui::imshow(winname, img);
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::waitKey(0);
    unsetenv("LOWGUI_HEADLESS_RENDER");
    return Lowgui::readFramebuffer();
}

class OffscreenRenderingTest : public testing::Test {
protected:
    void SetUp() override {
        Lowgui::destroyAllWindows();
        // The offscreen binary must never open a real window, even when a
        // display is present (e.g. under Xvfb): force the offscreen render
        // path so waitKey stays bounded in every test.
        setenv("LOWGUI_FORCE_OFFSCREEN", "1", 1);
        // The active-window pointer is process-global (WindowManager
        // singleton) and outlives destroyAllWindows historically: reset it so
        // each test starts from a clean slate instead of inheriting the
        // previous test's window. See the waitKey/readFramebuffer active-window
        // wiring in lowgui.cpp.
        cv::lowgui::detail::WindowManager::instance().setActiveWindow("");
    }

    void TearDown() override {
        Lowgui::destroyAllWindows();
        cv::lowgui::detail::WindowManager::instance().setActiveWindow("");
        unsetenv("LOWGUI_HEADLESS_RENDER");
        unsetenv("LOWGUI_FORCE_OFFSCREEN");
    }
};

TEST_F(OffscreenRenderingTest, single_window_solid_red_fills_framebuffer) {
    cv::Mat red = cv::Mat::zeros(960, 960, CV_8UC3);
    red.setTo(cv::Scalar(0, 0, 255));
    cv::UMat fb = renderHeadless("red_win", red);
    ASSERT_FALSE(fb.empty());
    EXPECT_EQ(fb.cols, 960);
    EXPECT_EQ(fb.rows, 960);

    cv::Mat expected(960, 960, CV_8UC4, cv::Scalar(255, 0, 0, 255));
    EXPECT_LT(cv::norm(fb, expected, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, single_window_solid_blue_fills_framebuffer) {
    cv::Mat blue = cv::Mat::zeros(960, 960, CV_8UC3);
    blue.setTo(cv::Scalar(255, 0, 0));
    cv::UMat fb = renderHeadless("blue_win", blue);
    ASSERT_FALSE(fb.empty());
    EXPECT_EQ(fb.cols, 960);
    EXPECT_EQ(fb.rows, 960);

    cv::Mat expected(960, 960, CV_8UC4, cv::Scalar(0, 0, 255, 255));
    EXPECT_LT(cv::norm(fb, expected, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, grayscale_image_renders_correctly) {
    cv::Mat gray = cv::Mat::zeros(960, 960, CV_8UC1);
    gray.setTo(128);
    cv::UMat fb = renderHeadless("gray_win", gray);
    ASSERT_FALSE(fb.empty());
    EXPECT_EQ(fb.cols, 960);
    EXPECT_EQ(fb.rows, 960);

    cv::Mat expected(960, 960, CV_8UC4, cv::Scalar(128, 128, 128, 255));
    EXPECT_LT(cv::norm(fb, expected, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, readFramebuffer_returns_empty_without_headless) {
    Lowgui::namedWindow("noheadless_win");
    Lowgui::imshow("noheadless_win", cv::Mat::zeros(10, 10, CV_8UC3));
    Lowgui::waitKey(0);

    cv::UMat fb = Lowgui::readFramebuffer();
    EXPECT_TRUE(fb.empty());
}

TEST_F(OffscreenRenderingTest, unsupported_channel_count_does_not_kill_engine) {
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::namedWindow("c2_win");
    cv::Mat two(32, 32, CV_8UC2);
    two.setTo(0);
    Lowgui::imshow("c2_win", two);

    cv::Mat red = cv::Mat::zeros(960, 960, CV_8UC3);
    red.setTo(cv::Scalar(0, 0, 255));
    Lowgui::imshow("c2_win", red);
    Lowgui::waitKey(0);

    cv::UMat fb = Lowgui::readFramebuffer();
    ASSERT_FALSE(fb.empty());
    EXPECT_EQ(fb.cols, 960);
    EXPECT_EQ(fb.rows, 960);

    cv::Mat expected(960, 960, CV_8UC4, cv::Scalar(255, 0, 0, 255));
    EXPECT_LT(cv::norm(fb, expected, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, unsupported_depth_does_not_kill_engine) {
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::namedWindow("s32_win");
    cv::Mat s32(32, 32, CV_32SC1);
    s32.setTo(1);
    Lowgui::imshow("s32_win", s32);

    cv::Mat blue = cv::Mat::zeros(960, 960, CV_8UC3);
    blue.setTo(cv::Scalar(255, 0, 0));
    Lowgui::imshow("s32_win", blue);
    Lowgui::waitKey(0);

    cv::UMat fb = Lowgui::readFramebuffer();
    ASSERT_FALSE(fb.empty());
    cv::Mat expected(960, 960, CV_8UC4, cv::Scalar(0, 0, 255, 255));
    EXPECT_LT(cv::norm(fb, expected, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, readFramebuffer_returns_newest_settled_frame_after_churn) {
    // L7 stress: a sustained imshow stream churns the generation counter so a
    // capture may be skipped or delayed mid-churn; the final waitKey(0) must
    // capture the last (settled) frame, not a stale churned one.
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::namedWindow("churn_win");
    cv::Mat tmp(32, 32, CV_8UC3);
    for (int i = 0; i < 200; ++i) {
        tmp.setTo(cv::Scalar(i & 0xff, 0, (255 - i) & 0xff));
        Lowgui::imshow("churn_win", tmp);
    }

    cv::Mat green = cv::Mat::zeros(960, 960, CV_8UC3);
    green.setTo(cv::Scalar(0, 255, 0));
    Lowgui::imshow("churn_win", green);
    Lowgui::waitKey(0);

    cv::UMat fb = Lowgui::readFramebuffer();
    ASSERT_FALSE(fb.empty());
    EXPECT_EQ(fb.cols, 960);
    EXPECT_EQ(fb.rows, 960);
cv::Mat expected(960, 960, CV_8UC4, cv::Scalar(0, 255, 0, 255));
    EXPECT_LT(cv::norm(fb, expected, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, two_windows_render_independently) {
    // Two logical windows in one process each get their own engine thread and
    // plan; each window must capture its own content (no cross-window leakage,
    // no crash). readFramebuffer() targets the active window, so switching the
    // active window re-routes the capture to the other plan.
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    cv::Mat red = cv::Mat::zeros(240, 240, CV_8UC3);
    red.setTo(cv::Scalar(0, 0, 255));
    cv::Mat blue = cv::Mat::zeros(240, 240, CV_8UC3);
    blue.setTo(cv::Scalar(255, 0, 0));

    Lowgui::namedWindow("win_a");
    Lowgui::imshow("win_a", red);
    Lowgui::namedWindow("win_b");
    Lowgui::imshow("win_b", blue);

    auto& wm = cv::lowgui::detail::WindowManager::instance();

    wm.setActiveWindow("win_a");
    Lowgui::waitKey(0);
    cv::UMat fbA = Lowgui::readFramebuffer();
    ASSERT_FALSE(fbA.empty());
    cv::Mat expectedA(240, 240, CV_8UC4, cv::Scalar(255, 0, 0, 255));
    EXPECT_LT(cv::norm(fbA, expectedA, cv::NORM_INF), 3);

    wm.setActiveWindow("win_b");
    Lowgui::waitKey(0);
    cv::UMat fbB = Lowgui::readFramebuffer();
    ASSERT_FALSE(fbB.empty());
    cv::Mat expectedB(240, 240, CV_8UC4, cv::Scalar(0, 0, 255, 255));
    EXPECT_LT(cv::norm(fbB, expectedB, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, destroy_then_recreate_window_captures_fresh_state) {
    // A recreated window must start from a clean plan/registry (no stale
    // framebuffer or capture state leaked from the destroyed instance).
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::namedWindow("cycle_win");
    cv::Mat red = cv::Mat::zeros(300, 300, CV_8UC3);
    red.setTo(cv::Scalar(0, 0, 255));
    Lowgui::imshow("cycle_win", red);
    Lowgui::waitKey(0);
    ASSERT_FALSE(Lowgui::readFramebuffer().empty());

    Lowgui::destroyWindow("cycle_win");
    EXPECT_FALSE(Lowgui::hasWindow("cycle_win"));

    Lowgui::namedWindow("cycle_win");
    cv::Mat green = cv::Mat::zeros(300, 300, CV_8UC3);
    green.setTo(cv::Scalar(0, 255, 0));
    Lowgui::imshow("cycle_win", green);
    Lowgui::waitKey(0);

    cv::UMat fb = Lowgui::readFramebuffer();
    ASSERT_FALSE(fb.empty());
    cv::Mat expected(300, 300, CV_8UC4, cv::Scalar(0, 255, 0, 255));
    EXPECT_LT(cv::norm(fb, expected, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, unscaled_depth_image_renders_not_skipped) {
    // CV_16U must render (scaled by 1/256) rather than being treated as
    // unsupported like CV_32S; the capture proves the conversion ran.
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::namedWindow("u16_win");
    cv::Mat u16(16, 16, CV_16UC1);
    u16.setTo(0xffff);  // 65535 * (1/256) -> 255 (clamped by convertTo)
    Lowgui::imshow("u16_win", u16);
    Lowgui::waitKey(0);

    cv::UMat fb = Lowgui::readFramebuffer();
    ASSERT_FALSE(fb.empty());
    cv::Mat expected(16, 16, CV_8UC4, cv::Scalar(255, 255, 255, 255));
    EXPECT_LT(cv::norm(fb, expected, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, transient_messages_do_not_corrupt_captured_pixels) {
    // displayOverlay/displayStatusBar store transient text; in headless mode
    // the status bar and overlay must not be drawn over the capture (the
    // framebuffer must stay uniform) while the message remains registered.
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::namedWindow("msg_win");
    cv::Mat red = cv::Mat::zeros(480, 480, CV_8UC3);
    red.setTo(cv::Scalar(0, 0, 255));
    Lowgui::imshow("msg_win", red);
    Lowgui::displayStatusBar("msg_win", "hello status", 0);
    Lowgui::displayOverlay("msg_win", "hello overlay", 0);
    Lowgui::waitKey(0);

    cv::UMat fb = Lowgui::readFramebuffer();
    ASSERT_FALSE(fb.empty());
    cv::Mat expected(480, 480, CV_8UC4, cv::Scalar(255, 0, 0, 255));
    EXPECT_LT(cv::norm(fb, expected, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, setMouseCallback_nonexistent_window_is_safe) {
    // Mirrors destroyWindow/resizeWindow: registering a callback on a window
    // that never existed must be a no-op, not an error.
    EXPECT_NO_THROW(Lowgui::setMouseCallback("ghost_win", nullptr, nullptr));
}

TEST_F(OffscreenRenderingTest, waitKey_delivers_key_injected_into_active_window_queue) {
    // The waitKey contract: keys land in the active window's queue and are
    // returned low-byte-masked (waitKey) or verbatim (waitKeyEx), then drained.
    Lowgui::namedWindow("key_win");
    Lowgui::imshow("key_win", cv::Mat::zeros(64, 64, CV_8UC3));
    Lowgui::waitKey(0);  // starts the offscreen engine; returns immediately

    cv::lowgui::detail::WindowManager::instance().getKeyQueue("key_win")->push('q');
    EXPECT_EQ(Lowgui::waitKey(1), 'q');

    cv::lowgui::detail::WindowManager::instance().getKeyQueue("key_win")->push(65363);
    EXPECT_EQ(Lowgui::waitKeyEx(1), 65363);

    EXPECT_EQ(Lowgui::waitKey(1), -1);  // drained
}

TEST_F(OffscreenRenderingTest, gwe_injected_keypress_is_delivered_to_waitKey) {
    // Full input pipeline: a synthetic gwe::Keyboard event (the same queue the
    // GLFW callbacks feed) must reach the running plan's key node, be mapped
    // through keyToCode, and surface through waitKey.
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::namedWindow("gwe_key_win");
    Lowgui::imshow("gwe_key_win", cv::Mat::zeros(64, 64, CV_8UC3));
    // The engine's event edges fetch from a per-thread gwe queue that is only
    // registered the first time the graph runs. Drive one capture frame first
    // so the queue exists before we broadcast an event into it.
    Lowgui::waitKey(0);

    gwe::detail::push(std::make_shared<cv::v4d::event::Keyboard>(
        cv::v4d::event::Keyboard::PRESS, cv::v4d::event::Keyboard::A));
    // The offscreen engine advances frames on demand (a new SinkSource frame or
    // a capture request), so push a fresh frame to make the graph run a pass
    // and consume the queued event.
    Lowgui::imshow("gwe_key_win", cv::Mat::zeros(64, 64, CV_8UC4));

    int code = -1;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        code = Lowgui::waitKey(1);
        if (code != -1) break;
    }
    EXPECT_EQ(code, 'a');
}

TEST_F(OffscreenRenderingTest, gwe_injected_mouse_event_fires_mouse_callback) {
    // Full input pipeline for the mouse: a synthetic PRESS on the LEFT button
    // must be dispatched to the window's MouseCallback with the Qt/OpenCV event
    // code, image-local coordinates, and held-button flags.
    struct MouseCtx {
        std::atomic<int> count{0};
        std::atomic<int> lastEvent{-1};
        std::atomic<int> lastX{-1};
        std::atomic<int> lastY{-1};
        std::atomic<int> lastFlags{-1};
    } ctx;

    Lowgui::namedWindow("gwe_mouse_win");
    Lowgui::setMouseCallback("gwe_mouse_win",
        [](int event, int x, int y, int flags, void* ud) {
            auto* c = static_cast<MouseCtx*>(ud);
            c->count.fetch_add(1, std::memory_order_relaxed);
            c->lastEvent.store(event, std::memory_order_relaxed);
            c->lastX.store(x, std::memory_order_relaxed);
            c->lastY.store(y, std::memory_order_relaxed);
            c->lastFlags.store(flags, std::memory_order_relaxed);
        }, &ctx);
    // A 960x960 image in the 960x960 viewport fits with zoom 1, pan (0,0), so
    // image-local coordinates equal the injected viewport position exactly.
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::imshow("gwe_mouse_win", cv::Mat::zeros(960, 960, CV_8UC3));
    // Drive one capture frame so the worker's per-thread gwe queue is registered
    // before we broadcast an event into it (see gwe_injected_keypress test).
    Lowgui::waitKey(0);

    gwe::detail::push(std::make_shared<cv::v4d::event::Mouse>(
        cv::v4d::event::Mouse::PRESS, cv::v4d::event::Mouse::LEFT,
        cv::Point(100, 50)));
    // Drive a frame (see gwe_injected_keypress test): without a new frame the
    // offscreen engine has nothing to advance and would never consume the event.
    Lowgui::imshow("gwe_mouse_win", cv::Mat::zeros(960, 960, CV_8UC4));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ctx.count.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ASSERT_GT(ctx.count.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(ctx.lastEvent.load(), cv::lowgui::EVENT_LBUTTONDOWN);
    EXPECT_EQ(ctx.lastX.load(), 100);
    EXPECT_EQ(ctx.lastY.load(), 50);
    EXPECT_EQ(ctx.lastFlags.load(), cv::lowgui::EVENT_FLAG_LBUTTON);
}

// ---------- waitKey/waitKeyEx/waitKey semantics with a live engine ----------

TEST_F(OffscreenRenderingTest, waitKey_lowbyte_masks_waitKeyEx_verbatim) {
    // waitKey must mask the queue's key codes to 8 bits (highgui parity) while
    // waitKeyEx returns them verbatim; both drain the same FIFO queue.
    Lowgui::namedWindow("mask_win");
    Lowgui::imshow("mask_win", cv::Mat::zeros(64, 64, CV_8UC3));
    Lowgui::waitKey(0);  // starts the offscreen engine; returns immediately

    const int full = 0x1FF;
    auto* q = cv::lowgui::detail::WindowManager::instance().getKeyQueue("mask_win");
    ASSERT_NE(q, nullptr);

    q->push(full);
    EXPECT_EQ(Lowgui::waitKey(1), full & 0xff);

    q->push(full);
    EXPECT_EQ(Lowgui::waitKeyEx(1), full);

    EXPECT_EQ(Lowgui::waitKey(1), -1);  // drained
}

TEST_F(OffscreenRenderingTest, pollKey_returns_and_drains_pending_keys) {
    Lowgui::namedWindow("poll_win");
    Lowgui::imshow("poll_win", cv::Mat::zeros(64, 64, CV_8UC3));
    Lowgui::waitKey(0);  // starts the offscreen engine

    auto* q = cv::lowgui::detail::WindowManager::instance().getKeyQueue("poll_win");
    ASSERT_NE(q, nullptr);
    q->push('x');
    q->push('y');
    EXPECT_EQ(Lowgui::pollKey(), 'x');
    EXPECT_EQ(Lowgui::pollKey(), 'y');
    EXPECT_EQ(Lowgui::pollKey(), -1);
}

TEST_F(OffscreenRenderingTest, positive_delay_waitKey_returns_promptly_while_engine_runs) {
    Lowgui::namedWindow("delay_win");
    Lowgui::imshow("delay_win", cv::Mat::zeros(64, 64, CV_8UC3));
    Lowgui::waitKey(0);  // starts the offscreen engine

    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(Lowgui::waitKey(25), -1);
    auto elapsed = std::chrono::steady_clock::now() - start;
    // In offscreen mode waitKey(delay>0) sleeps the requested delay, then polls.
    EXPECT_GE(elapsed, std::chrono::milliseconds(15));
}

TEST_F(OffscreenRenderingTest, keys_route_to_active_window_only) {
    // waitKey* polls the *active* window's queue; switching the active window
    // re-routes delivery so each logical window keeps its own key stream and a
    // key pushed to a non-active window stays queued until it becomes active.
    Lowgui::namedWindow("route_a");
    Lowgui::imshow("route_a", cv::Mat::zeros(64, 64, CV_8UC3));
    Lowgui::namedWindow("route_b");
    Lowgui::imshow("route_b", cv::Mat::zeros(64, 64, CV_8UC3));

    auto& wm = cv::lowgui::detail::WindowManager::instance();
    wm.setActiveWindow("route_a");
    Lowgui::waitKey(0);

    wm.getKeyQueue("route_a")->push('a');
    EXPECT_EQ(Lowgui::waitKey(1), 'a');

    wm.getKeyQueue("route_b")->push('b');
    EXPECT_EQ(Lowgui::waitKey(1), -1);  // b's key is not routed while a is active

    wm.setActiveWindow("route_b");
    EXPECT_EQ(Lowgui::waitKeyEx(1), 'b');
}

// ---------- Engine lifecycle through the public API ----------

TEST_F(OffscreenRenderingTest, destroyWindow_stops_its_engine) {
    Lowgui::namedWindow("kill_win");
    Lowgui::imshow("kill_win", cv::Mat::zeros(64, 64, CV_8UC3));
    Lowgui::waitKey(0);  // starts (but never captures for) the offscreen engine
    auto& wm = cv::lowgui::detail::WindowManager::instance();
    EXPECT_TRUE(wm.isEngineRunning("kill_win"));

    Lowgui::destroyWindow("kill_win");
    EXPECT_FALSE(Lowgui::hasWindow("kill_win"));
    EXPECT_FALSE(wm.isEngineRunning("kill_win"));
}

TEST_F(OffscreenRenderingTest, destroyAllWindows_stops_all_engines) {
    Lowgui::namedWindow("kill_a");
    Lowgui::imshow("kill_a", cv::Mat::zeros(16, 16, CV_8UC1));
    Lowgui::namedWindow("kill_b");
    Lowgui::imshow("kill_b", cv::Mat::zeros(16, 16, CV_8UC3));
    Lowgui::waitKey(0);
    auto& wm = cv::lowgui::detail::WindowManager::instance();
    EXPECT_TRUE(wm.isEngineRunning("kill_a"));
    EXPECT_TRUE(wm.isEngineRunning("kill_b"));

    Lowgui::destroyAllWindows();
    EXPECT_FALSE(Lowgui::hasWindow("kill_a"));
    EXPECT_FALSE(Lowgui::hasWindow("kill_b"));
    EXPECT_FALSE(wm.isEngineRunning("kill_a"));
    EXPECT_FALSE(wm.isEngineRunning("kill_b"));

    // With no windows left, waitKey returns -1 promptly instead of hanging.
    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(Lowgui::waitKey(0), -1);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

// ---------- Public trackbar / button / property APIs with a live engine ----------

namespace {
void offscreenTrackbarCb(int, void* ud) {
    if (ud) ++*static_cast<int*>(ud);
}
} // namespace

TEST_F(OffscreenRenderingTest, trackbar_public_api_roundtrip_with_engine) {
    Lowgui::namedWindow("tb_win");
    Lowgui::imshow("tb_win", cv::Mat::zeros(64, 64, CV_8UC3));
    Lowgui::waitKey(0);  // engine alive

    int cbCalls = 0;
    int v = 0;
    EXPECT_EQ(Lowgui::createTrackbar("gamma", "tb_win", &v, 100, offscreenTrackbarCb, &cbCalls), 1);
    EXPECT_EQ(Lowgui::getTrackbarPos("gamma", "tb_win"), 0);

    Lowgui::setTrackbarPos("gamma", "tb_win", 42);
    EXPECT_EQ(Lowgui::getTrackbarPos("gamma", "tb_win"), 42);
    EXPECT_EQ(v, 42);
    EXPECT_EQ(cbCalls, 1);

    // Out-of-range set clamps to max and still fires the callback.
    Lowgui::setTrackbarPos("gamma", "tb_win", 500);
    EXPECT_EQ(Lowgui::getTrackbarPos("gamma", "tb_win"), 100);
    EXPECT_EQ(v, 100);
    EXPECT_EQ(cbCalls, 2);

    // Shrinking the max below the current position clamps down and fires.
    Lowgui::setTrackbarMax("gamma", "tb_win", 30);
    EXPECT_EQ(Lowgui::getTrackbarPos("gamma", "tb_win"), 30);
    EXPECT_EQ(v, 30);
    EXPECT_EQ(cbCalls, 3);

    // Raising the min above the current position clamps up and fires.
    Lowgui::setTrackbarMin("gamma", "tb_win", 40);
    EXPECT_EQ(Lowgui::getTrackbarPos("gamma", "tb_win"), 40);
    EXPECT_EQ(v, 40);
    EXPECT_EQ(cbCalls, 4);

    EXPECT_EQ(Lowgui::getTrackbarPos("missing", "tb_win"), -1);
}

TEST_F(OffscreenRenderingTest, button_public_api_tracks_state) {
    Lowgui::namedWindow("btn_win");
    Lowgui::imshow("btn_win", cv::Mat::zeros(32, 32, CV_8UC3));
    Lowgui::waitKey(0);  // engine alive

    int cbCalls = 0;
    int barId = Lowgui::createButton("chk", offscreenTrackbarCb, &cbCalls,
                                    cv::lowgui::QT_CHECKBOX, false);
    EXPECT_GT(barId, 0);

    auto& wm = cv::lowgui::detail::WindowManager::instance();
    EXPECT_EQ(wm.setButtonState("chk", 1), 0);  // returns the previous state
    EXPECT_EQ(wm.setButtonState("chk", 0), 1);

    bool found = false;
    for (const auto& b : wm.getControlButtons()) {
        if (b.name == "chk") {
            found = true;
            EXPECT_EQ(b.state, 0);
        }
    }
    EXPECT_TRUE(found);
    // Truly missing buttons also report -1 (documented ambiguity, see unit tests).
    EXPECT_EQ(wm.setButtonState("missing", 1), -1);
}

TEST_F(OffscreenRenderingTest, window_properties_roundtrip_while_engine_runs) {
    Lowgui::namedWindow("prop_win");
    Lowgui::imshow("prop_win", cv::Mat::zeros(32, 32, CV_8UC3));
    Lowgui::waitKey(0);  // engine alive

    Lowgui::setWindowProperty("prop_win", cv::lowgui::WND_PROP_VISIBLE, 0);
    EXPECT_EQ(Lowgui::getWindowProperty("prop_win", cv::lowgui::WND_PROP_VISIBLE), 0);
    Lowgui::setWindowProperty("prop_win", cv::lowgui::WND_PROP_VISIBLE, 1);
    EXPECT_EQ(Lowgui::getWindowProperty("prop_win", cv::lowgui::WND_PROP_VISIBLE), 1);

    Lowgui::setWindowProperty("prop_win", cv::lowgui::WND_PROP_AUTOSIZE,
                              cv::lowgui::WINDOW_AUTOSIZE);
    EXPECT_EQ(Lowgui::getWindowProperty("prop_win", cv::lowgui::WND_PROP_AUTOSIZE),
              cv::lowgui::WINDOW_AUTOSIZE);
    Lowgui::setWindowProperty("prop_win", cv::lowgui::WND_PROP_AUTOSIZE,
                              cv::lowgui::WINDOW_NORMAL);
    EXPECT_EQ(Lowgui::getWindowProperty("prop_win", cv::lowgui::WND_PROP_AUTOSIZE),
              cv::lowgui::WINDOW_NORMAL);

    Lowgui::setWindowProperty("prop_win", cv::lowgui::WND_PROP_FULLSCREEN,
                              cv::lowgui::WINDOW_FULLSCREEN);
    EXPECT_EQ(Lowgui::getWindowProperty("prop_win", cv::lowgui::WND_PROP_FULLSCREEN),
              cv::lowgui::WINDOW_FULLSCREEN);
    // Qt-parity no-ops stay -1 even with an engine alive.
    EXPECT_EQ(Lowgui::getWindowProperty("prop_win", cv::lowgui::WND_PROP_OPENGL), -1);
}

// ---------- End-to-end gwe event dispatch beyond the PRESS path ----------

TEST_F(OffscreenRenderingTest, gwe_injected_scroll_dispatches_wheel_event_with_delta_flags) {
    struct Ctx {
        std::atomic<int> count{0};
        std::atomic<int> lastEvent{-1};
        std::atomic<int> lastFlags{-1};
    } ctx;

    Lowgui::namedWindow("gwe_wheel_win");
    Lowgui::setMouseCallback("gwe_wheel_win",
        [](int event, int, int, int flags, void* ud) {
            auto* c = static_cast<Ctx*>(ud);
            c->count.fetch_add(1, std::memory_order_relaxed);
            c->lastEvent.store(event, std::memory_order_relaxed);
            c->lastFlags.store(flags, std::memory_order_relaxed);
        }, &ctx);
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::imshow("gwe_wheel_win", cv::Mat::zeros(960, 960, CV_8UC3));
    // Drive one capture frame so the worker's per-thread gwe queue is registered
    // before we broadcast an event into it (see gwe_injected_keypress test).
    Lowgui::waitKey(0);

    // One vertical scroll "notch" (delta y = +1): the dispatched flags carry
    // delta * 120 in the high 16 bits (getMouseWheelDelta contract).
    gwe::detail::push(std::make_shared<cv::v4d::event::Mouse>(
        cv::v4d::event::Mouse::SCROLL, cv::Point(480, 480), cv::Point(0, 1)));
    // Drive a frame so the offscreen engine consumes the queued event.
    Lowgui::imshow("gwe_wheel_win", cv::Mat::zeros(960, 960, CV_8UC4));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ctx.count.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ASSERT_GT(ctx.count.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(ctx.lastEvent.load(), cv::lowgui::EVENT_MOUSEWHEEL);
    EXPECT_EQ(Lowgui::getMouseWheelDelta(ctx.lastFlags.load()), 120);
}

TEST_F(OffscreenRenderingTest, gwe_injected_double_click_dispatches_dblclk_event) {
    struct Ctx {
        std::atomic<int> count{0};
        std::atomic<int> lastEvent{-1};
        std::atomic<int> lastX{-1};
        std::atomic<int> lastY{-1};
    } ctx;

    Lowgui::namedWindow("gwe_dbl_win");
    Lowgui::setMouseCallback("gwe_dbl_win",
        [](int event, int x, int y, int, void* ud) {
            auto* c = static_cast<Ctx*>(ud);
            c->count.fetch_add(1, std::memory_order_relaxed);
            c->lastEvent.store(event, std::memory_order_relaxed);
            c->lastX.store(x, std::memory_order_relaxed);
            c->lastY.store(y, std::memory_order_relaxed);
        }, &ctx);
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::imshow("gwe_dbl_win", cv::Mat::zeros(960, 960, CV_8UC3));
    // Drive one capture frame so the worker's per-thread gwe queue is registered.
    Lowgui::waitKey(0);

    gwe::detail::push(std::make_shared<cv::v4d::event::Mouse>(
        cv::v4d::event::Mouse::DOUBLE_CLICK, cv::v4d::event::Mouse::LEFT,
        cv::Point(100, 50)));
    // Drive a frame so the offscreen engine consumes the queued event.
    Lowgui::imshow("gwe_dbl_win", cv::Mat::zeros(960, 960, CV_8UC4));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ctx.count.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ASSERT_GT(ctx.count.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(ctx.lastEvent.load(), cv::lowgui::EVENT_LBUTTONDBLCLK);
    EXPECT_EQ(ctx.lastX.load(), 100);
    EXPECT_EQ(ctx.lastY.load(), 50);
}

} // namespace
} // namespace opencv_test

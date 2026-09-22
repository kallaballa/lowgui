#include <opencv2/lowgui/lowgui.hpp>
#include <opencv2/lowgui/window_manager.hpp>
#include "lowgui_input.hpp"
#include "lowgui_root_plan.hpp"
#include <opencv2/v4d/v4d.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace cv {
namespace lowgui {
namespace detail {

cv::UMat LowguiRootPlan::s_framebuffer;
std::mutex LowguiRootPlan::s_fbMutex;
std::atomic<long> LowguiRootPlan::s_frameCount{0};
std::mutex LowguiRootPlan::s_captureMtx;
std::condition_variable LowguiRootPlan::s_captureCv;
long LowguiRootPlan::s_captureRequested = 0;
long LowguiRootPlan::s_captureDone = 0;
std::atomic<std::uint64_t> LowguiRootPlan::s_frameDrawStartGen{0};
std::atomic<std::uint64_t> LowguiRootPlan::s_frameDrawEndGen{0};
std::uint64_t LowguiRootPlan::s_captureRequestedGen = 0;
std::atomic<std::uint64_t> LowguiRootPlan::s_frameDrawDoneGen{0};
std::mutex LowguiRootPlan::s_frameDrawMtx;
std::condition_variable LowguiRootPlan::s_frameDrawCv;

std::map<std::string, LowguiRootPlan::ViewState> LowguiRootPlan::s_viewStates;
std::string LowguiRootPlan::s_activeWindow;
std::atomic<bool> LowguiRootPlan::s_headless{false};
std::atomic<int> LowguiRootPlan::s_winW{0};
std::atomic<int> LowguiRootPlan::s_winH{0};

}

}
}

using namespace cv::lowgui;
using namespace cv::lowgui::detail;
using namespace cv::v4d;

namespace {

// The V4D run loop can only be entered once per process (finish/rendezvous
// state is process-global), so lowgui starts a single long-lived render
// engine on the first waitKey call and keeps it running. waitKey controls
// sequencing and framebuffer snapshots instead of the loop lifecycle.

std::mutex gEngineMtx;
std::thread gEngineThread;
bool gEngineThreadStarted = false;
std::atomic<bool> gEngineLoopAlive{false};
std::atomic<bool> gEngineModeOffscreen{false};
bool gEngineShutdownRegistered = false;

bool hasNativeDisplay() {
    const char* d = std::getenv("DISPLAY");
    const char* w = std::getenv("WAYLAND_DISPLAY");
    return (d && d[0]) != 0 || (w && w[0]) != 0;
}

bool shouldRenderOffscreen() {
    if (std::getenv("LOWGUI_HEADLESS_RENDER")) return true;
    if (std::getenv("LOWGUI_FORCE_OFFSCREEN")) return true;
    return !hasNativeDisplay();
}

void engineFn(bool offscreen) {
    try {
        LowguiRootPlan::setHeadless(offscreen);
        cv::Rect viewport(0, 0, 960, 960);
        cv::Ptr<V4D> runtime = V4D::init(viewport, "lowgui",
                                         AllocateFlags::NANOVG | AllocateFlags::IMGUI,
                                         offscreen ? (ConfigFlags::OFFSCREEN | ConfigFlags::DISPLAY_MODE)
                                                   : ConfigFlags::DISPLAY_MODE);
        V4DPlan::run<LowguiRootPlan>(0);
    } catch (const std::exception& ex) {
        CV_LOG_ERROR(nullptr, "lowgui render engine terminated: " << ex.what());
    } catch (...) {
        CV_LOG_ERROR(nullptr, "lowgui render engine terminated with unknown error.");
    }
    // Wake any thread blocked in waitKey(0) so it returns -1 at shutdown.
    cv::lowgui::detail::keyQueue().notify();
    gEngineLoopAlive = false;
}

void startEngine(bool offscreen) {
    {
        std::lock_guard<std::mutex> lock(gEngineMtx);
        if (gEngineThreadStarted) return;
        gEngineThreadStarted = true;
        gEngineModeOffscreen = offscreen;
        gEngineLoopAlive = true;
        gEngineThread = std::thread(engineFn, offscreen);
    }
    if (!gEngineShutdownRegistered) {
        gEngineShutdownRegistered = true;
        std::atexit([]() {
            std::thread t;
            {
                std::lock_guard<std::mutex> lock(gEngineMtx);
                if (!gEngineThreadStarted) return;
                gEngineThreadStarted = false;
                t = std::move(gEngineThread);
            }
            if (gEngineLoopAlive.load()) cv::v4d::request_finish();
            if (t.joinable()) t.join();
            cv::lowgui::detail::keyQueue().notify();
        });
    }
}

} // namespace

void Lowgui::namedWindow(const std::string& winname, int flags) {
    WindowManager::instance().createWindow(winname, flags);
}

void Lowgui::imshow(const std::string& winname, InputArray mat) {
    if (mat.empty()) return;
    auto& wm = WindowManager::instance();
    if (!wm.hasWindow(winname)) {
        // Matches highgui: imshow auto-creates missing windows (AUTOSIZE).
        wm.createWindow(winname, WINDOW_AUTOSIZE);
    }
    cv::UMat umat = mat.getUMat();
    wm.pushImage(winname, umat);
}

namespace {
int waitKeyImpl(int delay, bool lowByte) {
    static const int kCaptureTimeoutMs = 10000;

    if (WindowManager::instance().windowCount() == 0) {
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        return -1;
    }

    startEngine(shouldRenderOffscreen());

    if (!gEngineLoopAlive.load()) {
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        return -1;
    }

    const bool headlessCapture = std::getenv("LOWGUI_HEADLESS_RENDER") != nullptr;

    if (gEngineModeOffscreen.load()) {
        // Headless/offscreen (LOWGUI_HEADLESS_RENDER / LOWGUI_FORCE_OFFSCREEN /
        // no display): keep the capture sequencing, then drain any key that was
        // injected through gwe::push. No injected key means -1. Mirror the
        // real-display branch and clear any stale capture from earlier runs so
        // readFramebuffer() reflects only what this waitKey produced.
        LowguiRootPlan::clearFramebuffer();
        if (headlessCapture) {
            long id = LowguiRootPlan::requestFrameCapture(WindowManager::instance().generation());
            if (!LowguiRootPlan::waitForFrameCapture(id, kCaptureTimeoutMs)) {
                CV_LOG_WARNING(nullptr, "lowgui: timed out waiting for framebuffer capture");
            }
        }
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        int code = cv::lowgui::detail::keyQueue().poll();
        if (code < 0) return -1;
        return lowByte ? (code & 0xff) : code;
    }

    // Real display: present one settled frame before waiting for a key so the
    // caller's most recent imshow is visible. waitKey(0) blocks until a key
    // press or the engine/window is closed.
    LowguiRootPlan::clearFramebuffer();
    if (delay == 0) {
        std::uint64_t req = WindowManager::instance().generation();
        if (!LowguiRootPlan::waitForSettledFrame(req, kCaptureTimeoutMs)) {
            CV_LOG_WARNING(nullptr, "lowgui: timed out waiting for a settled frame");
        }
    }
    int code = cv::lowgui::detail::keyQueue().wait(delay);
    if (code < 0) return -1;
    return lowByte ? (code & 0xff) : code;
}
} // namespace

int Lowgui::waitKey(int delay) {
    return waitKeyImpl(delay, true);
}

int Lowgui::pollKey() {
    return waitKeyImpl(1, true);
}

int Lowgui::waitKeyEx(int delay) {
    return waitKeyImpl(delay, false);
}

void Lowgui::destroyWindow(const std::string& winname) {
    WindowManager::instance().destroyWindow(winname);
}

void Lowgui::destroyAllWindows() {
    WindowManager::instance().destroyAllWindows();
}

bool Lowgui::hasWindow(const std::string& winname) {
    return WindowManager::instance().hasWindow(winname);
}

void Lowgui::resizeWindow(const std::string& winname, int width, int height) {
    auto wd = WindowManager::instance().getWindowShared(winname);
    if (wd) {
        std::lock_guard<std::mutex> lock(wd->sink->mtx);
        wd->viewport.width = width;
        wd->viewport.height = height;
        wd->userViewport = true;
    }
}

void Lowgui::moveWindow(const std::string& winname, int x, int y) {
    auto wd = WindowManager::instance().getWindowShared(winname);
    if (wd) {
        std::lock_guard<std::mutex> lock(wd->sink->mtx);
        wd->viewport.x = x;
        wd->viewport.y = y;
        wd->userViewport = true;
    }
}

void Lowgui::setWindowTitle(const std::string& winname, const std::string& title) {
    auto wd = WindowManager::instance().getWindowShared(winname);
    if (wd) {
        std::lock_guard<std::mutex> lock(wd->sink->mtx);
        wd->title = title;
    }
}

cv::UMat Lowgui::readFramebuffer() {
    return detail::LowguiRootPlan::getFramebuffer();
}

void Lowgui::setMouseCallback(const std::string& winname, MouseCallback onMouse,
                              void* userdata) {
    WindowManager::instance().setMouseCallback(winname, onMouse, userdata);
}

int Lowgui::getMouseWheelDelta(int flags) {
    // Wheel delta is packed in the upper 16 bits of the flags as a multiple of
    // 120 per notch (see handleInput). Recover it as a signed value so negative
    // (up/away) scrolling comes through as a negative delta.
    return (int)(int16_t)((flags >> 16) & 0xffff);
}

int Lowgui::createTrackbar(const std::string& trackbarname, const std::string& winname,
                           int* value, int count,
                           TrackbarCallback onChange, void* userdata) {
    return WindowManager::instance()
        .createTrackbar(trackbarname, winname, value, count, onChange, userdata) ? 1 : 0;
}

int Lowgui::getTrackbarPos(const std::string& trackbarname, const std::string& winname) {
    return WindowManager::instance().getTrackbarPos(trackbarname, winname);
}

void Lowgui::setTrackbarPos(const std::string& trackbarname, const std::string& winname, int pos) {
    WindowManager::instance().setTrackbarPos(trackbarname, winname, pos);
}

void Lowgui::setTrackbarMax(const std::string& trackbarname, const std::string& winname, int maxval) {
    WindowManager::instance().setTrackbarMax(trackbarname, winname, maxval);
}

void Lowgui::setTrackbarMin(const std::string& trackbarname, const std::string& winname, int minval) {
    WindowManager::instance().setTrackbarMin(trackbarname, winname, minval);
}

int Lowgui::createButton(const std::string& bar_name, ButtonCallback on_change,
                         void* userdata, int type, bool initial_button_state) {
    return WindowManager::instance().createButton(bar_name, on_change, userdata,
                                                   type, initial_button_state);
}

void Lowgui::setWindowProperty(const std::string& winname, int prop_id, int prop_value) {
    WindowManager::instance().setProperty(winname, prop_id, prop_value);
    if (prop_id == WND_PROP_FULLSCREEN) {
        // The root plan writes V4D::Keys::FULLSCREEN from the stored property on
        // every frame (whole native window). Notify the settled-frame waiters so
        // a subsequent waitKey(0) presents the new fullscreen state.
        LowguiRootPlan::notifySettledFrame(WindowManager::instance().generation());
    }
}

double Lowgui::getWindowProperty(const std::string& winname, int prop_id) {
    return WindowManager::instance().getProperty(winname, prop_id);
}

cv::Rect Lowgui::getWindowImageRect(const std::string& winname) {
    cv::Size sz = detail::LowguiRootPlan::windowSize();
    if (sz.width <= 0 || sz.height <= 0) sz = cv::Size(960, 960);
    cv::Rect r = detail::LowguiRootPlan::viewportFor(winname, sz);
    return r.width > 0 ? r : cv::Rect();
}

void Lowgui::displayOverlay(const std::string& winname, const std::string& text, int delayms) {
    WindowManager::instance().setMessage(winname, text, delayms, true);
}

void Lowgui::displayStatusBar(const std::string& winname, const std::string& text, int delayms) {
    WindowManager::instance().setMessage(winname, text, delayms, false);
}

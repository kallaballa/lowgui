#include <opencv2/lowgui/lowgui.hpp>
#include <opencv2/lowgui/window_manager.hpp>
#include <opencv2/lowgui/lowgui_root_plan.hpp>
#include <opencv2/v4d/v4d.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
std::mutex gEngineStateMtx;
std::condition_variable gEngineStateCv;
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
    {
        std::lock_guard<std::mutex> lock(gEngineStateMtx);
        gEngineLoopAlive = false;
    }
    gEngineStateCv.notify_all();
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
        });
    }
}

void waitEngineFinished() {
    std::unique_lock<std::mutex> lock(gEngineStateMtx);
    gEngineStateCv.wait(lock, []() { return !gEngineLoopAlive.load(); });
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
    cv::Mat cpu = mat.getMat();
    cv::UMat umat = cpu.getUMat(cv::ACCESS_READ);
    wm.pushImage(winname, umat);
}

int Lowgui::waitKey(int delay) {
    static const int kCaptureTimeoutMs = 10000;

    if (WindowManager::instance().windowCount() == 0) {
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        return -1;
    }

    startEngine(shouldRenderOffscreen());

    if (!gEngineLoopAlive.load()) {
        // The engine exited on its own (e.g. the window was closed in windowed
        // mode). The V4D loop cannot be restarted, so degrade to a sleep.
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        return -1;
    }

    if (std::getenv("LOWGUI_HEADLESS_RENDER")) {
        long id = LowguiRootPlan::requestFrameCapture();
        if (!LowguiRootPlan::waitForFrameCapture(id, kCaptureTimeoutMs)) {
            CV_LOG_WARNING(nullptr, "lowgui: timed out waiting for framebuffer capture");
        }
    } else {
        LowguiRootPlan::clearFramebuffer();
        // OpenCV semantics for waitKey(0): block until the user closes the
        // window. In offscreen mode there is nothing to wait for interactively.
        if (delay == 0 && !gEngineModeOffscreen.load()) {
            waitEngineFinished();
        }
    }

    if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    return -1;
}

int Lowgui::pollKey() {
    return -1;
}

int Lowgui::waitKeyEx(int delay) {
    return waitKey(delay);
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
        std::lock_guard<std::mutex> lock(wd->sink.mtx);
        wd->viewport.width = width;
        wd->viewport.height = height;
        wd->userViewport = true;
    }
}

void Lowgui::moveWindow(const std::string& winname, int x, int y) {
    auto wd = WindowManager::instance().getWindowShared(winname);
    if (wd) {
        std::lock_guard<std::mutex> lock(wd->sink.mtx);
        wd->viewport.x = x;
        wd->viewport.y = y;
        wd->userViewport = true;
    }
}

void Lowgui::setWindowTitle(const std::string& winname, const std::string& title) {
    auto wd = WindowManager::instance().getWindowShared(winname);
    if (wd) {
        std::lock_guard<std::mutex> lock(wd->sink.mtx);
        wd->title = title;
    }
}

cv::UMat Lowgui::readFramebuffer() {
    return detail::LowguiRootPlan::getFramebuffer();
}
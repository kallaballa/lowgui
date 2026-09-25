// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 lowgui contributors
// Clean-room reimplementation of OpenCV highgui (see README.md).
#include <opencv2/lowgui/lowgui.hpp>
#include <opencv2/lowgui/window_manager.hpp>
#include "lowgui_input.hpp"
#include "lowgui_engine.hpp"
#include "lowgui_window_plan.hpp"
#include <opencv2/core/utils/logger.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>

// glfwSetWindowShouldClose (used in the native-window close handler) must be
// declared; v4d/events.hpp includes GLFW in NONE mode.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace cv {
namespace lowgui {
namespace detail {

int waitKeyImpl(int delay, bool lowByte) {
    static const int kCaptureTimeoutMs = 10000;

    if (WindowManager::instance().windowCount() == 0) {
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        return -1;
    }

    std::string active = WindowManager::instance().getActiveWindow();
    if (active.empty()) {
        auto names = WindowManager::instance().getWindowNames();
        if (!names.empty()) {
            active = names.front();
            WindowManager::instance().setActiveWindow(active);
        }
    }

    if (active.empty()) {
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        return -1;
    }

    auto wd = WindowManager::instance().getWindowShared(active);
    if (!wd) {
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        return -1;
    }

    startEngineForWindow(active);

    if (!wd->engineRunning.load(std::memory_order_acquire)) {
        static bool warnedPostDeath = false;
        if (!warnedPostDeath) {
            warnedPostDeath = true;
            CV_LOG_WARNING(nullptr, "lowgui: render engine for window '" << active
                << "' has terminated; waitKey* returns -1.");
        }
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        return -1;
    }

    const bool headlessCapture = std::getenv("LOWGUI_HEADLESS_RENDER") != nullptr;

    if (shouldRenderOffscreen()) {
        if (headlessCapture) {
            // Route the capture through the active window's own plan instance.
            // waitForPlanForWindow absorbs the engine-startup race: on the first
            // waitKey(0) the plan is constructed on its engine thread shortly
            // after startEngineForWindow spawns it, so let it register first.
            LowguiWindowPlan* plan = LowguiWindowPlan::waitForPlanForWindow(
                active, kCaptureTimeoutMs);
            if (plan) {
                plan->requestFrameCapture(WindowManager::instance().generation());
                if (!plan->waitForFrameCapture(0, kCaptureTimeoutMs) && delay == 0) {
                    CV_LOG_WARNING(nullptr,
                        "lowgui: timed out waiting for framebuffer capture");
                }
            }
        }
        if (delay > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        int code = wd->keyQueue->poll();
        if (code >= 0) return lowByte ? (code & 0xff) : code;
        return -1;
    }

    while (wd->engineRunning.load(std::memory_order_acquire)) {
        int code = wd->keyQueue->wait(delay);
        if (code >= 0) return lowByte ? (code & 0xff) : code;
        std::string newActive = WindowManager::instance().getActiveWindow();
        if (newActive != active) {
            active = newActive;
            wd = WindowManager::instance().getWindowShared(active);
            if (!wd) { if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay)); return -1; }
            continue;
        }
        if (delay > 0) return -1;
    }
    return -1;
}

} // namespace detail

using namespace cv::lowgui::detail;

void Lowgui::namedWindow(const std::string& winname, int flags) {
    WindowManager::instance().createWindow(winname, flags);
}

void Lowgui::imshow(const std::string& winname, InputArray mat) {
    if (mat.empty()) return;
    auto& wm = WindowManager::instance();
    if (!wm.hasWindow(winname)) {
        wm.createWindow(winname, WINDOW_AUTOSIZE);
    }
    cv::UMat umat = mat.getUMat();
    wm.pushImage(winname, umat);
    cv::lowgui::detail::startEngineForWindow(winname);
}

int Lowgui::waitKey(int delay) {
    return cv::lowgui::detail::waitKeyImpl(delay, true);
}

int Lowgui::pollKey() {
    return cv::lowgui::detail::waitKeyImpl(1, true);
}

int Lowgui::waitKeyEx(int delay) {
    return cv::lowgui::detail::waitKeyImpl(delay, false);
}

void Lowgui::destroyWindow(const std::string& winname) {
    cv::lowgui::detail::stopEngineForWindow(winname);
    WindowManager::instance().destroyWindow(winname);
}

void Lowgui::destroyAllWindows() {
    auto names = WindowManager::instance().getWindowNames();
    for (const auto& name : names)
        cv::lowgui::detail::stopEngineForWindow(name);
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
    return cv::lowgui::detail::LowguiWindowPlan::readActiveFramebuffer();
}

void Lowgui::setMouseCallback(const std::string& winname, MouseCallback onMouse,
                              void* userdata) {
    WindowManager::instance().setMouseCallback(winname, onMouse, userdata);
}

int Lowgui::getMouseWheelDelta(int flags) {
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
}

double Lowgui::getWindowProperty(const std::string& winname, int prop_id) {
    return WindowManager::instance().getProperty(winname, prop_id);
}

cv::Rect Lowgui::getWindowImageRect(const std::string& winname) {
    auto wd = WindowManager::instance().getWindowShared(winname);
    if (!wd) return cv::Rect();
    int w = wd->winW.load(std::memory_order_relaxed);
    int h = wd->winH.load(std::memory_order_relaxed);
    if (w > 0 && h > 28)
        return cv::Rect(0, 0, w, h - 28);
    return cv::Rect();
}

void Lowgui::displayOverlay(const std::string& winname, const std::string& text, int delayms) {
    WindowManager::instance().setMessage(winname, text, delayms, true);
}

void Lowgui::displayStatusBar(const std::string& winname, const std::string& text, int delayms) {
    WindowManager::instance().setMessage(winname, text, delayms, false);
}

} // namespace lowgui
} // namespace cv

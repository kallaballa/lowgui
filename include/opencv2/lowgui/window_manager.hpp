// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 lowgui contributors
// Clean-room reimplementation of OpenCV highgui (see README.md).
#ifndef OPENCV_LOWGUI_WINDOW_MANAGER_HPP_
#define OPENCV_LOWGUI_WINDOW_MANAGER_HPP_

#include <opencv2/core.hpp>
#include <opencv2/lowgui/sink_source.hpp>
#include <opencv2/lowgui/lowgui.hpp>
#include "lowgui_input.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace cv {
namespace lowgui {
namespace detail {

//! A trackbar bound to a window (or the global control panel when winname is empty).
struct Trackbar {
    std::string name;                    // trackbar name (registry key)
    std::string winname;                 // owning window; empty => control panel
    int* value = nullptr;                // optional external int kept in sync
    int pos = 0;
    int min = 0;
    int max = 100;
    TrackbarCallback cb = nullptr;
    void* userdata = nullptr;
};

//! A control-panel button (created via createButton).
struct Button {
    std::string name;
    int type = QT_PUSH_BUTTON;
    int barId = 0;                       // buttonbar grouping (QT_NEW_BUTTONBAR)
    int state = -1;                      // -1 push, 0/1 check/radio
    ButtonCallback cb = nullptr;
    void* userdata = nullptr;
};

//! A transient status-bar / overlay message (delayms of 0 => never expires).
struct TransientMsg {
    std::string text;
    std::chrono::steady_clock::time_point expire;

    TransientMsg() : expire(std::chrono::steady_clock::time_point::max()) {}

    void set(const std::string& t, int delayms) {
        text = t;
        if (delayms <= 0) {
            expire = std::chrono::steady_clock::time_point::max();
        } else {
            expire = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayms);
        }
    }

    void clear() { text.clear(); }

    bool active() const {
        if (text.empty()) return false;
        return expire > std::chrono::steady_clock::now();
    }
};

struct WindowData {
    static std::uint64_t nextInstanceId() {
        static std::atomic<std::uint64_t> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    std::string name;
    std::string title;
    int flags;
    std::shared_ptr<SinkSource> sink;
    std::atomic<std::uint64_t> contentSerial{nextInstanceId() << 32};
    std::atomic<int> imageW{0};
    std::atomic<int> imageH{0};
    // Latest native-window size set by the render loop (0,0 before the first
    // frame). Used by getWindowImageRect in the per-native-window architecture.
    std::atomic<int> winW{0};
    std::atomic<int> winH{0};
    cv::Rect viewport;
    bool userViewport = false;

    MouseCallback mouseCb = nullptr;
    void* mouseUserdata = nullptr;
    std::map<std::string, Trackbar> trackbars;
    int propAutosize = 0;
    bool propKeepRatio = true;
    int propFullscreen = 0;
    int propVisible = 1;
    TransientMsg statusMsg;
    TransientMsg overlayMsg;

    std::unique_ptr<KeyQueue> keyQueue;

    mutable std::mutex engineMtx;
    std::thread engineThread;
    std::atomic<bool> engineRunning{false};

    WindowData() = default;
    WindowData(const WindowData&) = delete;
    WindowData& operator=(const WindowData&) = delete;
    WindowData(WindowData&&) = delete;
    WindowData& operator=(WindowData&&) = delete;

    WindowData(std::string n, std::string t, int f)
        : name(std::move(n)), title(std::move(t)), flags(f),
          sink(std::make_shared<SinkSource>()), viewport(),
          keyQueue(std::make_unique<KeyQueue>()) {
        propAutosize = (f & WINDOW_AUTOSIZE) ? WINDOW_AUTOSIZE : 0;
        propKeepRatio = true;
        propVisible = 1;
        propFullscreen = WINDOW_NORMAL;
    }
};

class WindowManager {
    // Windows are owned via shared_ptr so concurrent render/API accessors can
    // hold a reference across a destroyWindow() call instead of dangling.
    std::unordered_map<std::string, std::shared_ptr<WindowData>> windows_;
    std::vector<std::string> windowOrder_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool running_ = false;
    // Monotonically increased on every mutating operation (create/destroy/push).
    // waitKey and the render loop use it to ensure a captured frame was drawn
    // from state that is at least as new as what an API caller has set up.
    std::atomic<std::uint64_t> generation_{0};

    // Global control panel (empty-winname) bars, guarded by mtx_.
    std::map<std::string, Trackbar> controlTrackbars_;
    std::vector<Button> controlButtons_;
    int nextBarId_ = 0;
    std::string activeWindow_;

public:
    static WindowManager& instance();

    void createWindow(const std::string& name, int flags);
    void destroyWindow(const std::string& name);
    void destroyAllWindows();
    bool hasWindow(const std::string& name) const;

    std::uint64_t generation() const { return generation_.load(std::memory_order_relaxed); }

    void pushImage(const std::string& name, const cv::UMat& img);
    bool popImage(const std::string& name, cv::UMat& img);
    bool getImage(const std::string& name, cv::UMat& img) const;

    // Shared ownership is preferred: raw pointers are only valid while the
    // WindowManager mutex is held, so callers should use getWindowShared.
    std::shared_ptr<WindowData> getWindowShared(const std::string& name);
    std::shared_ptr<const WindowData> getWindowShared(const std::string& name) const;

    std::vector<std::string> getWindowNames() const;

    void setRunning(bool r);
    bool isRunning() const;

    void waitForWindow();
    void notifyAll();

    size_t windowCount() const;

    // ---------- Mouse callback registry ----------
    void setMouseCallback(const std::string& name, MouseCallback cb, void* userdata);

    // ---------- Trackbars ----------
    // Returns true and creates/updates the trackbar (Qt dedupes by name+window).
    bool createTrackbar(const std::string& tb, const std::string& win,
                        int* val, int count, TrackbarCallback cb, void* ud);
    bool getTrackbar(const std::string& tb, const std::string& win, Trackbar& out) const;
    int getTrackbarPos(const std::string& tb, const std::string& win) const;
    // Returns true and (if already differing) dispatches the callback (Qt setValue).
    bool setTrackbarPos(const std::string& tb, const std::string& win, int pos);
    bool setTrackbarMin(const std::string& tb, const std::string& win, int min);
    bool setTrackbarMax(const std::string& tb, const std::string& win, int max);
    // Returns the (updated) position so callers don't need a separate lookup.
    int updateTrackbarPos(const std::string& tb, const std::string& win, int pos);

    std::vector<Trackbar> getWindowTrackbars(const std::string& win) const;
    std::vector<Trackbar> getControlTrackbars() const;
    // True when the global control panel has any trackbar or button (used to
    // gate the "Display properties window" action and the panel menu item).
    bool hasControlPanelContent() const;

    // ---------- Buttons ----------
    int createButton(const std::string& name, ButtonCallback cb, void* ud,
                     int type, bool initial);
    std::vector<Button> getControlButtons() const;
    // Updates the stored state of a control-panel button; returns the previous state.
    int setButtonState(const std::string& name, int state);

    // ---------- Window properties ----------
    void setProperty(const std::string& name, int prop, int value);
    int getProperty(const std::string& name, int prop) const;

    // ---------- Transient messages ----------
    void setMessage(const std::string& name, const std::string& text, int delayms, bool overlay);

    // ---------- Engine lifecycle (implemented in lowgui_engine.cpp) ----------
    // startEngineForWindow / stopEngineForWindow are free functions in
    // lowgui_engine.cpp (the WindowManager intentionally does not own engine
    // threads; storing-and-joining a thread handle here duplicated that logic
    // and regressed the sequential-engine lifecycle).
    bool isEngineRunning(const std::string& name) const;
    KeyQueue* getKeyQueue(const std::string& name) const;

    // ---------- Active window (for per-window key routing) --------------------
    void setActiveWindow(const std::string& name);
    std::string getActiveWindow() const;

    // Called by the render plan each frame to publish the latest native-window
    // size so API-thread callers (getWindowImageRect) see an up-to-date rect.
    void setWindowNativeSize(const std::string& name, int w, int h);
};

} // namespace detail
} // namespace lowgui
} // namespace cv

#endif // OPENCV_LOWGUI_WINDOW_MANAGER_HPP_
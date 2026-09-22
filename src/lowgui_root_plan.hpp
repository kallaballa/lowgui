// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 lowgui contributors
// Clean-room reimplementation of OpenCV highgui (see README.md).
#ifndef OPENCV_LOWGUI_LOWGUI_ROOT_PLAN_HPP_
#define OPENCV_LOWGUI_LOWGUI_ROOT_PLAN_HPP_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/v4d/v4d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/lowgui/window_manager.hpp>
#include "lowgui_input.hpp"
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace cv {
namespace lowgui {
namespace detail {

using namespace cv::v4d;
using namespace cv::v4d::event;

/**
 * Root Plan-V4D plan that owns the long-lived render loop.
 *
 * Every frame it re-reads the current window set from the WindowManager
 * (windows can be created/destroyed between frames and across waitKey calls)
 * and draws all of them in a single NanoVG grid. Each window keeps its own
 * interactive ViewState (zoom/pan, cursor, dialog state) in a static map
 * shared between the render worker and the ImGui (menu bar) transaction on the
 * display thread via RWS edges.
 *
 * A headless (offscreen) framebuffer snapshot is produced on demand: waitKey
 * callers record a capture request id and wait until the engine has rendered
 * and copied the framebuffer, so readFramebuffer() deterministically reflects
 * the most recently pushed image. In headless mode the interactive overlays
 * (status bar, deep-zoom pixel readout, grid) are suppressed so captured
 * frames stay a clean render of the source images.
 */
class LowguiRootPlan : public V4DPlan {
public:
    struct ViewState {
        // Image
        int imageW = 0;
        int imageH = 0;
        int channels = 0;
        cv::Mat bgra8;            // BGRA8 copy of the current frame (status bar / deep zoom readout)
        int imageHandle = -1;     // NanoVG texture handle (worker thread only)
        cv::Mat rgbaCopy;         // keeps the texture's source pixels alive ("filename")
        std::string label;

        // Texture-cache bookkeeping: the last WindowData::contentSerial that
        // was uploaded (or rejected) for this window. kNeverUploaded forces the
        // first upload; unchanged content skips the per-frame re-encode/upload.
        static constexpr std::uint64_t kNeverUploaded = ~std::uint64_t(0);
        std::uint64_t uploadedSerial = kNeverUploaded;
        std::uint64_t badSerial = kNeverUploaded;

        // View transform (viewport-local coordinates)
        float zoom = 1.0f;
        cv::Point2f pan = {0.0f, 0.0f};
        bool isDragging = false;
        // ASPECT_RATIO parity (mirrors wd->propKeepRatio): KEEPRATIO letterboxes
        // with a uniform scale (fitToCell); FREERATIO stretches to fill the cell.
        // Deep-zoom / pixel-grid overlays only make sense under KEEPRATIO.
        bool keepRatio = true;

        // Transient displayOverlay/displayStatusBar content, refreshed every
        // frame from wd->statusMsg / wd->overlayMsg under wd->sink->mtx.
        std::string statusText;
        std::string overlayText;

        // Cursor tracking (viewport-local)
        cv::Point2f mousePos = {-1.0f, -1.0f};
        bool mouseInside = false;

        // UI toggles
        bool showProperties = false;
        bool showHelp = true;
        bool showStatusBar = true;
        // Set by the worker on right-click (Qt context menu); consumed by gui().
        bool showContextMenu = false;
        // Global "Display properties window" control panel (trackbars/buttons).
        bool showControlPanel = false;

        // Deep zoom threshold (mirrors OpenCV's QT imshow behaviour).
        static constexpr float kDeepZoomThreshold = 30.0f;

        // Save dialog state
        bool showSaveDialog = false;
        bool showSaveViewDialog = false;
        bool lastImageSaveOk = true;
        std::string lastImageSaveMsg;
        bool lastViewSaveOk = true;
        std::string lastViewSaveMsg;
        char saveBuf[1024] = {};
        int saveFormat = 0; // 0=PNG, 1=JPG, 2=BMP

        // File browser state
        bool showFileDialog = false;
        char fileDialogPath[1024] = {};
        char fileDialogSelected[256] = {};
        int fileDialogSelectedIdx = -1;
        std::vector<std::pair<std::string, bool>> fileDialogEntries;
        std::string fileDialogErrorMsg;
        static constexpr const char* kImageExts[] = {
            ".png", ".jpg", ".jpeg", ".bmp", ".tiff", ".tif", ".gif", ".webp", ".pnm", ".ppm", ".pgm", ".pbm"
        };

        // Reload request from GUI
        bool reloadRequested = false;
        std::string newFilename;
    };

private:
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

    static std::map<std::string, ViewState> s_viewStates;
    static std::string s_activeWindow;
    static std::atomic<bool> s_headless;

    // Last native-window size drawn by the worker, so API calls on other
    // threads (getWindowImageRect) can compute cell rects without a V4D handle.
    static std::atomic<int> s_winW;
    static std::atomic<int> s_winH;

    // NanoVG image handles to freed in the next nvg node (collected by
    // reconcileStates when a window disappears). Worker thread only.
    std::vector<int> orphanedTextures_;

    // Last FULLSCREEN value written to V4D (worker thread only); used to fire
    // the framebuffer resize callback only on transitions.
    bool lastFullscreen_ = false;

    static cv::UMat s_framebuffer;
    static std::mutex s_fbMutex;

    // Per-frame counter monotonically increased by the render loop.
    static std::atomic<long> s_frameCount;

    // one-shot capture coordination between waitKey (requester) and the loop.
    static std::mutex s_captureMtx;
    static std::condition_variable s_captureCv;
    static long s_captureRequested;
    static long s_captureDone;

    // WindowManager generation observed at the start/end of drawWindows. A
    // capture is only served by a frame that drew a fully-settled state: no
    // mutation happened mid-draw (start==end) and the drawn state is at least
    // as new as the requester's (end >= requested generation).
    static std::atomic<std::uint64_t> s_frameDrawStartGen;
    static std::atomic<std::uint64_t> s_frameDrawEndGen;
    static std::uint64_t s_captureRequestedGen;

    // Monotonically increased (under s_frameDrawMtx) whenever drawWindows drew
    // a settled frame (startGen==endGen). waitKey uses it to present at least
    // one settled frame before returning instead of blocking on window close.
    static std::atomic<std::uint64_t> s_frameDrawDoneGen;
    static std::mutex s_frameDrawMtx;
    static std::condition_variable s_frameDrawCv;

public:
    LowguiRootPlan() = default;

    static void setHeadless(bool value) { s_headless.store(value); }

    // Last native-window size drawn by the worker (0,0 before the first frame).
    static cv::Size windowSize() {
        return cv::Size(s_winW.load(std::memory_order_relaxed),
                        s_winH.load(std::memory_order_relaxed));
    }

    // Cell rect of a window in the current layout (used by getWindowImageRect).
    static cv::Rect viewportFor(const std::string& name, const cv::Size& sz);

    // Wakes settled-frame waiters (used by setWindowProperty(FULLSCREEN) so a
    // subsequent waitKey(0) presents the updated layout/fullscreen state).
    static void notifySettledFrame(std::uint64_t gen);

    // Engine-termination hook: releases waitKey(0) threads parked on the
    // capture/settle condition variables so they return -1 promptly (via the
    // post-death path) instead of stalling until the 10 s timeout elapses.
    static void notifyWaitersShutdown();

    static cv::UMat getFramebuffer() {
        std::lock_guard<std::mutex> lock(s_fbMutex);
        return s_framebuffer.clone();
    }

    static void clearFramebuffer() {
        std::lock_guard<std::mutex> lock(s_fbMutex);
        s_framebuffer.release();
    }

    static long requestFrameCapture(std::uint64_t gen) {
        std::lock_guard<std::mutex> lock(s_captureMtx);
        ++s_captureRequested;
        s_captureRequestedGen = gen;
        return s_captureRequested;
    }

    static bool waitForFrameCapture(long id, int timeoutMs) {
        std::unique_lock<std::mutex> lock(s_captureMtx);
        return s_captureCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [id]() { return s_captureDone >= id; });
    }

    // Blocks until the render loop has drawn a settled frame whose window-set
    // generation is at least gen, or until the timeout elapses.
    static bool waitForSettledFrame(std::uint64_t gen, int timeoutMs) {
        std::unique_lock<std::mutex> lock(s_frameDrawMtx);
        return s_frameDrawCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [gen]() { return s_frameDrawDoneGen.load() >= gen; });
    }

    void setup() override {
        set(GlobalState::Keys::TIME_TRACKER, V(false));
        set(GlobalState::Keys::SHOW_FRAME_TIME, V(false));
    }

    void infer() override {
        set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(30, 30, 30, 255)));
        clear();

        // Sync the per-window state with the live window set and apply any
        // pending reloads requested through the file browser.
        plain([this](std::map<std::string, ViewState>& states,
                     std::string& activeWindow) {
            reconcileStates(states, activeWindow);
            handleReloads(states);
        }, RWS(s_viewStates), RWS(s_activeWindow));

        // Reconcile is a plain node, but input routing needs a settled window
        // set immediately, so it runs right after on the same worker thread.
        plain([](const Keyboard::List& pressEvents,
                 const Keyboard::List& releaseEvents) {
            handleKeys(pressEvents, releaseEvents);
        }, keyPress_, keyRelease_);

        plain([](const Mouse::List& scrollEvents,
                 const Mouse::List& dragEvents,
                 const Mouse::List& pressLeftEvents,
                 const Mouse::List& releaseLeftEvents,
                 const Mouse::List& pressRightEvents,
                 const Mouse::List& releaseRightEvents,
                 const Mouse::List& pressMiddleEvents,
                 const Mouse::List& releaseMiddleEvents,
                 const Mouse::List& doubleClickLeftEvents,
                 const Mouse::List& doubleClickRightEvents,
                 const Mouse::List& doubleClickMiddleEvents,
                 const Mouse::List& moveEvents,
                 const Mouse::List& hoverEnterEvents,
                 const Mouse::List& hoverExitEvents,
                 const cv::Size& sz,
                 std::map<std::string, ViewState>& states,
                 std::string& activeWindow) {
            handleInput(scrollEvents, dragEvents, pressLeftEvents, releaseLeftEvents,
                        pressRightEvents, releaseRightEvents, pressMiddleEvents,
                        releaseMiddleEvents, doubleClickLeftEvents, doubleClickRightEvents,
                        doubleClickMiddleEvents, moveEvents, hoverEnterEvents, hoverExitEvents,
                        sz, states, activeWindow);
        }, scroll_, drag_, pressLeft_, releaseLeft_, pressRight_, releaseRight_,
           pressMiddle_, releaseMiddle_, doubleClickLeft_, doubleClickRight_,
           doubleClickMiddle_, move_, hoverEnter_, hoverExit_, size_,
           RWS(s_viewStates), RWS(s_activeWindow));

        // Draw every window (image + grid + deep-zoom overlay + status bar).
        nvg([this](const cv::Size& sz, std::map<std::string, ViewState>& states) {
            drawWindows(sz, states);
        }, size_, RWS(s_viewStates));

        // Headless framebuffer snapshot (run after the nvg node each frame).
        plain([this](const cv::Size&) {
            maybeCaptureFramebuffer();
        }, size_);
    }

    // ImGui menu bar, dialogs and keyboard shortcuts on the display thread.
    void gui() override {
        imgui([](std::map<std::string, ViewState>& states,
                 std::string& activeWindow,
                 const cv::Size& sz) {
            guiBody(states, activeWindow, sz);
        }, RWS(s_viewStates), RWS(s_activeWindow), size_);
    }

    void teardown() override {
        // The worker tears down its own plan; free any remaining textures.
        nvg([this](std::map<std::string, ViewState>& states) {
            using namespace cv::v4d::nvg;
            for (auto& [name, st] : states) {
                if (st.imageHandle > 0) {
                    deleteImage(st.imageHandle);
                    st.imageHandle = -1;
                }
            }
            for (int h : orphanedTextures_) if (h > 0) deleteImage(h);
            orphanedTextures_.clear();
        }, RWS(s_viewStates));
    }

private:
    Event<Mouse> scroll_      = E<Mouse>(Mouse::SCROLL);
    Event<Mouse> drag_        = E<Mouse>(Mouse::DRAG);
    Event<Mouse> pressLeft_   = E<Mouse>(Mouse::PRESS,   Mouse::LEFT);
    Event<Mouse> releaseLeft_ = E<Mouse>(Mouse::RELEASE, Mouse::LEFT);
    Event<Mouse> pressRight_  = E<Mouse>(Mouse::PRESS,   Mouse::RIGHT);
    Event<Mouse> releaseRight_= E<Mouse>(Mouse::RELEASE, Mouse::RIGHT);
    Event<Mouse> pressMiddle_ = E<Mouse>(Mouse::PRESS,   Mouse::MIDDLE);
    Event<Mouse> releaseMiddle_= E<Mouse>(Mouse::RELEASE, Mouse::MIDDLE);
    Event<Mouse> doubleClickLeft_   = E<Mouse>(Mouse::DOUBLE_CLICK, Mouse::LEFT);
    Event<Mouse> doubleClickRight_  = E<Mouse>(Mouse::DOUBLE_CLICK, Mouse::RIGHT);
    Event<Mouse> doubleClickMiddle_ = E<Mouse>(Mouse::DOUBLE_CLICK, Mouse::MIDDLE);
    Event<Mouse> move_        = E<Mouse>(Mouse::MOVE);
    Event<Mouse> hoverEnter_  = E<Mouse>(Mouse::HOVER_ENTER);
    Event<Mouse> hoverExit_   = E<Mouse>(Mouse::HOVER_EXIT);
    Event<Keyboard> keyPress_   = E<Keyboard>(Keyboard::PRESS);
    Event<Keyboard> keyRelease_ = E<Keyboard>(Keyboard::RELEASE);

    // ---------- Key/modifier state (worker thread only) ---------------------
    // Plain statics are fine: the key node and handleInput are sequential plain
    // nodes on the same render worker thread.
    inline static bool sKeyShift = false;
    inline static bool sKeyCtrl = false;
    inline static bool sKeyAlt = false;
    inline static bool sLButton = false;
    inline static bool sRButton = false;
    inline static bool sMButton = false;

    // ---------- Layout ------------------------------------------------------

    static std::vector<std::pair<std::string, cv::Rect>> computeLayout(const cv::Size& sz) {
        std::vector<std::string> names;
        for (const std::string& n : WindowManager::instance().getWindowNames()) {
            auto wd = WindowManager::instance().getWindowShared(n);
            if (!wd) continue;
            bool visible = true;
            {
                std::lock_guard<std::mutex> lock(wd->sink->mtx);
                visible = wd->propVisible != 0;
            }
            // WND_PROP_VISIBLE == 0 hides the cell entirely (no grid slot, no
            // mouse hit, empty getWindowImageRect) — parity with Qt.
            if (visible) names.push_back(n);
        }
        std::vector<std::pair<std::string, cv::Rect>> layout;
        layout.reserve(names.size());
        int n = (int)names.size();
        if (n <= 0) return layout;
        int cols = std::ceil(std::sqrt((double)n));
        int rows = std::ceil((double)n / cols);
        int cellW = sz.width / cols;
        int cellH = sz.height / rows;
        for (int i = 0; i < n; ++i) {
            cv::Rect cell(i % cols * cellW, i / cols * cellH, cellW, cellH);
            cv::Rect vp = cell;
            auto wd = WindowManager::instance().getWindowShared(names[i]);
            if (wd) {
                int autosize = 0;
                bool userViewport;
                {
                    std::lock_guard<std::mutex> lock(wd->sink->mtx);
                    userViewport = wd->userViewport;
                    autosize = wd->propAutosize;
                }
                if (userViewport) {
                    std::lock_guard<std::mutex> lock(wd->sink->mtx);
                    vp = wd->viewport;
                } else if (autosize && wd->imageW > 0 && wd->imageH > 0) {
                    // WINDOW_AUTOSIZE: the cell is sized to the image, clamped
                    // to the grid slot and centered within it (Qt parity).
                    int iw = std::min((int)wd->imageW.load(), cell.width);
                    int ih = std::min((int)wd->imageH.load(), cell.height);
                    vp = cv::Rect(cell.x + (cell.width - iw) / 2,
                                  cell.y + (cell.height - ih) / 2, iw, ih);
                }
            }
            layout.emplace_back(names[i], vp);
        }
        return layout;
    }

    void reconcileStates(std::map<std::string, ViewState>& states,
                         std::string& activeWindow) {
        std::vector<std::string> names = WindowManager::instance().getWindowNames();
        for (auto it = states.begin(); it != states.end();) {
            if (std::find(names.begin(), names.end(), it->first) == names.end()) {
                // deleteImage needs an nvg context, so the handle is queued and
                // freed at the start of the next nvg draw node instead.
                if (it->second.imageHandle > 0)
                    orphanedTextures_.push_back(it->second.imageHandle);
                it = states.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& name : names) {
            auto [it, inserted] = states.try_emplace(name);
            if (inserted) {
                it->second.label = name;
                std::snprintf(it->second.fileDialogPath, sizeof(it->second.fileDialogPath), "%s", ".");
            }
        }
        if (states.empty()) {
            activeWindow.clear();
        } else if (activeWindow.empty() || states.find(activeWindow) == states.end()) {
            activeWindow = names.front();
        }
    }

    // Handles reloads requested through the file browser by pushing the loaded
    // image back into the WindowManager for that window (the render loop picks
    // it up on the next frame and re-fits the view on a size change).
    static void handleReloads(std::map<std::string, ViewState>& states) {
        for (auto& [name, st] : states) {
            if (!st.reloadRequested) continue;
            st.reloadRequested = false;
            cv::Mat tmp = cv::imread(st.newFilename, cv::IMREAD_UNCHANGED);
            auto& wm = WindowManager::instance();
            if (!tmp.empty() && wm.hasWindow(name)) {
                cv::UMat umat;
                tmp.copyTo(umat);
                wm.pushImage(name, umat);
                st.lastImageSaveOk = true;
                st.lastImageSaveMsg = "Loaded " + st.newFilename;
            } else if (tmp.empty()) {
                st.lastImageSaveOk = false;
                st.lastImageSaveMsg = "Failed to load '" + st.newFilename + "'";
            } else {
                st.lastImageSaveOk = false;
                st.lastImageSaveMsg = "Window no longer exists";
            }
        }
    }

    // ---------- Input -------------------------------------------------------

    static std::pair<std::string, cv::Rect> windowAt(
        const std::vector<std::pair<std::string, cv::Rect>>& layout, const cv::Point& p) {
        for (auto it = layout.rbegin(); it != layout.rend(); ++it) {
            if (it->second.contains(p)) return *it;
        }
        return {std::string(), cv::Rect()};
    }

    static cv::Point2f toLocal(const cv::Rect& vp, const cv::Point& p) {
        return cv::Point2f((float)(p.x - vp.x), (float)(p.y - vp.y));
    }

    // Tracks Shift/Ctrl/Alt (and mouse-button) held state for the mouse
    // callback flags and delivers key presses to the key queue. Modifier keys
    // themselves never produce a key code; Ctrl-prefixed combos are consumed by
    // ImGui/Qt as shortcuts and are suppressed here too.
    static void handleKeys(const Keyboard::List& pressEvents,
                           const Keyboard::List& releaseEvents) {
        using K = Keyboard::Key;
        for (const auto& e : pressEvents) {
            K k = e->key();
            switch (k) {
                case K::LEFT_SHIFT:  case K::RIGHT_SHIFT:  sKeyShift = true;  continue;
                case K::LEFT_CONTROL:case K::RIGHT_CONTROL:sKeyCtrl  = true;  continue;
                case K::LEFT_ALT:    case K::RIGHT_ALT:    sKeyAlt   = true;  continue;
                default: break;
            }
            if (sKeyCtrl) continue; // Ctrl shortcuts never reach the queue.
            int code = keyToCode(k);
            if (code > 0) keyQueue().push(code);
        }
        for (const auto& e : releaseEvents) {
            K k = e->key();
            switch (k) {
                case K::LEFT_SHIFT:  case K::RIGHT_SHIFT:  sKeyShift = false; break;
                case K::LEFT_CONTROL:case K::RIGHT_CONTROL:sKeyCtrl  = false; break;
                case K::LEFT_ALT:    case K::RIGHT_ALT:    sKeyAlt   = false; break;
                default: break;
            }
        }
    }

    static void handleInput(const Mouse::List& scrollEvents,
                            const Mouse::List& dragEvents,
                            const Mouse::List& pressLeftEvents,
                            const Mouse::List& releaseLeftEvents,
                            const Mouse::List& pressRightEvents,
                            const Mouse::List& releaseRightEvents,
                            const Mouse::List& pressMiddleEvents,
                            const Mouse::List& releaseMiddleEvents,
                            const Mouse::List& doubleClickLeftEvents,
                            const Mouse::List& doubleClickRightEvents,
                            const Mouse::List& doubleClickMiddleEvents,
                            const Mouse::List& moveEvents,
                            const Mouse::List& hoverEnterEvents,
                            const Mouse::List& hoverExitEvents,
                            const cv::Size& sz,
                            std::map<std::string, ViewState>& states,
                            std::string& activeWindow) {
        (void)hoverEnterEvents;
        if (states.empty()) return;
        auto layout = computeLayout(sz);
        if (layout.empty()) return;

        // Scroll wheel: dispatch Qt callbacks, then zoom around the cursor.
        // MOUSEWHEEL for the vertical axis, MOUSEHWHEEL for a dominant-horizontal
        // axis; the delta (notches*120) is packed into the upper 16 flag bits.
        for (auto se : scrollEvents) {
            auto [name, vp] = windowAt(layout, se->position());
            if (name.empty() || !states.count(name)) continue;
            bool horizontal = std::abs(se->data().x) > std::abs(se->data().y);
            int delta = (int)(horizontal ? se->data().x : se->data().y);
            if (delta == 0) delta = 1;
            int wflags = ((int)(delta * 120) & 0xffff) << 16;
            dispatchMouse(name, vp, states,
                          horizontal ? EVENT_MOUSEHWHEEL : EVENT_MOUSEWHEEL,
                          se->position(), heldButtonFlags() | wflags);
            float factor = (se->data().y > 0) ? 1.1f : (1.0f / 1.1f);
            zoomAt(states[name], toLocal(vp, se->position()), factor);
            activeWindow = name;
        }

        // Left-press starts a drag on the window under the cursor.
        for (auto pe : pressLeftEvents) {
            auto [name, vp] = windowAt(layout, pe->position());
            if (name.empty() || !states.count(name)) continue;
            sLButton = true;
            dispatchMouse(name, vp, states, EVENT_LBUTTONDOWN, pe->position(), heldButtonFlags());
            states[name].isDragging = true;
            activeWindow = name;
        }
        for (auto re : releaseLeftEvents) {
            sLButton = false;
            auto [name, vp] = windowAt(layout, re->position());
            if (name.empty() || !states.count(name)) continue;
            dispatchMouse(name, vp, states, EVENT_LBUTTONUP, re->position(), heldButtonFlags());
            states[name].isDragging = false;
            activeWindow = name;
        }

        // Left-drag: pan. Prefer the window under the cursor; fall back to any
        // window that is being dragged (in case the cursor left its bounds).
        for (auto de : dragEvents) {
            bool handled = false;
            auto [name, vp] = windowAt(layout, de->position());
            if (!name.empty() && states.count(name) && states[name].isDragging) {
                states[name].pan.x += (float)de->data().x;
                states[name].pan.y += (float)de->data().y;
                activeWindow = name;
                handled = true;
            } else {
                for (auto& [n, st] : states) {
                    if (st.isDragging) {
                        st.pan.x += (float)de->data().x;
                        st.pan.y += (float)de->data().y;
                        activeWindow = n;
                        break;
                    }
                }
            }
        }

        // Right-click: deliver EVENT_RBUTTONDOWN, then queue the context menu.
        // Zoom is no longer reset (Qt parity).
        for (auto re : pressRightEvents) {
            auto [name, vp] = windowAt(layout, re->position());
            if (name.empty() || !states.count(name)) continue;
            sRButton = true;
            dispatchMouse(name, vp, states, EVENT_RBUTTONDOWN, re->position(), heldButtonFlags());
            activeWindow = name;
            states[name].showContextMenu = true;
        }
        for (auto re : releaseRightEvents) {
            sRButton = false;
            auto [name, vp] = windowAt(layout, re->position());
            if (name.empty() || !states.count(name)) continue;
            dispatchMouse(name, vp, states, EVENT_RBUTTONUP, re->position(), heldButtonFlags());
        }

        // Middle-click: deliver EVENT_MBUTTONDOWN, then jump to deep zoom.
        for (auto me : pressMiddleEvents) {
            auto [name, vp] = windowAt(layout, me->position());
            if (name.empty() || !states.count(name)) continue;
            sMButton = true;
            dispatchMouse(name, vp, states, EVENT_MBUTTONDOWN, me->position(), heldButtonFlags());
            zoomRegion(states[name], vp.size());
            activeWindow = name;
        }
        for (auto me : releaseMiddleEvents) {
            sMButton = false;
            auto [name, vp] = windowAt(layout, me->position());
            if (name.empty() || !states.count(name)) continue;
            dispatchMouse(name, vp, states, EVENT_MBUTTONUP, me->position(), heldButtonFlags());
        }

        // Double-clicks.
        for (auto de : doubleClickLeftEvents) {
            auto [name, vp] = windowAt(layout, de->position());
            if (name.empty() || !states.count(name)) continue;
            dispatchMouse(name, vp, states, EVENT_LBUTTONDBLCLK, de->position(), heldButtonFlags());
        }
        for (auto de : doubleClickRightEvents) {
            auto [name, vp] = windowAt(layout, de->position());
            if (name.empty() || !states.count(name)) continue;
            dispatchMouse(name, vp, states, EVENT_RBUTTONDBLCLK, de->position(), heldButtonFlags());
        }
        for (auto de : doubleClickMiddleEvents) {
            auto [name, vp] = windowAt(layout, de->position());
            if (name.empty() || !states.count(name)) continue;
            dispatchMouse(name, vp, states, EVENT_MBUTTONDBLCLK, de->position(), heldButtonFlags());
        }

        // Cursor position per window (for the status bar / pixel readout).
        for (auto me : moveEvents) {
            for (const auto& [name, vp] : layout) {
                auto it = states.find(name);
                if (it == states.end()) continue;
                if (vp.contains(me->position())) {
                    it->second.mouseInside = true;
                    it->second.mousePos = toLocal(vp, me->position());
                    dispatchMouse(name, vp, states, EVENT_MOUSEMOVE,
                                  me->position(), heldButtonFlags());
                } else {
                    it->second.mouseInside = false;
                }
            }
        }
        for (auto& _ : hoverExitEvents) {
            for (auto& [name, st] : states) {
                st.mouseInside = false;
                st.mousePos = {-1.0f, -1.0f};
            }
        }
    }

    // Combines the currently held mouse buttons and modifier keys into the
    // EVENT_FLAG_* bitmask handed to mouse callbacks (worker thread only).
    static int heldButtonFlags() {
        int flags = 0;
        if (sLButton) flags |= EVENT_FLAG_LBUTTON;
        if (sRButton) flags |= EVENT_FLAG_RBUTTON;
        if (sMButton) flags |= EVENT_FLAG_MBUTTON;
        if (sKeyShift) flags |= EVENT_FLAG_SHIFTKEY;
        if (sKeyCtrl)  flags |= EVENT_FLAG_CTRLKEY;
        if (sKeyAlt)   flags |= EVENT_FLAG_ALTKEY;
        return flags;
    }

    // Invokes the window's mouse callback (if any) with the event mapped to
    // image-pixel coordinates, matching the status-bar readout transform.
    static void dispatchMouse(const std::string& name, const cv::Rect& vp,
                              std::map<std::string, ViewState>& states,
                              int event, const cv::Point& pos, int flags) {
        auto it = states.find(name);
        if (it == states.end()) return;
        auto wd = WindowManager::instance().getWindowShared(name);
        if (!wd) return;
        MouseCallback cb = nullptr;
        void* ud = nullptr;
        {
            std::lock_guard<std::mutex> lock(wd->sink->mtx);
            cb = wd->mouseCb;
            ud = wd->mouseUserdata;
        }
        if (!cb) return;
        const ViewState& st = it->second;
        float invZ = 1.0f / st.zoom;
        int ix = (int)std::floor((pos.x - vp.x - st.pan.x) * invZ);
        int iy = (int)std::floor((pos.y - vp.y - st.pan.y) * invZ);
        cb(event, ix, iy, flags, ud);
    }

    // ---------- View helpers ------------------------------------------------

    static void zoomAt(ViewState& st, const cv::Point2f& anchor, float factor) {
        float worldX = (anchor.x - st.pan.x) / st.zoom;
        float worldY = (anchor.y - st.pan.y) / st.zoom;
        st.zoom *= factor;
        st.zoom = std::clamp(st.zoom, 0.01f, 1000.0f);
        st.pan.x = anchor.x - worldX * st.zoom;
        st.pan.y = anchor.y - worldY * st.zoom;
    }

    static void zoomAround(ViewState& st, const cv::Size& cell, float factor) {
        zoomAt(st, cv::Point2f(cell.width * 0.5f, cell.height * 0.5f), factor);
    }

    static void resetZoom(ViewState& st, const cv::Size& cell) {
        if (st.imageW <= 0 || st.imageH <= 0) return;
        st.zoom = 1.0f;
        st.pan.x = (cell.width  - st.imageW)  / 2.0f;
        st.pan.y = (cell.height - st.imageH) / 2.0f;
    }

    static void zoomRegion(ViewState& st, const cv::Size& cell) {
        float target = ViewState::kDeepZoomThreshold;
        float factor = (target / st.zoom) - 1.0f;
        if (factor != 0.0f) zoomAround(st, cell, 1.0f + factor);
    }

    static void fitToCell(ViewState& st, const cv::Size& cell) {
        if (st.imageW <= 0 || st.imageH <= 0) return;
        float zx = (float)cell.width  / st.imageW;
        float zy = (float)cell.height / st.imageH;
        st.zoom = std::min(zx, zy);
        st.pan.x = (cell.width  - st.imageW  * st.zoom) * 0.5f;
        st.pan.y = (cell.height - st.imageH * st.zoom) * 0.5f;
    }

    // WINDOW_FREERATIO: non-uniform stretch-fill. The letterbox reset comes from
    // renderImage scaling by cell/image per axis at zoom 1, so pan must be 0.
    static void fitStretch(ViewState& st, const cv::Size& cell) {
        if (st.imageW <= 0 || st.imageH <= 0) return;
        st.zoom = 1.0f;
        st.pan.x = 0.0f;
        st.pan.y = 0.0f;
    }

    // ---------- Rendering ---------------------------------------------------

    void drawWindows(const cv::Size& sz, std::map<std::string, ViewState>& states) {
        using namespace cv::v4d::nvg;

        std::uint64_t startGen = WindowManager::instance().generation();
        std::vector<std::string> names = WindowManager::instance().getWindowNames();

        s_winW.store(sz.width);
        s_winH.store(sz.height);

        // FULLSCREEN is a whole-native-window property: if any visible window
        // wants it, apply it. Guarded by the last-set value so setFullscreen is
        // only invoked on an actual transition, not every frame.
        bool fullscreenWanted = false;

        // Release NanoVG textures of windows removed since the last frame.
        // reconcileStates queued the handles; they are only deleted here while
        // the nvg context is current and the previous frame's batch is flushed.
        for (int h : orphanedTextures_) if (h > 0) deleteImage(h);
        orphanedTextures_.clear();

        int n = (int)names.size();
        if (n <= 0) {
            // No windows: record an empty, settled draw so any pending capture
            // can be satisfied with an empty/cleared framebuffer.
            s_frameDrawStartGen.store(startGen);
            s_frameDrawEndGen.store(startGen);
            notifySettledFrame(startGen);
            return;
        }

        const bool verbose = !s_headless.load();
        auto layout = computeLayout(sz);

        for (auto& [name, vp] : layout) {
            auto stIt = states.find(name);
            if (stIt == states.end()) continue;
            ViewState& st = stIt->second;
            if (vp.width <= 0 || vp.height <= 0) continue;

            auto wd = WindowManager::instance().getWindowShared(name);
            if (!wd) continue;
            std::string title;
            bool winFullscreen = false;
            {
                std::lock_guard<std::mutex> lock(wd->sink->mtx);
                title = wd->title;
                st.keepRatio = wd->propKeepRatio;
                st.statusText = wd->statusMsg.active() ? wd->statusMsg.text : std::string();
                st.overlayText = wd->overlayMsg.active() ? wd->overlayMsg.text : std::string();
                winFullscreen = wd->propFullscreen != 0;
            }
            fullscreenWanted = fullscreenWanted || winFullscreen;
            const std::uint64_t serial = wd->contentSerial.load(std::memory_order_relaxed);

            // Unchanged content already uploaded: re-render from the existing
            // texture / BGRA copy instead of re-encoding and re-uploading.
            if (st.imageHandle > 0 && serial == st.uploadedSerial) {
                renderWindowContent(st, vp, verbose, title);
                continue;
            }
            // Content already rejected as unsupported: skip until it changes.
            if (serial == st.badSerial || serial == ViewState::kNeverUploaded) {
                continue;
            }

            cv::UMat img = wd->sink->frame();
            if (img.empty()) {
                st.badSerial = serial;
                continue;
            }

            cv::UMat rgba8;
            try {
                if (!prepareRgba(img, rgba8)) {
                    // One bad window must not kill the shared render engine: skip
                    // it and let the caller see the warning instead.
                    st.badSerial = serial;
                    CV_LOG_WARNING(nullptr, "lowgui: skipping window '" << name
                        << "' with unsupported image format for imshow");
                    continue;
                }
            } catch (const std::exception& ex) {
                st.badSerial = serial;
                CV_LOG_WARNING(nullptr, "lowgui: skipping window '" << name
                    << "' with unrenderable image: " << ex.what());
                continue;
            }

            // Refresh the BGRA copy used by the status bar / pixel readout.
            cv::Mat host = rgba8.getMat(cv::ACCESS_READ);
            st.bgra8 = host.clone();
            cv::cvtColor(st.bgra8, st.bgra8, cv::COLOR_RGBA2BGRA);

            int origCh = img.channels();
            if (st.imageW != rgba8.cols || st.imageH != rgba8.rows || st.channels != origCh) {
                st.imageW = rgba8.cols;
                st.imageH = rgba8.rows;
                st.channels = origCh;
                if (st.keepRatio) fitToCell(st, vp.size());
                else fitStretch(st, vp.size());
            }

            // Delete the previous frame's handle (its batch was flushed at the
            // end of the previous nvg node) before creating the new one.
            if (st.imageHandle > 0) {
                deleteImage(st.imageHandle);
                st.imageHandle = -1;
            }
            cv::Mat hostCopy = host.clone();
            int handle = createImageRGBA(rgba8.cols, rgba8.rows, NVG_IMAGE_NEAREST, hostCopy.data);
            if (handle <= 0) continue;
            st.imageHandle = handle;
            st.rgbaCopy = hostCopy;
            st.uploadedSerial = serial;
            st.badSerial = ViewState::kNeverUploaded;

            renderWindowContent(st, vp, verbose, title);
        }

        // Apply FULLSCREEN on the worker (nvg node) thread only on a
        // transition. V4D::set is the direct property setter (not the
        // infer()-only transactional `set`); it triggers the framebuffer
        // resize callback registered for the key.
        if (fullscreenWanted != lastFullscreen_) {
            lastFullscreen_ = fullscreenWanted;
            V4D::set(V4D::Keys::FULLSCREEN, fullscreenWanted);
        }

        s_frameDrawEndGen.store(WindowManager::instance().generation());
        s_frameDrawStartGen.store(startGen);
        if (s_frameDrawStartGen.load() == s_frameDrawEndGen.load()) {
            notifySettledFrame(s_frameDrawEndGen.load());
        }
    }

    static void renderWindowContent(ViewState& st, const cv::Rect& vp,
                                    bool verbose, const std::string& title) {
        using namespace cv::v4d::nvg;
        save();
        scissor(vp.x, vp.y, vp.width, vp.height);
        translate((float)vp.x, (float)vp.y);
        renderImage(st, vp.size(), verbose);
        restore();
        if (verbose && st.showStatusBar) {
            save();
            scissor(vp.x, vp.y, vp.width, vp.height);
            translate((float)vp.x, (float)vp.y);
            renderStatusBar(st, vp.size(), title);
            restore();
        }
        // Transient displayOverlay text: a floating read-out centered over the
        // image cell, Mirrored on the headless path so captures can verify it.
        if (!st.overlayText.empty()) {
            save();
            scissor(vp.x, vp.y, vp.width, vp.height);
            translate((float)vp.x, (float)vp.y);
            renderOverlayMessage(st, vp.size());
            restore();
        }
    }

    static void renderOverlayMessage(ViewState& st, const cv::Size& cell) {
        using namespace cv::v4d::nvg;
        std::string txt = st.overlayText;
        if (txt.empty()) return;
        // Qt darkens the image edge and draws the message center-bottom.
        float fs = 22.0f;
        fontSize(fs);
        fontFace("sans-bold");
        fillColor(cv::Scalar(255, 255, 0, 255));
        textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        float tx = (float)cell.width * 0.5f;
        float ty = (float)cell.height * 0.5f;
        text(tx, ty, txt.c_str(), txt.c_str() + txt.size());
    }

    static void renderImage(ViewState& st, const cv::Size& cell, bool verbose) {
        using namespace cv::v4d::nvg;
        if (st.imageHandle <= 0 || st.imageW <= 0 || st.imageH <= 0) return;

        save();
        translate(st.pan.x, st.pan.y);
        if (st.keepRatio) {
            scale(st.zoom, st.zoom);
        } else {
            // WINDOW_FREERATIO: non-uniform stretch-fill of the cell (x zoom).
            scale(st.zoom * (float)cell.width  / (float)st.imageW,
                  st.zoom * (float)cell.height / (float)st.imageH);
        }

        beginPath();
        rect(0.0f, 0.0f, (float)st.imageW, (float)st.imageH);
        fillPaint(imagePattern(0.0f, 0.0f, (float)st.imageW, (float)st.imageH,
                               0.0f, st.imageHandle, 1.0f));
        fill();

        // Snapping pixel-grid and the deep-zoom RGB readout assume uniform
        // KEEPRATIO scaling; never draw them under FREERATIO.
        if (verbose && st.keepRatio && st.zoom >= 8.0f && st.zoom < ViewState::kDeepZoomThreshold) {
            float gridAlpha = std::min(1.0f, (st.zoom - 8.0f) / 8.0f);
            strokeColor(cv::Scalar(128, 128, 128, (int)(gridAlpha * 255)));
            strokeWidth(1.0f / st.zoom);
            beginPath();
            for (int x = 0; x <= st.imageW; ++x) {
                moveTo((float)x, 0.0f);
                lineTo((float)x, (float)st.imageH);
            }
            stroke();
            beginPath();
            for (int y = 0; y <= st.imageH; ++y) {
                moveTo(0.0f, (float)y);
                lineTo((float)st.imageW, (float)y);
            }
            stroke();
        }

        restore();
        if (verbose && st.keepRatio) drawDeepZoomOverlay(st, cell);
    }

    static void drawDeepZoomOverlay(ViewState& st, const cv::Size& cell) {
        using namespace cv::v4d::nvg;
        if (!(st.zoom >= ViewState::kDeepZoomThreshold &&
              st.channels >= 1 && st.channels <= 4)) {
            return;
        }
        if (st.bgra8.empty()) return;

        float pixelW = st.zoom;
        float pixelH = st.zoom;

        int imgX0 = std::max(-1, (int)std::floor(-st.pan.x / pixelW) - 1);
        int imgY0 = std::max(-1, (int)std::floor(-st.pan.y / pixelH) - 1);
        int imgX1 = std::min(st.imageW,
            (int)std::ceil((cell.width  - st.pan.x) / pixelW) + 1);
        int imgY1 = std::min(st.imageH,
            (int)std::ceil((cell.height - st.pan.y) / pixelH) + 1);

        float fs = 10.0f + (pixelH - ViewState::kDeepZoomThreshold) / 5.0f;
        fs = std::clamp(fs, 6.0f, 48.0f);
        fontSize(fs);
        fontFace("sans-bold");
        textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

        if (st.channels == 3 || st.channels == 4) {
            for (int imgY = imgY0; imgY < imgY1; ++imgY) {
                for (int imgX = imgX0; imgX < imgX1; ++imgX) {
                    if (imgX < 0 || imgY < 0 ||
                        imgX >= st.imageW || imgY >= st.imageH)
                        continue;
                    const uchar* p = st.bgra8.ptr(imgY, imgX);
                    int b = p[0], g = p[1], r = p[2];
                    char buf[8];
                    float px = st.pan.x + (imgX + 0.5f) * pixelW;
                    float pyR = st.pan.y + (imgY + 1.0f/6.0f) * pixelH;
                    float pyG = st.pan.y + (imgY + 0.5f)      * pixelH;
                    float pyB = st.pan.y + (imgY + 5.0f/6.0f) * pixelH;
                    std::snprintf(buf, sizeof(buf), "%d", r);
                    fillColor(cv::Scalar(255, 0,   0,   255));
                    text(px, pyR, buf, buf + std::strlen(buf));
                    std::snprintf(buf, sizeof(buf), "%d", g);
                    fillColor(cv::Scalar(0,   255, 0,   255));
                    text(px, pyG, buf, buf + std::strlen(buf));
                    std::snprintf(buf, sizeof(buf), "%d", b);
                    fillColor(cv::Scalar(255, 255, 255, 255));
                    text(px, pyB, buf, buf + std::strlen(buf));
                }
            }
        } else if (st.channels == 1) {
            for (int imgY = imgY0; imgY < imgY1; ++imgY) {
                for (int imgX = imgX0; imgX < imgX1; ++imgX) {
                    if (imgX < 0 || imgY < 0 ||
                        imgX >= st.imageW || imgY >= st.imageH)
                        continue;
                    const uchar* p = st.bgra8.ptr(imgY, imgX);
                    int v = p[0];
                    int tv = (v > 127) ? (v - 127) : (127 + v);
                    char buf[8];
                    float px = st.pan.x + (imgX + 0.5f) * pixelW;
                    float py = st.pan.y + (imgY + 0.5f) * pixelH;
                    std::snprintf(buf, sizeof(buf), "%d", v);
                    fillColor(cv::Scalar(tv, tv, tv, 255));
                    text(px, py, buf, buf + std::strlen(buf));
                }
            }
        }

        strokeColor(cv::Scalar(0, 0, 0, 180));
        strokeWidth(1.0f);
        beginPath();
        for (int imgX = imgX0; imgX <= imgX1; ++imgX) {
            float sx = st.pan.x + imgX * pixelW;
            moveTo(sx, st.pan.y);
            lineTo(sx, st.pan.y + st.imageH * pixelH);
        }
        stroke();
        beginPath();
        for (int imgY = imgY0; imgY <= imgY1; ++imgY) {
            float sy = st.pan.y + imgY * pixelH;
            moveTo(st.pan.x, sy);
            lineTo(st.pan.x + st.imageW * pixelW, sy);
        }
        stroke();
    }

    static void renderStatusBar(ViewState& st, const cv::Size& cell,
                                const std::string& title) {
        using namespace cv::v4d::nvg;
        std::ostringstream oss;
        if (!title.empty()) {
            oss << title.c_str();
        } else {
            oss << st.label.c_str();
        }
        if (st.mouseInside) {
            float invZ = 1.0f / st.zoom;
            int ix = (int)std::floor((st.mousePos.x - st.pan.x) * invZ);
            int iy = (int)std::floor((st.mousePos.y - st.pan.y) * invZ);
            if (ix >= 0 && iy >= 0 &&
                ix < st.imageW && iy < st.imageH && !st.bgra8.empty()) {
                const uchar* p = st.bgra8.ptr(iy, ix);
                oss << "   |   (x=" << ix << ", y=" << iy << ")";
                if (st.channels == 1) {
                    oss << "   L:" << (int)p[0];
                } else {
                    oss << "   R:" << (int)p[2]
                        << " G:" << (int)p[1]
                        << " B:" << (int)p[0];
                    if (st.channels == 4)
                        oss << " A:" << (int)p[3];
                }
            } else {
                oss << "   |   (x=-, y=-)";
            }
        } else {
            oss << "   |   (x=-, y=-)";
        }
        oss << "   |   " << st.imageW << "x" << st.imageH
            << "   |   zoom: " << (int)(st.zoom * 100.0f) << "%";

        if (!st.statusText.empty()) {
            oss << "   |   " << st.statusText;
        }

        float barH = 28.0f;
        float yTop = (float)cell.height - barH;
        beginPath();
        rect(0.0f, yTop, (float)cell.width, barH);
        fillColor(cv::Scalar(20, 20, 30, 230));
        fill();

        beginPath();
        rect(0.0f, yTop, (float)cell.width, 1.0f);
        fillColor(cv::Scalar(255, 255, 255, 120));
        fill();

        fontSize(15.0f);
        fontFace("sans-bold");
        fillColor(cv::Scalar(230, 230, 230, 255));
        textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        std::string txt = oss.str();
        text(10.0f, yTop + barH * 0.5f, txt.c_str(), txt.c_str() + txt.size());
    }

    // Mirrors highgui's imshow depth handling: 8S +128, 16U>>8, 16S(+128)>>8,
    // 32F/64F * 255 (saturated). 32S is rejected like highgui does. Only 1/3/4
    // channels are renderable; anything else is rejected up front so the shared
    // render engine is never torn down by cvtColor throwing on an unknown code.
    static bool prepareRgba(const cv::UMat& src, cv::UMat& rgba8) {
        if (src.depth() != CV_8U) {
            double alpha = 1.0, beta = 0.0;
            switch (src.depth()) {
                case CV_16U: alpha = 1.0 / 256.0; break;
                case CV_8S:  beta = 128.0; break;
                case CV_16S: alpha = 1.0 / 256.0; beta = 128.0; break;
                case CV_32F:
                case CV_64F: alpha = 255.0; break;
                default: return false;
            }
            cv::UMat u8;
            src.convertTo(u8, CV_8U, alpha, beta);
            cv::Mat cpu = u8.getMat(cv::ACCESS_READ);
            return convertToRgba(cpu, rgba8);
        } else {
            cv::Mat cpu = src.getMat(cv::ACCESS_READ);
            return convertToRgba(cpu, rgba8);
        }
    }

    static bool convertToRgba(const cv::Mat& cpu, cv::UMat& rgba8) {
        int code = colorCode(cpu.channels());
        if (code < 0) return false;
        cv::cvtColor(cpu, rgba8, code);
        return !rgba8.empty();
    }

    static int colorCode(int channels) {
        switch (channels) {
            case 1: return cv::COLOR_GRAY2RGBA;
            case 3: return cv::COLOR_BGR2RGBA;
            case 4: return cv::COLOR_BGRA2RGBA;
            default: return -1;
        }
    }

    // ---------- ImGui helpers ----------------------------------------------

    // Qt's 11-actions context menu is disabled for GUI_NORMAL windows (and the
    // menu bar / help overlay / status bar too, see step 7).
    static bool windowGuiNormal(const std::string& name) {
        auto wd = WindowManager::instance().getWindowShared(name);
        if (!wd) return false;
        // `flags` is set at WindowData construction and never mutated, so no
        // lock is required to read it safely here.
        return (wd->flags & WINDOW_GUI_NORMAL) != 0;
    }

    // Renders one Qt-style SliderInt strip for a window's per-window trackbars,
    // positioned directly above the 28px status bar (imshow parity). Collapses
    // to a popup trigger button when the cell is too narrow for sliders.
    static void drawTrackbarStrip(const std::string& winname, const cv::Rect& vp) {
        using namespace ImGui;
        auto bars = WindowManager::instance().getWindowTrackbars(winname);
        if (bars.empty()) return;

        const float kStatusBarH = 28.0f;
        const float kStripH = 30.0f;
        if (vp.width < 240) {
            // Narrow cell: one small button opens a popup listing the sliders.
            SetNextWindowPos(ImVec2((float)vp.x, (float)(vp.y + vp.height - kStatusBarH - kStripH)),
                             ImGuiCond_Always);
            SetNextWindowSize(ImVec2((float)vp.width, kStripH), ImGuiCond_Always);
            Begin((std::string("##tbstrip.") + winname).c_str(), nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
            if (ImGui::Button("Trackbars...")) {
                OpenPopup(("##tbstrip_popup." + winname).c_str());
            }
            if (BeginPopup(("##tbstrip_popup." + winname).c_str())) {
                for (auto& b : bars) {
                    int pos = b.pos;
                    if (SliderInt((b.name + "##tbstrip." + winname).c_str(), &pos,
                                  b.min, b.max)) {
                        WindowManager::instance().updateTrackbarPos(b.name, winname, pos);
                    }
                }
                EndPopup();
            }
            End();
            return;
        }

        SetNextWindowPos(ImVec2((float)vp.x, (float)(vp.y + vp.height - kStatusBarH - kStripH)),
                         ImGuiCond_Always);
        SetNextWindowSize(ImVec2((float)vp.width, kStripH), ImGuiCond_Always);
        Begin((std::string("##tbstrip.") + winname).c_str(), nullptr,
              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        PushID(winname.c_str());
        float spacing = GetStyle().ItemSpacing.x;
        float avail = GetContentRegionAvail().x;
        float perBar = std::max(120.0f, (avail - spacing * (float)(bars.size() - 1)) / (float)bars.size());
        for (size_t i = 0; i < bars.size(); ++i) {
            const auto& b = bars[i];
            int pos = b.pos;
            SetNextItemWidth(perBar);
            if (SliderInt((b.name + "##tbstrip").c_str(), &pos, b.min, b.max)) {
                WindowManager::instance().updateTrackbarPos(b.name, winname, pos);
            }
            if (i + 1 < bars.size()) SameLine();
        }
        PopID();
        End();
    }

    // The "…settings" control panel: global (control-panel) trackbars and
    // buttons with Qt semantics (push -> cb(-1,ud); check/radio -> 0/1).
    static void drawControlPanel() {
        using namespace ImGui;
        auto& wm = WindowManager::instance();
        std::vector<Trackbar> bars = wm.getControlTrackbars();
        std::vector<Button> btns = wm.getControlButtons();
        if (bars.empty() && btns.empty()) return;

        SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Appearing);
        Begin("lowgui settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        for (auto& b : bars) {
            int pos = b.pos;
            if (SliderInt((b.name + "##panel").c_str(), &pos, b.min, b.max)) {
                wm.updateTrackbarPos(b.name, "", pos);
            }
        }
        if (!bars.empty() && !btns.empty()) Separator();

        for (const auto& btn : btns) {
            int type = btn.type & ~QT_NEW_BUTTONBAR;
            if (type == QT_PUSH_BUTTON) {
                if (ImGui::Button(btn.name.c_str())) {
                    wm.setButtonState(btn.name, -1);
                    if (btn.cb) btn.cb(-1, btn.userdata);
                }
            } else if (type == QT_CHECKBOX) {
                bool checked = btn.state == 1;
                if (Checkbox(btn.name.c_str(), &checked)) {
                    int s = checked ? 1 : 0;
                    wm.setButtonState(btn.name, s);
                    if (btn.cb) btn.cb(s, btn.userdata);
                }
            } else if (type == QT_RADIOBOX) {
                // Exclusivity is enforced within a buttonbar (same barId): all
                // siblings go to 0, the selected one to 1; each changed button
                // fires its callback.
                bool selected = btn.state == 1;
                if (RadioButton(btn.name.c_str(), selected)) {
                    for (const auto& other : btns) {
                        if (&other == &btn) continue;
                        if ((other.type & ~QT_NEW_BUTTONBAR) != QT_RADIOBOX) continue;
                        if (other.barId != btn.barId) continue;
                        if (other.state != 0) {
                            wm.setButtonState(other.name, 0);
                            if (other.cb) other.cb(0, other.userdata);
                        }
                    }
                    wm.setButtonState(btn.name, 1);
                    if (btn.cb) btn.cb(1, btn.userdata);
                }
            }
        }
        End();
    }

    // Best-effort copy of the window's current source image to the X clipboard
    // via xclip on Linux. Shared by the Ctrl+C shortcut and the context menu
    // so they stay identical. The PNG is encoded in memory and written straight
    // to xclip's stdin: no predictable temp file (avoids the symlink race) and
    // no blocking disk round-trip on the ImGui thread for large images.
    static void copyWindowToClipboard(ViewState& st, const std::string& winname) {
        auto wd = WindowManager::instance().getWindowShared(winname);
        cv::UMat um;
        if (wd) um = wd->sink->frame();
        if (um.empty()) {
            st.lastImageSaveOk = false;
            st.lastImageSaveMsg = "No image to copy.";
            return;
        }
        cv::Mat src = um.getMat(cv::ACCESS_READ);
        std::vector<uchar> png;
        if (!cv::imencode(".png", src, png)) {
            st.lastImageSaveOk = false;
            st.lastImageSaveMsg = "Failed to encode image for clipboard.";
            return;
        }
        FILE* p = ::popen("xclip -selection clipboard -t image/png", "w");
        if (!p) {
            st.lastImageSaveOk = false;
            st.lastImageSaveMsg = "Failed to copy image to clipboard (xclip missing?).";
            return;
        }
        size_t written = std::fwrite(png.data(), 1, png.size(), p);
        int rc = ::pclose(p);
        if (written != png.size() || rc != 0) {
            st.lastImageSaveOk = false;
            st.lastImageSaveMsg = "Failed to copy image to clipboard (xclip error).";
            return;
        }
        st.lastImageSaveOk = true;
        st.lastImageSaveMsg = "Copied image to clipboard.";
    }

    static bool isImageFile(const std::string& name) {
        std::error_code ec;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        size_t dot = lower.rfind('.');
        if (dot == std::string::npos) return false;
        std::string ext = lower.substr(dot);
        for (const char* e : ViewState::kImageExts) {
            if (ext == e) return true;
        }
        return false;
    }

    static void refreshFileDialogEntries(ViewState& st) {
        st.fileDialogEntries.clear();
        st.fileDialogErrorMsg.clear();
        std::error_code ec;
        std::filesystem::path dir(st.fileDialogPath);
        if (!std::filesystem::is_directory(dir, ec)) {
            st.fileDialogErrorMsg = ec ? ec.message()
                                       : ("Not a directory: " + std::string(st.fileDialogPath));
            return;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) {
                st.fileDialogErrorMsg = ec.message();
                break;
            }
            std::string name = entry.path().filename().string();
            if (entry.is_directory(ec)) {
                st.fileDialogEntries.emplace_back(name, true);
            } else if (isImageFile(name)) {
                st.fileDialogEntries.emplace_back(name, false);
            }
        }
        std::sort(st.fileDialogEntries.begin(), st.fileDialogEntries.end(),
            [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });
    }

    static void openFileDialogSelection(ViewState& st) {
        if (st.fileDialogSelected[0] == '\0') return;
        std::error_code ec;
        std::filesystem::path full =
            std::filesystem::path(st.fileDialogPath) / st.fileDialogSelected;
        if (std::filesystem::is_regular_file(full, ec)) {
            st.newFilename = full.string();
            st.reloadRequested = true;
        }
    }

    static void guiBody(std::map<std::string, ViewState>& states,
                        std::string& activeWindow,
                        const cv::Size& sz) {
        using namespace ImGui;
        if (states.empty()) return;
        if (states.count(activeWindow) == 0) activeWindow = states.begin()->first;

        ViewState& st = states[activeWindow];
        cv::Rect vp = viewportFor(activeWindow, sz);
        if (vp.width <= 0 || vp.height <= 0) return;

        ImFont* font = GetFont();
        font->Scale = 1.5f;
        PushFont(font);

        auto panFrac = [&](float dx, float dy) {
            st.pan.x += dx * vp.width;
            st.pan.y += dy * vp.height;
        };

        // ---------- Keyboard shortcuts ----------
        if (IsKeyDown(ImGuiKey_LeftCtrl) || IsKeyDown(ImGuiKey_RightCtrl)) {
            if (IsKeyPressed(ImGuiKey_LeftArrow))  panFrac( 0.05f, 0.0f);
            if (IsKeyPressed(ImGuiKey_RightArrow)) panFrac(-0.05f, 0.0f);
            if (IsKeyPressed(ImGuiKey_UpArrow))    panFrac(0.0f,  0.05f);
            if (IsKeyPressed(ImGuiKey_DownArrow))  panFrac(0.0f, -0.05f);
            if (IsKeyPressed(ImGuiKey_Equal) || IsKeyPressed(ImGuiKey_KeypadAdd))
                zoomAround(st, vp.size(), 1.5f);
            if (IsKeyPressed(ImGuiKey_Minus) || IsKeyPressed(ImGuiKey_KeypadSubtract))
                zoomAround(st, vp.size(), 1.0f / 1.5f);
            if (IsKeyPressed(ImGuiKey_0) || IsKeyPressed(ImGuiKey_Keypad0))
                resetZoom(st, vp.size());
            if (IsKeyPressed(ImGuiKey_Z)) resetZoom(st, vp.size());
            // Ctrl+P toggles the control panel (Qt: "Display properties window").
            if (IsKeyPressed(ImGuiKey_P)) {
                st.showControlPanel = !st.showControlPanel;
            }
            if (IsKeyPressed(ImGuiKey_F)) {
                if (st.keepRatio) fitToCell(st, vp.size());
                else fitStretch(st, vp.size());
            }
            if (IsKeyPressed(ImGuiKey_X)) zoomRegion(st, vp.size());

            bool saveViewShortcut = (IsKeyDown(ImGuiKey_LeftShift) ||
                                     IsKeyDown(ImGuiKey_RightShift)) &&
                                    IsKeyPressed(ImGuiKey_S);
            if (saveViewShortcut) {
                std::snprintf(st.saveBuf, sizeof(st.saveBuf), "%s", st.label.c_str());
                st.showSaveViewDialog = true;
            } else if (IsKeyPressed(ImGuiKey_S)) {
                std::snprintf(st.saveBuf, sizeof(st.saveBuf), "%s", st.label.c_str());
                st.showSaveDialog = true;
            }
            if (IsKeyPressed(ImGuiKey_O)) {
                st.showFileDialog = true;
                st.fileDialogSelectedIdx = -1;
                st.fileDialogSelected[0] = '\0';
                refreshFileDialogEntries(st);
            }
            if (IsKeyPressed(ImGuiKey_C)) {
                // Copy the source image of the active window to clipboard via
                // xclip on Linux (shared with the context menu).
                copyWindowToClipboard(st, activeWindow);
            }
        }

        // ESC closes dialogs / overlays.
        if (IsKeyPressed(ImGuiKey_Escape)) {
            if (st.showProperties)         st.showProperties         = false;
            else if (st.showSaveDialog)    st.showSaveDialog         = false;
            else if (st.showSaveViewDialog)st.showSaveViewDialog     = false;
            else if (st.showFileDialog)    st.showFileDialog         = false;
            else if (st.showControlPanel)  st.showControlPanel       = false;
            else if (st.showHelp)          st.showHelp               = false;
        }

        // ---------- Main menu bar ----------
        if (BeginMainMenuBar()) {
            if (BeginMenu("File")) {
                if (MenuItem("Open image...", "Ctrl+O")) {
                    st.showFileDialog = true;
                    st.fileDialogSelectedIdx = -1;
                    st.fileDialogSelected[0] = '\0';
                    refreshFileDialogEntries(st);
                }
                if (MenuItem("Save image as...", "Ctrl+S")) {
                    std::snprintf(st.saveBuf, sizeof(st.saveBuf), "%s", st.label.c_str());
                    st.showSaveDialog = true;
                }
                if (MenuItem("Save view as...", "Ctrl+Shift+S")) {
                    std::snprintf(st.saveBuf, sizeof(st.saveBuf), "%s", st.label.c_str());
                    st.showSaveViewDialog = true;
                }
                Separator();
                if (MenuItem("Quit", "Alt+F4")) {
                    cv::v4d::request_finish();
                }
                EndMenu();
            }
            if (BeginMenu("View")) {
                if (MenuItem("Zoom x1", "Ctrl+0 | Ctrl+Z")) resetZoom(st, vp.size());
                if (MenuItem("Zoom x30 (deep zoom)", "Ctrl+X")) zoomRegion(st, vp.size());
                Separator();
                if (MenuItem("Zoom in",  "Ctrl++"))  zoomAround(st, vp.size(), 1.5f);
                if (MenuItem("Zoom out", "Ctrl+-"))  zoomAround(st, vp.size(), 1.0f / 1.5f);
                if (MenuItem("Fit to window", "Ctrl+F")) {
                    if (st.keepRatio) fitToCell(st, vp.size());
                    else fitStretch(st, vp.size());
                }
                Separator();
                Checkbox("Status bar",        &st.showStatusBar);
                Checkbox("Show help overlay", &st.showHelp);
                Separator();
                MenuItem("Properties...", nullptr, &st.showProperties);
                bool panelContent = WindowManager::instance().hasControlPanelContent();
                if (MenuItem("Display control panel", "Ctrl+P", &st.showControlPanel, panelContent)) {
                    // toggle happened inside MenuItem
                }
                EndMenu();
            }
            if (BeginMenu("Navigate")) {
                if (MenuItem("Pan left",  "Ctrl+Left"))  panFrac( 0.05f, 0.0f);
                if (MenuItem("Pan right", "Ctrl+Right")) panFrac(-0.05f, 0.0f);
                if (MenuItem("Pan up",    "Ctrl+Up"))    panFrac(0.0f,  0.05f);
                if (MenuItem("Pan down",  "Ctrl+Down"))  panFrac(0.0f, -0.05f);
                EndMenu();
            }
            if (BeginMenu("Help")) {
                if (MenuItem("Show controls")) st.showHelp = !st.showHelp;
                EndMenu();
            }
            EndMainMenuBar();
        }

        // ---------- Right-click context menu (Qt parity) ----------
        // The worker queues showContextMenu on right-click and sets
        // activeWindow. Consume it every frame regardless so it never lingers;
        // GUI_NORMAL windows get no context menu (Qt same).
        if (st.showContextMenu) {
            st.showContextMenu = false;
            if (!windowGuiNormal(activeWindow)) {
                OpenPopup("lowgui_context_menu");
            }
        }
        if (BeginPopup("lowgui_context_menu")) {
            if (MenuItem("Panning left",    "Ctrl+Left"))  panFrac( 0.05f, 0.0f);
            if (MenuItem("Panning right",   "Ctrl+Right")) panFrac(-0.05f, 0.0f);
            if (MenuItem("Panning up",      "Ctrl+Up"))    panFrac(0.0f,  0.05f);
            if (MenuItem("Panning down",    "Ctrl+Down"))  panFrac(0.0f, -0.05f);
            Separator();
            if (MenuItem("Zoom x1", "Ctrl+0 | Ctrl+Z"))  resetZoom(st, vp.size());
            if (MenuItem("Zoom x30 (deep zoom)", "Ctrl+X"))  zoomRegion(st, vp.size());
            if (MenuItem("Zoom in",              "Ctrl++"))  zoomAround(st, vp.size(), 1.5f);
            if (MenuItem("Zoom out",             "Ctrl+-"))  zoomAround(st, vp.size(), 1.0f / 1.5f);
            Separator();
            if (MenuItem("Save image...", "Ctrl+S")) {
                std::snprintf(st.saveBuf, sizeof(st.saveBuf), "%s", st.label.c_str());
                st.showSaveDialog = true;
            }
            if (MenuItem("Copy image to clipboard", "Ctrl+C")) {
                copyWindowToClipboard(st, activeWindow);
            }
            Separator();
            bool panelContent = WindowManager::instance().hasControlPanelContent();
            if (MenuItem("Display properties window", "Ctrl+P", false, panelContent)) {
                st.showControlPanel = true;
            }
            EndPopup();
        }

        // ---------- File browser dialog (Open image...) ----------
        if (st.showFileDialog) {
            SetNextWindowSize(ImVec2(560, 420), ImGuiCond_Appearing);
            Begin("Open image", &st.showFileDialog);
            Text("Current directory: %s", st.fileDialogPath);
            SameLine();
            if (!st.fileDialogErrorMsg.empty()) {
                TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                            "Error: %s", st.fileDialogErrorMsg.c_str());
                if (ImGui::Button("Clear error")) st.fileDialogErrorMsg.clear();
                SameLine();
            }
            if (ImGui::Button("Up")) {
                std::error_code ec;
                std::filesystem::path p(st.fileDialogPath);
                if (p.has_parent_path()) {
                    p = p.parent_path();
                    if (std::filesystem::is_directory(p, ec)) {
                        std::snprintf(st.fileDialogPath, sizeof(st.fileDialogPath),
                                      "%s", p.string().c_str());
                        st.fileDialogSelectedIdx = -1;
                        st.fileDialogSelected[0] = '\0';
                        refreshFileDialogEntries(st);
                    }
                }
            }
            Separator();

            float entryH = GetTextLineHeightWithSpacing() + 4.0f;
            float listH = 10 * entryH;
            if (BeginListBox("##files", ImVec2(-1, listH))) {
                for (int i = 0; i < (int)st.fileDialogEntries.size(); ++i) {
                    const auto& entry = st.fileDialogEntries[i];
                    bool isDir = entry.second;
                    const char* label = isDir ? "[DIR]  " : "       ";
                    std::string display = label + entry.first;
                    if (Selectable(display.c_str(), st.fileDialogSelectedIdx == i)) {
                        st.fileDialogSelectedIdx = i;
                        std::snprintf(st.fileDialogSelected, sizeof(st.fileDialogSelected),
                                      "%s", entry.first.c_str());
                    }
                    if (IsItemHovered() && IsMouseDoubleClicked(0)) {
                        st.fileDialogSelectedIdx = i;
                        std::snprintf(st.fileDialogSelected, sizeof(st.fileDialogSelected),
                                      "%s", entry.first.c_str());
                        if (isDir) {
                            std::error_code ec;
                            std::filesystem::path newPath =
                                std::filesystem::path(st.fileDialogPath) / entry.first;
                            if (std::filesystem::is_directory(newPath, ec)) {
                                std::snprintf(st.fileDialogPath, sizeof(st.fileDialogPath),
                                              "%s", newPath.string().c_str());
                                st.fileDialogSelectedIdx = -1;
                                st.fileDialogSelected[0] = '\0';
                                refreshFileDialogEntries(st);
                            }
                        } else {
                            st.showFileDialog = false;
                            openFileDialogSelection(st);
                        }
                    }
                }
                EndListBox();
            }

            InputText("Selected", st.fileDialogSelected, sizeof(st.fileDialogSelected));
            SetItemDefaultFocus();
            if (IsWindowAppearing()) SetKeyboardFocusHere(-1);

            if (ImGui::Button("Open") && st.fileDialogSelected[0] != '\0') {
                std::error_code ec;
                std::filesystem::path sel(st.fileDialogSelected);
                std::filesystem::path full = std::filesystem::path(st.fileDialogPath) / sel;
                if (std::filesystem::is_directory(full, ec)) {
                    std::snprintf(st.fileDialogPath, sizeof(st.fileDialogPath),
                                  "%s", full.string().c_str());
                    st.fileDialogSelectedIdx = -1;
                    st.fileDialogSelected[0] = '\0';
                    refreshFileDialogEntries(st);
                } else if (isImageFile(sel.string())) {
                    st.showFileDialog = false;
                    openFileDialogSelection(st);
                }
            }
            SameLine();
            if (ImGui::Button("Cancel")) {
                st.showFileDialog = false;
            }
            End();
        }

        // ---------- Save dialog (Save image as...) ----------
        if (st.showSaveDialog) {
            SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
            Begin("Save image as...", &st.showSaveDialog, ImGuiWindowFlags_AlwaysAutoResize);
            Text("Save the original image (without zoom/pan overlays).");
            InputText("Path", st.saveBuf, sizeof(st.saveBuf));
            const char* fmts[] = { ".png", ".jpg", ".bmp" };
            Combo("Format", &st.saveFormat, fmts, IM_ARRAYSIZE(fmts));
            if (!st.lastImageSaveMsg.empty()) {
                if (st.lastImageSaveOk) TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                                                    "%s", st.lastImageSaveMsg.c_str());
                else                    TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                                    "%s", st.lastImageSaveMsg.c_str());
            }
            if (ImGui::Button("Save") && st.saveBuf[0] != '\0') {
                std::string path(st.saveBuf);
                std::string ext = fmts[std::clamp(st.saveFormat, 0, 2)];
                if (path.size() < ext.size() ||
                    path.compare(path.size() - ext.size(), ext.size(), ext) != 0) {
                    path += ext;
                }
                auto wd = WindowManager::instance().getWindowShared(activeWindow);
                cv::UMat um;
                if (wd) um = wd->sink->frame();
                if (um.empty()) {
                    st.lastImageSaveOk = false;
                    st.lastImageSaveMsg = "No image to save";
                } else {
                    cv::Mat src = um.getMat(cv::ACCESS_READ);
                    std::vector<int> params;
                    if (ext == ".jpg") {
                        params.push_back(IMWRITE_JPEG_QUALITY);
                        params.push_back(95);
                    }
                    st.lastImageSaveOk = imwrite(path, src, params);
                    st.lastImageSaveMsg = st.lastImageSaveOk
                        ? ("Saved to " + path)
                        : ("Failed to save to " + path);
                }
            }
            SameLine();
            if (ImGui::Button("Cancel")) {
                st.showSaveDialog = false;
                st.lastImageSaveMsg.clear();
            }
            End();
        }

        // ---------- Save view dialog (saves the rendered viewport) ----------
        if (st.showSaveViewDialog) {
            SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
            Begin("Save view as...", &st.showSaveViewDialog, ImGuiWindowFlags_AlwaysAutoResize);
            Text("Save the currently rendered viewport as an image.");
            InputText("Path", st.saveBuf, sizeof(st.saveBuf));
            const char* fmts[] = { ".png", ".jpg", ".bmp" };
            Combo("Format", &st.saveFormat, fmts, IM_ARRAYSIZE(fmts));
            if (!st.lastViewSaveMsg.empty()) {
                if (st.lastViewSaveOk) TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                                                   "%s", st.lastViewSaveMsg.c_str());
                else                   TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                                   "%s", st.lastViewSaveMsg.c_str());
            }
            if (ImGui::Button("Save") && st.saveBuf[0] != '\0') {
                std::string path(st.saveBuf);
                std::string ext = fmts[std::clamp(st.saveFormat, 0, 2)];
                if (path.size() < ext.size() ||
                    path.compare(path.size() - ext.size(), ext.size(), ext) != 0) {
                    path += ext;
                }
                // The rendered viewport would need a compositing pass; keep the
                // source image of the active window for a predictable result.
                auto wd = WindowManager::instance().getWindowShared(activeWindow);
                cv::UMat um;
                if (wd) um = wd->sink->frame();
                if (um.empty()) {
                    st.lastViewSaveOk = false;
                    st.lastViewSaveMsg = "No image to save";
                } else {
                    cv::Mat src = um.getMat(cv::ACCESS_READ);
                    std::vector<int> params;
                    if (ext == ".jpg") {
                        params.push_back(IMWRITE_JPEG_QUALITY);
                        params.push_back(95);
                    }
                    st.lastViewSaveOk = imwrite(path, src, params);
                    st.lastViewSaveMsg = st.lastViewSaveOk
                        ? ("Saved to " + path)
                        : ("Failed to save to " + path);
                }
            }
            SameLine();
            if (ImGui::Button("Cancel")) {
                st.showSaveViewDialog = false;
                st.lastViewSaveMsg.clear();
            }
            End();
        }

        // ---------- Properties dialog ----------
        if (st.showProperties) {
            Begin("Image properties", &st.showProperties);
            Text("File:     %s", st.label.c_str());
            Text("Size:     %d x %d", st.imageW, st.imageH);
            Text("Channels: %d", st.channels);
            const char* depth = "unknown";
            if      (st.channels == 1) depth = "8U (grayscale)";
            else if (st.channels == 3) depth = "8U (BGR)";
            else if (st.channels == 4) depth = "8U (BGRA)";
            Text("Depth:    %s", depth);
            Separator();
            Text("Zoom:     %.2f %%", st.zoom * 100.0f);
            Text("Pan:      (%.1f, %.1f)", st.pan.x, st.pan.y);
            Separator();
            Text("Mouse controls:");
            BulletText("Scroll: zoom in/out (around cursor)");
            BulletText("Left-drag: pan");
            BulletText("Right-click: context menu");
            BulletText("Middle-click: zoom to region (deep zoom)");
            Text("Keyboard:");
            BulletText("Ctrl+Arrows: pan by 5%% of viewport");
            BulletText("Ctrl+'+' / Ctrl+'-': zoom in / out");
            BulletText("Ctrl+0 / Ctrl+Z: reset zoom");
            BulletText("Ctrl+F: fit to window");
            BulletText("Ctrl+X: deep zoom (x%.0f)", ViewState::kDeepZoomThreshold);
            BulletText("Ctrl+S: save image, Ctrl+Shift+S: save view");
            BulletText("Ctrl+C: copy image to clipboard (xclip)");
            BulletText("Esc: close dialogs");
            End();
        }

        // ---------- Help overlay ----------
        if (st.showHelp) {
            SetNextWindowPos(ImVec2(10, 40), ImGuiCond_Once);
            SetNextWindowBgAlpha(0.55f);
            Begin("Controls (Esc to hide)", &st.showHelp,
                  ImGuiWindowFlags_NoResize |
                  ImGuiWindowFlags_AlwaysAutoResize |
                  ImGuiWindowFlags_NoTitleBar);
            Text("OpenCV lowgui (Plan-V4D)");
            Separator();
            BulletText("Scroll wheel:       zoom (around cursor)");
            BulletText("Left drag:          pan");
            BulletText("Right click:        context menu");
            BulletText("Middle click:       deep zoom (x%.0f)", ViewState::kDeepZoomThreshold);
            Separator();
            BulletText("Ctrl+Arrows:        pan by 5%% of viewport");
            BulletText("Ctrl+'+'/'-':       zoom in / out");
            BulletText("Ctrl+0 / Ctrl+Z:    reset zoom");
            BulletText("Ctrl+P:             control panel");
            BulletText("Ctrl+F:             fit to window");
            BulletText("Ctrl+X:             deep zoom");
            BulletText("Ctrl+S:             save image as...");
            BulletText("Ctrl+Shift+S:       save view as...");
            BulletText("Ctrl+O:             open image...");
            BulletText("Ctrl+C:             copy to clipboard");
            BulletText("Esc:                close dialogs");
            Separator();
            Text("Deep zoom (%.0fx and above) overlays", ViewState::kDeepZoomThreshold);
            Text("R / G / B values inside each pixel.");
            End();
        }

        // ---------- Control panel (M4) ----------
        // Per-window trackbar strips go over every cell above the status bar;
        // the global "…settings" window only when st.showControlPanel is set.
        for (const auto& e : computeLayout(sz)) {
            drawTrackbarStrip(e.first, e.second);
        }
        if (st.showControlPanel) {
            drawControlPanel();
        }
        PopFont();
    }

    // ---------- Framebuffer snapshot ---------------------------------------

    void maybeCaptureFramebuffer() {
        long target = 0;
        std::uint64_t requestedGen = 0;
        {
            std::lock_guard<std::mutex> lock(s_captureMtx);
            if (s_captureDone >= s_captureRequested) return;
            target = s_captureRequested;
            requestedGen = s_captureRequestedGen;
        }

        // Only capture a frame that drew a settled state: no window/image
        // mutation happened during the draw and the drawn state is at least as
        // new as the requester's setup. Otherwise skip this frame and let the
        // caller keep waiting for the next one.
        if (s_frameDrawStartGen.load() != s_frameDrawEndGen.load()) return;
        if (s_frameDrawEndGen.load() < requestedGen) return;

        captureFramebuffer();

        std::lock_guard<std::mutex> lock(s_captureMtx);
        if (target > s_captureDone) {
            s_captureDone = target;
            s_captureCv.notify_all();
        }
    }

    void captureFramebuffer() {
        cv::Ptr<PlanRuntime> pr = runtime();
        V4D* v4d = dynamic_cast<V4D*>(pr.get());
        if (!v4d) return;
        cv::Ptr<PlanContext> nvg_ctx = v4d->nvgCtx();
        auto* nc = dynamic_cast<cv::v4d::detail::NanoVGContext*>(nvg_ctx.get());
        if (!nc) return;
        cv::Ptr<FrameBufferContext> nvgFb = nc->fbCtx();
        if (!nvgFb) return;

        cv::Size sz = nvgFb->size();
        cv::Mat rgba(sz, CV_8UC4);
        FrameBufferContext::WindowScope winScope(nvgFb);
        FrameBufferContext::GLScope glScope(nvgFb, GL_READ_FRAMEBUFFER);
        GL_CHECK(glReadBuffer(GL_COLOR_ATTACHMENT0));
        GL_CHECK(glFinish());
        GL_CHECK(glReadPixels(0, 0, sz.width, sz.height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data));
	
        std::lock_guard<std::mutex> lock(s_fbMutex);
        cv::flip(rgba, s_framebuffer, 0);
    }
};

cv::Rect LowguiRootPlan::viewportFor(const std::string& name, const cv::Size& sz) {
    for (const auto& e : computeLayout(sz)) {
        if (e.first == name) return e.second;
    }
    return cv::Rect();
}

void LowguiRootPlan::notifySettledFrame(std::uint64_t gen) {
    s_frameDrawDoneGen.store(gen);
    std::lock_guard<std::mutex> lock(s_frameDrawMtx);
    s_frameDrawCv.notify_all();
}

void LowguiRootPlan::notifyWaitersShutdown() {
    {
        std::lock_guard<std::mutex> lock(s_captureMtx);
        s_captureDone = s_captureRequested;
        s_captureCv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(s_frameDrawMtx);
        s_frameDrawDoneGen.store(std::numeric_limits<std::uint64_t>::max());
        s_frameDrawCv.notify_all();
    }
}

} // namespace detail
} // namespace lowgui
} // namespace cv

#endif // OPENCV_LOWGUI_LOWGUI_ROOT_PLAN_HPP_

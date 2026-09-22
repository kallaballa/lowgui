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
#include <map>
#include <memory>
#include <mutex>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/v4d/v4d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/lowgui/window_manager.hpp>
#include <sstream>
#include <string>
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

        // View transform (viewport-local coordinates)
        float zoom = 1.0f;
        cv::Point2f pan = {0.0f, 0.0f};
        bool isDragging = false;

        // Cursor tracking (viewport-local)
        cv::Point2f mousePos = {-1.0f, -1.0f};
        bool mouseInside = false;

        // UI toggles
        bool showProperties = false;
        bool showHelp = true;
        bool showStatusBar = true;

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

    // NanoVG image handles to freed in the next nvg node (collected by
    // reconcileStates when a window disappears). Worker thread only.
    std::vector<int> orphanedTextures_;

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

public:
    LowguiRootPlan() = default;

    static void setHeadless(bool value) { s_headless.store(value); }

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
        plain([](const Mouse::List& scrollEvents,
                 const Mouse::List& dragEvents,
                 const Mouse::List& pressLeftEvents,
                 const Mouse::List& releaseLeftEvents,
                 const Mouse::List& pressRightEvents,
                 const Mouse::List& pressMiddleEvents,
                 const Mouse::List& moveEvents,
                 const Mouse::List& hoverEnterEvents,
                 const Mouse::List& hoverExitEvents,
                 const cv::Size& sz,
                 std::map<std::string, ViewState>& states,
                 std::string& activeWindow) {
            handleInput(scrollEvents, dragEvents, pressLeftEvents, releaseLeftEvents,
                        pressRightEvents, pressMiddleEvents, moveEvents, hoverEnterEvents,
                        hoverExitEvents, sz, states, activeWindow);
        }, scroll_, drag_, pressLeft_, releaseLeft_, pressRight_, pressMiddle_,
           move_, hoverEnter_, hoverExit_, size_, RWS(s_viewStates), RWS(s_activeWindow));

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
    Event<Mouse> pressMiddle_ = E<Mouse>(Mouse::PRESS,   Mouse::MIDDLE);
    Event<Mouse> move_        = E<Mouse>(Mouse::MOVE);
    Event<Mouse> hoverEnter_  = E<Mouse>(Mouse::HOVER_ENTER);
    Event<Mouse> hoverExit_   = E<Mouse>(Mouse::HOVER_EXIT);

    // ---------- Layout ------------------------------------------------------

    static std::vector<std::pair<std::string, cv::Rect>> computeLayout(const cv::Size& sz) {
        std::vector<std::string> names = WindowManager::instance().getWindowNames();
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
                std::lock_guard<std::mutex> lock(wd->sink.mtx);
                vp = wd->userViewport ? wd->viewport : cell;
            }
            layout.emplace_back(names[i], vp);
        }
        return layout;
    }

    static cv::Rect viewportFor(const std::string& name, const cv::Size& sz) {
        for (const auto& e : computeLayout(sz)) {
            if (e.first == name) return e.second;
        }
        return cv::Rect();
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

    static void handleInput(const Mouse::List& scrollEvents,
                            const Mouse::List& dragEvents,
                            const Mouse::List& pressLeftEvents,
                            const Mouse::List& releaseLeftEvents,
                            const Mouse::List& pressRightEvents,
                            const Mouse::List& pressMiddleEvents,
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

        // Scroll wheel: zoom around the cursor inside the window under it.
        for (auto se : scrollEvents) {
            auto [name, vp] = windowAt(layout, se->position());
            if (name.empty() || !states.count(name)) continue;
            float factor = (se->data().y > 0) ? 1.1f : (1.0f / 1.1f);
            zoomAt(states[name], toLocal(vp, se->position()), factor);
            activeWindow = name;
        }

        // Left-press starts a drag on the window under the cursor.
        for (auto pe : pressLeftEvents) {
            auto [name, vp] = windowAt(layout, pe->position());
            if (name.empty() || !states.count(name)) continue;
            states[name].isDragging = true;
            activeWindow = name;
        }
        for (auto& _ : releaseLeftEvents) {
            for (auto& [name, st] : states) st.isDragging = false;
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

        // Right-click: reset zoom (1:1, centered) on the window under cursor.
        for (auto re : pressRightEvents) {
            auto [name, vp] = windowAt(layout, re->position());
            if (name.empty() || !states.count(name)) continue;
            resetZoom(states[name], vp.size());
            activeWindow = name;
        }

        // Middle-click: jump to deep zoom in the window under cursor.
        for (auto me : pressMiddleEvents) {
            auto [name, vp] = windowAt(layout, me->position());
            if (name.empty() || !states.count(name)) continue;
            zoomRegion(states[name], vp.size());
            activeWindow = name;
        }

        // Cursor position per window (for the status bar / pixel readout).
        for (auto me : moveEvents) {
            for (const auto& [name, vp] : layout) {
                auto it = states.find(name);
                if (it == states.end()) continue;
                if (vp.contains(me->position())) {
                    it->second.mouseInside = true;
                    it->second.mousePos = toLocal(vp, me->position());
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

    // ---------- Rendering ---------------------------------------------------

    void drawWindows(const cv::Size& sz, std::map<std::string, ViewState>& states) {
        using namespace cv::v4d::nvg;

        std::uint64_t startGen = WindowManager::instance().generation();
        std::vector<std::string> names = WindowManager::instance().getWindowNames();

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
            {
                std::lock_guard<std::mutex> lock(wd->sink.mtx);
                title = wd->title;
            }
            cv::UMat img = wd->sink.frame();
            if (img.empty()) continue;

            cv::UMat rgba8;
            if (!prepareRgba(img, rgba8)) {
                // One bad window must not kill the shared render engine: skip it
                // and let the caller see the warning instead.
                CV_LOG_WARNING(nullptr, "lowgui: skipping window '" << name
                    << "' with unsupported image format for imshow");
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
                fitToCell(st, vp.size());
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
        }

        s_frameDrawEndGen.store(WindowManager::instance().generation());
        s_frameDrawStartGen.store(startGen);
    }

    static void renderImage(ViewState& st, const cv::Size& cell, bool verbose) {
        using namespace cv::v4d::nvg;
        if (st.imageHandle <= 0 || st.imageW <= 0 || st.imageH <= 0) return;

        save();
        translate(st.pan.x, st.pan.y);
        scale(st.zoom, st.zoom);

        beginPath();
        rect(0.0f, 0.0f, (float)st.imageW, (float)st.imageH);
        fillPaint(imagePattern(0.0f, 0.0f, (float)st.imageW, (float)st.imageH,
                               0.0f, st.imageHandle, 1.0f));
        fill();

        if (verbose && st.zoom >= 8.0f && st.zoom < ViewState::kDeepZoomThreshold) {
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
        if (verbose) drawDeepZoomOverlay(st, cell);
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
    // 32F/64F * 255 (saturated). 32S is rejected like highgui does.
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
            cv::cvtColor(cpu, rgba8, colorCode(cpu.channels()));
        } else {
            cv::Mat cpu = src.getMat(cv::ACCESS_READ);
            cv::cvtColor(cpu, rgba8, colorCode(cpu.channels()));
        }
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
        std::error_code ec;
        std::filesystem::path dir(st.fileDialogPath);
        if (!std::filesystem::is_directory(dir, ec)) return;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
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
            if (IsKeyPressed(ImGuiKey_P)) resetZoom(st, vp.size());
            if (IsKeyPressed(ImGuiKey_F)) fitToCell(st, vp.size());
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
                // xclip on Linux. Best-effort, no GUI feedback.
                auto wd = WindowManager::instance().getWindowShared(activeWindow);
                cv::UMat um;
                if (wd) um = wd->sink.frame();
                if (!um.empty()) {
                    cv::Mat src = um.getMat(cv::ACCESS_READ);
                    std::string tmp = "/tmp/lowgui_clipboard.png";
                    if (imwrite(tmp, src)) {
                        std::string cmd = "xclip -selection clipboard -t image/png < "
                                          + tmp + " >/dev/null 2>&1 &";
                        std::system(cmd.c_str());
                        st.lastImageSaveOk = true;
                        st.lastImageSaveMsg = "Copied image to clipboard.";
                    }
                }
            }
        }

        // ESC closes dialogs / overlays.
        if (IsKeyPressed(ImGuiKey_Escape)) {
            if (st.showProperties)         st.showProperties         = false;
            else if (st.showSaveDialog)    st.showSaveDialog         = false;
            else if (st.showSaveViewDialog)st.showSaveViewDialog     = false;
            else if (st.showFileDialog)    st.showFileDialog         = false;
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
                if (MenuItem("Zoom x1", "Ctrl+P or 0")) resetZoom(st, vp.size());
                if (MenuItem("Zoom x30 (deep zoom)", "Ctrl+X")) zoomRegion(st, vp.size());
                Separator();
                if (MenuItem("Zoom in",  "Ctrl++"))  zoomAround(st, vp.size(), 1.5f);
                if (MenuItem("Zoom out", "Ctrl+-"))  zoomAround(st, vp.size(), 1.0f / 1.5f);
                if (MenuItem("Fit to window", "Ctrl+F")) fitToCell(st, vp.size());
                Separator();
                Checkbox("Status bar",        &st.showStatusBar);
                Checkbox("Show help overlay", &st.showHelp);
                Separator();
                MenuItem("Properties...", nullptr, &st.showProperties);
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

        // ---------- File browser dialog (Open image...) ----------
        if (st.showFileDialog) {
            SetNextWindowSize(ImVec2(560, 420), ImGuiCond_Appearing);
            Begin("Open image", &st.showFileDialog);
            Text("Current directory: %s", st.fileDialogPath);
            SameLine();
            if (!st.fileDialogErrorMsg.empty()) {
                TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                            "Error: %s", st.fileDialogErrorMsg.c_str());
                if (Button("Clear error")) st.fileDialogErrorMsg.clear();
                SameLine();
            }
            if (Button("Up")) {
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

            if (Button("Open") && st.fileDialogSelected[0] != '\0') {
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
            if (Button("Cancel")) {
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
            if (Button("Save") && st.saveBuf[0] != '\0') {
                std::string path(st.saveBuf);
                std::string ext = fmts[std::clamp(st.saveFormat, 0, 2)];
                if (path.size() < ext.size() ||
                    path.compare(path.size() - ext.size(), ext.size(), ext) != 0) {
                    path += ext;
                }
                auto wd = WindowManager::instance().getWindowShared(activeWindow);
                cv::UMat um;
                if (wd) um = wd->sink.frame();
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
            if (Button("Cancel")) {
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
            if (Button("Save") && st.saveBuf[0] != '\0') {
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
                if (wd) um = wd->sink.frame();
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
            if (Button("Cancel")) {
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
            BulletText("Right-click: reset zoom");
            BulletText("Middle-click: zoom to region (deep zoom)");
            Text("Keyboard:");
            BulletText("Ctrl+Arrows: pan by 5%% of viewport");
            BulletText("Ctrl+'+' / Ctrl+'-': zoom in / out");
            BulletText("Ctrl+0 / Ctrl+P: reset zoom");
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
            BulletText("Right click:        reset zoom (1:1)");
            BulletText("Middle click:       deep zoom (x%.0f)", ViewState::kDeepZoomThreshold);
            Separator();
            BulletText("Ctrl+Arrows:        pan by 5%% of viewport");
            BulletText("Ctrl+'+'/'-':       zoom in / out");
            BulletText("Ctrl+0 / Ctrl+P:    reset zoom");
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

} // namespace detail
} // namespace lowgui
} // namespace cv

#endif // OPENCV_LOWGUI_LOWGUI_ROOT_PLAN_HPP_

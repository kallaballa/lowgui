#ifndef OPENCV_LOWGUI_LOWGUI_ROOT_PLAN_HPP_
#define OPENCV_LOWGUI_LOWGUI_ROOT_PLAN_HPP_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <opencv2/v4d/v4d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/lowgui/window_manager.hpp>

namespace cv {
namespace lowgui {
namespace detail {

using namespace cv::v4d;

/**
 * Root Plan-V4D plan that owns the long-lived render loop.
 *
 * Every frame it re-reads the current window set from the WindowManager
 * (windows can be created/destroyed between frames and across waitKey calls)
 * and draws all of them in a single NanoVG node with fit-to-viewport scaling.
 *
 * A headless (offscreen) framebuffer snapshot is produced on demand: waitKey
 * callers record a capture request id and wait until the engine has rendered
 * and copied the framebuffer, so readFramebuffer() deterministically reflects
 * the most recently pushed image.
 */
class LowguiRootPlan : public V4DPlan {
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

    static cv::UMat s_framebuffer;
    static std::mutex s_fbMutex;

    // Per-frame counter monotonically increased by the render loop.
    static std::atomic<long> s_frameCount;

    // one-shot capture coordination between waitKey (requester) and the loop.
    static std::mutex s_captureMtx;
    static std::condition_variable s_captureCv;
    static long s_captureRequested;
    static long s_captureDone;

public:
    LowguiRootPlan() = default;

    static cv::UMat getFramebuffer() {
        std::lock_guard<std::mutex> lock(s_fbMutex);
        return s_framebuffer.clone();
    }

    static void clearFramebuffer() {
        std::lock_guard<std::mutex> lock(s_fbMutex);
        s_framebuffer.release();
    }

    static long requestFrameCapture() {
        std::lock_guard<std::mutex> lock(s_captureMtx);
        ++s_captureRequested;
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

        nvg([this](const cv::Size& sz) {
            drawWindows(sz);
        }, size_);
        plain([this](const cv::Size&) {
            maybeCaptureFramebuffer();
        }, size_);
    }

    void teardown() override {}

private:
    // NanoVG image handles must outlive the frame they are drawn in: NanoVG
    // batches all drawing and only issues GL commands in nvgEndFrame() (at the
    // end of the nvg node), so deleting the texture inline right after fill()
    // leaves the batched quad referencing a freed texture -> black pixels.
    // Handles are therefore cached per window and the previous frame's handle
    // is deleted at the START of the window's next draw, when its own batch has
    // already been flushed. Only the single worker plan runs drawWindows, so
    // this map needs no extra locking. deleteImage is only invoked while the
    // nvg context is current (inside the nvg node), which is satisfied here.
    std::map<std::string, int> imageHandles_;

    void drawWindows(const cv::Size& sz) {
        using namespace cv::v4d::nvg;

        std::vector<std::string> names = WindowManager::instance().getWindowNames();

        // Reclaim handles for windows destroyed since the last frame.
        for (auto it = imageHandles_.begin(); it != imageHandles_.end();) {
            if (std::find(names.begin(), names.end(), it->first) == names.end()) {
                if (it->second > 0) deleteImage(it->second);
                it = imageHandles_.erase(it);
            } else {
                ++it;
            }
        }

        int n = (int)names.size();
        if (n <= 0) return;

        int cols = std::ceil(std::sqrt((double)n));
        int rows = std::ceil((double)n / cols);
        int cellW = sz.width / cols;
        int cellH = sz.height / rows;

        for (int i = 0; i < n; ++i) {
            auto wd = WindowManager::instance().getWindowShared(names[i]);
            if (!wd) continue;

            cv::Rect vp;
            {
                std::lock_guard<std::mutex> sinkLock(wd->sink.mtx);
                cv::Rect cell(i % cols * cellW, i / cols * cellH, cellW, cellH);
                vp = wd->userViewport ? wd->viewport : cell;
            }
            cv::UMat img = wd->sink.frame();
            if (img.empty()) continue;
            if (vp.width <= 0 || vp.height <= 0) continue;

            cv::UMat rgba8;
            if (!prepareRgba(img, rgba8)) {
                CV_Error(Error::StsUnsupportedFormat, "Unsupported image format for imshow");
            }

            // Delete the previous frame's handle (its batch was flushed at the
            // end of the previous nvg node) before creating the new one.
            auto it = imageHandles_.find(names[i]);
            if (it != imageHandles_.end()) {
                if (it->second > 0) deleteImage(it->second);
                imageHandles_.erase(it);
            }

            save();
            // Cover (not contain) the cell: the grid lays each window out to
            // fill its cell, cropping the image when its aspect ratio differs
            // from the cell's (matches the test expectations and highgui's
            // "image fills the viewport" behaviour).
            float sc = std::max((float)vp.width / rgba8.cols, (float)vp.height / rgba8.rows);
            float drawW = rgba8.cols * sc;
            float drawH = rgba8.rows * sc;
            float x = vp.x + (vp.width - drawW) / 2.0f;
            float y = vp.y + (vp.height - drawH) / 2.0f;

            translate(x, y);
            scale(sc, sc);

            cv::Mat host = rgba8.getMat(cv::ACCESS_READ);
            int handle = createImageRGBA(rgba8.cols, rgba8.rows, NVG_IMAGE_NEAREST, host.data);
            if (handle > 0) {
                beginPath();
                rect(0, 0, (float)rgba8.cols, (float)rgba8.rows);
                fillPaint(imagePattern(0, 0, (float)rgba8.cols, (float)rgba8.rows, 0.0f, handle, 1.0f));
                fill();
                imageHandles_[names[i]] = handle;
            }
            restore();
        }
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

    void maybeCaptureFramebuffer() {
        long target = 0;
        {
            std::lock_guard<std::mutex> lock(s_captureMtx);
            if (s_captureDone >= s_captureRequested) return;
            target = s_captureRequested;
        }

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

        cv::Ptr<PlanContext> main_fb_ctx = v4d->fbCtx();
        FrameBufferContext* mainFb = dynamic_cast<FrameBufferContext*>(main_fb_ctx.get());
        cv::Mat rgbaMain(sz, CV_8UC4);
        cv::Mat rgbaWin(sz, CV_8UC4);
        if (mainFb) {
            FrameBufferContext::GLScope mainGl(nvgFb, GL_READ_FRAMEBUFFER, mainFb->getFramebufferID());
            GL_CHECK(glFinish());
            GL_CHECK(glReadPixels(0, 0, sz.width, sz.height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaMain.data));
            FrameBufferContext::GLScope winGl(nvgFb, GL_READ_FRAMEBUFFER, 0);
            GL_CHECK(glFinish());
            GL_CHECK(glReadPixels(0, 0, sz.width, sz.height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaWin.data));
            cv::Scalar nvgm = mean(rgba), mmm = mean(rgbaMain), wm = mean(rgbaWin);
            double nmin, nmax, mmin2, mmax2, wmin, wmax;
            cv::minMaxLoc(rgba, &nmin, &nmax);
            cv::minMaxLoc(rgbaMain, &mmin2, &mmax2);
            cv::minMaxLoc(rgbaWin, &wmin, &wmax);
            CV_LOG_INFO(nullptr, "LOWGUI-DBG child mean=" << nvgm << " min=" << nmin << " max=" << nmax
                << " | main mean=" << mmm << " min=" << mmin2 << " max=" << mmax2
                << " | win mean=" << wm << " min=" << wmin << " max=" << wmax);
        }
        cv::Mat flipped;
        cv::flip(rgba, flipped, 0);

        std::lock_guard<std::mutex> lock(s_fbMutex);
        s_framebuffer = flipped.getUMat(cv::ACCESS_READ);
        if (!s_framebuffer.empty()) {
            cv::Scalar mm = mean(s_framebuffer);
            double mmin, mmax;
            cv::minMaxLoc(s_framebuffer, &mmin, &mmax);
            CV_LOG_INFO(nullptr, "LOWGUI-DBG fb " << s_framebuffer.cols << "x" << s_framebuffer.rows
                << " mean=" << mm << " min=" << mmin << " max=" << mmax);
        }
    }
};

}
}
}

#endif // OPENCV_LOWGUI_LOWGUI_ROOT_PLAN_HPP_
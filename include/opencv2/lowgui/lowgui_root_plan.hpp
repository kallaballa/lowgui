#ifndef OPENCV_LOWGUI_LOWGUI_ROOT_PLAN_HPP_
#define OPENCV_LOWGUI_LOWGUI_ROOT_PLAN_HPP_

#include <cmath>
#include <mutex>
#include <opencv2/v4d/v4d.hpp>
#include <opencv2/lowgui/window_manager.hpp>
#include <opencv2/lowgui/window_plan.hpp>

namespace cv {
namespace lowgui {
namespace detail {

using namespace cv::v4d;

class LowguiRootPlan : public V4DPlan {
    std::vector<std::string> window_names_;
    std::vector<cv::Ptr<WindowPlan>> window_plans_;
    std::vector<cv::Rect> viewports_;
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

    static cv::UMat s_framebuffer;
    static std::mutex s_fbMutex;

public:
    LowguiRootPlan() = default;

    void captureFramebuffer() {
        cv::Ptr<PlanRuntime> pr = runtime();
        V4D* v4d = dynamic_cast<V4D*>(pr.get());
        if (v4d) {
            cv::Ptr<PlanContext> fb_ctx = v4d->fbCtx();
            FrameBufferContext* fb = dynamic_cast<FrameBufferContext*>(fb_ctx.get());
            if (fb) {
                std::lock_guard<std::mutex> lock(s_fbMutex);
                fb->copyTo(s_framebuffer);
            }
        }
    }

    static cv::UMat getFramebuffer() {
        std::lock_guard<std::mutex> lock(s_fbMutex);
        return s_framebuffer.clone();
    }

    void setup() override {
        set(GlobalState::Keys::TIME_TRACKER, V(false));
        set(GlobalState::Keys::SHOW_FRAME_TIME, V(false));

        window_names_ = WindowManager::instance().getWindowNames();
        for (const auto& name : window_names_) {
            window_plans_.push_back(cv::makePtr<WindowPlan>(name));
        }

        cv::Size sz = V4D::get<cv::Size>(V4D::Keys::SIZE);
        int n = window_names_.size();
        if (n == 0) return;

        int cols = std::ceil(std::sqrt(n));
        int rows = std::ceil((double)n / cols);
        int cellW = sz.width / cols;
        int cellH = sz.height / rows;

        viewports_.resize(n);
        for (int i = 0; i < n; ++i) {
            int col = i % cols;
            int row = i / cols;
            viewports_[i] = cv::Rect(col * cellW, row * cellH, cellW, cellH);
        }

        for (auto& plan : window_plans_) {
            subSetup(plan);
        }
    }

    void infer() override {
        set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(30, 30, 30, 255)));
        clear();

        int n = window_names_.size();
        if (n > 0) {
            cv::Size sz = V4D::get<cv::Size>(V4D::Keys::SIZE);
            int cols = std::ceil(std::sqrt(n));
            int rows = std::ceil((double)n / cols);
            int cellW = sz.width / cols;
            int cellH = sz.height / rows;

            for (int i = 0; i < n; ++i) {
                int col = i % cols;
                int row = i / cols;
                viewports_[i] = cv::Rect(col * cellW, row * cellH, cellW, cellH);
            }
        }

        for (int i = 0; i < n; ++i) {
            WindowData* wd = WindowManager::instance().getWindow(window_names_[i]);
            if (!wd) continue;
            wd->viewport = viewports_[i];
            set(cv::v4d::V4D::Keys::VIEWPORT, V(viewports_[i]));
            this->subInfer(window_plans_[i]);
        }

        this->write();
    }

    void teardown() override {
        if (std::getenv("LOWGUI_HEADLESS_RENDER")) {
            captureFramebuffer();
        }
        for (auto& plan : window_plans_) {
            this->subTeardown(plan);
        }
    }
};

}
}
}

#endif // OPENCV_LOWGUI_LOWGUI_ROOT_PLAN_HPP_

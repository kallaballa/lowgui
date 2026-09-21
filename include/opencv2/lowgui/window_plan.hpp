#ifndef OPENCV_LOWGUI_WINDOW_PLAN_HPP_
#define OPENCV_LOWGUI_WINDOW_PLAN_HPP_

#include <opencv2/v4d/v4d.hpp>
#include <opencv2/lowgui/window_manager.hpp>

namespace cv {
namespace lowgui {
namespace detail {

using namespace cv::v4d;

class WindowPlan : public V4DPlan {
    std::string window_name_;
    cv::UMat rgba_;
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

public:
    explicit WindowPlan(const std::string& name) : window_name_(name) {}

    void setup() override {
        set(GlobalState::Keys::TIME_TRACKER, V(false));
        set(GlobalState::Keys::SHOW_FRAME_TIME, V(false));
    }

    void infer() override {
        plain([this](UMat& rgba) {
            WindowData* wd = WindowManager::instance().getWindow(window_name_);
            cv::UMat img;
            if (wd) {
                std::lock_guard<std::mutex> lock(wd->buffer.mtx);
                wd->buffer.image.copyTo(img);
            }
            if (img.empty()) {
                rgba = UMat();
                return;
            }
            if (img.channels() == 1) {
                cvtColor(img, rgba, COLOR_GRAY2RGBA);
            } else if (img.channels() == 3) {
                cvtColor(img, rgba, COLOR_BGR2RGBA);
            } else if (img.channels() == 4) {
                cvtColor(img, rgba, COLOR_BGRA2RGBA);
            } else {
                CV_Error(Error::StsError, "Unsupported image format");
            }
        }, RW(rgba_));

        nvg([this](const UMat& rgba, const cv::Size& sz) {
            using namespace cv::v4d::nvg;
            if (rgba.empty()) return;

            save();
            float sc = std::min(
                (float)sz.width / rgba.cols,
                (float)sz.height / rgba.rows
            );
            int drawW = rgba.cols * sc;
            int drawH = rgba.rows * sc;
            int x = (sz.width - drawW) / 2;
            int y = (sz.height - drawH) / 2;

            translate(x, y);
            scale(sc, sc);

            cv::Mat host = rgba.getMat(cv::ACCESS_READ);
            int handle = createImageRGBA(rgba.cols, rgba.rows, NVG_IMAGE_NEAREST, host.data);
            if (handle > 0) {
                beginPath();
                rect(0, 0, rgba.cols, rgba.rows);
                fillPaint(imagePattern(0, 0, rgba.cols, rgba.rows, 0, handle, 1.0f));
                fill();
                deleteImage(handle);
            }
            restore();
        }, R(rgba_), size_);
    }

    void teardown() override {}
};

}
}
}

#endif // OPENCV_LOWGUI_WINDOW_PLAN_HPP_

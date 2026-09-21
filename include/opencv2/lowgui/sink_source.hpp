#ifndef OPENCV_LOWGUI_SINK_SOURCE_HPP_
#define OPENCV_LOWGUI_SINK_SOURCE_HPP_

#include <opencv2/core.hpp>
#include <opencv2/v4d/v4d.hpp>
#include <functional>
#include <mutex>

namespace cv {
namespace lowgui {
namespace detail {

/**
 * @brief A thread-safe buffer that can act as a source for V4D.
 * 
 * This class holds a single image buffer that can be pushed from external threads
 * and consumed by the V4D runtime through a generator function.
 */
class CV_EXPORTS SinkSource {
    cv::UMat frame_;
    bool has_frame_ = false;
    mutable std::mutex mtx_;
    float fps_ = 30.0f;

public:
    /**
     * @brief Constructs a SinkSource with the given fps.
     * @param fps Frames per second (default: 30).
     */
    explicit SinkSource(float fps = 30.0f) : fps_(fps) {}

    /**
     * @brief Pushes an image into the buffer.
     * 
     * This method is thread-safe and can be called from any thread.
     * The image will be available on the next generation.
     * 
     * @param img Image to push (will be converted to UMat).
     */
    void push(const cv::Mat& img) {
        std::lock_guard<std::mutex> lock(mtx_);
        img.copyTo(frame_);
        has_frame_ = true;
    }

    /**
     * @brief Pushes a UMat image into the buffer.
     * @param img Image to push.
     */
    void push(const cv::UMat& img) {
        std::lock_guard<std::mutex> lock(mtx_);
        img.copyTo(frame_);
        has_frame_ = true;
    }

    /**
     * @brief Checks if there's an image available.
     */
    bool hasImage() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return has_frame_;
    }

    /**
     * @brief Gets the fps.
     */
    float fps() const { return fps_; }

    /**
     * @brief Generates the next frame (called by V4D runtime).
     * 
     * This is the generator function that can be passed to cv::v4d::Source.
     * It returns the last pushed image or an empty UMat if no image has been pushed.
     * 
     * @return The next frame.
     */
    cv::UMat next() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (has_frame_) {
            has_frame_ = false;
            return frame_.clone();
        }
        return cv::UMat();
    }

    /**
     * @brief Creates a cv::v4d::Source from this SinkSource.
     * 
     * @return A Source object that wraps this buffer.
     */
    cv::Ptr<cv::v4d::Source> toSource() {
        auto self = this;
        return cv::makePtr<cv::v4d::Source>(
            [self](cv::UMat& frame) -> bool {
                cv::UMat result = self->next();
                if (!result.empty()) {
                    frame = std::move(result);
                    return true;
                }
                return false;
            },
            fps_
        );
    }
};

} // namespace detail
} // namespace lowgui
} // namespace cv

#endif // OPENCV_LOWGUI_SINK_SOURCE_HPP_
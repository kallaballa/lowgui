#ifndef OPENCV_LOWGUI_SINK_SOURCE_HPP_
#define OPENCV_LOWGUI_SINK_SOURCE_HPP_

#include <opencv2/core.hpp>
#include <opencv2/v4d/v4d.hpp>
#include <functional>
#include <memory>
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
class CV_EXPORTS SinkSource : public std::enable_shared_from_this<SinkSource> {
    cv::UMat frame_;
    bool has_frame_ = false;
    float fps_ = 30.0f;

public:
    // Public so per-window state (viewport/title) can be guarded with the same
    // lock that protects the frame (see WindowData).
    mutable std::mutex mtx;

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
        if (img.empty()) return;
        std::lock_guard<std::mutex> lock(mtx);
        img.copyTo(frame_);
        has_frame_ = true;
    }

    /**
     * @brief Pushes a UMat image into the buffer.
     * @param img Image to push.
     */
    void push(const cv::UMat& img) {
        if (img.empty()) return;
        std::lock_guard<std::mutex> lock(mtx);
        img.copyTo(frame_);
        has_frame_ = true;
    }

    /**
     * @brief Checks if there's an image available.
     */
    bool hasImage() const {
        std::lock_guard<std::mutex> lock(mtx);
        return has_frame_;
    }

    /**
     * @brief Gets the fps.
     */
    float fps() const { return fps_; }

    /**
     * @brief Returns the latest pushed frame without consuming it.
     *
     * The render loop uses this to keep displaying the last known image; it is
     * a clone so the caller can render it after releasing the lock.
     *
     * @return A copy of the latest frame, or an empty UMat if none was pushed.
     */
    cv::UMat frame() const {
        std::lock_guard<std::mutex> lock(mtx);
        if (has_frame_) return frame_.clone();
        return cv::UMat();
    }

    /**
     * @brief Generates the next frame (called by V4D runtime).
     * 
     * This is the generator function that can be passed to cv::v4d::Source.
     * It returns the last pushed image or an empty UMat if no image has been pushed.
     * 
     * @return The next frame.
     */
    cv::UMat next() {
        std::lock_guard<std::mutex> lock(mtx);
        if (has_frame_) {
            has_frame_ = false;
            return frame_.clone();
        }
        return cv::UMat();
    }

    /**
     * @brief Creates a cv::v4d::Source from this SinkSource.
     * 
     * The Source keeps the SinkSource alive for as long as it is referenced, so
     * it must be shared (e.g. created via cv::makePtr or owned through a
     * shared_ptr) for the ownership to be safe. If called on a non-shared
     * instance an always-empty Source is returned instead of risking a dangling
     * capture. WindowManager-owned SinkSources are shared.
     * 
     * @return A Source object that wraps this buffer.
     */
    cv::Ptr<cv::v4d::Source> toSource() {
        std::shared_ptr<SinkSource> self = weak_from_this().lock();
        if (!self) {
            return cv::makePtr<cv::v4d::Source>(
                [](cv::UMat&) -> bool { return false; },
                fps_
            );
        }
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
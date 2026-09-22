#ifndef OPENCV_LOWGUI_WINDOW_MANAGER_HPP_
#define OPENCV_LOWGUI_WINDOW_MANAGER_HPP_

#include <opencv2/core.hpp>
#include <opencv2/lowgui/sink_source.hpp>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace cv {
namespace lowgui {
namespace detail {

struct WindowData {
    std::string name;
    std::string title;
    int flags;
    // Per-window transport: imshow pushes into it, the render loop reads the
    // latest frame from it. Its mutex also guards viewport/title/userViewport.
    SinkSource sink;
    cv::Rect viewport;
    // When set (via resizeWindow/moveWindow) the viewport is used as-is
    // instead of being overridden by the auto grid layout.
    bool userViewport = false;

    // Delete copy/move operations because SinkSource is non-copyable/non-movable
    WindowData() = default;
    WindowData(const WindowData&) = delete;
    WindowData& operator=(const WindowData&) = delete;
    WindowData(WindowData&&) = delete;
    WindowData& operator=(WindowData&&) = delete;

    // Constructor for easy creation
    WindowData(std::string n, std::string t, int f)
        : name(std::move(n)), title(std::move(t)), flags(f), sink(), viewport() {}
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

    WindowData* getWindow(const std::string& name);
    const WindowData* getWindow(const std::string& name) const;
    std::shared_ptr<WindowData> getWindowShared(const std::string& name);
    std::shared_ptr<const WindowData> getWindowShared(const std::string& name) const;

    std::vector<std::string> getWindowNames() const;

    void setRunning(bool r);
    bool isRunning() const;

    void waitForWindow();
    void notifyAll();

    size_t windowCount() const;
};

}
}
}

#endif // OPENCV_LOWGUI_WINDOW_MANAGER_HPP_

#ifndef OPENCV_LOWGUI_WINDOW_MANAGER_HPP_
#define OPENCV_LOWGUI_WINDOW_MANAGER_HPP_

#include <opencv2/core.hpp>
#include <opencv2/lowgui/sink_source.hpp>
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
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
    std::map<std::string, std::shared_ptr<WindowData>> windows_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool running_ = false;

public:
    static WindowManager& instance();

    void createWindow(const std::string& name, int flags);
    void destroyWindow(const std::string& name);
    void destroyAllWindows();
    bool hasWindow(const std::string& name) const;

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

#ifndef OPENCV_LOWGUI_WINDOW_MANAGER_HPP_
#define OPENCV_LOWGUI_WINDOW_MANAGER_HPP_

#include <opencv2/core.hpp>
#include <string>
#include <map>
#include <mutex>
#include <condition_variable>
#include <vector>

namespace cv {
namespace lowgui {
namespace detail {

struct ImageBuffer {
    std::mutex mtx;
    cv::UMat image;
    bool has_image = false;
};

struct WindowData {
    std::string name;
    std::string title;
    int flags;
    ImageBuffer buffer;
    cv::Rect viewport;
};

class WindowManager {
    std::map<std::string, WindowData> windows_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool running_ = false;

public:
    static WindowManager& instance();

    void createWindow(const std::string& name, int flags);
    void destroyWindow(const std::string& name);
    void destroyAllWindows();

    void pushImage(const std::string& name, const cv::UMat& img);
    bool popImage(const std::string& name, cv::UMat& img);
    bool getImage(const std::string& name, cv::UMat& img) const;

    WindowData* getWindow(const std::string& name);
    const WindowData* getWindow(const std::string& name) const;

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

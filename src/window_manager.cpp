#include <opencv2/lowgui/window_manager.hpp>
#include <vector>

namespace cv {
namespace lowgui {
namespace detail {

WindowManager& WindowManager::instance() {
    static WindowManager inst;
    return inst;
}

void WindowManager::createWindow(const std::string& name, int flags) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = windows_.find(name);
    if (it == windows_.end()) {
        windows_.emplace(name, std::make_shared<WindowData>(name, name, flags));
    }
}

void WindowManager::destroyWindow(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx_);
    windows_.erase(name);
    cv_.notify_all();
}

void WindowManager::destroyAllWindows() {
    std::lock_guard<std::mutex> lock(mtx_);
    windows_.clear();
    cv_.notify_all();
}

bool WindowManager::hasWindow(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return windows_.find(name) != windows_.end();
}

void WindowManager::pushImage(const std::string& name, const cv::UMat& img) {
    std::unique_lock<std::mutex> lock(mtx_);
    auto it = windows_.find(name);
    if (it == windows_.end()) return;
    it->second->sink.push(img);
}

bool WindowManager::popImage(const std::string& name, cv::UMat& img) {
    std::unique_lock<std::mutex> lock(mtx_);
    auto it = windows_.find(name);
    if (it == windows_.end()) return false;
    img = it->second->sink.next();
    return !img.empty();
}

bool WindowManager::getImage(const std::string& name, cv::UMat& img) const {
    std::unique_lock<std::mutex> lock(mtx_);
    auto it = windows_.find(name);
    if (it == windows_.end()) return false;
    img = it->second->sink.frame();
    return !img.empty();
}

WindowData* WindowManager::getWindow(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = windows_.find(name);
    if (it != windows_.end()) {
        return it->second.get();
    }
    return nullptr;
}

const WindowData* WindowManager::getWindow(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = windows_.find(name);
    if (it != windows_.end()) {
        return it->second.get();
    }
    return nullptr;
}

std::shared_ptr<WindowData> WindowManager::getWindowShared(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = windows_.find(name);
    if (it != windows_.end()) {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<const WindowData> WindowManager::getWindowShared(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = windows_.find(name);
    if (it != windows_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::string> WindowManager::getWindowNames() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::string> names;
    for (const auto& kv : windows_) {
        names.push_back(kv.first);
    }
    return names;
}

void WindowManager::setRunning(bool r) {
    std::lock_guard<std::mutex> lock(mtx_);
    running_ = r;
    if (r) cv_.notify_all();
}

bool WindowManager::isRunning() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return running_;
}

void WindowManager::waitForWindow() {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this]{ return !windows_.empty(); });
}

void WindowManager::notifyAll() {
    cv_.notify_all();
}

size_t WindowManager::windowCount() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return windows_.size();
}

}
}
}
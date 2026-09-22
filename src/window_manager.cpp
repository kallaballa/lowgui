#include <opencv2/lowgui/window_manager.hpp>
#include <vector>
#include <algorithm>

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
        windowOrder_.push_back(name);
        generation_.fetch_add(1, std::memory_order_relaxed);
    }
}

void WindowManager::destroyWindow(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx_);
    windows_.erase(name);
    windowOrder_.erase(std::remove(windowOrder_.begin(), windowOrder_.end(), name), windowOrder_.end());
    generation_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_all();
}

void WindowManager::destroyAllWindows() {
    std::lock_guard<std::mutex> lock(mtx_);
    windows_.clear();
    windowOrder_.clear();
    generation_.fetch_add(1, std::memory_order_relaxed);
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
    it->second->sink->push(img);
    it->second->contentSerial.fetch_add(1, std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_relaxed);
}

bool WindowManager::popImage(const std::string& name, cv::UMat& img) {
    std::unique_lock<std::mutex> lock(mtx_);
    auto it = windows_.find(name);
    if (it == windows_.end()) return false;
    img = it->second->sink->next();
    return !img.empty();
}

bool WindowManager::getImage(const std::string& name, cv::UMat& img) const {
    std::unique_lock<std::mutex> lock(mtx_);
    auto it = windows_.find(name);
    if (it == windows_.end()) return false;
    img = it->second->sink->frame();
    return !img.empty();
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
    return windowOrder_;
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

void WindowManager::setMouseCallback(const std::string& name, MouseCallback cb, void* userdata) {
    auto wd = getWindowShared(name);
    if (!wd) return;
    std::lock_guard<std::mutex> lock(wd->sink->mtx);
    wd->mouseCb = cb;
    wd->mouseUserdata = userdata;
}

bool WindowManager::createTrackbar(const std::string& tb, const std::string& win,
                                   int* val, int count, TrackbarCallback cb, void* ud) {
    if (win.empty()) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = controlTrackbars_.find(tb);
        if (it == controlTrackbars_.end()) {
            Trackbar t;
            t.name = tb;
            t.winname = win;
            t.value = val;
            t.max = count;
            t.cb = cb;
            t.userdata = ud;
            t.pos = val ? std::clamp(*val, t.min, t.max) : 0;
            if (val) *val = t.pos;
            controlTrackbars_[tb] = t;
        } else {
            Trackbar& t = it->second;
            t.value = val;
            t.cb = cb;
            t.userdata = ud;
            t.max = count;
            if (t.pos > t.max) t.pos = t.max;
            if (val) *val = t.pos;
        }
        return true;
    }
    auto wd = getWindowShared(win);
    if (!wd) return false;
    std::lock_guard<std::mutex> lock(wd->sink->mtx);
    auto it = wd->trackbars.find(tb);
    if (it == wd->trackbars.end()) {
        Trackbar t;
        t.name = tb;
        t.winname = win;
        t.value = val;
        t.max = count;
        t.cb = cb;
        t.userdata = ud;
        t.pos = val ? std::clamp(*val, t.min, t.max) : 0;
        if (val) *val = t.pos;
        wd->trackbars[tb] = t;
    } else {
        Trackbar& t = it->second;
        t.name = tb;
        t.value = val;
        t.cb = cb;
        t.userdata = ud;
        t.max = count;
        if (t.pos > t.max) t.pos = t.max;
        if (val) *val = t.pos;
    }
    return true;
}

bool WindowManager::getTrackbar(const std::string& tb, const std::string& win, Trackbar& out) const {
    if (win.empty()) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = controlTrackbars_.find(tb);
        if (it == controlTrackbars_.end()) return false;
        out = it->second;
        return true;
    }
    auto wd = getWindowShared(win);
    if (!wd) return false;
    std::lock_guard<std::mutex> lock(wd->sink->mtx);
    auto it = wd->trackbars.find(tb);
    if (it == wd->trackbars.end()) return false;
    out = it->second;
    return true;
}

int WindowManager::getTrackbarPos(const std::string& tb, const std::string& win) const {
    Trackbar t;
    return getTrackbar(tb, win, t) ? t.pos : -1;
}

namespace {
void dispatchTrackbarCallback(const detail::Trackbar& t) {
    if (t.cb) t.cb(t.pos, t.userdata);
}
}

bool WindowManager::setTrackbarPos(const std::string& tb, const std::string& win, int pos) {
    Trackbar updated;
    bool changed = false;
    if (win.empty()) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = controlTrackbars_.find(tb);
        if (it == controlTrackbars_.end()) return false;
        int np = std::clamp(pos, it->second.min, it->second.max);
        if (np != it->second.pos) {
            it->second.pos = np;
            if (it->second.value) *it->second.value = np;
            updated = it->second;
            changed = true;
        }
    } else {
        auto wd = getWindowShared(win);
        if (!wd) return false;
        std::lock_guard<std::mutex> lock(wd->sink->mtx);
        auto it = wd->trackbars.find(tb);
        if (it == wd->trackbars.end()) return false;
        int np = std::clamp(pos, it->second.min, it->second.max);
        if (np != it->second.pos) {
            it->second.pos = np;
            if (it->second.value) *it->second.value = np;
            updated = it->second;
            changed = true;
        }
    }
    if (changed) dispatchTrackbarCallback(updated);
    return changed;
}

int WindowManager::updateTrackbarPos(const std::string& tb, const std::string& win, int pos) {
    Trackbar updated;
    bool changed = false;
    if (win.empty()) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = controlTrackbars_.find(tb);
        if (it == controlTrackbars_.end()) return -1;
        int np = std::clamp(pos, it->second.min, it->second.max);
        if (np != it->second.pos) {
            it->second.pos = np;
            if (it->second.value) *it->second.value = np;
            updated = it->second;
            changed = true;
        }
        if (!changed) return it->second.pos;
    } else {
        auto wd = getWindowShared(win);
        if (!wd) return -1;
        std::lock_guard<std::mutex> lock(wd->sink->mtx);
        auto it = wd->trackbars.find(tb);
        if (it == wd->trackbars.end()) return -1;
        int np = std::clamp(pos, it->second.min, it->second.max);
        if (np != it->second.pos) {
            it->second.pos = np;
            if (it->second.value) *it->second.value = np;
            updated = it->second;
            changed = true;
        }
        if (!changed) return it->second.pos;
    }
    if (changed) dispatchTrackbarCallback(updated);
    return updated.pos;
}

bool WindowManager::setTrackbarMin(const std::string& tb, const std::string& win, int minval) {
    Trackbar updated;
    bool changed = false;
    if (win.empty()) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = controlTrackbars_.find(tb);
        if (it == controlTrackbars_.end()) return false;
        it->second.min = minval;
        if (it->second.max < it->second.min) it->second.max = it->second.min;
        if (it->second.pos < it->second.min) {
            it->second.pos = it->second.min;
            if (it->second.value) *it->second.value = it->second.pos;
            updated = it->second;
            changed = true;
        }
    } else {
        auto wd = getWindowShared(win);
        if (!wd) return false;
        std::lock_guard<std::mutex> lock(wd->sink->mtx);
        auto it = wd->trackbars.find(tb);
        if (it == wd->trackbars.end()) return false;
        it->second.min = minval;
        if (it->second.max < it->second.min) it->second.max = it->second.min;
        if (it->second.pos < it->second.min) {
            it->second.pos = it->second.min;
            if (it->second.value) *it->second.value = it->second.pos;
            updated = it->second;
            changed = true;
        }
    }
    if (changed) dispatchTrackbarCallback(updated);
    return true;
}

bool WindowManager::setTrackbarMax(const std::string& tb, const std::string& win, int maxval) {
    Trackbar updated;
    bool changed = false;
    if (win.empty()) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = controlTrackbars_.find(tb);
        if (it == controlTrackbars_.end()) return false;
        it->second.max = maxval;
        if (it->second.max < it->second.min) it->second.max = it->second.min;
        if (it->second.pos > it->second.max) {
            it->second.pos = it->second.max;
            if (it->second.value) *it->second.value = it->second.pos;
            updated = it->second;
            changed = true;
        }
    } else {
        auto wd = getWindowShared(win);
        if (!wd) return false;
        std::lock_guard<std::mutex> lock(wd->sink->mtx);
        auto it = wd->trackbars.find(tb);
        if (it == wd->trackbars.end()) return false;
        it->second.max = maxval;
        if (it->second.max < it->second.min) it->second.max = it->second.min;
        if (it->second.pos > it->second.max) {
            it->second.pos = it->second.max;
            if (it->second.value) *it->second.value = it->second.pos;
            updated = it->second;
            changed = true;
        }
    }
    if (changed) dispatchTrackbarCallback(updated);
    return true;
}

std::vector<Trackbar> WindowManager::getWindowTrackbars(const std::string& win) const {
    std::vector<Trackbar> result;
    auto wd = getWindowShared(win);
    if (!wd) return result;
    std::lock_guard<std::mutex> lock(wd->sink->mtx);
    result.reserve(wd->trackbars.size());
    for (const auto& [n, t] : wd->trackbars) result.push_back(t);
    return result;
}

std::vector<Trackbar> WindowManager::getControlTrackbars() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<Trackbar> result;
    result.reserve(controlTrackbars_.size());
    for (const auto& [n, t] : controlTrackbars_) result.push_back(t);
    return result;
}

bool WindowManager::hasControlPanelContent() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return !controlTrackbars_.empty() || !controlButtons_.empty();
}

int WindowManager::createButton(const std::string& name, ButtonCallback cb, void* ud,
                                int type, bool initial) {
    std::lock_guard<std::mutex> lock(mtx_);
    Button btn;
    btn.name = name;
    btn.type = (type & ~QT_NEW_BUTTONBAR);
    btn.cb = cb;
    btn.userdata = ud;
    if (type & QT_NEW_BUTTONBAR) {
        btn.barId = ++nextBarId_;
    } else {
        btn.barId = controlButtons_.empty() ? ++nextBarId_
                                            : controlButtons_.back().barId;
    }
    btn.state = (btn.type == QT_PUSH_BUTTON) ? -1 : (initial ? 1 : 0);
    controlButtons_.push_back(btn);
    return btn.barId;
}

std::vector<Button> WindowManager::getControlButtons() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return controlButtons_;
}

int WindowManager::setButtonState(const std::string& name, int state) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& b : controlButtons_) {
        if (b.name == name) {
            int prev = b.state;
            b.state = state;
            return prev;
        }
    }
    return -1;
}

void WindowManager::setProperty(const std::string& name, int prop, int value) {
    auto wd = getWindowShared(name);
    if (!wd) return;
    std::lock_guard<std::mutex> lock(wd->sink->mtx);
    switch (prop) {
        case WND_PROP_AUTOSIZE:
            wd->propAutosize = (value == WINDOW_AUTOSIZE) ? WINDOW_AUTOSIZE : 0;
            break;
        case WND_PROP_ASPECT_RATIO:
            wd->propKeepRatio = (value == WINDOW_KEEPRATIO) ? WINDOW_KEEPRATIO : 0;
            break;
        case WND_PROP_FULLSCREEN:
            wd->propFullscreen = (value == WINDOW_FULLSCREEN) ? WINDOW_FULLSCREEN : 0;
            break;
        case WND_PROP_VISIBLE:
            wd->propVisible = value ? 1 : 0;
            break;
        // WND_PROP_OPENGL / WND_PROP_TOPMOST / WND_PROP_VSYNC: set is a no-op
        // (Qt parity; get returns -1).
        default: break;
    }
}

int WindowManager::getProperty(const std::string& name, int prop) const {
    auto wd = getWindowShared(name);
    if (!wd) return -1;
    std::lock_guard<std::mutex> lock(wd->sink->mtx);
    switch (prop) {
        case WND_PROP_AUTOSIZE:
            return wd->propAutosize ? WINDOW_AUTOSIZE : WINDOW_NORMAL;
        case WND_PROP_ASPECT_RATIO:
            return wd->propKeepRatio ? WINDOW_KEEPRATIO : WINDOW_FREERATIO;
        case WND_PROP_FULLSCREEN:
            return wd->propFullscreen ? WINDOW_FULLSCREEN : WINDOW_NORMAL;
        case WND_PROP_VISIBLE:
            return wd->propVisible ? 1 : 0;
        case WND_PROP_OPENGL:
        case WND_PROP_TOPMOST:
        case WND_PROP_VSYNC:
        default:
            return -1;
    }
}

void WindowManager::setMessage(const std::string& name, const std::string& text,
                               int delayms, bool overlay) {
    auto wd = getWindowShared(name);
    if (!wd) return;
    std::lock_guard<std::mutex> lock(wd->sink->mtx);
    if (overlay) wd->overlayMsg.set(text, delayms);
    else wd->statusMsg.set(text, delayms);
}

}
}
}
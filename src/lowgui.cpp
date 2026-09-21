#include <opencv2/lowgui/lowgui.hpp>
#include <opencv2/lowgui/window_manager.hpp>
#include <opencv2/lowgui/lowgui_root_plan.hpp>
#include <opencv2/v4d/v4d.hpp>

using namespace cv::lowgui;
using namespace cv::lowgui::detail;
using namespace cv::v4d;

void Lowgui::namedWindow(const std::string& winname, int flags) {
    WindowManager::instance().createWindow(winname, flags);
}

void Lowgui::imshow(const std::string& winname, InputArray mat) {
    if (mat.empty()) return;
    cv::Mat cpu = mat.getMat();
    cv::UMat umat = cpu.getUMat(cv::ACCESS_READ);
    WindowManager::instance().pushImage(winname, umat);
}

int Lowgui::waitKey(int delay) {
    if (!WindowManager::instance().isRunning()) {
        WindowManager::instance().setRunning(true);

        cv::Rect viewport(0, 0, 960, 960);
        cv::Ptr<V4D> runtime = V4D::init(viewport, "lowgui",
                                         AllocateFlags::NANOVG | AllocateFlags::IMGUI,
                                         ConfigFlags::DISPLAY_MODE);

        V4DPlan::run<LowguiRootPlan>(0);
        WindowManager::instance().setRunning(false);
    }
    return -1;
}

int Lowgui::pollKey() {
    return -1;
}

int Lowgui::waitKeyEx(int delay) {
    return waitKey(delay);
}

void Lowgui::destroyWindow(const std::string& winname) {
    WindowManager::instance().destroyWindow(winname);
}

void Lowgui::destroyAllWindows() {
    WindowManager::instance().destroyAllWindows();
}

bool Lowgui::hasWindow(const std::string& winname) {
    return WindowManager::instance().getWindow(winname) != nullptr;
}

void Lowgui::resizeWindow(const std::string& winname, int width, int height) {
    WindowData* wd = WindowManager::instance().getWindow(winname);
    if (wd) {
        wd->viewport.width = width;
        wd->viewport.height = height;
    }
}

void Lowgui::moveWindow(const std::string& winname, int x, int y) {
    WindowData* wd = WindowManager::instance().getWindow(winname);
    if (wd) {
        wd->viewport.x = x;
        wd->viewport.y = y;
    }
}

void Lowgui::setWindowTitle(const std::string& winname, const std::string& title) {
    WindowData* wd = WindowManager::instance().getWindow(winname);
    if (wd) {
        wd->title = title;
    }
}

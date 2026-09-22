#include "test_precomp.hpp"
#include <opencv2/lowgui/window_manager.hpp>
#include <thread>
#include <chrono>

namespace opencv_test {
namespace {
using namespace cv::lowgui::detail;

class WindowManagerTest : public testing::Test {
protected:
    void SetUp() override {
        WindowManager::instance().destroyAllWindows();
        WindowManager::instance().setRunning(false);
    }

    void TearDown() override {
        WindowManager::instance().destroyAllWindows();
        WindowManager::instance().setRunning(false);
    }
};

TEST_F(WindowManagerTest, createWindow_hasWindow_windowCount) {
    WindowManager::instance().createWindow("win1", 0);
    EXPECT_TRUE(WindowManager::instance().hasWindow("win1"));
    EXPECT_EQ(WindowManager::instance().windowCount(), 1u);

    WindowManager::instance().createWindow("win2", 0);
    EXPECT_TRUE(WindowManager::instance().hasWindow("win2"));
    EXPECT_EQ(WindowManager::instance().windowCount(), 2u);
}

TEST_F(WindowManagerTest, createWindow_duplicate_name_is_idempotent) {
    WindowManager::instance().createWindow("win1", 0);
    WindowManager::instance().createWindow("win1", 0);
    EXPECT_EQ(WindowManager::instance().windowCount(), 1u);
}

TEST_F(WindowManagerTest, destroyWindow_removes_window) {
    WindowManager::instance().createWindow("win1", 0);
    WindowManager::instance().destroyWindow("win1");
    EXPECT_FALSE(WindowManager::instance().hasWindow("win1"));
    EXPECT_EQ(WindowManager::instance().windowCount(), 0u);
}

TEST_F(WindowManagerTest, destroyWindow_nonexistent_is_safe) {
    EXPECT_NO_THROW(WindowManager::instance().destroyWindow("nonexistent"));
}

TEST_F(WindowManagerTest, destroyAllWindows_clears_all) {
    WindowManager::instance().createWindow("win1", 0);
    WindowManager::instance().createWindow("win2", 0);
    WindowManager::instance().destroyAllWindows();
    EXPECT_EQ(WindowManager::instance().windowCount(), 0u);
    EXPECT_FALSE(WindowManager::instance().hasWindow("win1"));
    EXPECT_FALSE(WindowManager::instance().hasWindow("win2"));
}

TEST_F(WindowManagerTest, getWindow_returns_valid_pointer) {
    WindowManager::instance().createWindow("win1", 0);
    auto wd = WindowManager::instance().getWindowShared("win1");
    ASSERT_NE(wd, nullptr);
    EXPECT_EQ(wd->name, "win1");
}

TEST_F(WindowManagerTest, getWindow_returns_nullptr_for_missing) {
    EXPECT_EQ(WindowManager::instance().getWindowShared("nonexistent"), nullptr);
}

TEST_F(WindowManagerTest, getWindowNames_returns_all_names) {
    WindowManager::instance().createWindow("win1", 0);
    WindowManager::instance().createWindow("win2", 0);
    auto names = WindowManager::instance().getWindowNames();
    ASSERT_EQ(names.size(), 2u);
    EXPECT_TRUE(std::find(names.begin(), names.end(), "win1") != names.end());
    EXPECT_TRUE(std::find(names.begin(), names.end(), "win2") != names.end());
}

TEST_F(WindowManagerTest, pushImage_getImage_roundtrip) {
    WindowManager::instance().createWindow("win1", 0);
    cv::Mat src = cv::Mat::zeros(10, 10, CV_8UC3);
    cv::UMat umat(10, 10, CV_8UC3);
    umat.setTo(cv::Scalar(10, 20, 30));
    WindowManager::instance().pushImage("win1", umat);

    cv::UMat dst;
    EXPECT_TRUE(WindowManager::instance().getImage("win1", dst));
    EXPECT_FALSE(dst.empty());
    EXPECT_EQ(dst.cols, 10);
    EXPECT_EQ(dst.rows, 10);
}

TEST_F(WindowManagerTest, pushImage_popImage_roundtrip) {
    WindowManager::instance().createWindow("win1", 0);
    cv::Mat src = cv::Mat::zeros(8, 8, CV_8UC1);
    src.setTo(128);
    cv::UMat umat(8, 8, CV_8UC1);
    umat.setTo(128);
    WindowManager::instance().pushImage("win1", umat);

    cv::UMat dst;
    EXPECT_TRUE(WindowManager::instance().popImage("win1", dst));
    EXPECT_FALSE(dst.empty());
    EXPECT_EQ(dst.cols, 8);
    EXPECT_EQ(dst.rows, 8);

    cv::UMat dst2;
    EXPECT_FALSE(WindowManager::instance().popImage("win1", dst2));
}

TEST_F(WindowManagerTest, pushImage_empty_image_is_ignored) {
    WindowManager::instance().createWindow("win1", 0);
    cv::UMat empty;
    WindowManager::instance().pushImage("win1", empty);
    cv::UMat dst;
    EXPECT_FALSE(WindowManager::instance().getImage("win1", dst));
}

TEST_F(WindowManagerTest, pushImage_grayscale_color_and_empty) {
    WindowManager::instance().createWindow("win1", 0);
    WindowManager::instance().createWindow("win2", 0);
    WindowManager::instance().createWindow("win3", 0);

    cv::UMat grayU(5, 5, CV_8UC1);
    cv::UMat colorU(5, 5, CV_8UC3);
    grayU.setTo(0);
    colorU.setTo(0);

    WindowManager::instance().pushImage("win1", grayU);
    WindowManager::instance().pushImage("win2", colorU);

    cv::UMat dst1, dst2;
    EXPECT_TRUE(WindowManager::instance().getImage("win1", dst1));
    EXPECT_TRUE(WindowManager::instance().getImage("win2", dst2));
    EXPECT_EQ(dst1.channels(), 1);
    EXPECT_EQ(dst2.channels(), 3);
}

TEST_F(WindowManagerTest, pushImage_thread_safety) {
    WindowManager::instance().createWindow("win1", 0);
    const int kThreads = 4;
    const int kIterations = 50;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t]() {
            for (int i = 0; i < kIterations; ++i) {
                cv::UMat umat(4, 4, CV_8UC1);
                umat.setTo(t * 10 + i);
                WindowManager::instance().pushImage("win1", umat);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    cv::UMat dst;
    EXPECT_TRUE(WindowManager::instance().getImage("win1", dst));
    EXPECT_FALSE(dst.empty());
}

TEST_F(WindowManagerTest, getWindow_const_overload) {
    WindowManager::instance().createWindow("win1", 0);
    const WindowManager& wm = WindowManager::instance();
    auto wd = wm.getWindowShared("win1");
    ASSERT_NE(wd, nullptr);
    EXPECT_EQ(wd->name, "win1");
}

TEST_F(WindowManagerTest, isRunning_setRunning) {
    EXPECT_FALSE(WindowManager::instance().isRunning());
    WindowManager::instance().setRunning(true);
    EXPECT_TRUE(WindowManager::instance().isRunning());
    WindowManager::instance().setRunning(false);
    EXPECT_FALSE(WindowManager::instance().isRunning());
}

TEST_F(WindowManagerTest, destroyWindow_clears_image_buffer) {
    WindowManager::instance().createWindow("win1", 0);
    cv::Mat src = cv::Mat::zeros(3, 3, CV_8UC1);
    src.setTo(42);
    cv::UMat umat = src.getUMat(cv::ACCESS_READ);
    WindowManager::instance().pushImage("win1", umat);

    WindowManager::instance().destroyWindow("win1");
    WindowManager::instance().createWindow("win1", 0);

    cv::UMat dst;
    EXPECT_FALSE(WindowManager::instance().getImage("win1", dst));
}

TEST_F(WindowManagerTest, waitForWindow_returns_once_a_window_appears) {
    // L8: waitForWindow previously had no callers (dead API) and createWindow
    // never notified the condition variable, so a waiter could block forever.
    std::thread creator([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        WindowManager::instance().createWindow("late_win", 0);
    });
    WindowManager::instance().waitForWindow();
    creator.join();
    EXPECT_TRUE(WindowManager::instance().hasWindow("late_win"));
    WindowManager::instance().destroyAllWindows();
}

TEST_F(WindowManagerTest, waitForWindow_returns_immediately_when_windows_exist) {
    // L8 caller: the predicate short-circuits when a window is already present.
    WindowManager::instance().createWindow("present_win", 0);
    auto start = std::chrono::steady_clock::now();
    WindowManager::instance().waitForWindow();
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::milliseconds(100));
}

TEST_F(WindowManagerTest, notifyAll_is_safe_to_call_without_state_change) {
    // L8 caller: notifyAll broadcasts without touching the window map; a waiter
    // whose predicate is still false must simply keep waiting, never crash.
    EXPECT_NO_THROW(WindowManager::instance().notifyAll());
    WindowManager::instance().createWindow("w", 0);
    EXPECT_NO_THROW(WindowManager::instance().notifyAll());
}

} // namespace
} // namespace opencv_test

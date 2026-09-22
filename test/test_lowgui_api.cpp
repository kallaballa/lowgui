#include "test_precomp.hpp"
#include <opencv2/lowgui/lowgui.hpp>
#include "lowgui_root_plan.hpp"
#include <thread>
#include <chrono>

namespace opencv_test {
namespace {
using namespace cv::lowgui;

using KeyboardKey = cv::v4d::event::Keyboard::Key;

class LowguiApiTest : public testing::Test {
protected:
    void SetUp() override {
        Lowgui::destroyAllWindows();
    }

    void TearDown() override {
        Lowgui::destroyAllWindows();
    }
};

TEST_F(LowguiApiTest, namedWindow_hasWindow_lifecycle) {
    EXPECT_FALSE(Lowgui::hasWindow("test_win"));
    Lowgui::namedWindow("test_win");
    EXPECT_TRUE(Lowgui::hasWindow("test_win"));
    Lowgui::destroyWindow("test_win");
    EXPECT_FALSE(Lowgui::hasWindow("test_win"));
}

TEST_F(LowguiApiTest, namedWindow_flags_default) {
    Lowgui::namedWindow("win_flags");
    EXPECT_TRUE(Lowgui::hasWindow("win_flags"));
    Lowgui::destroyWindow("win_flags");
}

TEST_F(LowguiApiTest, namedWindow_duplicate_is_idempotent) {
    Lowgui::namedWindow("dup_win");
    Lowgui::namedWindow("dup_win");
    EXPECT_TRUE(Lowgui::hasWindow("dup_win"));
    Lowgui::destroyAllWindows();
    EXPECT_FALSE(Lowgui::hasWindow("dup_win"));
}

TEST_F(LowguiApiTest, imshow_stores_image_data) {
    Lowgui::namedWindow("img_win");
    cv::Mat img = cv::Mat::zeros(10, 10, CV_8UC1);
    img.setTo(128);
    Lowgui::imshow("img_win", img);
    EXPECT_TRUE(Lowgui::hasWindow("img_win"));
    Lowgui::destroyAllWindows();
}

TEST_F(LowguiApiTest, imshow_empty_is_ignored) {
    Lowgui::namedWindow("empty_win");
    cv::Mat empty;
    EXPECT_NO_THROW(Lowgui::imshow("empty_win", empty));
    Lowgui::destroyAllWindows();
}

TEST_F(LowguiApiTest, imshow_different_sizes) {
    Lowgui::namedWindow("size_win");
    cv::Mat small = cv::Mat::zeros(5, 5, CV_8UC1);
    cv::Mat large = cv::Mat::zeros(100, 100, CV_8UC3);
    Lowgui::imshow("size_win", small);
    Lowgui::imshow("size_win", large);
    EXPECT_TRUE(Lowgui::hasWindow("size_win"));
    Lowgui::destroyAllWindows();
}

TEST_F(LowguiApiTest, resizeWindow_updates_viewport) {
    Lowgui::namedWindow("resize_win");
    Lowgui::resizeWindow("resize_win", 640, 480);
    EXPECT_TRUE(Lowgui::hasWindow("resize_win"));
    Lowgui::destroyAllWindows();
}

TEST_F(LowguiApiTest, resizeWindow_nonexistent_is_safe) {
    EXPECT_NO_THROW(Lowgui::resizeWindow("nonexistent", 100, 100));
}

TEST_F(LowguiApiTest, moveWindow_updates_position) {
    Lowgui::namedWindow("move_win");
    Lowgui::moveWindow("move_win", 100, 200);
    EXPECT_TRUE(Lowgui::hasWindow("move_win"));
    Lowgui::destroyAllWindows();
}

TEST_F(LowguiApiTest, moveWindow_nonexistent_is_safe) {
    EXPECT_NO_THROW(Lowgui::moveWindow("nonexistent", 0, 0));
}

TEST_F(LowguiApiTest, setWindowTitle_updates_title) {
    Lowgui::namedWindow("title_win");
    Lowgui::setWindowTitle("title_win", "New Title");
    EXPECT_TRUE(Lowgui::hasWindow("title_win"));
    Lowgui::destroyAllWindows();
}

TEST_F(LowguiApiTest, aspect_ratio_property_roundtrip) {
    // Regression guard: WINDOW_KEEPRATIO is 0x00000000 (same value as
    // WINDOW_NORMAL), so it must roundtrip through the store instead of being
    // indistinguishable from "unset" (which silently flipped every window to
    // WINDOW_FREERATIO / anisotropic stretch). Default is KEEPRATIO.
    Lowgui::namedWindow("ratio_win");
    EXPECT_EQ(Lowgui::getWindowProperty("ratio_win", cv::lowgui::WND_PROP_ASPECT_RATIO),
              cv::lowgui::WINDOW_KEEPRATIO);

    Lowgui::setWindowProperty("ratio_win", cv::lowgui::WND_PROP_ASPECT_RATIO,
                              cv::lowgui::WINDOW_FREERATIO);
    EXPECT_EQ(Lowgui::getWindowProperty("ratio_win", cv::lowgui::WND_PROP_ASPECT_RATIO),
              cv::lowgui::WINDOW_FREERATIO);

    Lowgui::setWindowProperty("ratio_win", cv::lowgui::WND_PROP_ASPECT_RATIO,
                              cv::lowgui::WINDOW_KEEPRATIO);
    EXPECT_EQ(Lowgui::getWindowProperty("ratio_win", cv::lowgui::WND_PROP_ASPECT_RATIO),
              cv::lowgui::WINDOW_KEEPRATIO);
    Lowgui::destroyAllWindows();
}

TEST_F(LowguiApiTest, setWindowTitle_nonexistent_is_safe) {
    EXPECT_NO_THROW(Lowgui::setWindowTitle("nonexistent", "title"));
}

TEST_F(LowguiApiTest, destroyWindow_removes_window_and_clears_state) {
    Lowgui::namedWindow("del_win");
    Lowgui::imshow("del_win", cv::Mat::zeros(10, 10, CV_8UC1));
    Lowgui::destroyWindow("del_win");
    EXPECT_FALSE(Lowgui::hasWindow("del_win"));
}

TEST_F(LowguiApiTest, destroyAllWindows_clears_all_state) {
    Lowgui::namedWindow("win1");
    Lowgui::namedWindow("win2");
    Lowgui::imshow("win1", cv::Mat::zeros(10, 10, CV_8UC1));
    Lowgui::imshow("win2", cv::Mat::zeros(10, 10, CV_8UC1));
    Lowgui::destroyAllWindows();
    EXPECT_FALSE(Lowgui::hasWindow("win1"));
    EXPECT_FALSE(Lowgui::hasWindow("win2"));
}

TEST_F(LowguiApiTest, pollKey_returns_minus_one_without_input) {
    int key = Lowgui::pollKey();
    EXPECT_EQ(key, -1);
}

TEST_F(LowguiApiTest, waitKey_returns_promptly_with_zero_delay) {
    auto start = std::chrono::steady_clock::now();
    int key = Lowgui::waitKey(0);
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(key, -1);
    EXPECT_LT(elapsed, std::chrono::seconds(5));
}

TEST_F(LowguiApiTest, waitKeyEx_returns_promptly_with_zero_delay) {
    auto start = std::chrono::steady_clock::now();
    int key = Lowgui::waitKeyEx(0);
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(key, -1);
    EXPECT_LT(elapsed, std::chrono::seconds(5));
}

TEST_F(LowguiApiTest, waitKey_with_positive_delay_returns_minus_one) {
    auto start = std::chrono::steady_clock::now();
    int key = Lowgui::waitKey(10);
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(key, -1);
    EXPECT_GE(elapsed, std::chrono::milliseconds(5));
}

TEST_F(LowguiApiTest, multiple_windows_independent_lifecycle) {
    Lowgui::namedWindow("win_a");
    Lowgui::namedWindow("win_b");
    Lowgui::imshow("win_a", cv::Mat::zeros(5, 5, CV_8UC1));
    Lowgui::imshow("win_b", cv::Mat::zeros(5, 5, CV_8UC3));
    EXPECT_TRUE(Lowgui::hasWindow("win_a"));
    EXPECT_TRUE(Lowgui::hasWindow("win_b"));
    Lowgui::destroyWindow("win_a");
    EXPECT_FALSE(Lowgui::hasWindow("win_a"));
    EXPECT_TRUE(Lowgui::hasWindow("win_b"));
    Lowgui::destroyAllWindows();
}

TEST_F(LowguiApiTest, namedWindow_then_imshow_does_not_crash) {
    Lowgui::namedWindow("crash_win");
    Lowgui::imshow("crash_win", cv::Mat::zeros(10, 10, CV_8UC1));
    Lowgui::imshow("crash_win", cv::Mat::zeros(10, 10, CV_8UC3));
    Lowgui::imshow("crash_win", cv::Mat::zeros(10, 10, CV_8UC4));
    Lowgui::destroyAllWindows();
}

} // namespace
} // namespace opencv_test

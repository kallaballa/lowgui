#include "test_precomp.hpp"
#include <opencv2/lowgui/lowgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <cstdlib>
#include <cmath>

namespace opencv_test {
namespace {

using namespace cv::lowgui;

static cv::UMat renderHeadless(const std::string& winname, const cv::Mat& img) {
    Lowgui::namedWindow(winname);
    Lowgui::imshow(winname, img);
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::waitKey(0);
    unsetenv("LOWGUI_HEADLESS_RENDER");
    return Lowgui::readFramebuffer();
}

class OffscreenRenderingTest : public testing::Test {
protected:
    void SetUp() override {
        Lowgui::destroyAllWindows();
        // The offscreen binary must never open a real window, even when a
        // display is present (e.g. under Xvfb): force the offscreen render
        // path so waitKey stays bounded in every test.
        setenv("LOWGUI_FORCE_OFFSCREEN", "1", 1);
    }

    void TearDown() override {
        Lowgui::destroyAllWindows();
        unsetenv("LOWGUI_HEADLESS_RENDER");
        unsetenv("LOWGUI_FORCE_OFFSCREEN");
    }
};

TEST_F(OffscreenRenderingTest, single_window_solid_red_fills_framebuffer) {
    cv::Mat red = cv::Mat::zeros(960, 960, CV_8UC3);
    red.setTo(cv::Scalar(0, 0, 255));
    cv::UMat fb = renderHeadless("red_win", red);
    ASSERT_FALSE(fb.empty());
    EXPECT_EQ(fb.cols, 960);
    EXPECT_EQ(fb.rows, 960);

    cv::Mat expected(960, 960, CV_8UC4, cv::Scalar(255, 0, 0, 255));
    EXPECT_LT(cv::norm(fb, expected, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, single_window_solid_blue_fills_framebuffer) {
    cv::Mat blue = cv::Mat::zeros(960, 960, CV_8UC3);
    blue.setTo(cv::Scalar(255, 0, 0));
    cv::UMat fb = renderHeadless("blue_win", blue);
    ASSERT_FALSE(fb.empty());
    EXPECT_EQ(fb.cols, 960);
    EXPECT_EQ(fb.rows, 960);

    cv::Mat expected(960, 960, CV_8UC4, cv::Scalar(0, 0, 255, 255));
    EXPECT_LT(cv::norm(fb, expected, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, two_windows_horizontal_split) {
    cv::Mat red = cv::Mat::zeros(480, 960, CV_8UC3);
    red.setTo(cv::Scalar(0, 0, 255));
    cv::Mat blue = cv::Mat::zeros(480, 960, CV_8UC3);
    blue.setTo(cv::Scalar(255, 0, 0));

    Lowgui::namedWindow("win_left");
    Lowgui::imshow("win_left", red);
    Lowgui::namedWindow("win_right");
    Lowgui::imshow("win_right", blue);

    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::waitKey(0);
    unsetenv("LOWGUI_HEADLESS_RENDER");

    cv::UMat fb = Lowgui::readFramebuffer();
    ASSERT_FALSE(fb.empty());
    EXPECT_EQ(fb.cols, 960);
    EXPECT_EQ(fb.rows, 960);

    cv::Mat fb_mat = fb.getMat(cv::ACCESS_READ);
    cv::Mat left_half = fb_mat(cv::Rect(0, 0, 480, 960));
    cv::Mat right_half = fb_mat(cv::Rect(480, 0, 480, 960));

    cv::Mat left_expected(960, 480, CV_8UC4, cv::Scalar(255, 0, 0, 255));
    cv::Mat right_expected(960, 480, CV_8UC4, cv::Scalar(0, 0, 255, 255));

    EXPECT_LT(cv::norm(left_half, left_expected, cv::NORM_INF), 3);
    EXPECT_LT(cv::norm(right_half, right_expected, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, four_windows_grid_layout) {
    cv::Mat red   = cv::Mat::zeros(480, 480, CV_8UC3); red.setTo(cv::Scalar(0, 0, 255));
    cv::Mat green = cv::Mat::zeros(480, 480, CV_8UC3); green.setTo(cv::Scalar(0, 255, 0));
    cv::Mat blue  = cv::Mat::zeros(480, 480, CV_8UC3); blue.setTo(cv::Scalar(255, 0, 0));
    cv::Mat yellow = cv::Mat::zeros(480, 480, CV_8UC3); yellow.setTo(cv::Scalar(0, 255, 255));

    Lowgui::namedWindow("r"); Lowgui::imshow("r", red);
    Lowgui::namedWindow("g"); Lowgui::imshow("g", green);
    Lowgui::namedWindow("b"); Lowgui::imshow("b", blue);
    Lowgui::namedWindow("y"); Lowgui::imshow("y", yellow);

    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::waitKey(0);
    unsetenv("LOWGUI_HEADLESS_RENDER");

    cv::UMat fb = Lowgui::readFramebuffer();
    ASSERT_FALSE(fb.empty());
    EXPECT_EQ(fb.cols, 960);
    EXPECT_EQ(fb.rows, 960);

    cv::Mat fb_mat = fb.getMat(cv::ACCESS_READ);
    cv::Mat q1 = fb_mat(cv::Rect(0,   0,   480, 480));
    cv::Mat q2 = fb_mat(cv::Rect(480, 0,   480, 480));
    cv::Mat q3 = fb_mat(cv::Rect(0,   480, 480, 480));
    cv::Mat q4 = fb_mat(cv::Rect(480, 480, 480, 480));

    cv::Mat exp_r(480, 480, CV_8UC4, cv::Scalar(255, 0,   0,   255));
    cv::Mat exp_g(480, 480, CV_8UC4, cv::Scalar(0,   255, 0,   255));
    cv::Mat exp_b(480, 480, CV_8UC4, cv::Scalar(0,   0,   255, 255));
    cv::Mat exp_y(480, 480, CV_8UC4, cv::Scalar(0,   255, 255, 255));

    EXPECT_LT(cv::norm(q1, exp_r, cv::NORM_INF), 3);
    EXPECT_LT(cv::norm(q2, exp_g, cv::NORM_INF), 3);
    EXPECT_LT(cv::norm(q3, exp_b, cv::NORM_INF), 3);
    EXPECT_LT(cv::norm(q4, exp_y, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, grayscale_image_renders_correctly) {
    cv::Mat gray = cv::Mat::zeros(960, 960, CV_8UC1);
    gray.setTo(128);
    cv::UMat fb = renderHeadless("gray_win", gray);
    ASSERT_FALSE(fb.empty());
    EXPECT_EQ(fb.cols, 960);
    EXPECT_EQ(fb.rows, 960);

    cv::Mat expected(960, 960, CV_8UC4, cv::Scalar(128, 128, 128, 255));
    EXPECT_LT(cv::norm(fb, expected, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, readFramebuffer_returns_empty_without_headless) {
    Lowgui::namedWindow("noheadless_win");
    Lowgui::imshow("noheadless_win", cv::Mat::zeros(10, 10, CV_8UC3));
    Lowgui::waitKey(0);

    cv::UMat fb = Lowgui::readFramebuffer();
    EXPECT_TRUE(fb.empty());
}

}
} // namespace opencv_test

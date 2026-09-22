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

TEST_F(OffscreenRenderingTest, unsupported_channel_count_does_not_kill_engine) {
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::namedWindow("c2_win");
    cv::Mat two(32, 32, CV_8UC2);
    two.setTo(0);
    Lowgui::imshow("c2_win", two);

    cv::Mat red = cv::Mat::zeros(960, 960, CV_8UC3);
    red.setTo(cv::Scalar(0, 0, 255));
    Lowgui::imshow("c2_win", red);
    Lowgui::waitKey(0);

    cv::UMat fb = Lowgui::readFramebuffer();
    ASSERT_FALSE(fb.empty());
    EXPECT_EQ(fb.cols, 960);
    EXPECT_EQ(fb.rows, 960);

    cv::Mat expected(960, 960, CV_8UC4, cv::Scalar(255, 0, 0, 255));
    EXPECT_LT(cv::norm(fb, expected, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, unsupported_depth_does_not_kill_engine) {
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::namedWindow("s32_win");
    cv::Mat s32(32, 32, CV_32SC1);
    s32.setTo(1);
    Lowgui::imshow("s32_win", s32);

    cv::Mat blue = cv::Mat::zeros(960, 960, CV_8UC3);
    blue.setTo(cv::Scalar(255, 0, 0));
    Lowgui::imshow("s32_win", blue);
    Lowgui::waitKey(0);

    cv::UMat fb = Lowgui::readFramebuffer();
    ASSERT_FALSE(fb.empty());
    cv::Mat expected(960, 960, CV_8UC4, cv::Scalar(0, 0, 255, 255));
    EXPECT_LT(cv::norm(fb, expected, cv::NORM_INF), 3);
}

TEST_F(OffscreenRenderingTest, readFramebuffer_returns_newest_settled_frame_after_churn) {
    // L7 stress: a sustained imshow stream churns the generation counter so a
    // capture may be skipped or delayed mid-churn; the final waitKey(0) must
    // capture the last (settled) frame, not a stale churned one.
    setenv("LOWGUI_HEADLESS_RENDER", "1", 1);
    Lowgui::namedWindow("churn_win");
    cv::Mat tmp(32, 32, CV_8UC3);
    for (int i = 0; i < 200; ++i) {
        tmp.setTo(cv::Scalar(i & 0xff, 0, (255 - i) & 0xff));
        Lowgui::imshow("churn_win", tmp);
    }

    cv::Mat green = cv::Mat::zeros(960, 960, CV_8UC3);
    green.setTo(cv::Scalar(0, 255, 0));
    Lowgui::imshow("churn_win", green);
    Lowgui::waitKey(0);

    cv::UMat fb = Lowgui::readFramebuffer();
    ASSERT_FALSE(fb.empty());
    EXPECT_EQ(fb.cols, 960);
    EXPECT_EQ(fb.rows, 960);
    cv::Mat expected(960, 960, CV_8UC4, cv::Scalar(0, 255, 0, 255));
    EXPECT_LT(cv::norm(fb, expected, cv::NORM_INF), 3);
}

}
} // namespace opencv_test

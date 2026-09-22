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

}
} // namespace opencv_test

#include <opencv2/lowgui/lowgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <algorithm>
#include <iostream>

using namespace cv;
using namespace cv::lowgui;

int main(int argc, char** argv) {
    // A single horizontal HSV rainbow gradient (H 0..179 over the width), so
    // the loop is bounded and the colour wraps instead of overflowing.
    for (int x = 0; x < 4096; ++x) {
        int hue = (x * 180 / 512) % 180;
        cv::UMat hsv(512, 512, CV_8UC3, cv::Scalar(hue, 255, 255));
        cv::UMat bgr(512, 512, CV_8UC3);
        cv::cvtColor(hsv, bgr, cv::COLOR_HSV2BGR);
        Lowgui::namedWindow("bgr");
        Lowgui::namedWindow("hsv");
	Lowgui::imshow("bgr", bgr);
	Lowgui::imshow("hsv", hsv);
	Lowgui::waitKey(0);
    }

    Lowgui::destroyAllWindows();
    return 0;
}

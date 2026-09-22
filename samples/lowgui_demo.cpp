#include <opencv2/lowgui/lowgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <iostream>

using namespace cv;
using namespace cv::lowgui;

int main(int argc, char** argv) {
    cv::UMat img(512, 512, CV_8UC3);
    int r = 0, g = 0, b = 0;

    for(size_t i = 0; i < 255*255; ++i) {
        img.setTo(cv::Scalar(++r, (++g + 95) % 255, (++b + 127) % 255));
        Lowgui::namedWindow("lowgui demo");
        Lowgui::imshow("lowgui demo", img);
        Lowgui::waitKey(1);
    }

    Lowgui::destroyAllWindows();
    return 0;
}

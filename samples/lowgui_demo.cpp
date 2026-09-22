#include <opencv2/lowgui/lowgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <iostream>

using namespace cv;
using namespace cv::lowgui;

int main(int argc, char** argv) {
    cv::UMat img(1024, 768, CV_8UC3);

    for(size_t i = 0; i < 255*255; ++i) {
        img.setTo(i);
        Lowgui::namedWindow("lowgui demo");
        Lowgui::imshow("lowgui demo", img);
        Lowgui::waitKey(0);
    }

    Lowgui::destroyAllWindows();
    return 0;
}

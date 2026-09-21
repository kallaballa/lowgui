#include <opencv2/lowgui/lowgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <iostream>

using namespace cv;
using namespace cv::lowgui;

int main(int argc, char** argv) {
    std::string imgFile = argc > 1 ? argv[1] : cv::samples::findFile("lena.png");
    cv::Mat img = cv::imread(imgFile);
    if (img.empty()) {
        std::cerr << "Could not load image: " << imgFile << std::endl;
        return 1;
    }

    Lowgui::namedWindow("lowgui demo");
    Lowgui::imshow("lowgui demo", img);
    Lowgui::waitKey(0);

    Lowgui::destroyAllWindows();
    return 0;
}

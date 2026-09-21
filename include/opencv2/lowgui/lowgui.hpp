#ifndef OPENCV_LOWGUI_LOWGUI_HPP_
#define OPENCV_LOWGUI_LOWGUI_HPP_

#include <opencv2/core.hpp>
#include <string>

namespace cv {
namespace lowgui {

//! @brief Window creation flags
enum WindowFlags {
    WINDOW_NORMAL   = 0x00000000, //!< user can resize the window
    WINDOW_AUTOSIZE = 0x00000001, //!< window size is constrained by the image
    WINDOW_OPENGL   = 0x00001000, //!< window with OpenGL support
};

/**
 * @brief The Lowgui class provides a clean-room reimplementation of OpenCV's highgui
 * image viewer on top of Plan-V4D. It maps window names to Plan-instances and
 * feeds them images through a SinkSource implementation.
 */
class CV_EXPORTS Lowgui {
public:
    /**
     * @brief Creates a window that can be used as a placeholder for images.
     * @param winname Name of the window.
     * @param flags Window flags (default: WINDOW_AUTOSIZE).
     */
    static void namedWindow(const std::string& winname, int flags = WINDOW_AUTOSIZE);

    /**
     * @brief Displays an image in the specified window.
     * @param winname Name of the window.
     * @param mat Image to be shown.
     */
    static void imshow(const std::string& winname, InputArray mat);

    /**
     * @brief Waits for a key event.
     * @param delay Delay in milliseconds. 0 means "forever".
     * @return The key code, or -1 if no key was pressed.
     */
    static int waitKey(int delay = 0);

    /**
     * @brief Destroys the specified window.
     * @param winname Name of the window to be destroyed.
     */
    static void destroyWindow(const std::string& winname);

    /**
     * @brief Destroys all of the opened windows.
     */
    static void destroyAllWindows();

    /**
     * @brief Checks if a window with the given name exists.
     * @param winname Window name.
     * @return true if the window exists.
     */
    static bool hasWindow(const std::string& winname);

    /**
     * @brief Resizes the window to the specified size.
     * @param winname Window name.
     * @param width New window width.
     * @param height New window height.
     */
    static void resizeWindow(const std::string& winname, int width, int height);

    /**
     * @brief Moves the window to the specified position.
     * @param winname Name of the window.
     * @param x The new x-coordinate.
     * @param y The new y-coordinate.
     */
    static void moveWindow(const std::string& winname, int x, int y);

    /**
     * @brief Updates the window title.
     * @param winname Name of the window.
     * @param title New title.
     */
    static void setWindowTitle(const std::string& winname, const std::string& title);

    /**
     * @brief Polls for a pressed key without waiting.
     * @return The key code, or -1 if no key was pressed.
     */
    static int pollKey();

    /**
     * @brief Waits for a key event, returning the full key code.
     * @param delay Delay in milliseconds. 0 means "forever".
     * @return The full key code, or -1 if no key was pressed.
     */
    static int waitKeyEx(int delay = 0);

    /**
     * @brief Reads the framebuffer from the last headless render.
     * Only valid when LOWGUI_HEADLESS_RENDER is set and waitKey has returned.
     * @return The framebuffer image, or empty UMat if not available.
     */
    static cv::UMat readFramebuffer();
};

} // namespace lowgui
} // namespace cv

#endif // OPENCV_LOWGUI_LOWGUI_HPP_
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 lowgui contributors
// Clean-room reimplementation of OpenCV highgui (see README.md).
#ifndef OPENCV_LOWGUI_LOWGUI_HPP_
#define OPENCV_LOWGUI_LOWGUI_HPP_

#include <opencv2/core.hpp>
#include <string>

namespace cv {
namespace lowgui {

//! @addtogroup lowgui_window_flags
//! @{

//! Flags for cv::lowgui::Lowgui::namedWindow
enum WindowFlags {
    WINDOW_NORMAL      = 0x00000000, //!< the user can resize the window
    WINDOW_AUTOSIZE    = 0x00000001, //!< the window size is constrained by the image
    WINDOW_OPENGL      = 0x00001000, //!< window with opengl support (no-op in lowgui)

    WINDOW_FULLSCREEN  = 1,          //!< change the window to fullscreen.
    WINDOW_FREERATIO   = 0x00000100, //!< the image expends as much as it can (no ratio constraint).
    WINDOW_KEEPRATIO   = 0x00000000, //!< the ratio of the image is respected.
    WINDOW_GUI_EXPANDED= 0x00000000, //!< show tool/status bar (control-panel style).
    WINDOW_GUI_NORMAL  = 0x00000010, //!< old fashioned way (no menu/status bar).
};

//! Flags for cv::lowgui::Lowgui::setWindowProperty / getWindowProperty
enum WindowPropertyFlags {
    WND_PROP_FULLSCREEN   = 0, //!< fullscreen property (WINDOW_NORMAL or WINDOW_FULLSCREEN).
    WND_PROP_AUTOSIZE     = 1, //!< autosize property   (WINDOW_NORMAL or WINDOW_AUTOSIZE).
    WND_PROP_ASPECT_RATIO = 2, //!< aspect ratio        (WINDOW_FREERATIO or WINDOW_KEEPRATIO).
    WND_PROP_OPENGL       = 3, //!< opengl support      (no-op; get returns -1).
    WND_PROP_VISIBLE      = 4, //!< checks whether the window exists and is visible.
    WND_PROP_TOPMOST      = 5, //!< topmost             (no-op; get returns -1).
    WND_PROP_VSYNC        = 6  //!< vsync               (no-op; get returns -1).
};

//! Mouse event codes, see cv::lowgui::Lowgui::setMouseCallback
enum MouseEventTypes {
    EVENT_MOUSEMOVE      = 0,
    EVENT_LBUTTONDOWN    = 1,
    EVENT_RBUTTONDOWN    = 2,
    EVENT_MBUTTONDOWN    = 3,
    EVENT_LBUTTONUP      = 4,
    EVENT_RBUTTONUP      = 5,
    EVENT_MBUTTONUP      = 6,
    EVENT_LBUTTONDBLCLK  = 7,
    EVENT_RBUTTONDBLCLK  = 8,
    EVENT_MBUTTONDBLCLK  = 9,
    EVENT_MOUSEWHEEL     = 10,
    EVENT_MOUSEHWHEEL    = 11
};

//! Mouse event flags, see cv::lowgui::Lowgui::setMouseCallback
enum MouseEventFlags {
    EVENT_FLAG_LBUTTON   = 1,
    EVENT_FLAG_RBUTTON   = 2,
    EVENT_FLAG_MBUTTON   = 4,
    EVENT_FLAG_CTRLKEY   = 8,
    EVENT_FLAG_SHIFTKEY  = 16,
    EVENT_FLAG_ALTKEY    = 32
};

//! Qt "button" types, see cv::lowgui::Lowgui::createButton
enum QtButtonTypes {
    QT_PUSH_BUTTON   = 0,
    QT_CHECKBOX      = 1,
    QT_RADIOBOX      = 2,
    QT_NEW_BUTTONBAR = 1024
};

//! @}

//! Callback function for mouse events. See cv::lowgui::Lowgui::setMouseCallback
typedef void (*MouseCallback)(int event, int x, int y, int flags, void* userdata);

//! Callback function for a trackbar. See cv::lowgui::Lowgui::createTrackbar
typedef void (*TrackbarCallback)(int pos, void* userdata);

//! Callback function for a button. See cv::lowgui::Lowgui::createButton
typedef void (*ButtonCallback)(int state, void* userdata);

/**
 * @brief The Lowgui class provides a clean-room reimplementation of OpenCV's highgui
 * image viewer on top of Plan-V4D. It maps window names to Plan-instances and
 * feeds them images through a SinkSource implementation.
 *
 * Every window behaves like a Qt `imshow` window: keys are delivered to
 * `waitKey`/`waitKeyEx`, mouse callbacks can be installed, trackbars and buttons
 * can be attached, and window properties follow the Qt semantics. Each logical
 * window is presented in its own native (GLFW) window.
 */
class CV_EXPORTS Lowgui {
public:
    // ---------- Basic windows ----------

    /** @brief Creates a window that can be used as a placeholder for images.
     * @param winname Name of the window.
     * @param flags Window flags (default: WINDOW_AUTOSIZE).
     */
    static void namedWindow(const std::string& winname, int flags = WINDOW_AUTOSIZE);

    /** @brief Displays an image in the specified window.
     * @param winname Name of the window.
     * @param mat Image to be shown.
     */
    static void imshow(const std::string& winname, InputArray mat);

    /** @brief Waits for a key event.
     * @param delay Delay in milliseconds. 0 means "forever" (block until a key is
     * pressed or the native window is closed).
     * @return The key code (lower 8 bits, like highgui), or -1 if no key was pressed.
     */
    static int waitKey(int delay = 0);

    /** @brief Polls for a pressed key (1 ms poll, i.e. `waitKey(1)`).
     * @return The key code (lower 8 bits), or -1 if no key was pressed.
     */
    static int pollKey();

    /** @brief Waits for a key event, returning the full key code.
     * @param delay Delay in milliseconds. 0 means "forever".
     * @return The full key code, or -1 if no key was pressed.
     */
    static int waitKeyEx(int delay = 0);

    /** @brief Destroys the specified window.
     * @param winname Name of the window to be destroyed.
     */
    static void destroyWindow(const std::string& winname);

    /** @brief Destroys all of the opened windows.
     */
    static void destroyAllWindows();

    /** @brief Checks if a window with the given name exists.
     * @param winname Window name.
     * @return true if the window exists.
     */
    static bool hasWindow(const std::string& winname);

    /** @brief Resizes the window to the specified size.
     * @param winname Window name.
     * @param width New window width.
     * @param height New window height.
     */
    static void resizeWindow(const std::string& winname, int width, int height);

    /** @brief Moves the window to the specified position.
     * @param winname Name of the window.
     * @param x The new x-coordinate.
     * @param y The new y-coordinate.
     */
    static void moveWindow(const std::string& winname, int x, int y);

    /** @brief Updates the window title.
     * @param winname Name of the window.
     * @param title New title.
     */
    static void setWindowTitle(const std::string& winname, const std::string& title);

    /** @brief Reads the framebuffer from the last headless render.
     * Only valid when LOWGUI_HEADLESS_RENDER is set and waitKey has returned.
     * @return The framebuffer image, or empty UMat if not available.
     */
    static cv::UMat readFramebuffer();

    // ---------- Mouse ----------

    /** @brief Sets mouse handler for the specified window.
     * @param winname Name of the window.
     * @param onMouse Callback function for mouse events.
     * @param userdata Optional user data passed to the callback.
     */
    static void setMouseCallback(const std::string& winname, MouseCallback onMouse,
                                 void* userdata = 0);

    /** @brief Gets the mouse-wheel motion delta from a mouse callback flags value.
     * @param flags The mouse callback flags parameter.
     * @return The wheel delta (a multiple of 120 for a one-notch rotation).
     */
    static int getMouseWheelDelta(int flags);

    // ---------- Trackbars & control panel ----------

    /** @brief Creates a trackbar and attaches it to the specified window.
     * @param trackbarname Name of the created trackbar.
     * @param winname Name of the window that will contain the trackbar. Can be empty
     * to attach the trackbar to the global control panel (Qt semantics).
     * @param value Pointer to the integer value that will be changed by the trackbar.
     * @param count Maximum position of the trackbar (min is 0).
     * @param onChange Pointer to the function called on trackbar position change.
     * @param userdata Optional user data passed to the callback.
     * @return 1 on success, 0 on failure.
     */
    static int createTrackbar(const std::string& trackbarname, const std::string& winname,
                              int* value, int count,
                              TrackbarCallback onChange = 0, void* userdata = 0);

    /** @brief Returns the trackbar position.
     */
    static int getTrackbarPos(const std::string& trackbarname, const std::string& winname);

    /** @brief Sets the trackbar position (firing the callback like Qt `setValue`).
     */
    static void setTrackbarPos(const std::string& trackbarname, const std::string& winname, int pos);

    /** @brief Sets the trackbar maximum position.
     */
    static void setTrackbarMax(const std::string& trackbarname, const std::string& winname, int maxval);

    /** @brief Sets the trackbar minimum position.
     */
    static void setTrackbarMin(const std::string& trackbarname, const std::string& winname, int minval);

    /** @brief Attaches a button to the control panel.
     * @param bar_name Name of the button.
     * @param on_change Callback called every time the button changes state.
     * @param userdata Pointer passed to the callback.
     * @param type Button type (QT_PUSH_BUTTON / QT_CHECKBOX / QT_RADIOBOX, optionally
     * combined with QT_NEW_BUTTONBAR).
     * @param initial_button_state Default state for check/radio boxes.
     */
    static int createButton(const std::string& bar_name, ButtonCallback on_change,
                            void* userdata = 0, int type = QT_PUSH_BUTTON,
                            bool initial_button_state = false);

    // ---------- Window properties ----------

    /** @brief Sets a window property.
     */
    static void setWindowProperty(const std::string& winname, int prop_id, int prop_value);

    /** @brief Returns a window property.
     */
    static double getWindowProperty(const std::string& winname, int prop_id);

    /** @brief Returns the rectangle of the image rendering area within the native window.
     */
    static cv::Rect getWindowImageRect(const std::string& winname);

    // ---------- Status bar / overlay ----------

    /** @brief Displays a text on a window image as an overlay for a specified duration.
     */
    static void displayOverlay(const std::string& winname, const std::string& text, int delayms = 0);

    /** @brief Displays a text on the window status bar for a specified duration.
     */
    static void displayStatusBar(const std::string& winname, const std::string& text, int delayms = 0);
};

} // namespace lowgui
} // namespace cv

#endif // OPENCV_LOWGUI_LOWGUI_HPP_
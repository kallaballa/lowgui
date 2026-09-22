// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 lowgui contributors
// Clean-room reimplementation of OpenCV highgui (see README.md).
#ifndef OPENCV_LOWGUI_LOWGUI_INPUT_HPP_
#define OPENCV_LOWGUI_LOWGUI_INPUT_HPP_

#include <opencv2/v4d/v4d.hpp>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace cv {
namespace lowgui {
namespace detail {

/**
 * A thread-safe queue of Qt/OpenCV key codes shared between the render worker
 * (pushes via the root plan's key node) and waitKey/waitKeyEx (polls/blocks).
 *
 * waitKey(0) blocks on wait(0) until a key arrives or the queue is closed; the
 * queue is closed when the render engine dies or the native window is closed so
 * waitKey(0) unblocks at shutdown instead of hanging.
 */
class KeyQueue {
public:
    void push(int code) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            keys_.push_back(code);
        }
        cv_.notify_one();
    }

    // Returns the next pending key code, or -1 when idle.
    int poll() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (keys_.empty()) return -1;
        int code = keys_.front();
        keys_.pop_front();
        return code;
    }

    // Waits for a key, the timeout (timeoutMs > 0), or queue close. timeoutMs
    // of 0 blocks until a key or close. Returns the key code or -1.
    int wait(int timeoutMs) {
        std::unique_lock<std::mutex> lock(mtx_);
        auto ready = [this] { return closed_ || !keys_.empty(); };
        if (timeoutMs <= 0) {
            cv_.wait(lock, ready);
        } else {
            cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready);
        }
        if (keys_.empty()) return -1;
        int code = keys_.front();
        keys_.pop_front();
        return code;
    }

    // Closes the queue and wakes any blocked waiter (engine death / native
    // window close). Once closed every wait returns immediately.
    void notify() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    void clear() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            keys_.clear();
        }
        cv_.notify_one();
    }

private:
    std::deque<int> keys_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool closed_ = false;
};

//! Singleton accessor shared by the render worker and the waitKey API.
inline KeyQueue& keyQueue() {
    static KeyQueue queue;
    return queue;
}

//! Translates a V4D keyboard key into the Qt/OpenCV key code delivered to
//! waitKey/waitKeyEx. Translatable keys map to ASCII (lowercase / unshifted);
//! non-translatable keys map to X11 keysyms. Pure and unit-testable.
inline int keyToCode(cv::v4d::event::Keyboard::Key k) {
    using K = cv::v4d::event::Keyboard::Key;
    switch (k) {
        // Translatable -> ASCII
        case K::A: return 'a';
        case K::B: return 'b';
        case K::C: return 'c';
        case K::D: return 'd';
        case K::E: return 'e';
        case K::F: return 'f';
        case K::G: return 'g';
        case K::H: return 'h';
        case K::I: return 'i';
        case K::J: return 'j';
        case K::K: return 'k';
        case K::L: return 'l';
        case K::M: return 'm';
        case K::N: return 'n';
        case K::O: return 'o';
        case K::P: return 'p';
        case K::Q: return 'q';
        case K::R: return 'r';
        case K::S: return 's';
        case K::T: return 't';
        case K::U: return 'u';
        case K::V: return 'v';
        case K::W: return 'w';
        case K::X: return 'x';
        case K::Y: return 'y';
        case K::Z: return 'z';
        case K::N0: return '0';
        case K::N1: return '1';
        case K::N2: return '2';
        case K::N3: return '3';
        case K::N4: return '4';
        case K::N5: return '5';
        case K::N6: return '6';
        case K::N7: return '7';
        case K::N8: return '8';
        case K::N9: return '9';
        case K::SPACE:     return 32;
        case K::ENTER:     return 13;
        case K::BACKSPACE: return 8;
        case K::TAB:       return 9;
        case K::ESCAPE:    return 27;
        case K::APOSTROPHE:   return '\'';
        case K::COMMA:        return ',';
        case K::MINUS:        return '-';
        case K::PERIOD:       return '.';
        case K::SLASH:        return '/';
        case K::SEMICOLON:    return ';';
        case K::EQUAL:        return '=';
        case K::LEFT_BRACKET: return '[';
        case K::BACKSLASH:    return '\\';
        case K::RIGHT_BRACKET:return ']';
        case K::GRAVE_ACCENT: return '`';
        // Non-translatable -> X11 keysyms
        case K::LEFT:      return 65361;
        case K::RIGHT:     return 65363;
        case K::UP:        return 65362;
        case K::DOWN:      return 65364;
        case K::HOME:      return 65360;
        case K::END:       return 65367;
        case K::PAGE_UP:   return 65365;
        case K::PAGE_DOWN: return 65366;
        case K::INSERT:    return 65379;
        case K::DELETE:    return 65535;
        case K::F1:  return 65470;
        case K::F2:  return 65471;
        case K::F3:  return 65472;
        case K::F4:  return 65473;
        case K::F5:  return 65474;
        case K::F6:  return 65475;
        case K::F7:  return 65476;
        case K::F8:  return 65477;
        case K::F9:  return 65478;
        case K::F10: return 65479;
        case K::F11: return 65480;
        case K::F12: return 65481;
        case K::KP_0: return 65456;
        case K::KP_1: return 65457;
        case K::KP_2: return 65458;
        case K::KP_3: return 65459;
        case K::KP_4: return 65460;
        case K::KP_5: return 65461;
        case K::KP_6: return 65462;
        case K::KP_7: return 65463;
        case K::KP_8: return 65464;
        case K::KP_9: return 65465;
        case K::KP_DIVIDE:    return 65455;
        case K::KP_MULTIPLY:  return 65450;
        case K::KP_SUBTRACT:  return 65451;
        case K::KP_ADD:       return 65453;
        case K::KP_ENTER:     return 65421;
        case K::KP_DECIMAL:   return 65454;
        case K::KP_EQUAL:     return 65469;
        default: return -1;
    }
}

} // namespace detail
} // namespace lowgui
} // namespace cv

#endif // OPENCV_LOWGUI_LOWGUI_INPUT_HPP_
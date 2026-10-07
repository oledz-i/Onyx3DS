// SPDX-License-Identifier: GPL-3.0-or-later
//
// Connects the 3DS software keyboard applet (games asking for a name, a number,
// a message) to the on-screen keyboard.
//
// Flow: the game opens its keyboard -> Azahar's DefaultKeyboard calls our hook on
// the emulation thread (patch 0050) -> `open` goes to the UI -> the player types
// -> Submit()/Abort() leave the result in a mailbox -> the emulation thread
// applies it just before the next frame (Pump) -> the core validates it and
// either finishes the request or the UI is told why it was refused.
//
// Nothing here runs unless a page installs handlers, and an idle Pump() is one
// atomic load.
#pragma once

#include "onyx/text_input.h"

namespace onyx::app {

struct KeyboardRequestInfo {
    int button_config = 0;        // 0 = Ok, 1 = Cancel | Ok, 2 = Cancel | I forgot | Ok, 3 = none
    std::string button_text[3];   // [0] Cancel, [1] I forgot, [2] Ok; empty = default wording
    int accept_mode = 0;          // 0 anything, 1 not empty, 2 not empty and not blank, 3 not blank, 4 fixed length
    TextRules rules;              // limits for the text model
    bool numpad = false;          // show the number pad
    bool password = false;
    bool callback = false;        // the game checks the text itself after Ok
    std::string hint;
    std::string error;            // set when the game refused the previous text
    // The button index that means "Ok" (the core validates the text for it).
    int OkButton() const { return button_config; }
    bool HasCancel() const { return button_config == 1 || button_config == 2; }
    bool HasForgot() const { return button_config == 2; }
};

struct KeyboardHandlers {
    // Any thread: the game asked for text.
    std::function<void(const KeyboardRequestInfo&)> open;
    // Any thread: the request is over (answered, or the game went away).
    std::function<void()> close;
    // Emulation thread: the core refused the submitted text; the message says why.
    std::function<void(const std::string&)> rejected;
};

class KeyboardBridge {
public:
    static KeyboardBridge& Get();

    // Installs (open set) or removes (open empty) the handlers and the Azahar hook. UI thread.
    void SetHandlers(KeyboardHandlers handlers);

    // UI thread: hand the player's answer to the core. `button` is the 3DS button index
    // (0 = left; OkButton() for Ok). Applied on the emulation thread.
    void Submit(std::string utf8_text, int button);
    // UI thread: close the request whatever the text (Cancel when the game has one,
    // otherwise Ok with `fallback_text`, unvalidated). The way out of a stuck keyboard.
    void Abort(std::string fallback_text);

    // Emulation thread, before each frame. Free when nothing is waiting.
    void Pump();

    // Called by the hook trampoline.
    void OnRequest(const void* request);

private:
    KeyboardBridge() = default;

    struct Mail {
        enum class Kind { None, Submit, Abort } kind = Kind::None;
        std::string text;
        int button = 0;
    };
    std::mutex mutex_;
    KeyboardHandlers handlers_;
    Mail mail_;
    std::atomic<bool> has_mail_{false};
};

} // namespace onyx::app

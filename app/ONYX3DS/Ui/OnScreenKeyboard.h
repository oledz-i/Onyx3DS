// SPDX-License-Identifier: GPL-3.0-or-later
//
// The on-screen keyboard shown over a running game while a 3DS software
// keyboard request is open (a name, a number, a message). Built in code like
// the rest of the UI kit, and only while a request is open: nothing exists, is
// laid out or ticks while it is hidden.
//
// Controller: D-pad / left stick move the highlight, A types the key,
// B deletes, X space, Y Shift (tap again for Caps), LB / RB move the cursor,
// LT / RT switch the key page, Menu = OK, View = Cancel (hold View for 1.5 s
// to close the keyboard no matter what). A USB keyboard types too.
#pragma once

#include "Emu/KeyboardBridge.h"

namespace onyx::app::ui {

class OnScreenKeyboard : public std::enable_shared_from_this<OnScreenKeyboard> {
public:
    struct Callbacks {
        // UI thread: the player pressed a button. `button` is the 3DS button index.
        std::function<void(const std::string& text, int button)> submit;
        // UI thread: the player used the way out (hold View). Closes the request.
        std::function<void(const std::string& fallback_text)> abort;
    };

    OnScreenKeyboard(winrt::Windows::UI::Xaml::Controls::Panel host, Callbacks callbacks);
    ~OnScreenKeyboard();

    bool IsOpen() const { return static_cast<bool>(root_); }
    // Opens (or reopens) the keyboard for a request.
    void Show(const KeyboardRequestInfo& info);
    // Removes it from the page and frees everything. Safe to call when closed.
    void Hide();
    // The core refused the submitted text.
    void ShowRefusal(const std::string& message);
    // The system Back request (B on some builds of the OS). Does what B does, once.
    void OnBack();

private:
    enum class KeyKind { Char, Shift, Page, Space, Backspace, Left, Right, Enter, Clear };
    struct KeyDef {
        KeyKind kind = KeyKind::Char;
        std::string text; // Char: what it types
    };
    struct CharSlot {
        winrt::Windows::UI::Xaml::Controls::Button button{nullptr};
        winrt::Windows::UI::Xaml::Controls::TextBlock label{nullptr};
        int def = 0;
    };
    enum class ShiftState { Off, Once, Caps };

    void Build();
    void BuildAlphaKeys(winrt::Windows::UI::Xaml::Controls::Grid const& grid);
    void BuildNumberPad(winrt::Windows::UI::Xaml::Controls::Grid const& grid);
    winrt::Windows::UI::Xaml::Controls::Button AddKey(winrt::Windows::UI::Xaml::Controls::Grid const& grid,
                                                      KeyKind kind, const std::string& text,
                                                      int row, int col, int col_span,
                                                      winrt::Windows::UI::Xaml::UIElement const& label);
    void ApplyLook(winrt::Windows::UI::Xaml::Controls::Button const& key, bool focused);
    void RefreshKeys();   // character keys and the Shift / page labels
    void RefreshText();   // the text, cursor, counter
    void Press(int def_index);
    void TypeText(const std::string& utf8);
    void Backspace();
    void CycleShift();
    void CyclePage(int delta);
    void MoveCaret(int delta);
    void Commit(int button);
    void SetMessage(const std::string& text, bool is_error); // empty text hides the line
    void OnTick();
    void FocusFirstKey();
    void OnGamepadKey(winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs const& e);
    void OnPhysicalKey(winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs const& e);
    void OnGamepadKeyUp(winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs const& e);
    void OnCharacter(uint32_t code);
    void StartHoldClose();
    void StopHoldClose();
    std::string Shown(const std::string& s) const; // masks a password

    winrt::Windows::UI::Xaml::Controls::Panel host_;
    Callbacks callbacks_;
    KeyboardRequestInfo info_;
    TextBuffer buffer_;
    std::string retry_text_; // the text last sent, offered again when the game refuses it

    // Present only while open.
    winrt::Windows::UI::Xaml::Controls::Grid root_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Border panel_{nullptr};
    winrt::Windows::UI::Xaml::Controls::ScrollViewer scroll_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock text_{nullptr};
    winrt::Windows::UI::Xaml::Documents::Run before_{nullptr};
    winrt::Windows::UI::Xaml::Documents::Run caret_{nullptr};
    winrt::Windows::UI::Xaml::Documents::Run after_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock counter_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock message_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button shift_key_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock shift_label_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock page_label_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button first_key_{nullptr};
    winrt::Windows::UI::Xaml::DispatcherTimer tick_{nullptr};
    winrt::Windows::UI::Xaml::DispatcherTimer hold_{nullptr};
    winrt::event_token char_token_{};
    bool char_hooked_ = false;
    std::vector<KeyDef> defs_;
    std::vector<CharSlot> slots_;

    // Brushes for the current theme.
    winrt::Windows::UI::Xaml::Media::SolidColorBrush face_{nullptr}, special_{nullptr}, edge_{nullptr},
        accent_{nullptr}, active_{nullptr}, text_brush_{nullptr}, muted_{nullptr}, accent_text_{nullptr},
        clear_{nullptr}, error_{nullptr};

    KeyPage page_ = KeyPage::Letters;
    ShiftState shift_ = ShiftState::Off;
    bool caret_on_ = true;
    bool busy_ = false; // an answer is on its way to the core
    std::chrono::steady_clock::time_point opened_at_{};
    std::chrono::steady_clock::time_point busy_since_{};
    std::chrono::steady_clock::time_point last_backspace_{};
    std::chrono::steady_clock::time_point last_pad_key_{};  // last controller button seen
    std::chrono::steady_clock::time_point last_real_key_{}; // last real keyboard key seen
};

} // namespace onyx::app::ui

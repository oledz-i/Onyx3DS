// SPDX-License-Identifier: GPL-3.0-or-later
//
// The text model behind the on-screen keyboard that answers a game's software
// keyboard request: a caret-edited string that enforces the limits the game
// asked for (length in UTF-16 units like the 3DS counts it, digit limit,
// blocked characters), and the key layouts. Pure logic, no UI.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace onyx {

struct TextRules {
    int max_units = 0;  // UTF-16 code units; 0 = no limit
    int max_digits = 0; // used with prevent_digit: at most this many digits
    bool prevent_digit = false;
    bool prevent_at = false;
    bool prevent_percent = false;
    bool prevent_backslash = false;
    bool digits_only = false; // number pad: 0-9 only
    bool multiline = false;   // a line break may be typed
};

enum class InsertResult { Ok, TooLong, NotAllowed, TooManyDigits };

std::u32string Utf8ToCodepoints(std::string_view utf8); // invalid bytes become U+FFFD
std::string CodepointsToUtf8(std::u32string_view text);

class TextBuffer {
public:
    explicit TextBuffer(TextRules rules = {}) : rules_(rules) {}

    // Inserts at the caret, one code point at a time. Stops at the first one the
    // rules refuse (the ones before it stay) and says why.
    InsertResult Insert(std::string_view utf8);
    bool Backspace(); // false when there is nothing before the caret
    bool Delete();    // false when there is nothing after the caret
    void Clear();
    void MoveCaret(int delta); // clamped to the text
    void CaretHome() { caret_ = 0; }
    void CaretEnd() { caret_ = text_.size(); }

    std::string Text() const { return CodepointsToUtf8(text_); }
    std::string Before() const { return CodepointsToUtf8(std::u32string_view(text_).substr(0, caret_)); }
    std::string After() const { return CodepointsToUtf8(std::u32string_view(text_).substr(caret_)); }
    std::size_t Caret() const { return caret_; } // in code points
    std::size_t Length() const { return text_.size(); } // in code points
    int Units() const;                                  // UTF-16 code units, as the 3DS counts
    bool Empty() const { return text_.empty(); }
    const TextRules& Rules() const { return rules_; }

private:
    TextRules rules_;
    std::u32string text_;
    std::size_t caret_ = 0;
};

// ---- key layouts ---------------------------------------------------------------
enum class KeyPage { Letters, Symbols, Accents };
constexpr int kKeyPageCount = 3;

// The character keys of one page: four rows of 10, 10, 10 and 7 keys (the
// digits are always the first row). `upper` is Shift/Caps.
std::vector<std::vector<std::string>> AlphaKeyRows(KeyPage page, bool upper);

} // namespace onyx

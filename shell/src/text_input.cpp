// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/text_input.h"

namespace onyx {

std::u32string Utf8ToCodepoints(std::string_view s) {
    std::u32string out;
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0xFFFD;
        std::size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07;
        }
        if (len > 1) {
            bool ok = i + len <= s.size();
            for (std::size_t k = 1; ok && k < len; ++k) {
                const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                if ((cc & 0xC0) != 0x80) ok = false;
                else cp = (cp << 6) | (cc & 0x3F);
            }
            const bool overlong = (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
            if (!ok || overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                cp = 0xFFFD;
                len = 1;
            }
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string CodepointsToUtf8(std::u32string_view text) {
    std::string out;
    for (char32_t cp : text) {
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

namespace {
int UnitsOf(char32_t cp) {
    return cp > 0xFFFF ? 2 : 1;
}
bool IsDigit(char32_t cp) {
    return cp >= U'0' && cp <= U'9';
}
} // namespace

int TextBuffer::Units() const {
    int n = 0;
    for (char32_t cp : text_) n += UnitsOf(cp);
    return n;
}

InsertResult TextBuffer::Insert(std::string_view utf8) {
    for (char32_t cp : Utf8ToCodepoints(utf8)) {
        if (cp == U'\n') {
            if (!rules_.multiline) return InsertResult::NotAllowed;
        } else if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) {
            return InsertResult::NotAllowed;
        }
        if (rules_.digits_only && !IsDigit(cp)) return InsertResult::NotAllowed;
        if ((rules_.prevent_at && cp == U'@') || (rules_.prevent_percent && cp == U'%') ||
            (rules_.prevent_backslash && cp == U'\\'))
            return InsertResult::NotAllowed;
        if (rules_.max_units > 0 && Units() + UnitsOf(cp) > rules_.max_units) return InsertResult::TooLong;
        if (rules_.prevent_digit && IsDigit(cp)) {
            int digits = 0;
            for (char32_t c : text_) digits += IsDigit(c) ? 1 : 0;
            if (digits + 1 > rules_.max_digits) return InsertResult::TooManyDigits;
        }
        text_.insert(text_.begin() + static_cast<std::ptrdiff_t>(caret_), cp);
        ++caret_;
    }
    return InsertResult::Ok;
}

bool TextBuffer::Backspace() {
    if (caret_ == 0) return false;
    text_.erase(text_.begin() + static_cast<std::ptrdiff_t>(caret_ - 1));
    --caret_;
    return true;
}

bool TextBuffer::Delete() {
    if (caret_ >= text_.size()) return false;
    text_.erase(text_.begin() + static_cast<std::ptrdiff_t>(caret_));
    return true;
}

void TextBuffer::Clear() {
    text_.clear();
    caret_ = 0;
}

void TextBuffer::MoveCaret(int delta) {
    const long long next = static_cast<long long>(caret_) + delta;
    if (next < 0) caret_ = 0;
    else if (next > static_cast<long long>(text_.size())) caret_ = text_.size();
    else caret_ = static_cast<std::size_t>(next);
}

std::vector<std::vector<std::string>> AlphaKeyRows(KeyPage page, bool upper) {
    // Each row is a string of single code points (UTF-8, split below).
    std::vector<std::string> rows;
    switch (page) {
    case KeyPage::Symbols:
        rows = {"1234567890", "!@#$%^&*()", "-_=+/\\|;:\"", "~[]{}<>"};
        break;
    case KeyPage::Accents:
        rows = {"1234567890", "àáâäãåæçèé",
                "êëìíîïñòóô",
                "öøùúûüß"};
        break;
    default:
        rows = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm"};
        break;
    }
    std::vector<std::vector<std::string>> out;
    for (std::size_t r = 0; r < rows.size(); ++r) {
        std::vector<std::string> keys;
        for (char32_t cp : Utf8ToCodepoints(rows[r])) {
            if (upper && r > 0) {
                if (cp >= U'a' && cp <= U'z') cp -= 32;
                else if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) cp -= 32;
            }
            keys.push_back(CodepointsToUtf8(std::u32string(1, cp)));
        }
        out.push_back(std::move(keys));
    }
    return out;
}

} // namespace onyx

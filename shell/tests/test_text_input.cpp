// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "onyx/text_input.h"

using namespace onyx;

TEST_CASE("TextBuffer edits at the caret and counts UTF-16 units like the 3DS") {
    TextRules rules;
    rules.max_units = 5;
    TextBuffer b(rules);
    CHECK(b.Insert("abc") == InsertResult::Ok);
    b.MoveCaret(-1);
    CHECK(b.Insert("X") == InsertResult::Ok);
    CHECK(b.Text() == "abXc");
    CHECK(b.Before() == "abX");
    CHECK(b.After() == "c");
    CHECK(b.Backspace());
    CHECK(b.Text() == "abc");
    CHECK(b.Delete());
    CHECK(b.Text() == "ab");
    b.MoveCaret(-99);
    CHECK(b.Caret() == 0);
    CHECK_FALSE(b.Backspace());
    b.CaretEnd();
    // An emoji is two UTF-16 units: 2 + 2 fits in 5, a second one does not.
    CHECK(b.Insert("\xF0\x9F\x98\x80") == InsertResult::Ok);
    CHECK(b.Units() == 4);
    CHECK(b.Insert("\xF0\x9F\x98\x80") == InsertResult::TooLong);
    CHECK(b.Insert("x") == InsertResult::Ok);
    CHECK(b.Insert("y") == InsertResult::TooLong);
    CHECK(b.Units() == 5);
}

TEST_CASE("TextBuffer applies the game's character and digit filters") {
    TextRules rules;
    rules.prevent_at = rules.prevent_percent = rules.prevent_backslash = true;
    rules.prevent_digit = true;
    rules.max_digits = 2;
    TextBuffer b(rules);
    CHECK(b.Insert("a@") == InsertResult::NotAllowed);
    CHECK(b.Text() == "a"); // the part before the refused character stays
    CHECK(b.Insert("%") == InsertResult::NotAllowed);
    CHECK(b.Insert("\\") == InsertResult::NotAllowed);
    CHECK(b.Insert("12") == InsertResult::Ok);
    CHECK(b.Insert("3") == InsertResult::TooManyDigits);
    CHECK(b.Insert("b") == InsertResult::Ok);
    CHECK(b.Insert("\n") == InsertResult::NotAllowed); // single line
    CHECK(b.Insert("\t") == InsertResult::NotAllowed);
    TextRules nl;
    nl.multiline = true;
    TextBuffer m(nl);
    CHECK(m.Insert("a\nb") == InsertResult::Ok);
    CHECK(m.Text() == "a\nb");
}

TEST_CASE("Number pad only takes digits") {
    TextRules rules;
    rules.digits_only = true;
    rules.max_units = 4;
    TextBuffer b(rules);
    CHECK(b.Insert("12") == InsertResult::Ok);
    CHECK(b.Insert("a") == InsertResult::NotAllowed);
    CHECK(b.Insert("-") == InsertResult::NotAllowed);
    CHECK(b.Insert("345") == InsertResult::TooLong);
    CHECK(b.Text() == "1234");
    b.Clear();
    CHECK(b.Empty());
}

TEST_CASE("UTF-8 round trip and invalid input") {
    CHECK(CodepointsToUtf8(Utf8ToCodepoints("h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x98\x80")) ==
          "h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x98\x80");
    CHECK(Utf8ToCodepoints("\xFF").size() == 1);
    CHECK(Utf8ToCodepoints("\xFF")[0] == 0xFFFD);
    CHECK(Utf8ToCodepoints("\xE2\x82").size() == 2); // truncated sequence: replacement per byte
}

TEST_CASE("Key pages have the same shape and shift upper-cases") {
    for (int p = 0; p < kKeyPageCount; ++p)
        for (bool up : {false, true}) {
            const auto rows = AlphaKeyRows(static_cast<KeyPage>(p), up);
            REQUIRE(rows.size() == 4);
            CHECK(rows[0].size() == 10);
            CHECK(rows[1].size() == 10);
            CHECK(rows[2].size() == 10);
            CHECK(rows[3].size() == 7);
            CHECK(rows[0][0] == "1");
        }
    CHECK(AlphaKeyRows(KeyPage::Letters, true)[1][0] == "Q");
    CHECK(AlphaKeyRows(KeyPage::Letters, true)[2][9] == "'");
    CHECK(AlphaKeyRows(KeyPage::Accents, true)[1][9] == "\xC3\x89"); // E acute
    CHECK(AlphaKeyRows(KeyPage::Accents, true)[3][6] == "\xC3\x9F"); // sharp s stays
    CHECK(AlphaKeyRows(KeyPage::Symbols, true)[1][0] == "!");
}

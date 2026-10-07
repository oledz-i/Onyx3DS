// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Ui/OnScreenKeyboard.h"

#include "Platform/Log.h"
#include "Ui/AppServices.h"
#include "Ui/UiKit.h"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Media;
using winrt::Windows::Foundation::IInspectable;

namespace onyx::app::ui {

namespace {
using std::chrono::milliseconds;
constexpr milliseconds kInputGuard{300};   // ignore presses right after opening (the A that opened the game's dialog)
constexpr milliseconds kBackDedupe{400};   // KeyDown(B) and the system Back request are one press
constexpr milliseconds kBusyTimeout{3000};
constexpr int kHoldCloseMs = 1500;

const Theme& T() {
    return AppServices::Get().CurrentTheme();
}
SolidColorBrush B(const std::string& hex) {
    return AppServices::Get().ThemeBrush(hex);
}
} // namespace

OnScreenKeyboard::OnScreenKeyboard(Panel host, Callbacks callbacks)
    : host_(std::move(host)), callbacks_(std::move(callbacks)) {}

OnScreenKeyboard::~OnScreenKeyboard() {
    try {
        Hide();
    } catch (...) {
    }
}

std::string OnScreenKeyboard::Shown(const std::string& s) const {
    if (!info_.password) return s;
    std::string out;
    const std::size_t n = Utf8ToCodepoints(s).size();
    for (std::size_t i = 0; i < n; ++i) out += "\xE2\x80\xA2"; // bullet
    return out;
}

// ---------------------------------------------------------------------------
// Open / close

void OnScreenKeyboard::Show(const KeyboardRequestInfo& info) {
    if (root_) Hide();
    info_ = info;
    buffer_ = TextBuffer(info.rules);
    if (!info.error.empty() && !retry_text_.empty()) buffer_.Insert(retry_text_);
    else retry_text_.clear();
    page_ = KeyPage::Letters;
    // Sentence-style capital for the first letter, except where that would be wrong.
    shift_ = (!info.numpad && !info.password && buffer_.Empty()) ? ShiftState::Once : ShiftState::Off;
    busy_ = false;
    caret_on_ = true;
    opened_at_ = std::chrono::steady_clock::now();

    Build();
    host_.Children().Append(root_);
    Entrance(panel_, 0, 36, 0, 200);

    auto weak = weak_from_this();
    tick_ = DispatcherTimer();
    tick_.Interval(std::chrono::milliseconds(500));
    tick_.Tick([weak](auto&&, auto&&) {
        if (auto s = weak.lock()) s->OnTick();
    });
    tick_.Start();

    // Typed characters from a USB keyboard (key presses themselves come through XAML).
    try {
        auto window = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread();
        char_token_ = window.CharacterReceived(
            [weak](winrt::Windows::UI::Core::CoreWindow const&,
                   winrt::Windows::UI::Core::CharacterReceivedEventArgs const& a) {
                if (auto s = weak.lock()) s->OnCharacter(a.KeyCode());
            });
        char_hooked_ = true;
    } catch (...) {
        char_hooked_ = false;
    }

    if (!info.error.empty()) SetMessage(info.error, true);
    AppServices::Get().PlaySfx("select");
}

void OnScreenKeyboard::Hide() {
    if (!root_) return;
    StopHoldClose();
    hold_ = nullptr;
    if (tick_) {
        tick_.Stop();
        tick_ = nullptr;
    }
    if (char_hooked_) {
        try {
            winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().CharacterReceived(char_token_);
        } catch (...) {
        }
        char_hooked_ = false;
    }
    uint32_t index = 0;
    if (host_.Children().IndexOf(root_, index)) host_.Children().RemoveAt(index);
    root_ = nullptr;
    panel_ = nullptr;
    scroll_ = nullptr;
    text_ = nullptr;
    before_ = nullptr;
    caret_ = nullptr;
    after_ = nullptr;
    counter_ = nullptr;
    message_ = nullptr;
    shift_key_ = nullptr;
    shift_label_ = nullptr;
    page_label_ = nullptr;
    first_key_ = nullptr;
    defs_.clear();
    slots_.clear();
    busy_ = false;
}

void OnScreenKeyboard::ShowRefusal(const std::string& message) {
    if (!root_) return;
    busy_ = false;
    SetMessage(message.empty() ? "That text was not accepted." : message, true);
    AppServices::Get().PlaySfx("back");
}

void OnScreenKeyboard::OnBack() {
    if (!root_) return;
    // B arrives as a key press and, on some system versions, as a Back request as well.
    if (std::chrono::steady_clock::now() - last_backspace_ < kBackDedupe) return;
    Backspace();
}

// ---------------------------------------------------------------------------
// Building the UI

Button OnScreenKeyboard::AddKey(Grid const& grid, KeyKind kind, const std::string& text, int row, int col,
                                int col_span, UIElement const& label) {
    Button b;
    b.Content(label);
    b.HorizontalAlignment(HorizontalAlignment::Stretch);
    b.VerticalAlignment(VerticalAlignment::Stretch);
    b.HorizontalContentAlignment(HorizontalAlignment::Center);
    b.VerticalContentAlignment(VerticalAlignment::Center);
    b.Padding(Thickness{0, 0, 0, 0});
    b.Margin(Thickness{4, 4, 4, 4});
    b.MinHeight(info_.numpad ? 84 : 70);
    b.CornerRadius(CornerRadius{16, 16, 16, 16});
    b.BorderThickness(Thickness{2, 2, 2, 2});
    b.BorderBrush(edge_);
    b.UseSystemFocusVisuals(false); // the accent fill below is the highlight
    b.RenderTransformOrigin(winrt::Windows::Foundation::Point{0.5f, 0.5f});
    b.RenderTransform(ScaleTransform());
    const int def_index = static_cast<int>(defs_.size());
    defs_.push_back(KeyDef{kind, text});
    b.Tag(box_value(static_cast<int32_t>(def_index)));
    Grid::SetRow(b, row);
    Grid::SetColumn(b, col);
    if (col_span > 1) Grid::SetColumnSpan(b, col_span);

    auto weak = weak_from_this();
    b.Click([weak, def_index](IInspectable const&, RoutedEventArgs const&) {
        if (auto s = weak.lock()) s->Press(def_index);
    });
    b.GotFocus([weak](IInspectable const& sender, RoutedEventArgs const&) {
        auto s = weak.lock();
        auto key = sender.try_as<Button>();
        if (!s || !key || !s->root_) return;
        s->ApplyLook(key, true);
        AppServices::Get().PlaySfx("move");
    });
    b.LostFocus([weak](IInspectable const& sender, RoutedEventArgs const&) {
        auto s = weak.lock();
        auto key = sender.try_as<Button>();
        if (!s || !key || !s->root_) return;
        s->ApplyLook(key, false);
    });
    grid.Children().Append(b);
    ApplyLook(b, false);
    return b;
}

void OnScreenKeyboard::ApplyLook(Button const& key, bool focused) {
    const auto tag = key.Tag();
    if (!tag) return;
    const int index = unbox_value<int32_t>(tag);
    if (index < 0 || static_cast<std::size_t>(index) >= defs_.size()) return;
    const KeyDef& d = defs_[static_cast<std::size_t>(index)];
    const bool special = d.kind != KeyKind::Char;
    const bool active = d.kind == KeyKind::Shift && shift_ != ShiftState::Off;
    key.Background(focused ? accent_ : active ? active_ : special ? special_ : face_);
    key.BorderBrush(focused || active ? accent_ : edge_);
    if (auto label = key.Content().try_as<TextBlock>()) label.Foreground(focused ? accent_text_ : text_brush_);
    if (auto scale = key.RenderTransform().try_as<ScaleTransform>()) {
        const double s = focused ? 1.06 : 1.0;
        scale.ScaleX(s);
        scale.ScaleY(s);
    }
    Canvas::SetZIndex(key, focused ? 5 : 0); // the enlarged key draws over its neighbours
}

void OnScreenKeyboard::BuildAlphaKeys(Grid const& grid) {
    auto star = [] { return GridLengthHelper::FromValueAndType(1, GridUnitType::Star); };
    for (int c = 0; c < 10; ++c) {
        ColumnDefinition cd;
        cd.Width(star());
        grid.ColumnDefinitions().Append(cd);
    }
    for (int r = 0; r < 5; ++r) {
        RowDefinition rd;
        rd.Height(GridLengthHelper::Auto());
        grid.RowDefinitions().Append(rd);
    }
    auto centred = [](TextBlock t) {
        t.TextWrapping(TextWrapping::NoWrap);
        t.HorizontalAlignment(HorizontalAlignment::Center);
        t.VerticalAlignment(VerticalAlignment::Center);
        return t;
    };
    auto char_label = [&](const std::string& s) { return centred(Text(s, 32, true, T().colors.text)); };
    auto word_label = [&](const std::string& s) { return centred(Text(s, 24, true, T().colors.text)); };

    auto add_slot = [&](int row, int col) {
        auto label = char_label("");
        auto key = AddKey(grid, KeyKind::Char, "", row, col, 1, label);
        slots_.push_back(CharSlot{key, label, static_cast<int>(defs_.size()) - 1});
        return key;
    };
    // Rows 0-2: digits, then ten letters (or symbols) each.
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 10; ++c) {
            auto key = add_slot(r, c);
            if (r == 2 && c == 4) first_key_ = key;
        }
    // Row 3: Shift, seven keys, backspace.
    shift_label_ = word_label("Shift");
    shift_key_ = AddKey(grid, KeyKind::Shift, "", 3, 0, 2, shift_label_);
    for (int c = 0; c < 7; ++c) add_slot(3, 2 + c);
    {
        auto g = centred(Glyph(L"", 30, T().colors.text)); // backspace
        AddKey(grid, KeyKind::Backspace, "", 3, 9, 1, g);
    }
    // Row 4: page, comma, space, period, ? or Enter, cursor left / right.
    page_label_ = word_label("#+=");
    AddKey(grid, KeyKind::Page, "", 4, 0, 2, page_label_);
    AddKey(grid, KeyKind::Char, ",", 4, 2, 1, char_label(","));
    AddKey(grid, KeyKind::Space, "", 4, 3, 3, word_label("Space"));
    AddKey(grid, KeyKind::Char, ".", 4, 6, 1, char_label("."));
    if (info_.rules.multiline) {
        auto g = centred(Glyph(L"", 30, T().colors.text)); // return
        AddKey(grid, KeyKind::Enter, "", 4, 7, 1, g);
    } else {
        AddKey(grid, KeyKind::Char, "?", 4, 7, 1, char_label("?"));
    }
    AddKey(grid, KeyKind::Left, "", 4, 8, 1, char_label("\xE2\x86\x90"));
    AddKey(grid, KeyKind::Right, "", 4, 9, 1, char_label("\xE2\x86\x92"));
}

void OnScreenKeyboard::BuildNumberPad(Grid const& grid) {
    for (int c = 0; c < 4; ++c) {
        ColumnDefinition cd;
        cd.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        grid.ColumnDefinitions().Append(cd);
    }
    for (int r = 0; r < 4; ++r) {
        RowDefinition rd;
        rd.Height(GridLengthHelper::Auto());
        grid.RowDefinitions().Append(rd);
    }
    auto label = [&](const std::string& s, double size) {
        auto t = Text(s, size, true, T().colors.text);
        t.TextWrapping(TextWrapping::NoWrap);
        t.HorizontalAlignment(HorizontalAlignment::Center);
        t.VerticalAlignment(VerticalAlignment::Center);
        return t;
    };
    const char* digits[3][3] = {{"7", "8", "9"}, {"4", "5", "6"}, {"1", "2", "3"}};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            auto key = AddKey(grid, KeyKind::Char, digits[r][c], r, c, 1, label(digits[r][c], 40));
            if (r == 1 && c == 1) first_key_ = key;
        }
    {
        auto g = Glyph(L"", 34, T().colors.text);
        g.HorizontalAlignment(HorizontalAlignment::Center);
        g.VerticalAlignment(VerticalAlignment::Center);
        AddKey(grid, KeyKind::Backspace, "", 0, 3, 1, g);
    }
    AddKey(grid, KeyKind::Left, "", 1, 3, 1, label("\xE2\x86\x90", 36));
    AddKey(grid, KeyKind::Right, "", 2, 3, 1, label("\xE2\x86\x92", 36));
    AddKey(grid, KeyKind::Clear, "", 3, 0, 1, label("Clear", 24));
    AddKey(grid, KeyKind::Char, "0", 3, 1, 2, label("0", 40));
}

void OnScreenKeyboard::Build() {
    const ThemeColors& c = T().colors;
    face_ = B(c.tile_face);
    special_ = B(WithAlpha(c.accent, 0x33));
    edge_ = B(c.tile_edge);
    accent_ = B(c.accent);
    active_ = B(WithAlpha(c.accent, 0x77));
    text_brush_ = B(c.text);
    muted_ = B(c.text_muted);
    accent_text_ = B(c.accent_text);
    clear_ = B("#00000000");
    error_ = B("#E0383E");
    defs_.clear();
    slots_.clear();
    shift_key_ = nullptr;
    first_key_ = nullptr;

    root_ = Grid();
    root_.Background(B("#66000000")); // dims the game a little and swallows mouse clicks

    panel_ = Border();
    panel_.VerticalAlignment(VerticalAlignment::Bottom);
    panel_.HorizontalAlignment(HorizontalAlignment::Center);
    panel_.MaxWidth(info_.numpad ? 760 : 1360);
    panel_.Margin(Thickness{96, 0, 96, 54});
    panel_.CornerRadius(CornerRadius{28, 28, 28, 28});
    panel_.Padding(Thickness{32, 26, 32, 22});
    panel_.Background(B(c.panel));
    panel_.BorderBrush(B(c.tile_edge));
    panel_.BorderThickness(Thickness{2, 2, 2, 2});

    StackPanel stack;
    stack.Spacing(12);

    // ---- the text and its counter ----
    Grid head;
    {
        ColumnDefinition c0, c1;
        c0.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        c1.Width(GridLengthHelper::Auto());
        head.ColumnDefinitions().Append(c0);
        head.ColumnDefinitions().Append(c1);
    }
    Border field;
    field.Background(face_);
    field.BorderBrush(accent_);
    field.BorderThickness(Thickness{3, 3, 3, 3});
    field.CornerRadius(CornerRadius{18, 18, 18, 18});
    field.Padding(Thickness{22, 8, 22, 8});
    field.MinHeight(76);
    scroll_ = ScrollViewer();
    scroll_.VerticalScrollBarVisibility(ScrollBarVisibility::Hidden);
    scroll_.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    scroll_.MaxHeight(info_.rules.multiline ? 190 : 130);
    scroll_.IsTabStop(false);
    text_ = Text("", 40, false, c.text);
    text_.TextWrapping(TextWrapping::Wrap);
    text_.VerticalAlignment(VerticalAlignment::Center);
    before_ = winrt::Windows::UI::Xaml::Documents::Run();
    caret_ = winrt::Windows::UI::Xaml::Documents::Run();
    after_ = winrt::Windows::UI::Xaml::Documents::Run();
    caret_.Text(L"|");
    text_.Inlines().Append(before_);
    text_.Inlines().Append(caret_);
    text_.Inlines().Append(after_);
    scroll_.Content(text_);
    field.Child(scroll_);
    head.Children().Append(field);
    counter_ = Text("", 26, true, c.text_muted);
    counter_.TextWrapping(TextWrapping::NoWrap);
    counter_.VerticalAlignment(VerticalAlignment::Center);
    counter_.Margin(Thickness{24, 0, 0, 0});
    Grid::SetColumn(counter_, 1);
    head.Children().Append(counter_);
    stack.Children().Append(head);

    // ---- validation / limit message (always takes its line, so nothing jumps) ----
    message_ = Text("", 22, true, "#E0383E");
    message_.MinHeight(30);
    message_.Margin(Thickness{6, 0, 6, 0});
    message_.Opacity(0);
    stack.Children().Append(message_);

    // ---- keys ----
    Grid keys;
    if (info_.numpad) BuildNumberPad(keys);
    else BuildAlphaKeys(keys);
    stack.Children().Append(keys);

    // ---- buttons ----
    auto weak = weak_from_this();
    StackPanel buttons;
    buttons.Orientation(Orientation::Horizontal);
    buttons.Spacing(16);
    buttons.HorizontalAlignment(HorizontalAlignment::Right);
    buttons.Margin(Thickness{0, 6, 0, 0});
    auto label_for = [&](int i, const char* fallback) {
        return info_.button_text[i].empty() ? std::string(fallback) : info_.button_text[i];
    };
    auto add_button = [&](const std::string& label, const wchar_t* glyph, int button, bool primary) {
        auto b = ActionButton(label, glyph, [weak, button] {
            if (auto s = weak.lock()) s->Commit(button);
        }, primary);
        b.Tag(box_value(static_cast<int32_t>(-1))); // marks the button as ours for the focus check
        buttons.Children().Append(b);
    };
    if (info_.HasCancel()) add_button(label_for(0, "Cancel"), nullptr, 0, false);
    if (info_.HasForgot()) add_button(label_for(1, "I forgot"), nullptr, 1, false);
    add_button(label_for(2, "OK"), glyph::Check, info_.OkButton(), true);
    stack.Children().Append(buttons);

    // ---- controller hints ----
    std::vector<std::pair<std::string, std::string>> hints;
    hints.emplace_back("A", "Type");
    hints.emplace_back("B", "Delete");
    if (!info_.numpad) {
        hints.emplace_back("X", "Space");
        hints.emplace_back("Y", "Shift");
    }
    hints.emplace_back("LB/RB", "Cursor");
    if (!info_.numpad) hints.emplace_back("LT/RT", "Page");
    hints.emplace_back("Menu", "OK");
    hints.emplace_back("View", info_.HasCancel() ? "Cancel (hold: close)" : "Hold: close");
    auto bar = HintBar(hints, c.text_muted);
    bar.as<FrameworkElement>().HorizontalAlignment(HorizontalAlignment::Center);
    stack.Children().Append(bar);

    panel_.Child(stack);
    root_.Children().Append(panel_);

    // ---- input ----
    root_.KeyDown([weak](IInspectable const&, KeyRoutedEventArgs const& e) {
        if (auto s = weak.lock()) s->OnGamepadKey(e);
    });
    root_.PreviewKeyDown([weak](IInspectable const&, KeyRoutedEventArgs const& e) {
        if (auto s = weak.lock()) s->OnPhysicalKey(e);
    });
    root_.KeyUp([weak](IInspectable const&, KeyRoutedEventArgs const& e) {
        if (auto s = weak.lock()) s->OnGamepadKeyUp(e);
    });
    root_.Loaded([weak](IInspectable const&, RoutedEventArgs const&) {
        if (auto s = weak.lock()) s->FocusFirstKey();
    });

    RefreshKeys();
    RefreshText();
}

void OnScreenKeyboard::FocusFirstKey() {
    if (first_key_) first_key_.Focus(FocusState::Programmatic);
}

// ---------------------------------------------------------------------------
// State -> UI

void OnScreenKeyboard::RefreshKeys() {
    if (!root_ || info_.numpad) return;
    const auto rows = AlphaKeyRows(page_, shift_ != ShiftState::Off);
    std::size_t k = 0;
    for (const auto& row : rows)
        for (const auto& key : row) {
            if (k < slots_.size()) {
                slots_[k].label.Text(H(key));
                defs_[static_cast<std::size_t>(slots_[k].def)].text = key;
            }
            ++k;
        }
    if (shift_label_)
        shift_label_.Text(shift_ == ShiftState::Caps ? L"CAPS" : shift_ == ShiftState::Once ? L"SHIFT" : L"Shift");
    if (page_label_) {
        const KeyPage next = static_cast<KeyPage>((static_cast<int>(page_) + 1) % kKeyPageCount);
        page_label_.Text(next == KeyPage::Symbols  ? L"#+="
                         : next == KeyPage::Accents ? L"é ü"
                                                    : L"ABC");
    }
    if (shift_key_) ApplyLook(shift_key_, shift_key_.FocusState() != FocusState::Unfocused);
}

void OnScreenKeyboard::RefreshText() {
    if (!root_) return;
    if (buffer_.Empty() && !info_.hint.empty()) {
        before_.Text(L"");
        after_.Text(H(info_.hint));
        after_.Foreground(muted_);
    } else {
        before_.Text(H(Shown(buffer_.Before())));
        after_.Text(H(Shown(buffer_.After())));
        after_.Foreground(text_brush_);
    }
    before_.Foreground(text_brush_);
    caret_on_ = true; // a keystroke always shows the cursor
    caret_.Foreground(accent_);
    std::string count = std::to_string(buffer_.Units());
    const int max = info_.rules.max_units;
    if (max > 0) count += " / " + std::to_string(max);
    counter_.Text(H(count));
    counter_.Foreground(max > 0 && buffer_.Units() >= max ? accent_ : muted_);
    if (buffer_.Caret() == buffer_.Length()) {
        scroll_.UpdateLayout();
        scroll_.ScrollToVerticalOffset(scroll_.ScrollableHeight());
    }
}

void OnScreenKeyboard::SetMessage(const std::string& text, bool) {
    if (!message_) return;
    message_.Text(H(text));
    message_.Opacity(text.empty() ? 0.0 : 1.0);
}

void OnScreenKeyboard::OnTick() {
    if (!root_) return;
    caret_on_ = !caret_on_;
    caret_.Foreground(caret_on_ ? accent_ : clear_);
    if (busy_ && std::chrono::steady_clock::now() - busy_since_ > kBusyTimeout) {
        busy_ = false;
        SetMessage("The game did not answer. Try again.", true);
    }
    // Keep the controller on the keyboard: if focus ever lands elsewhere, bring it back.
    bool ours = false;
    if (auto focused = FocusManager::GetFocusedElement()) {
        if (auto b = focused.try_as<Button>()) ours = static_cast<bool>(b.Tag());
    }
    if (!ours) FocusFirstKey();
}

// ---------------------------------------------------------------------------
// Editing

void OnScreenKeyboard::TypeText(const std::string& utf8) {
    const InsertResult r = buffer_.Insert(utf8);
    switch (r) {
    case InsertResult::Ok: SetMessage("", false); break;
    case InsertResult::TooLong: SetMessage("That is as long as it can be.", true); break;
    case InsertResult::TooManyDigits: SetMessage("No more digits are allowed here.", true); break;
    case InsertResult::NotAllowed: SetMessage("That character is not allowed here.", true); break;
    }
    RefreshText();
}

void OnScreenKeyboard::Backspace() {
    last_backspace_ = std::chrono::steady_clock::now();
    if (busy_ || last_backspace_ - opened_at_ < kInputGuard) return;
    buffer_.Backspace();
    SetMessage("", false);
    RefreshText();
}

void OnScreenKeyboard::MoveCaret(int delta) {
    buffer_.MoveCaret(delta);
    RefreshText();
}

void OnScreenKeyboard::CycleShift() {
    if (info_.numpad) return;
    shift_ = shift_ == ShiftState::Off ? ShiftState::Once : shift_ == ShiftState::Once ? ShiftState::Caps : ShiftState::Off;
    RefreshKeys();
}

void OnScreenKeyboard::CyclePage(int delta) {
    if (info_.numpad) return;
    page_ = static_cast<KeyPage>((static_cast<int>(page_) + delta + kKeyPageCount) % kKeyPageCount);
    RefreshKeys();
}

void OnScreenKeyboard::Press(int def_index) {
    if (def_index < 0 || static_cast<std::size_t>(def_index) >= defs_.size()) return;
    if (busy_ || std::chrono::steady_clock::now() - opened_at_ < kInputGuard) return;
    const KeyDef d = defs_[static_cast<std::size_t>(def_index)]; // a copy: refreshes below rewrite defs_
    switch (d.kind) {
    case KeyKind::Char:
        TypeText(d.text);
        if (shift_ == ShiftState::Once) {
            shift_ = ShiftState::Off;
            RefreshKeys();
        }
        break;
    case KeyKind::Shift: CycleShift(); break;
    case KeyKind::Page: CyclePage(1); break;
    case KeyKind::Space: TypeText(" "); break;
    case KeyKind::Backspace: Backspace(); break;
    case KeyKind::Left: MoveCaret(-1); break;
    case KeyKind::Right: MoveCaret(1); break;
    case KeyKind::Enter: TypeText("\n"); break;
    case KeyKind::Clear:
        buffer_.Clear();
        SetMessage("", false);
        RefreshText();
        break;
    }
    if (d.kind == KeyKind::Char || d.kind == KeyKind::Space) AppServices::Get().PlaySfx("select");
}

void OnScreenKeyboard::Commit(int button) {
    if (busy_ || !root_) return;
    if (std::chrono::steady_clock::now() - opened_at_ < kInputGuard) return;
    const bool is_ok = button == info_.OkButton();
    busy_ = true;
    busy_since_ = std::chrono::steady_clock::now();
    retry_text_ = is_ok ? buffer_.Text() : std::string();
    SetMessage("", false);
    // The core validates the text on its own thread: the answer comes back as Hide() (accepted)
    // or ShowRefusal().
    if (callbacks_.submit) callbacks_.submit(is_ok ? buffer_.Text() : std::string(), button);
}

// ---------------------------------------------------------------------------
// Controller and keyboard

void OnScreenKeyboard::StartHoldClose() {
    if (!hold_) {
        hold_ = DispatcherTimer();
        hold_.Interval(std::chrono::milliseconds(kHoldCloseMs));
        auto weak = weak_from_this();
        hold_.Tick([weak](auto&&, auto&&) {
            auto s = weak.lock();
            if (!s) return;
            s->StopHoldClose();
            if (s->busy_ || !s->root_) return;
            s->busy_ = true;
            s->busy_since_ = std::chrono::steady_clock::now();
            ONYX_INFO("Keyboard: closed with the hold-View escape");
            if (s->callbacks_.abort) s->callbacks_.abort(s->buffer_.Text());
        });
    }
    hold_.Start();
}

void OnScreenKeyboard::StopHoldClose() {
    if (hold_) hold_.Stop();
}

namespace {
// On Xbox the XAML Key can be a translated key (A arrives as Space or Enter); OriginalKey is
// the real button.
winrt::Windows::System::VirtualKey RealKey(winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
    try {
        return e.OriginalKey();
    } catch (...) {
        return e.Key();
    }
}
bool IsPadKey(winrt::Windows::System::VirtualKey k) {
    const int v = static_cast<int>(k);
    return v >= 195 && v <= 218;
}
} // namespace

void OnScreenKeyboard::OnGamepadKey(KeyRoutedEventArgs const& e) {
    using winrt::Windows::System::VirtualKey;
    last_pad_key_ = std::chrono::steady_clock::now();
    const bool repeat = e.KeyStatus().WasKeyDown;
    const bool ready = !busy_ && std::chrono::steady_clock::now() - opened_at_ >= kInputGuard;
    switch (RealKey(e)) {
    case VirtualKey::GamepadB:
        if (ready) Backspace();
        e.Handled(true);
        break;
    case VirtualKey::GamepadX:
        if (ready && !info_.numpad) TypeText(" ");
        e.Handled(true);
        break;
    case VirtualKey::GamepadY:
        if (ready && !repeat) CycleShift();
        e.Handled(true);
        break;
    case VirtualKey::GamepadLeftShoulder:
        if (ready) MoveCaret(-1);
        e.Handled(true);
        break;
    case VirtualKey::GamepadRightShoulder:
        if (ready) MoveCaret(1);
        e.Handled(true);
        break;
    case VirtualKey::GamepadLeftTrigger:
        if (ready && !repeat) CyclePage(-1);
        e.Handled(true);
        break;
    case VirtualKey::GamepadRightTrigger:
        if (ready && !repeat) CyclePage(1);
        e.Handled(true);
        break;
    case VirtualKey::GamepadMenu:
        if (!repeat) Commit(info_.OkButton());
        e.Handled(true);
        break;
    case VirtualKey::GamepadView:
        if (!repeat) StartHoldClose();
        e.Handled(true);
        break;
    default:
        break;
    }
}

void OnScreenKeyboard::OnGamepadKeyUp(KeyRoutedEventArgs const& e) {
    using winrt::Windows::System::VirtualKey;
    if (RealKey(e) != VirtualKey::GamepadView) return;
    e.Handled(true);
    if (!hold_ || !hold_.IsEnabled()) return; // the hold already closed it
    StopHoldClose();
    // A tap of View is Cancel, where the game has a Cancel button.
    if (info_.HasCancel()) Commit(0);
    else SetMessage("Hold View to close this keyboard.", false);
}

void OnScreenKeyboard::OnPhysicalKey(KeyRoutedEventArgs const& e) {
    using winrt::Windows::System::VirtualKey;
    const auto real = RealKey(e);
    if (IsPadKey(real)) { // controller buttons: OnGamepadKey
        last_pad_key_ = std::chrono::steady_clock::now();
        return;
    }
    last_real_key_ = std::chrono::steady_clock::now();
    const bool ready = !busy_ && std::chrono::steady_clock::now() - opened_at_ >= kInputGuard;
    switch (real) {
    case VirtualKey::Back:
        if (ready) Backspace();
        e.Handled(true);
        break;
    case VirtualKey::Delete:
        if (ready && buffer_.Delete()) RefreshText();
        e.Handled(true);
        break;
    case VirtualKey::Home:
        buffer_.CaretHome();
        RefreshText();
        e.Handled(true);
        break;
    case VirtualKey::End:
        buffer_.CaretEnd();
        RefreshText();
        e.Handled(true);
        break;
    case VirtualKey::Enter:
        // Stops a focused key from also being pressed.
        if (ready) {
            if (info_.rules.multiline) TypeText("\n");
            else Commit(info_.OkButton());
        }
        e.Handled(true);
        break;
    case VirtualKey::Space:
        e.Handled(true); // the space itself arrives as a character
        break;
    case VirtualKey::Escape:
        if (info_.HasCancel()) Commit(0);
        e.Handled(true);
        break;
    default:
        break;
    }
}

void OnScreenKeyboard::OnCharacter(uint32_t code) {
    if (!root_ || busy_) return;
    if (std::chrono::steady_clock::now() - opened_at_ < kInputGuard) return;
    // Only a real keyboard types. A controller button press can come with a stray space
    // character; accept characters only right after a real key and not during a pad press.
    const auto now = std::chrono::steady_clock::now();
    if (now - last_pad_key_ < std::chrono::milliseconds(600)) return;
    if (now - last_real_key_ > std::chrono::milliseconds(600)) return;
    if (code < 0x20 || code == 0x7F || code > 0x10FFFF) return; // Enter, Tab, Backspace, Esc...
    TypeText(CodepointsToUtf8(std::u32string(1, static_cast<char32_t>(code))));
}

} // namespace onyx::app::ui

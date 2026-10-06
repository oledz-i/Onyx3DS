// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Ui/UiKit.h"

#include "Platform/Log.h"
#include "Ui/AppServices.h"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Media::Animation;
using winrt::Windows::Foundation::IInspectable;

namespace onyx::app::ui {

hstring H(const std::string& utf8) {
    return hstring(Wide(utf8));
}

std::string S(hstring const& h) {
    return Utf8(h);
}

namespace {
const Theme& T() {
    return AppServices::Get().CurrentTheme();
}
Brush B(const std::string& hex) {
    return AppServices::Get().ThemeBrush(hex);
}
} // namespace

TextBlock Text(const std::string& text, double size, bool bold, const std::string& color_hex) {
    TextBlock tb;
    tb.Text(H(text));
    tb.FontSize(size);
    tb.FontFamily(AppServices::Get().ThemeFont(bold));
    if (bold && !AppServices::Get().ThemeHasBoldFont())
        tb.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    tb.Foreground(B(color_hex.empty() ? T().colors.text : color_hex));
    tb.TextWrapping(TextWrapping::WrapWholeWords);
    return tb;
}

TextBlock Glyph(const wchar_t* glyph, double size, const std::string& color_hex) {
    TextBlock tb;
    tb.Text(glyph);
    tb.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
    tb.FontSize(size);
    tb.Foreground(B(color_hex.empty() ? T().colors.accent : color_hex));
    return tb;
}

Border Card(UIElement const& content, double padding) {
    Border b;
    b.Background(B(T().colors.panel));
    b.CornerRadius(CornerRadius{18, 18, 18, 18});
    b.Padding(Thickness{padding, padding, padding, padding});
    b.BorderBrush(B(T().colors.tile_edge));
    b.BorderThickness(Thickness{1, 1, 1, 1});
    b.Child(content);
    return b;
}

Button ActionButton(const std::string& label, const wchar_t* glyph, std::function<void()> on_click,
                    bool primary) {
    Button btn;
    StackPanel row;
    row.Orientation(Orientation::Horizontal);
    row.Spacing(12);
    if (glyph) {
        auto g = Glyph(glyph, 22, primary ? T().colors.accent_text : T().colors.accent);
        g.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(g);
    }
    auto t = Text(label, 22, true, primary ? T().colors.accent_text : T().colors.text);
    t.VerticalAlignment(VerticalAlignment::Center);
    row.Children().Append(t);
    btn.Content(row);
    btn.Padding(Thickness{26, 14, 30, 14});
    btn.CornerRadius(CornerRadius{14, 14, 14, 14});
    btn.Background(B(primary ? T().colors.accent : T().colors.tile_face));
    btn.BorderBrush(B(T().colors.tile_edge));
    btn.MinWidth(180);
    btn.Click([on_click](auto&&, auto&&) {
        AppServices::Get().PlaySfx("select");
        if (on_click) on_click();
    });
    btn.GotFocus([](auto&&, auto&&) { AppServices::Get().PlaySfx("move"); });
    return btn;
}

ToggleSwitch Toggle(bool value, std::function<void(bool)> changed) {
    ToggleSwitch ts;
    ts.IsOn(value);
    ts.OnContent(box_value(L"On"));
    ts.OffContent(box_value(L"Off"));
    ts.Toggled([changed](IInspectable const& sender, auto&&) {
        if (changed) changed(sender.as<ToggleSwitch>().IsOn());
    });
    return ts;
}

ComboBox Choice(const std::vector<std::pair<std::string, std::string>>& value_label,
                const std::string& selected, std::function<void(const std::string&)> changed) {
    ComboBox cb;
    cb.MinWidth(320);
    int index = 0;
    for (size_t i = 0; i < value_label.size(); ++i) {
        ComboBoxItem item;
        item.Content(box_value(H(value_label[i].second)));
        item.Tag(box_value(H(value_label[i].first)));
        cb.Items().Append(item);
        if (value_label[i].first == selected) index = static_cast<int>(i);
    }
    if (!value_label.empty()) cb.SelectedIndex(index);
    cb.SelectionChanged([changed](IInspectable const& sender, auto&&) {
        auto box = sender.as<ComboBox>();
        if (auto item = box.SelectedItem().try_as<ComboBoxItem>(); item && changed)
            changed(S(unbox_value<hstring>(item.Tag())));
    });
    return cb;
}

Slider Range(double min, double max, double step, double value, std::function<void(double)> changed) {
    Slider s;
    s.Minimum(min);
    s.Maximum(max);
    s.StepFrequency(step);
    s.SmallChange(step);
    s.Value(value);
    s.MinWidth(320);
    s.ValueChanged([changed](auto&&, Primitives::RangeBaseValueChangedEventArgs const& e) {
        if (changed) changed(e.NewValue());
    });
    return s;
}

TextBox Field(const std::string& value, const std::string& placeholder,
              std::function<void(const std::string&)> committed, bool password) {
    TextBox tb;
    tb.Text(H(value));
    tb.PlaceholderText(H(placeholder));
    tb.MinWidth(420);
    tb.IsSpellCheckEnabled(false);
    tb.IsTextPredictionEnabled(false);
    if (password) {
        // Keep passwords out of the on-screen keyboard's suggestions.
        winrt::Windows::UI::Xaml::Input::InputScope scope;
        winrt::Windows::UI::Xaml::Input::InputScopeName name(
            winrt::Windows::UI::Xaml::Input::InputScopeNameValue::Password);
        scope.Names().Append(name);
        tb.InputScope(scope);
    }
    tb.LostFocus([committed](IInspectable const& sender, auto&&) {
        if (committed) committed(S(sender.as<TextBox>().Text()));
    });
    return tb;
}

UIElement SettingRow(const std::string& label, const std::string& help, UIElement const& control) {
    Grid g;
    ColumnDefinition c0, c1;
    c0.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    c1.Width(GridLengthHelper::Auto());
    g.ColumnDefinitions().Append(c0);
    g.ColumnDefinitions().Append(c1);
    StackPanel left;
    left.Spacing(4);
    left.Children().Append(Text(label, 22, true));
    if (!help.empty()) left.Children().Append(Text(help, 16, false, T().colors.text_muted));
    left.Margin(Thickness{0, 0, 32, 0});
    left.VerticalAlignment(VerticalAlignment::Center);
    g.Children().Append(left);
    auto fe = control.as<FrameworkElement>();
    fe.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(fe, 1);
    g.Children().Append(control);

    // The row lights up (accent wash + accent edge) while its control has focus, so
    // it's always clear on the TV which setting the controller is on.
    Border row;
    row.CornerRadius(CornerRadius{14, 14, 14, 14});
    row.Padding(Thickness{18, 12, 18, 12});
    row.Margin(Thickness{-18, 2, -18, 2});
    row.BorderThickness(Thickness{3, 0, 0, 0});
    const auto idle_bg = B("#00000000");
    const auto idle_edge = B("#00000000");
    row.Background(idle_bg);
    row.BorderBrush(idle_edge);
    row.BackgroundTransition(BrushTransition());
    row.Child(g);
    const std::string accent = T().colors.accent;
    // The handlers take the row from `sender`: capturing it would keep every row alive.
    row.GotFocus([accent](IInspectable const& sender, RoutedEventArgs const&) {
        if (auto r = sender.try_as<Border>()) {
            r.Background(B(WithAlpha(accent, 0x2A)));
            r.BorderBrush(B(accent));
        }
    });
    row.LostFocus([idle_bg, idle_edge](IInspectable const& sender, RoutedEventArgs const&) {
        if (auto r = sender.try_as<Border>()) {
            r.Background(idle_bg);
            r.BorderBrush(idle_edge);
        }
    });
    return row;
}

UIElement SectionHeader(const std::string& title) {
    StackPanel p;
    p.Orientation(Orientation::Horizontal);
    p.Spacing(14);
    p.Margin(Thickness{0, 28, 0, 8});
    Border bar;
    bar.Width(6);
    bar.CornerRadius(CornerRadius{3, 3, 3, 3});
    bar.Background(B(T().colors.accent));
    bar.Margin(Thickness{0, 6, 0, 6});
    p.Children().Append(bar);
    auto t = Text(title, 30, true);
    t.VerticalAlignment(VerticalAlignment::Center);
    p.Children().Append(t);
    p.Tag(box_value(hstring(L"hdr")));
    return p;
}

std::string WithAlpha(const std::string& hex, unsigned alpha) {
    if (hex.size() < 7) return hex;
    char a[3];
    std::snprintf(a, sizeof(a), "%02X", alpha & 0xFF);
    return hex.substr(0, 7) + a;
}

Border Chip(const std::string& text, const wchar_t* glyph, bool accent) {
    StackPanel row;
    row.Orientation(Orientation::Horizontal);
    row.Spacing(8);
    if (glyph) {
        auto g = Glyph(glyph, 16, accent ? T().colors.accent_text : T().colors.accent);
        g.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(g);
    }
    auto t = Text(text, 16, true, accent ? T().colors.accent_text : T().colors.text);
    t.TextWrapping(TextWrapping::NoWrap);
    t.VerticalAlignment(VerticalAlignment::Center);
    row.Children().Append(t);
    Border b;
    b.CornerRadius(CornerRadius{16, 16, 16, 16});
    b.Padding(Thickness{14, 6, 14, 6});
    b.Background(B(accent ? T().colors.accent : WithAlpha(T().colors.accent, 0x22)));
    b.BorderBrush(B(WithAlpha(T().colors.accent, 0x66)));
    b.BorderThickness(Thickness{1, 1, 1, 1});
    b.Child(row);
    return b;
}

UIElement ButtonHint(const std::string& button, const std::string& label,
                     const std::string& text_color_hex) {
    struct Look { const char* name; const char* fill; const char* ink; };
    static const Look looks[] = {
        {"A", "#3FAE2A", "#FFFFFF"}, {"B", "#E0383E", "#FFFFFF"},
        {"X", "#2F7FE0", "#FFFFFF"}, {"Y", "#F2B807", "#1A1A1A"},
    };
    const Look* look = nullptr;
    for (const auto& l : looks)
        if (button == l.name) look = &l;
    StackPanel row;
    row.Orientation(Orientation::Horizontal);
    row.Spacing(10);
    Border key;
    key.Height(30);
    key.MinWidth(30);
    key.CornerRadius(CornerRadius{15, 15, 15, 15});
    key.Padding(Thickness{look ? 0.0 : 10.0, 0, look ? 0.0 : 10.0, 0});
    key.Background(B(look ? look->fill : "#3A3F4AE6"));
    key.BorderBrush(B("#FFFFFF40"));
    key.BorderThickness(Thickness{1, 1, 1, 1});
    auto k = Text(button, look ? 17 : 14, true, look ? look->ink : "#FFFFFF");
    k.TextWrapping(TextWrapping::NoWrap);
    k.HorizontalAlignment(HorizontalAlignment::Center);
    k.VerticalAlignment(VerticalAlignment::Center);
    key.Child(k);
    row.Children().Append(key);
    auto t = Text(label, 18, false, text_color_hex.empty() ? T().colors.text : text_color_hex);
    t.TextWrapping(TextWrapping::NoWrap);
    t.VerticalAlignment(VerticalAlignment::Center);
    row.Children().Append(t);
    return row;
}

UIElement HintBar(const std::vector<std::pair<std::string, std::string>>& hints,
                  const std::string& text_color_hex) {
    StackPanel bar;
    bar.Orientation(Orientation::Horizontal);
    bar.Spacing(28);
    for (const auto& [button, label] : hints)
        bar.Children().Append(ButtonHint(button, label, text_color_hex));
    return bar;
}

UIElement OnyxMark(double size) {
    using namespace winrt::Windows::UI::Xaml::Shapes;
    Grid g;
    g.Width(size);
    g.Height(size);
    const double side = size * 0.70;
    Rectangle diamond;
    diamond.Width(side);
    diamond.Height(side);
    diamond.RadiusX(side * 0.22);
    diamond.RadiusY(side * 0.22);
    diamond.Fill(B("#18162A"));
    diamond.Stroke(B("#9B7BFF"));
    diamond.StrokeThickness(std::max(2.0, size * 0.06));
    diamond.RenderTransformOrigin(winrt::Windows::Foundation::Point{0.5, 0.5});
    RotateTransform rot;
    rot.Angle(45);
    diamond.RenderTransform(rot);
    diamond.HorizontalAlignment(HorizontalAlignment::Center);
    diamond.VerticalAlignment(VerticalAlignment::Center);
    g.Children().Append(diamond);
    auto screen = [&](double w, double h, double y, const char* fill) {
        Rectangle r;
        r.Width(w);
        r.Height(h);
        r.RadiusX(size * 0.035);
        r.RadiusY(size * 0.035);
        r.Fill(B(fill));
        r.HorizontalAlignment(HorizontalAlignment::Center);
        r.VerticalAlignment(VerticalAlignment::Center);
        r.Margin(Thickness{0, y, 0, -y});
        g.Children().Append(r);
    };
    screen(size * 0.36, size * 0.19, -size * 0.13, "#78DCF0");
    screen(size * 0.27, size * 0.19, size * 0.13, "#C8BEFF");
    return g;
}

void Entrance(UIElement const& e, double dx, double dy, int delay_ms, int duration_ms) {
    using winrt::Windows::Foundation::TimeSpan;
    const TimeSpan dur{std::chrono::milliseconds(duration_ms)};
    const TimeSpan delay{std::chrono::milliseconds(delay_ms)};
    e.Opacity(0);
    Storyboard sb;
    DoubleAnimation fade;
    fade.From(0.0);
    fade.To(1.0);
    fade.Duration(DurationHelper::FromTimeSpan(dur));
    fade.BeginTime(delay);
    Storyboard::SetTarget(fade, e);
    Storyboard::SetTargetProperty(fade, L"Opacity");
    sb.Children().Append(fade);
    if (dx != 0 || dy != 0) {
        TranslateTransform tr;
        tr.X(dx);
        tr.Y(dy);
        e.RenderTransform(tr);
        CubicEase ease;
        ease.EasingMode(EasingMode::EaseOut);
        for (int axis = 0; axis < 2; ++axis) {
            const double from = axis == 0 ? dx : dy;
            if (from == 0) continue;
            DoubleAnimation slide;
            slide.From(from);
            slide.To(0.0);
            slide.Duration(DurationHelper::FromTimeSpan(dur));
            slide.BeginTime(delay);
            slide.EasingFunction(ease);
            Storyboard::SetTarget(slide, tr);
            Storyboard::SetTargetProperty(slide, axis == 0 ? L"X" : L"Y");
            sb.Children().Append(slide);
        }
    }
    sb.Begin();
}

void ShowToast(Panel const& host, const std::string& text, const wchar_t* glyph) {
    if (!host) return;
    StackPanel row;
    row.Orientation(Orientation::Horizontal);
    row.Spacing(12);
    row.Children().Append(Glyph(glyph ? glyph : glyph::Info, 20));
    auto t = Text(text, 20, true);
    t.MaxWidth(560);
    row.Children().Append(t);
    auto card = Card(row, 16);
    card.Margin(Thickness{0, 8, 0, 0});
    card.HorizontalAlignment(HorizontalAlignment::Right);
    card.Opacity(0);
    host.Children().Append(card);

    // Fade in, hold, fade out, remove.
    Storyboard sb;
    DoubleAnimationUsingKeyFrames fade;
    auto key = [](double t, double v) {
        LinearDoubleKeyFrame k;
        k.KeyTime(KeyTimeHelper::FromTimeSpan(std::chrono::milliseconds(static_cast<int>(t))));
        k.Value(v);
        return k;
    };
    fade.KeyFrames().Append(key(0, 0));
    fade.KeyFrames().Append(key(180, 1));
    fade.KeyFrames().Append(key(3200, 1));
    fade.KeyFrames().Append(key(3600, 0));
    Storyboard::SetTarget(fade, card);
    Storyboard::SetTargetProperty(fade, L"Opacity");
    sb.Children().Append(fade);
    sb.Completed([host, card](auto&&, auto&&) {
        uint32_t index;
        if (host.Children().IndexOf(card, index)) host.Children().RemoveAt(index);
    });
    sb.Begin();
}

void Confirm(const std::string& title, const std::string& body, const std::string& yes,
             std::function<void()> on_yes) {
    ContentDialog d;
    d.Title(box_value(H(title)));
    d.Content(box_value(H(body)));
    d.PrimaryButtonText(H(yes));
    d.CloseButtonText(L"Cancel");
    d.DefaultButton(ContentDialogButton::Close);
    d.PrimaryButtonClick([on_yes](auto&&, auto&&) {
        if (on_yes) on_yes();
    });
    d.ShowAsync();
}

} // namespace onyx::app::ui

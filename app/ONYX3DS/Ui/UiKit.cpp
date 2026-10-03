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
    tb.FontFamily(FontFamily(H(T().style.font)));
    if (bold) tb.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
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
    g.Margin(Thickness{0, 10, 0, 10});
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
    return g;
}

TextBlock SectionHeader(const std::string& title) {
    auto t = Text(title, 30, true);
    t.Margin(Thickness{0, 24, 0, 8});
    return t;
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

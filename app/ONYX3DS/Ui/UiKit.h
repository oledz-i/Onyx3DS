// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small helpers for building gamepad-friendly XAML in code. The pages build
// most of their UI here rather than in XAML templates, which keeps the theme
// colours dynamic and avoids data-binding boilerplate.
#pragma once

namespace onyx::app::ui {

namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;

winrt::hstring H(const std::string& utf8);
std::string S(winrt::hstring const& h);

// Theme-aware text (colour from the current theme unless given).
wuxc::TextBlock Text(const std::string& text, double size, bool bold = false,
                     const std::string& color_hex = {});
wuxc::TextBlock Glyph(const wchar_t* glyph, double size, const std::string& color_hex = {});

// Rounded translucent panel using the theme's panel colour.
wuxc::Border Card(wux::UIElement const& content, double padding = 24);

wuxc::Button ActionButton(const std::string& label, const wchar_t* glyph,
                          std::function<void()> on_click, bool primary = false);
wuxc::ToggleSwitch Toggle(bool value, std::function<void(bool)> changed);
wuxc::ComboBox Choice(const std::vector<std::pair<std::string, std::string>>& value_label,
                      const std::string& selected, std::function<void(const std::string&)> changed);
wuxc::Slider Range(double min, double max, double step, double value,
                   std::function<void(double)> changed);
wuxc::TextBox Field(const std::string& value, const std::string& placeholder,
                    std::function<void(const std::string&)> committed, bool password = false);

// Label + one-line help on the left, control on the right.
wux::UIElement SettingRow(const std::string& label, const std::string& help,
                          wux::UIElement const& control);
wux::UIElement SectionHeader(const std::string& title); // Tag "hdr"

// Small rounded pill, e.g. "Series S" or "Build 6ac44363".
wuxc::Border Chip(const std::string& text, const wchar_t* glyph = nullptr, bool accent = false);
// Controller button hint: an Xbox-coloured round button with its letter, then a label.
// `button` is "A", "B", "X", "Y", "LB", "RB", "LB/RB", "View" or "Menu".
wux::UIElement ButtonHint(const std::string& button, const std::string& label,
                          const std::string& text_color_hex = {});
// A row of button hints.
wux::UIElement HintBar(const std::vector<std::pair<std::string, std::string>>& hints,
                       const std::string& text_color_hex = {});
// The ONYX mark (rounded diamond with the two 3DS screens), drawn with shapes.
wux::UIElement OnyxMark(double size);
// Fades an element in, optionally sliding it by (dx, dy) to its place. Elements that
// use the Translation/Scale properties must pass dx = dy = 0 (opacity only).
void Entrance(wux::UIElement const& e, double dx = 0, double dy = 0, int delay_ms = 0,
              int duration_ms = 260);
// Hex colour with an alpha byte ("#RRGGBB" + alpha -> "#RRGGBBAA").
std::string WithAlpha(const std::string& hex, unsigned alpha);

// Brief notification in the bottom-right of `host` (a Panel on the page).
void ShowToast(wuxc::Panel const& host, const std::string& text, const wchar_t* glyph = nullptr);

// Two-button confirmation. `on_yes` runs on the UI thread.
void Confirm(const std::string& title, const std::string& body, const std::string& yes,
             std::function<void()> on_yes);

// Segoe MDL2 glyphs used around the app.
namespace glyph {
inline constexpr const wchar_t* Play = L"";
inline constexpr const wchar_t* Settings = L"";
inline constexpr const wchar_t* Folder = L"";
inline constexpr const wchar_t* Save = L"";
inline constexpr const wchar_t* Load = L"";
inline constexpr const wchar_t* Camera = L"";
inline constexpr const wchar_t* Layout = L"";
inline constexpr const wchar_t* Cheat = L"";
inline constexpr const wchar_t* Reset = L"";
inline constexpr const wchar_t* Quit = L"";
inline constexpr const wchar_t* Star = L"";
inline constexpr const wchar_t* StarFilled = L"";
inline constexpr const wchar_t* Picture = L"";
inline constexpr const wchar_t* Download = L"";
inline constexpr const wchar_t* Trophy = L"";
inline constexpr const wchar_t* Grid = L"";
inline constexpr const wchar_t* Usb = L"";
inline constexpr const wchar_t* Music = L"";
inline constexpr const wchar_t* Info = L"";
inline constexpr const wchar_t* Back = L"";
inline constexpr const wchar_t* Refresh = L"";
inline constexpr const wchar_t* Hide = L"";
inline constexpr const wchar_t* Palette = L"";
inline constexpr const wchar_t* Gamepad = L"";
inline constexpr const wchar_t* Speed = L"";
inline constexpr const wchar_t* Check = L"";
inline constexpr const wchar_t* Warning = L"";
inline constexpr const wchar_t* Globe = L"";
inline constexpr const wchar_t* Package = L"";
} // namespace glyph

} // namespace onyx::app::ui

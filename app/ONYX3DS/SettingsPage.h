// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "SettingsPage.g.h"

namespace winrt::ONYX3DS::implementation {

struct SettingsPage : SettingsPageT<SettingsPage> {
    SettingsPage() = default;
    void InitializeComponent();
    void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs const& e);
    void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs const& e);

private:
    void Show(const std::string& section);
    void ApplyPageTheme();
    void BuildFolders();
    void BuildEmulation();
    void BuildControls();
    void BuildInterface();
    void BuildUpdates();
    void BuildServices();
    void BuildSystem();
    void BuildAbout();
    void Add(Windows::UI::Xaml::UIElement const& e);
    void Note(const std::string& text);

    std::string section_ = "folders";
    bool library_dirty_ = false;
};

} // namespace winrt::ONYX3DS::implementation

namespace winrt::ONYX3DS::factory_implementation {
struct SettingsPage : SettingsPageT<SettingsPage, implementation::SettingsPage> {};
} // namespace winrt::ONYX3DS::factory_implementation

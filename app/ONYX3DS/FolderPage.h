// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "FolderPage.g.h"
#include "onyx/paths.h"

namespace winrt::ONYX3DS::implementation {

struct FolderPage : FolderPageT<FolderPage> {
    FolderPage() = default;
    void InitializeComponent();
    void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs const& e);

private:
    void Browse(const std::string& dir);
    void Choose();

    onyx::FolderKind kind_ = onyx::FolderKind::Roms;
    std::string current_; // empty = list of drives
};

} // namespace winrt::ONYX3DS::implementation

namespace winrt::ONYX3DS::factory_implementation {
struct FolderPage : FolderPageT<FolderPage, implementation::FolderPage> {};
} // namespace winrt::ONYX3DS::factory_implementation

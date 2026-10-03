// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "App.xaml.g.h"

namespace winrt::ONYX3DS::implementation {

struct App : AppT<App> {
    App();
    void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs const& e);

private:
    void OnSuspending(Windows::Foundation::IInspectable const& sender,
                      Windows::ApplicationModel::SuspendingEventArgs const& e);
    void OnResuming(Windows::Foundation::IInspectable const& sender, Windows::Foundation::IInspectable const& e);
};

} // namespace winrt::ONYX3DS::implementation

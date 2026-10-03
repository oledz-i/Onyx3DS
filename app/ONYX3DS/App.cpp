// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "App.h"

#include "MainPage.h"
#include "Platform/Log.h"
#include "Ui/AppServices.h"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::ViewManagement;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Navigation;
using namespace onyx::app;

namespace winrt::ONYX3DS::implementation {

App::App() {
    InitializeComponent();
    Suspending({this, &App::OnSuspending});
    Resuming({this, &App::OnResuming});

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
    UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
        if (IsDebuggerPresent()) {
            auto message = e.Message();
            __debugbreak();
        }
    });
#endif
    UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
        ONYX_ERROR("Unhandled exception: %s", Utf8(e.Message()).c_str());
    });
}

void App::OnLaunched(LaunchActivatedEventArgs const& e) {
    LogInit(Wide(Paths().log_file));
    ONYX_INFO("ONYX 3DS starting");

    // Full 1920x1080 layout on Xbox (otherwise XAML scales to 200% = 960x540)
    // and draw to the edges; pages keep their own TV-safe margins.
    ApplicationViewScaling::TrySetDisableLayoutScaling(true);
    ApplicationView::GetForCurrentView().SetDesiredBoundsMode(ApplicationViewBoundsMode::UseCoreWindow);
    ApplicationView::GetForCurrentView().FullScreenSystemOverlayMode(FullScreenSystemOverlayMode::Minimal);

    AppServices::Get().Initialize();

    Frame root = Window::Current().Content().try_as<Frame>();
    if (!root) {
        root = Frame();
        root.NavigationFailed([](IInspectable const&, NavigationFailedEventArgs const& args) {
            ONYX_ERROR("Navigation to %s failed", Utf8(args.SourcePageType().Name).c_str());
        });
        Window::Current().Content(root);
    }
    // B / Back: let the page decide first (close a panel, ignore in-game),
    // then go back a page.
    winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested(
        [root](IInspectable const&, winrt::Windows::UI::Core::BackRequestedEventArgs const& args) {
            if (AppServices::Get().TryHandleBack()) {
                args.Handled(true);
                return;
            }
            if (root.CanGoBack()) {
                AppServices::Get().PlaySfx("back");
                root.GoBack();
                args.Handled(true);
            }
        });
    // Every page sets (or clears) its back override in OnNavigatedTo.
    if (!e.PrelaunchActivated()) {
        if (!root.Content()) root.Navigate(xaml_typename<ONYX3DS::MainPage>());
        Window::Current().Activate();
    }

    // Bring up graphics and run the Dozen self-test without blocking the menu.
    RunAsync([] {
        std::string error;
        if (!EmulatorSession::Get().Initialize(error)) {
            ONYX_ERROR("Graphics initialisation failed: %s", error.c_str());
            AppServices::Get().Toast("Graphics setup failed: " + error +
                                     ". See Settings > System check.");
        }
    });
}

void App::OnSuspending(IInspectable const&, SuspendingEventArgs const& e) {
    // The console suspends us when another app or game takes over. Pause the
    // emulator so time does not jump, and save settings while we can.
    auto deferral = e.SuspendingOperation().GetDeferral();
    auto& session = EmulatorSession::Get();
    if (session.State() == SessionState::Running) session.SetPaused(true);
    AppServices::Get().SaveSettings();
    deferral.Complete();
}

void App::OnResuming(IInspectable const&, IInspectable const&) {
    // Stay paused: the ONYX menu is open on top of the game, so the player
    // chooses when to continue.
    ONYX_INFO("Resumed");
}

} // namespace winrt::ONYX3DS::implementation

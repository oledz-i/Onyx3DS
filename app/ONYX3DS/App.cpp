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

namespace {
// Replaces the window content with the error text (and the log location) when
// a page cannot be built. Safe to call repeatedly; only the first message sticks.
void ShowFatal(std::wstring const& message) {
    static bool shown = false;
    if (shown) return;
    shown = true;
    try {
        TextBlock text;
        text.TextWrapping(TextWrapping::Wrap);
        text.FontSize(28);
        text.Margin(Thickness{96, 64, 96, 64});
        text.Foreground(winrt::Windows::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Colors::White()));
        text.Text(L"ONYX 3DS hit a problem.\n\n" + message +
                  L"\n\nTake a photo of this screen. The full log is in the Device Portal under "
                  L"File explorer > LocalAppData > ONYX3DS > LocalState > onyx.log.");
        Grid g;
        g.Background(winrt::Windows::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(255, 24, 26, 32)));
        g.Children().Append(text);
        Window::Current().Content(g);
        Window::Current().Activate();
    } catch (...) {
    }
}
} // namespace

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
    // Never vanish back to Dev Home: log it, keep running, and say so on
    // screen so there is something to report.
    UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
        ONYX_ERROR("Unhandled exception: %s", Utf8(e.Message()).c_str());
        e.Handled(true);
        ShowFatal(L"Unexpected error: " + std::wstring(e.Message()));
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
            ONYX_ERROR("Navigation to %s failed: 0x%08X", Utf8(args.SourcePageType().Name).c_str(),
                       static_cast<unsigned>(args.Exception().value));
            args.Handled(true);
            ShowFatal(L"Could not open " + std::wstring(args.SourcePageType().Name) + L" (error 0x" +
                      std::to_wstring(static_cast<unsigned>(args.Exception().value)) + L")");
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

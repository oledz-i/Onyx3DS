// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Platform/SaveBackup.h"
#include "EmulationPage.h"
#if __has_include("EmulationPage.g.cpp")
#include "EmulationPage.g.cpp"
#endif

#include "Emu/EmulatorSession.h"
#include "Emu/KeyboardBridge.h"
#include "Platform/Imaging.h"
#include "Platform/Log.h"
#include "Ui/AppServices.h"
#include "Ui/OnScreenKeyboard.h"
#include "Ui/UiKit.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Navigation;
using namespace onyx;
using namespace onyx::app;
namespace kit = onyx::app::ui;

namespace winrt::ONYX3DS::implementation {

namespace {
AppServices& Svc() {
    return AppServices::Get();
}
EmulatorSession& Emu() {
    return EmulatorSession::Get();
}
} // namespace

void EmulationPage::InitializeComponent() {
    EmulationPageT::InitializeComponent();
    fps_timer_ = DispatcherTimer();
    fps_timer_.Interval(std::chrono::milliseconds(500));
    fps_timer_.Tick([weak = get_weak()](auto&&, auto&&) {
        if (auto self = weak.get()) self->UpdateFps();
    });

    Panel().SizeChanged([](IInspectable const& sender, SizeChangedEventArgs const& e) {
        auto panel = sender.as<SwapChainPanel>();
        Emu().Presenter().OnPanelResized(static_cast<float>(e.NewSize().Width),
                                         static_cast<float>(e.NewSize().Height),
                                         panel.CompositionScaleX(), panel.CompositionScaleY());
    });
    Panel().CompositionScaleChanged([](SwapChainPanel const& panel, auto&&) {
        Emu().Presenter().OnPanelResized(static_cast<float>(panel.ActualWidth()),
                                         static_cast<float>(panel.ActualHeight()),
                                         panel.CompositionScaleX(), panel.CompositionScaleY());
    });
    // A USB mouse works as the touch screen.
    Panel().PointerMoved([weak = get_weak()](auto&&, PointerRoutedEventArgs const& e) {
        if (auto self = weak.get()) self->OnPointer(e, e.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed());
    });
    Panel().PointerPressed([weak = get_weak()](auto&&, PointerRoutedEventArgs const& e) {
        if (auto self = weak.get()) self->OnPointer(e, true);
    });
    Panel().PointerReleased([weak = get_weak()](auto&&, PointerRoutedEventArgs const& e) {
        if (auto self = weak.get()) self->OnPointer(e, false);
    });
}

void EmulationPage::OnNavigatedTo(NavigationEventArgs const& e) {
    const std::string param = kit::S(unbox_value_or<hstring>(e.Parameter(), L""));
    const auto nl = param.find('\n');
    const std::string path = param.substr(0, nl);
    launch_mode_ = nl == std::string::npos ? "" : param.substr(nl + 1);
    if (auto g = Svc().FindGame(path)) game_ = *g;
    else {
        game_ = GameEntry{};
        game_.path = path;
        game_.title = CleanFileTitle(FileName(path));
        if (IsNesRomExtension(Extension(path))) game_.system = GameSystem::Nes;
    }
    show_fps_ = Svc().Config().qol.show_fps;
    started_ = quitting_ = menu_open_ = false;

    auto weak = get_weak();
    // In game, B belongs to the 3DS. Back only closes the ONYX menu.
    Svc().SetBackOverride([weak] {
        auto self = weak.get();
        if (!self) return true;
        // B belongs to the on-screen keyboard while it is open (it deletes a letter).
        if (self->KeyboardOpen()) self->kb_->OnBack();
        else if (self->menu_open_) self->CloseMenu();
        return true;
    });
    Svc().SetToastSink([weak](const std::string& text) {
        if (auto self = weak.get()) kit::ShowToast(self->ToastHost(), text);
    });

    LoadingTitle().Text(kit::H(game_.DisplayTitle()));
    if (game_.system == GameSystem::Nes)
        LoadingHint().Text(L"Hold View + press Menu any time for the ONYX menu.");
    else
        LoadingHint().Text(L"First launch of a game builds its shader cache, so it can take a little longer. "
                           L"Hold View + press Menu any time for the ONYX menu.");
    if (!game_.icon_path.empty())
        LoadingIcon().Source(IconFromRgba(Svc().Fs().ReadAll(game_.icon_path), 48, 48));
    LoadingOverlay().Visibility(Visibility::Visible);
    MenuOverlay().Visibility(Visibility::Collapsed);
    FpsText().Visibility(show_fps_ ? Visibility::Visible : Visibility::Collapsed);

    if (Panel().IsLoaded()) StartGame();
    else Panel().Loaded([weak](auto&&, auto&&) {
        if (auto self = weak.get()) self->StartGame();
    });
}

void EmulationPage::OnNavigatedFrom(NavigationEventArgs const&) {
    // Nothing may answer to this page any more, and the controller goes back to the game.
    KeyboardBridge::Get().SetHandlers(KeyboardHandlers{});
    HideKeyboard();
    kb_.reset();
    fps_timer_.Stop();
    StopSoftwareView();
    Emu().SetEvents({});
    Emu().Presenter().Detach();
    Emu().Presenter().SetCpuView(false);
    Emu().Presenter().ClearMirror();
    sw_seq_ = 0;
}

void EmulationPage::StartGame() {
    auto& session = Emu();
    if (!session.Ready()) {
        std::string err;
        if (!session.Initialize(err)) {
            OnStopped("Graphics are not available on this console: " + err);
            return;
        }
    }
    auto& cfg = Svc().Config();
    session.Input().SetSwapFaceButtons(cfg.qol.swap_face_buttons);
    session.Input().SetDeadzone(cfg.qol.stick_deadzone / 100.0f);
    session.Presenter().Attach(Panel());

    auto weak = get_weak();
    SessionEvents ev;
    ev.started = [weak] { RunOnUi([weak] { if (auto s = weak.get()) s->OnStarted(); }); };
    ev.stopped = [weak](const std::string& reason) {
        RunOnUi([weak, reason] { if (auto s = weak.get()) s->OnStopped(reason); });
    };
    ev.message = [](const std::string& text) { Svc().Toast(text); };
    ev.hotkey = [weak](Hotkey h) {
        RunOnUi([weak, h] { if (auto s = weak.get()) s->OnHotkey(static_cast<int>(h)); });
    };
    ev.achievement = [](const AchievementEvent& a) {
        if (Svc().Config().services.ra_notifications) Svc().Toast("\U0001F3C6 " + a.title + (a.detail.empty() ? "" : " - " + a.detail));
    };
    session.SetEvents(ev);
    if (game_.system == GameSystem::N3DS) InstallKeyboard();
    Svc().RetroAchievements().SetEventSink(ev.achievement);
    Svc().RetroAchievements().SetHardcore(cfg.services.ra_hardcore);

    std::string error;
    if (!session.Start(game_, cfg, Svc().Model(), error)) OnStopped(error);
    fps_timer_.Start();
}

void EmulationPage::StartSoftwareView() {
    if (sw_render_active_) return;
    sw_render_active_ = true;
    sw_render_token_ = Windows::UI::Xaml::Media::CompositionTarget::Rendering(
        [weak = get_weak()](auto&&, auto&&) {
            if (auto self = weak.get()) self->UpdateSoftwareView();
        });
}

void EmulationPage::StopSoftwareView() {
    if (!sw_render_active_) return;
    sw_render_active_ = false;
    Windows::UI::Xaml::Media::CompositionTarget::Rendering(sw_render_token_);
}

void EmulationPage::UpdateSoftwareView() {
    // How long the UI thread went without a XAML frame, and how long handing a
    // frame to XAML took: tells a XAML stall apart from a GPU one in the log.
    const auto t_in = std::chrono::steady_clock::now();
    {
        static auto s_prev = t_in;
        static int s_logged = 0;
        const double gap = std::chrono::duration<double, std::milli>(t_in - s_prev).count();
        s_prev = t_in;
        if (gap > 100.0 && (s_logged < 30 || s_logged % 50 == 0))
            ONYX_WARN("XAML frame gap: %.0f ms without a UI frame", gap);
        if (gap > 100.0) ++s_logged;
    }
    uint32_t w = 0, h = 0;
    if (!Emu().Presenter().TakeMirror(sw_pixels_, w, h, sw_seq_)) return;
    struct Timer {
        std::chrono::steady_clock::time_point t0;
        ~Timer() {
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            static int s_logged = 0;
            if (ms > 30.0 && (s_logged < 30 || s_logged % 50 == 0))
                ONYX_WARN("XAML frame update took %.0f ms", ms);
            if (ms > 30.0) ++s_logged;
        }
    } timer{std::chrono::steady_clock::now()};
    try {
        if (!sw_bitmap_ || sw_bitmap_.PixelWidth() != static_cast<int32_t>(w) ||
            sw_bitmap_.PixelHeight() != static_cast<int32_t>(h)) {
            sw_bitmap_ = Windows::UI::Xaml::Media::Imaging::WriteableBitmap(w, h);
            SoftwareView().Source(sw_bitmap_);
            ONYX_INFO("Software view: %ux%u through XAML", w, h);
        }
        uint8_t* dst = sw_bitmap_.PixelBuffer().data();
        std::memcpy(dst, sw_pixels_.data(), std::min<size_t>(sw_pixels_.size(), sw_bitmap_.PixelBuffer().Length()));
        sw_bitmap_.Invalidate();
        if (++sw_shown_ == 1 || sw_shown_ % 600 == 0)
            ONYX_INFO("Software view: %llu frames shown", static_cast<unsigned long long>(sw_shown_));
    } catch (hresult_error const& e) {
        ONYX_ERROR("Software view failed: %s", Utf8(e.message()).c_str());
        StopSoftwareView();
    }
}

void EmulationPage::OnStarted() {
    started_ = true;
    // Picture first: nothing below may stop the game from being shown.
    LoadingOverlay().Visibility(Visibility::Collapsed);
    ONYX_INFO("Game started; loading screen hidden");
    // CPU frames are shown by the XAML image view, or with Direct display on,
    // drawn by the presenter into the swap chain under it.
    if (Emu().UsingCpuFrames()) {
        if (Svc().Config().qol.direct_display) {
            SoftwareView().Visibility(Visibility::Collapsed);
            Emu().Presenter().SetCpuView(false);
            ONYX_INFO("Display: direct (swap chain), XAML image view off");
        } else {
            SoftwareView().Visibility(Visibility::Visible);
            Emu().Presenter().SetCpuView(true);
            StartSoftwareView();
        }
    }
    // No system cursor over the game. (Mouse mode itself is turned off once, in
    // the App constructor; changing RequiresPointerMode later throws.)
    try {
        winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().PointerCursor(nullptr);
    } catch (...) {
    }
    // Extras (save state on launch, cheats, remembering the game) must never take
    // the running game down with them.
    try {
        auto& session = Emu();
        // The cached option catalogue feeds the 3DS Advanced page; the NES core has its own.
        if (game_.system == GameSystem::N3DS) Svc().StoreCatalog(session.Catalog());

        // Launch modes from the channel preview.
        const auto& cfg = Svc().Config();
        if (launch_mode_ == "resume" || (launch_mode_.empty() && cfg.qol.auto_load_state && session.HasState(0))) {
            session.LoadState(0);
        } else if (launch_mode_.rfind("slot:", 0) == 0) {
            session.LoadState(std::atoi(launch_mode_.c_str() + 5));
        }
        // Cheats switched on in the cheats file go live through the core.
        if (game_.system == GameSystem::N3DS) {
            const CheatFile cheats = CheatFile::Parse(Svc().Fs().ReadText(Svc().CheatFilePath(game_.title_id)));
            for (const auto& c : cheats.cheats) {
                if (c.enabled) {
                    session.ApplyCheats(cheats);
                    break;
                }
            }
        }
        auto& c = Svc().Config();
        c.last_played_path = game_.path;
        Svc().SaveSettings();
    } catch (hresult_error const& e) {
        ONYX_ERROR("Game start extras failed: 0x%08X %s", static_cast<unsigned>(e.code()),
                   Utf8(e.message()).c_str());
    } catch (std::exception const& e) {
        ONYX_ERROR("Game start extras failed: %s", e.what());
    }
}

void EmulationPage::OnStopped(const std::string& reason) {
    KeyboardBridge::Get().SetHandlers(KeyboardHandlers{});
    HideKeyboard();
    fps_timer_.Stop();
    StopSoftwareView();
    if (started_) {
        // Writes library.json; keep the file system off the UI thread.
        RunAsync([path = game_.path, system = game_.system, secs = Emu().SessionSeconds(), now = NowUnix()] {
            Svc().LibraryFor(system).RecordSession(path, secs, now);
            BackupSaves(Svc().Config().folders);
        });
    }
    auto weak = get_weak();
    auto leave = [weak] {
        if (auto self = weak.get(); self && self->Frame().CanGoBack()) self->Frame().GoBack();
    };
    if (reason.empty()) {
        leave();
        return;
    }
    ONYX_ERROR("Game stopped: %s", reason.c_str());
    LoadingOverlay().Visibility(Visibility::Collapsed);
    const bool crashed = Emu().CrashedInJit() || Emu().CrashedInGpu() ||
                         reason.rfind("The emulator hit an error", 0) == 0;
    if (Emu().CrashedInGpu()) {
        // Back to the software renderer for every game until the user opts in again.
        auto& cfg = Svc().Config();
        cfg.core[keys::kGraphicsApi] = "Software";
        for (auto& [title, opts] : cfg.per_game) opts.erase(keys::kGraphicsApi);
        Svc().SaveSettings();
        ONYX_WARN("Hardware renderer crashed; switched to software rendering");
    }
    if (Emu().CrashedInJit()) {
        // Fall back to the interpreter for every game until the user turns the JIT back on.
        auto& cfg = Svc().Config();
        cfg.core[keys::kCpuJit] = "disabled";
        for (auto& [title, opts] : cfg.per_game) opts.erase(keys::kCpuJit);
        Svc().SaveSettings();
        ONYX_WARN("CPU JIT crashed; switched to the interpreter");
    }
    ContentDialog d;
    d.Title(box_value(crashed ? L"The emulator stopped" : L"The game couldn't start"));
    d.Content(box_value(kit::H(
        crashed ? reason
        : game_.system == GameSystem::Nes
            ? reason + "\n\nCheck that the file is a working .nes or .unf game."
            : reason + "\n\nCommon fixes: use a decrypted dump, put aes_keys.txt / "
                       "seeddb.bin in your System folder, and check Settings > System check.")));
    d.CloseButtonText(L"Back to menu");
    if (crashed) {
        // The core can't run twice in one process after a crash: offer a restart
        // that reopens the same game (with the safer setting already saved).
        d.PrimaryButtonText(L"Restart ONYX");
        d.DefaultButton(ContentDialogButton::Primary);
    }
    d.Closed([leave, crashed, path = game_.path](ContentDialog const&, ContentDialogClosedEventArgs const& args) {
        if (crashed && args.Result() == ContentDialogResult::Primary) {
            Svc().RestartApp(path);
            return;
        }
        leave();
    });
    d.ShowAsync();
}

void EmulationPage::OnHotkey(int hotkey) {
    auto& session = Emu();
    switch (static_cast<Hotkey>(hotkey)) {
    case Hotkey::OpenMenu:
        if (menu_open_) CloseMenu();
        else OpenMenu();
        break;
    case Hotkey::FastForwardDown:
        if (Svc().Config().qol.fast_forward_mode == FastForwardMode::Toggle) {
            session.SetFastForward(!session.FastForward());
            Svc().Toast(session.FastForward() ? "Fast forward on" : "Fast forward off");
        } else {
            session.SetFastForward(true);
        }
        break;
    case Hotkey::FastForwardUp:
        if (Svc().Config().qol.fast_forward_mode == FastForwardMode::Hold) session.SetFastForward(false);
        break;
    case Hotkey::ToggleFps:
        show_fps_ = !show_fps_;
        FpsText().Visibility(show_fps_ ? Visibility::Visible : Visibility::Collapsed);
        break;
    default:
        break;
    }
}

void EmulationPage::UpdateFps() {
    if (!show_fps_ || !started_) return;
    const auto st = Emu().Presenter().GetStats();
    char buf[160];
    // Worst frame gap in the last half second: 16-17 ms is perfectly smooth.
    const double worst = Emu().TakeWorstFrameMs();
    std::snprintf(buf, sizeof(buf), "%.0f FPS   worst %.0f ms   %ux%u%s", st.emu_fps, worst,
                  st.frame_width, st.frame_height, Emu().FastForward() ? "   ▶▶" : "");
    FpsText().Text(kit::H(buf));
}

void EmulationPage::OnPointer(PointerRoutedEventArgs const& e, bool pressed) {
    if (menu_open_ || KeyboardOpen()) return;
    const auto pt = e.GetCurrentPoint(Panel()).Position();
    const double w = Panel().ActualWidth(), h = Panel().ActualHeight();
    const auto st = Emu().Presenter().GetStats();
    if (!w || !h || !st.frame_width || !st.frame_height) return;
    // Map into the letterboxed frame, then to libretro's [-1, 1] space.
    const double scale = std::min(w / st.frame_width, h / st.frame_height);
    const double fw = st.frame_width * scale, fh = st.frame_height * scale;
    const double x0 = (w - fw) / 2, y0 = (h - fh) / 2;
    const float nx = static_cast<float>((pt.X - x0) / fw * 2 - 1);
    const float ny = static_cast<float>((pt.Y - y0) / fh * 2 - 1);
    Emu().Input().SetPointer(nx, ny, pressed);
}

// ---------------------------------------------------------------------------
// On-screen keyboard (3DS software keyboard requests)

bool EmulationPage::KeyboardOpen() const {
    return kb_ && kb_->IsOpen();
}

void EmulationPage::InstallKeyboard() {
    auto weak = get_weak();
    KeyboardHandlers h;
    // These run on the emulation thread: only hop to the UI thread, never touch XAML here.
    h.open = [weak](const KeyboardRequestInfo& info) {
        RunOnUi([weak, info] {
            if (auto s = weak.get()) s->ShowKeyboard(info);
        });
    };
    h.close = [weak] {
        RunOnUi([weak] {
            if (auto s = weak.get()) s->HideKeyboard();
        });
    };
    h.rejected = [weak](const std::string& why) {
        RunOnUi([weak, why] {
            if (auto s = weak.get(); s && s->kb_) s->kb_->ShowRefusal(why);
        });
    };
    KeyboardBridge::Get().SetHandlers(std::move(h));
}

void EmulationPage::ShowKeyboard(const KeyboardRequestInfo& info) {
    if (quitting_) {
        KeyboardBridge::Get().Abort("");
        return;
    }
    if (!kb_) {
        kit::OnScreenKeyboard::Callbacks cb;
        cb.submit = [](const std::string& text, int button) { KeyboardBridge::Get().Submit(text, button); };
        cb.abort = [](const std::string& fallback) { KeyboardBridge::Get().Abort(fallback); };
        kb_ = std::make_shared<kit::OnScreenKeyboard>(Root(), std::move(cb));
    }
    // The controller belongs to the keyboard now; the game keeps running behind it.
    Emu().Input().SetBlocked(true);
    Emu().Input().SetPointer(0.0f, 0.0f, false);
    kb_->Show(info);
}

void EmulationPage::HideKeyboard() {
    const bool was_open = KeyboardOpen();
    if (kb_) kb_->Hide();
    Emu().Input().SetBlocked(false);
    // The A press that confirmed the text must not reach the game.
    if (was_open) Emu().Input().SuppressFor(std::chrono::milliseconds(300));
}

// ---------------------------------------------------------------------------
// The ONYX menu

void EmulationPage::OpenMenu() {
    if (!started_ || quitting_ || KeyboardOpen()) return;
    menu_open_ = true;
    menu_opened_at_ = std::chrono::steady_clock::now();
    Emu().SetPaused(true);
    Svc().PlaySfx("select");
    BuildMenu();
    MenuOverlay().Visibility(Visibility::Visible);
}

void EmulationPage::CloseMenu() {
    menu_open_ = false;
    MenuOverlay().Visibility(Visibility::Collapsed);
    MenuSideScroll().Visibility(Visibility::Collapsed);
    MenuSide().Children().Clear();
    Svc().PlaySfx("back");
    // The A press that picked "Resume" must not reach the game.
    Emu().Input().SuppressFor(std::chrono::milliseconds(250));
    Emu().SetPaused(false);
}

void EmulationPage::BuildMenu() {
    const Theme& t = Svc().CurrentTheme();
    MenuTopBand().Background(Svc().ThemeBrush(t.colors.panel));
    MenuBottomBand().Background(Svc().ThemeBrush(t.colors.panel));
    MenuTitle().Foreground(Svc().ThemeBrush(t.colors.text));
    MenuClock().Foreground(Svc().ThemeBrush(t.colors.text_muted));
    MenuHint().Foreground(Svc().ThemeBrush(t.colors.text_muted));
    MenuTitle().Text(kit::H(game_.DisplayTitle()));
    SYSTEMTIME st;
    GetLocalTime(&st);
    char clock[64];
    std::snprintf(clock, sizeof(clock), "%d:%02d   ·   Playing for %s", st.wHour % 12 ? st.wHour % 12 : 12,
                  st.wMinute, FormatPlayTime(Emu().SessionSeconds()).c_str());
    MenuClock().Text(kit::H(clock));
    MenuHint().Text(kit::H("State slot " + std::to_string(Emu().CurrentSlot()) +
                           "    ·    In game: View + D-pad Up/Down save/load, View + RB fast forward, "
                           "View + Y screenshot" +
                           std::string(game_.system == GameSystem::Nes ? "" : ", View + LB layout")));
    MenuOverlay().Background(SolidColorBrush({0x99, 0, 0, 0}));

    auto host = MenuButtons();
    host.Children().Clear();
    auto weak = get_weak();
    auto row = [&] {
        StackPanel r;
        r.Orientation(Orientation::Horizontal);
        r.Spacing(16);
        host.Children().Append(r);
        return r;
    };
    auto r1 = row();
    auto resume = kit::ActionButton("Resume", kit::glyph::Play, [weak] {
        if (auto s = weak.get()) s->CloseMenu();
    }, true);
    r1.Children().Append(resume);
    r1.Children().Append(kit::ActionButton("Save state", kit::glyph::Save, [weak] {
        if (auto s = weak.get()) s->ShowMenuStates(false);
    }));
    r1.Children().Append(kit::ActionButton("Load state", kit::glyph::Load, [weak] {
        if (auto s = weak.get()) s->ShowMenuStates(true);
    }));
    r1.Children().Append(kit::ActionButton("Screenshot", kit::glyph::Camera, [] { Emu().Screenshot(); }));
    auto r2 = row();
    if (game_.system == GameSystem::N3DS) {
        r2.Children().Append(kit::ActionButton("Screen layout", kit::glyph::Layout, [] { Emu().CycleLayout(); }));
        r2.Children().Append(kit::ActionButton("Cheats", kit::glyph::Cheat, [weak] {
            if (auto s = weak.get()) s->ShowMenuCheats();
        }));
    }
    r2.Children().Append(kit::ActionButton("Quick settings", kit::glyph::Settings, [weak] {
        if (auto s = weak.get()) s->ShowMenuQuickSettings();
    }));
    auto r3 = row();
    r3.Children().Append(kit::ActionButton("Reset", kit::glyph::Reset, [weak] {
        kit::Confirm("Reset the game?", "Unsaved progress since your last save is lost.", "Reset", [weak] {
            Emu().Reset();
            if (auto s = weak.get()) s->CloseMenu();
        });
    }));
    r3.Children().Append(kit::ActionButton("Quit to menu", kit::glyph::Quit, [weak] {
        auto s = weak.get();
        if (!s) return;
        if (Svc().Config().qol.confirm_quit) {
            kit::Confirm("Quit " + s->game_.DisplayTitle() + "?",
                         Svc().Config().qol.auto_save_state_on_exit
                             ? "Your place is auto-saved; pick Resume on the channel to continue."
                             : "Unsaved progress since your last in-game save is lost.",
                         "Quit", [weak] {
                             if (auto ss = weak.get()) ss->Quit();
                         });
        } else {
            s->Quit();
        }
    }));
    resume.Focus(FocusState::Programmatic);
}

void EmulationPage::ShowMenuStates(bool loading) {
    auto panel = MenuSide();
    panel.Children().Clear();
    panel.Children().Append(kit::SectionHeader(loading ? "Load state" : "Save state"));
    auto weak = get_weak();
    Control first{nullptr};
    for (int slot = loading ? 0 : 1; slot <= 9; ++slot) {
        const bool exists = Emu().HasState(slot);
        if (loading && !exists) continue;
        Button b;
        Grid g;
        ColumnDefinition c0, c1;
        c0.Width(GridLengthHelper::Auto());
        c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        g.ColumnDefinitions().Append(c0);
        g.ColumnDefinitions().Append(c1);
        Image img;
        img.Width(160);
        img.Height(120);
        if (exists) img.Source(ImageFromFile(Emu().StateThumbPath(slot), 160));
        g.Children().Append(img);
        std::string label = slot == 0 ? "Auto save" : "Slot " + std::to_string(slot);
        if (!exists) label += "  (empty)";
        if (slot == Emu().CurrentSlot()) label += "  • current";
        auto text = kit::Text(label, 22, true);
        text.VerticalAlignment(VerticalAlignment::Center);
        text.Margin(Thickness{18, 0, 0, 0});
        Grid::SetColumn(text, 1);
        g.Children().Append(text);
        b.Content(g);
        b.HorizontalAlignment(HorizontalAlignment::Stretch);
        b.HorizontalContentAlignment(HorizontalAlignment::Left);
        b.Click([weak, slot, loading](auto&&, auto&&) {
            if (slot > 0) Emu().SetCurrentSlot(slot);
            if (loading) Emu().LoadState(slot);
            else Emu().SaveState(slot);
            if (auto s = weak.get()) s->CloseMenu();
        });
        panel.Children().Append(b);
        if (!first) first = b;
    }
    if (!first) panel.Children().Append(kit::Text("No save states yet.", 22));
    MenuSideScroll().Visibility(Visibility::Visible);
    if (first) first.Focus(FocusState::Programmatic);
}

void EmulationPage::ShowMenuCheats() {
    auto panel = MenuSide();
    panel.Children().Clear();
    panel.Children().Append(kit::SectionHeader("Cheats"));
    const std::string file = Svc().CheatFilePath(game_.title_id);
    auto cheats = std::make_shared<CheatFile>(CheatFile::Parse(Svc().Fs().ReadText(file)));
    if (cheats->cheats.empty()) {
        panel.Children().Append(kit::Text(
            "No cheats for this game yet. Download them from the game's channel page (Cheats).", 20));
    }
    Control first{nullptr};
    for (size_t i = 0; i < cheats->cheats.size(); ++i) {
        auto toggle = kit::Toggle(cheats->cheats[i].enabled, [cheats, i, file](bool on) {
            cheats->cheats[i].enabled = on;
            Svc().Fs().WriteText(file, cheats->Serialize());
            Emu().ApplyCheats(*cheats); // takes effect immediately
        });
        panel.Children().Append(kit::SettingRow(cheats->cheats[i].name, "", toggle));
        if (!first) first = toggle;
    }
    MenuSideScroll().Visibility(Visibility::Visible);
    if (first) first.Focus(FocusState::Programmatic);
}

void EmulationPage::ShowMenuQuickSettings() {
    auto panel = MenuSide();
    panel.Children().Clear();
    panel.Children().Append(kit::SectionHeader("Quick settings"));
    panel.Children().Append(kit::Text(game_.system == GameSystem::Nes
                                          ? "Changes apply now, for this session."
                                          : "Changes apply now, for this session. Use Game settings on the "
                                            "channel page to keep them.", 18));
    const CoreOptions current = Emu().CurrentCoreOptions();
    const CoreOptionCatalog catalog = Emu().Catalog();
    auto pick = [&](const char* key, const char* label) {
        const CoreOptionDef* def = catalog.Find(key);
        if (!def) return;
        std::vector<std::pair<std::string, std::string>> values;
        for (const auto& v : def->values) values.emplace_back(v.value, v.label);
        const std::string k = key;
        auto it = current.find(key);
        auto combo = kit::Choice(values, it == current.end() ? def->default_value : it->second,
                                 [k](const std::string& v) { Emu().ApplyCoreOptions({{k, v}}); });
        panel.Children().Append(kit::SettingRow(label, "", combo));
    };
    if (game_.system == GameSystem::Nes) {
        // Picture and colour choices are kept in Settings (all NES games); FCEUmm's own
        // options (region, filter, turbo) are for this session.
        auto remember = [](const std::function<void(NesSettings&)>& change) {
            change(Svc().Config().nes);
            Svc().SaveSettings();
        };
        auto row = [&](const char* label, const char* help,
                       const std::vector<std::pair<std::string, std::string>>& values, const std::string& current,
                       std::function<void(const std::string&)> changed) {
            panel.Children().Append(kit::SettingRow(label, help, kit::Choice(values, current, std::move(changed))));
        };
        const NesSettings nes = Svc().Config().nes;
        pick("fceumm_region", "Region");
        row("Colours", "", NesPaletteChoices(), nes.palette, [remember](const std::string& v) {
            remember([&](NesSettings& n) { n.palette = v; });
            Emu().ApplyCoreOptions({{nes_keys::kPalette, v}});
        });
        row("Picture shape", "", NesAspectChoices(), DisplayAspectName(nes.aspect), [remember](const std::string& v) {
            const DisplayAspect a = DisplayAspectFromName(v);
            remember([&](NesSettings& n) { n.aspect = a; });
            Emu().Presenter().SetAspect(a);
        });
        row("Hide top and bottom", "Like TV overscan; hides scrolling glitches", NesCropChoices(),
            std::to_string(nes.crop_top_bottom), [remember](const std::string& v) {
                remember([&](NesSettings& n) { n.crop_top_bottom = std::atoi(v.c_str()); });
                Emu().ApplyCoreOptions(NesCoreOptions(Svc().Config().nes));
            });
        row("Hide left and right", "", NesCropChoices(), std::to_string(nes.crop_sides),
            [remember](const std::string& v) {
                remember([&](NesSettings& n) { n.crop_sides = std::atoi(v.c_str()); });
                Emu().ApplyCoreOptions(NesCoreOptions(Svc().Config().nes));
            });
        panel.Children().Append(kit::SettingRow(
            "Draw all sprites", "Removes the flicker of the original sprite limit",
            kit::Toggle(nes.no_sprite_limit, [remember](bool on) {
                remember([&](NesSettings& n) { n.no_sprite_limit = on; });
                Emu().ApplyCoreOptions(NesCoreOptions(Svc().Config().nes));
            })));
        pick("fceumm_ntsc_filter", "NTSC filter");
        pick("fceumm_turbo_enable", "Turbo buttons");
    } else {
        pick(keys::kResolution, "Internal resolution");
        pick(keys::kFrameSkip, "Frame skip");
        pick(keys::kLayout, "Screen layout");
        pick(keys::kLargeScreenProportion, "Big screen size");
        pick(keys::kTextureFilter, "Texture filter");
        pick(keys::kCpuClock, "CPU clock");
    }
    const bool nes_game = game_.system == GameSystem::Nes;
    const ScreenFilter current_filter =
        nes_game ? Svc().Config().nes.filter : Svc().Config().qol.screen_filter;
    auto filter = kit::Choice({{"sharp", "Sharp pixels"}, {"smooth", "Smooth"}, {"crt", "CRT"}},
                              current_filter == ScreenFilter::Sharp ? "sharp"
                              : current_filter == ScreenFilter::Crt ? "crt"
                                                                    : "smooth",
                              [nes_game](const std::string& v) {
                                  auto f = v == "sharp" ? ScreenFilter::Sharp
                                           : v == "crt" ? ScreenFilter::Crt
                                                        : ScreenFilter::Smooth;
                                  (nes_game ? Svc().Config().nes.filter : Svc().Config().qol.screen_filter) = f;
                                  Svc().SaveSettings();
                                  Emu().Presenter().SetFilter(f);
                              });
    panel.Children().Append(kit::SettingRow("Upscaling look",
                                            game_.system == GameSystem::Nes ? "How the NES image is scaled to your TV"
                                                                            : "How the 3DS image is scaled to your TV",
                                            filter));
    MenuSideScroll().Visibility(Visibility::Visible);
}

void EmulationPage::Quit() {
    if (quitting_) return;
    quitting_ = true;
    KeyboardBridge::Get().SetHandlers(KeyboardHandlers{});
    HideKeyboard();
    MenuOverlay().Visibility(Visibility::Collapsed);
    LoadingTitle().Text(kit::H(Svc().Config().qol.auto_save_state_on_exit ? "Saving and closing..."
                                                                         : "Closing..."));
    LoadingHint().Text(L"");
    LoadingOverlay().Visibility(Visibility::Visible);
    const bool save = Svc().Config().qol.auto_save_state_on_exit;
    // Stop joins the emulation thread; never block the UI thread on it.
    RunAsync([save] { Emu().Stop(save); });
}

} // namespace winrt::ONYX3DS::implementation

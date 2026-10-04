// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "EmulationPage.h"
#if __has_include("EmulationPage.g.cpp")
#include "EmulationPage.g.cpp"
#endif

#include "Emu/EmulatorSession.h"
#include "Platform/Imaging.h"
#include "Platform/Log.h"
#include "Ui/AppServices.h"
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
    if (auto g = Svc().Library().Find(path)) game_ = *g;
    else {
        game_ = GameEntry{};
        game_.path = path;
        game_.title = CleanFileTitle(FileName(path));
    }
    show_fps_ = Svc().Config().qol.show_fps;
    started_ = quitting_ = menu_open_ = false;

    auto weak = get_weak();
    // In game, B belongs to the 3DS. Back only closes the ONYX menu.
    Svc().SetBackOverride([weak] {
        if (auto self = weak.get(); self && self->menu_open_) self->CloseMenu();
        return true;
    });
    Svc().SetToastSink([weak](const std::string& text) {
        if (auto self = weak.get()) kit::ShowToast(self->ToastHost(), text);
    });

    LoadingTitle().Text(kit::H(game_.DisplayTitle()));
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
    fps_timer_.Stop();
    Emu().SetEvents({});
    Emu().Presenter().Detach();
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
    Svc().RetroAchievements().SetEventSink(ev.achievement);
    Svc().RetroAchievements().SetHardcore(cfg.services.ra_hardcore);

    std::string error;
    if (!session.Start(game_, cfg, Svc().Model(), error)) OnStopped(error);
    fps_timer_.Start();
}

void EmulationPage::OnStarted() {
    started_ = true;
    LoadingOverlay().Visibility(Visibility::Collapsed);
    ONYX_INFO("Game started; loading screen hidden");
    // Two seconds of colour bars: proves the picture path works before the game's first frame.
    if (Emu().UsingSoftwareRenderer()) RunAsync([] { Emu().Presenter().ShowTestPattern(); });
    auto& session = Emu();
    Svc().StoreCatalog(session.Catalog());

    // Launch modes from the channel preview.
    const auto& cfg = Svc().Config();
    if (launch_mode_ == "resume" || (launch_mode_.empty() && cfg.qol.auto_load_state && session.HasState(0))) {
        session.LoadState(0);
    } else if (launch_mode_.rfind("slot:", 0) == 0) {
        session.LoadState(std::atoi(launch_mode_.c_str() + 5));
    }
    // Cheats switched on in the cheats file go live through the core.
    const CheatFile cheats = CheatFile::Parse(Svc().Fs().ReadText(Svc().CheatFilePath(game_.title_id)));
    for (const auto& c : cheats.cheats) {
        if (c.enabled) {
            session.ApplyCheats(cheats);
            break;
        }
    }
    auto& c = Svc().Config();
    c.last_played_path = game_.path;
    Svc().SaveSettings();
}

void EmulationPage::OnStopped(const std::string& reason) {
    fps_timer_.Stop();
    if (started_) {
        Svc().Library().RecordSession(game_.path, Emu().SessionSeconds(), NowUnix());
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
                : reason + "\n\nCommon fixes: use a decrypted dump, put aes_keys.txt / "
                           "seeddb.bin in your System folder, and check Settings > System check.")));
    d.CloseButtonText(L"Back to menu");
    d.Closed([leave](auto&&, auto&&) { leave(); });
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
    std::snprintf(buf, sizeof(buf), "%.0f FPS   %ux%u%s", st.emu_fps, st.frame_width, st.frame_height,
                  Emu().FastForward() ? "   ▶▶" : "");
    FpsText().Text(kit::H(buf));
}

void EmulationPage::OnPointer(PointerRoutedEventArgs const& e, bool pressed) {
    if (menu_open_) return;
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
// The ONYX menu

void EmulationPage::OpenMenu() {
    if (!started_ || quitting_) return;
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
                           "View + Y screenshot, View + LB layout"));
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
    r2.Children().Append(kit::ActionButton("Screen layout", kit::glyph::Layout, [] { Emu().CycleLayout(); }));
    r2.Children().Append(kit::ActionButton("Cheats", kit::glyph::Cheat, [weak] {
        if (auto s = weak.get()) s->ShowMenuCheats();
    }));
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
    panel.Children().Append(kit::Text("Changes apply now, for this session. Use Game settings on the "
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
    pick(keys::kResolution, "Internal resolution");
    pick(keys::kLayout, "Screen layout");
    pick(keys::kLargeScreenProportion, "Big screen size");
    pick(keys::kTextureFilter, "Texture filter");
    pick(keys::kCpuClock, "CPU clock");
    auto filter = kit::Choice({{"sharp", "Sharp pixels"}, {"smooth", "Smooth"}, {"crt", "CRT"}},
                              Svc().Config().qol.screen_filter == ScreenFilter::Sharp ? "sharp"
                              : Svc().Config().qol.screen_filter == ScreenFilter::Crt ? "crt"
                                                                                       : "smooth",
                              [](const std::string& v) {
                                  auto f = v == "sharp" ? ScreenFilter::Sharp
                                           : v == "crt" ? ScreenFilter::Crt
                                                        : ScreenFilter::Smooth;
                                  Svc().Config().qol.screen_filter = f;
                                  Svc().SaveSettings();
                                  Emu().Presenter().SetFilter(f);
                              });
    panel.Children().Append(kit::SettingRow("Upscaling look", "How the 3DS image is scaled to your TV", filter));
    MenuSideScroll().Visibility(Visibility::Visible);
}

void EmulationPage::Quit() {
    if (quitting_) return;
    quitting_ = true;
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

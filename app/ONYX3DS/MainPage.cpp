// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "MainPage.h"
#if __has_include("MainPage.g.cpp")
#include "MainPage.g.cpp"
#endif

#include "Platform/Imaging.h"
#include "Platform/Log.h"
#include "Ui/AppServices.h"
#include "Ui/UiKit.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Numerics;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Media::Animation;
using namespace winrt::Windows::UI::Xaml::Navigation;
using namespace onyx;
using namespace onyx::app;
namespace kit = onyx::app::ui;

namespace winrt::ONYX3DS::implementation {

namespace {
// Layout of the channel area at 1920x1080 (TV-safe margins applied in XAML).
constexpr double kAreaWidth = 1728;
constexpr double kAreaHeight = 708;
constexpr double kGap = 36;

AppServices& Svc() {
    return AppServices::Get();
}

bool IsDark(const std::string& hex) {
    const auto c = Svc().ThemeColor(hex);
    return (0.2126 * c.R + 0.7152 * c.G + 0.0722 * c.B) < 128;
}

TimeSpan Ms(int ms) {
    return std::chrono::milliseconds(ms);
}
} // namespace

void MainPage::InitializeComponent() {
    MainPageT::InitializeComponent();
    KeyDown({this, &MainPage::HandleKeyDown});

    clock_ = DispatcherTimer();
    clock_.Interval(std::chrono::seconds(1));
    clock_.Tick([weak = get_weak()](auto&&, auto&&) {
        if (auto self = weak.get()) self->UpdateClock();
    });
}

void MainPage::OnNavigatedTo(NavigationEventArgs const&) {
    auto weak = get_weak();
    Svc().SetBackOverride([] { return true; }); // B does nothing on the home screen
    Svc().SetToastSink([weak](const std::string& text) {
        if (auto self = weak.get()) kit::ShowToast(self->ToastHost(), text);
    });
    if (theme_applied_ != Svc().CurrentTheme().id) ApplyTheme();
    BuildBarButtons();
    UpdateClock();
    clock_.Start();
    Svc().StartMusic();

    if (!initialised_) {
        initialised_ = true;
        games_ = Svc().Library().GridView(Svc().Config().qol);
        Rebuild(false);
        // Always rescan on launch so new files on the USB drive show up.
        Svc().RescanLibrary([weak](size_t) {
            if (auto self = weak.get()) self->Rebuild();
        });
    } else {
        Rebuild();
    }

    // "Resume last game on startup": once per app run.
    if (!resume_checked_) {
        resume_checked_ = true;
        const auto& cfg = Svc().Config();
        if (cfg.qol.resume_last_game && !cfg.last_played_path.empty()) {
            if (auto g = Svc().Library().Find(cfg.last_played_path)) {
                Frame().Navigate(xaml_typename<ONYX3DS::EmulationPage>(), box_value(kit::H(g->path)));
            }
        }
    }
}

void MainPage::OnNavigatedFrom(NavigationEventArgs const&) {
    clock_.Stop();
}

// ---------------------------------------------------------------------------
// Theme

void MainPage::ApplyTheme() {
    const Theme& t = Svc().CurrentTheme();
    theme_applied_ = t.id;
    Root().RequestedTheme(IsDark(t.colors.background_top) ? ElementTheme::Dark : ElementTheme::Light);
    Root().Background(Svc().ThemeBrush(t.colors.background_bottom));
    BackgroundImage().Source(ImageFromFile(Svc().ThemeAsset(t.textures.background)));
    FarLayer().Source(ImageFromFile(Svc().ThemeAsset(t.textures.background_far)));
    NearLayer().Source(ImageFromFile(Svc().ThemeAsset(t.textures.background_near)));
    BarImage().Source(ImageFromFile(Svc().ThemeAsset(t.textures.bar)));
    const auto font = winrt::Windows::UI::Xaml::Media::FontFamily(kit::H(t.style.font));
    for (auto tb : {TitleText(), InfoText(), ClockText(), DateText(), HintText()}) tb.FontFamily(font);
    TitleText().Foreground(Svc().ThemeBrush(t.colors.text));
    InfoText().Foreground(Svc().ThemeBrush(t.colors.text_muted));
    ClockText().Foreground(Svc().ThemeBrush(t.colors.bar_text));
    DateText().Foreground(Svc().ThemeBrush(t.colors.bar_text));
    HintText().Foreground(Svc().ThemeBrush(t.colors.bar_text));
    ClockText().Visibility(t.style.show_clock && Svc().Config().qol.show_clock ? Visibility::Visible
                                                                             : Visibility::Collapsed);
    HintText().Text(L"A Open    Y Favourite    X Details    LB / RB Pages    Menu Tools    View Settings");

    // Depth: one shared ThemeShadow, cast onto the layer behind the grid.
    shadow_ = ThemeShadow();
    shadow_.Receivers().Append(ShadowReceiver());
    StartAmbientMotion();
}

void MainPage::StartAmbientMotion() {
    const Theme& t = Svc().CurrentTheme();
    const bool motion = Svc().Config().qol.background_parallax && t.style.parallax > 0;
    for (auto layer : {FarLayer(), NearLayer()}) {
        layer.TranslationTransition(Vector3Transition());
        layer.TranslationTransition().Duration(Ms(1200));
    }
    if (!motion) return;
    // Slow drift on both light layers in opposite directions; the near layer
    // moves further, which is what sells the depth.
    auto drift = [&](Image const& img, double dx, double dy, int seconds) {
        CompositeTransform xf;
        img.RenderTransform(xf);
        Storyboard sb;
        for (auto [prop, to] : {std::pair{L"TranslateX", dx}, std::pair{L"TranslateY", dy}}) {
            DoubleAnimation a;
            a.From(-to);
            a.To(to);
            a.Duration(DurationHelper::FromTimeSpan(std::chrono::seconds(seconds)));
            a.AutoReverse(true);
            a.RepeatBehavior(RepeatBehaviorHelper::Forever());
            SineEase ease;
            ease.EasingMode(EasingMode::EaseInOut);
            a.EasingFunction(ease);
            Storyboard::SetTarget(a, xf);
            Storyboard::SetTargetProperty(a, prop);
            sb.Children().Append(a);
        }
        sb.Begin();
    };
    drift(FarLayer(), 24 * t.style.parallax, 10 * t.style.parallax, 23);
    drift(NearLayer(), -40 * t.style.parallax, -26 * t.style.parallax, 17);
}

void MainPage::MoveParallax(float nx, float ny) {
    const Theme& t = Svc().CurrentTheme();
    if (!Svc().Config().qol.background_parallax || t.style.parallax <= 0) return;
    const float p = static_cast<float>(t.style.parallax);
    FarLayer().Translation(float3{-nx * 18 * p, -ny * 10 * p, 0});
    NearLayer().Translation(float3{-nx * 42 * p, -ny * 24 * p, 0});
}

// ---------------------------------------------------------------------------
// Grid

void MainPage::Rebuild(bool keep_focus) {
    if (!keep_focus) focused_path_.clear();
    if (theme_applied_ != Svc().CurrentTheme().id) ApplyTheme();
    games_ = Svc().Library().GridView(Svc().Config().qol);
    columns_ = std::clamp(Svc().Config().qol.grid_columns, 3, 6);
    rows_ = 3;
    const int per_page = columns_ * rows_;
    const int pages = std::max(1, static_cast<int>((games_.size() + per_page - 1) / per_page));
    // Keep the focused game's page after a rescan.
    if (!focused_path_.empty()) {
        for (size_t i = 0; i < games_.size(); ++i)
            if (games_[i].path == focused_path_) page_ = static_cast<int>(i) / per_page;
    }
    page_ = std::clamp(page_, 0, pages - 1);
    BuildPage();
}

void MainPage::BuildPage() {
    auto grid = ChannelGrid();
    grid.Children().Clear();
    grid.RowDefinitions().Clear();
    grid.ColumnDefinitions().Clear();

    if (games_.empty()) {
        ShowEmptyState();
        ContentArea().Visibility(Visibility::Collapsed);
        return;
    }
    EmptyState().Visibility(Visibility::Collapsed);
    ContentArea().Visibility(Visibility::Visible);

    const double tw = std::min((kAreaWidth - (columns_ - 1) * kGap) / columns_,
                               ((kAreaHeight - (rows_ - 1) * kGap) / rows_) / 0.5625);
    const double th = tw * 0.5625;
    for (int c = 0; c < columns_; ++c) {
        ColumnDefinition cd;
        cd.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        grid.ColumnDefinitions().Append(cd);
    }
    for (int r = 0; r < rows_; ++r) {
        RowDefinition rd;
        rd.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        grid.RowDefinitions().Append(rd);
    }

    const int per_page = columns_ * rows_;
    const int first = page_ * per_page;
    const int empty_slots = Svc().CurrentTheme().style.empty_slots;
    Button focus_target{nullptr};
    for (int slot = 0; slot < per_page; ++slot) {
        const int idx = first + slot;
        FrameworkElement element{nullptr};
        if (idx < static_cast<int>(games_.size())) {
            auto tile = MakeTile(games_[idx], tw, th);
            if (games_[idx].path == focused_path_ || (!focus_target && focused_path_.empty()))
                focus_target = tile;
            element = tile;
        } else if (empty_slots > 0 && slot < std::max(empty_slots, per_page)) {
            element = MakeEmptySlot(tw, th).as<FrameworkElement>();
        } else {
            continue;
        }
        Grid::SetRow(element, slot / columns_);
        Grid::SetColumn(element, slot % columns_);
        grid.Children().Append(element);
    }

    // Page dots
    auto dots = PageDots();
    dots.Children().Clear();
    const int pages = std::max(1, static_cast<int>((games_.size() + per_page - 1) / per_page));
    if (pages > 1) {
        const Theme& t = Svc().CurrentTheme();
        for (int p = 0; p < pages; ++p) {
            Shapes::Ellipse e;
            const double size = p == page_ ? 16 : 10;
            e.Width(size);
            e.Height(size);
            e.VerticalAlignment(VerticalAlignment::Center);
            e.Fill(Svc().ThemeBrush(p == page_ ? t.colors.accent : t.colors.text_muted));
            dots.Children().Append(e);
        }
    }
    if (!focus_target && grid.Children().Size()) focus_target = grid.Children().GetAt(0).try_as<Button>();
    if (focus_target) focus_target.Focus(FocusState::Programmatic);
}

Button MainPage::MakeTile(const GameEntry& game, double w, double h) {
    const Theme& t = Svc().CurrentTheme();
    const double r = t.style.tile_corner_radius;
    Button tile;
    tile.Style(Application::Current().Resources().Lookup(box_value(L"ChannelButtonStyle")).as<winrt::Windows::UI::Xaml::Style>());
    tile.Width(w);
    tile.Height(h);
    tile.HorizontalAlignment(HorizontalAlignment::Center);
    tile.VerticalAlignment(VerticalAlignment::Center);

    Grid g;
    // Art (or the game's own 3DS icon on a themed card)
    Border art;
    art.Margin(Thickness{w * 0.026, w * 0.026, w * 0.026, w * 0.026});
    art.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{r * 0.8, r * 0.8, r * 0.8, r * 0.8});
    if (!game.grid_art.empty() && Svc().Fs().Exists(game.grid_art)) {
        ImageBrush brush;
        brush.ImageSource(ImageFromFile(game.grid_art, static_cast<int>(w * 1.3)));
        brush.Stretch(Stretch::UniformToFill);
        art.Background(brush);
    } else {
        LinearGradientBrush bg;
        GradientStop a, b;
        a.Color(Svc().ThemeColor(t.colors.tile_face));
        a.Offset(0);
        b.Color(Svc().ThemeColor(t.colors.tile_edge));
        b.Offset(1);
        bg.GradientStops().Append(a);
        bg.GradientStops().Append(b);
        bg.StartPoint(Point{0, 0});
        bg.EndPoint(Point{0, 1});
        art.Background(bg);
        StackPanel inner;
        inner.HorizontalAlignment(HorizontalAlignment::Center);
        inner.VerticalAlignment(VerticalAlignment::Center);
        inner.Spacing(8);
        if (!game.icon_path.empty()) {
            Image icon;
            icon.Width(h * 0.42);
            icon.Height(h * 0.42);
            icon.Source(IconFromRgba(Svc().Fs().ReadAll(game.icon_path), 48, 48));
            inner.Children().Append(icon);
        }
        auto title = kit::Text(game.DisplayTitle(), std::max(16.0, h * 0.11), true);
        title.TextAlignment(TextAlignment::Center);
        title.MaxWidth(w * 0.85);
        title.MaxLines(2);
        title.TextTrimming(TextTrimming::CharacterEllipsis);
        inner.Children().Append(title);
        art.Child(inner);
    }
    g.Children().Append(art);

    // Frame, glass highlight, favourite star, focus glow
    Image frame;
    frame.Source(ImageFromFile(Svc().ThemeAsset(t.textures.tile_frame)));
    frame.Stretch(Stretch::Fill);
    frame.NineGrid(Thickness{48, 48, 48, 48});
    g.Children().Append(frame);
    Image gloss;
    gloss.Source(ImageFromFile(Svc().ThemeAsset(t.textures.tile_gloss)));
    gloss.Stretch(Stretch::Fill);
    gloss.IsHitTestVisible(false);
    g.Children().Append(gloss);
    if (game.favorite) {
        auto star = kit::Glyph(kit::glyph::StarFilled, 26, t.colors.accent);
        star.HorizontalAlignment(HorizontalAlignment::Right);
        star.VerticalAlignment(VerticalAlignment::Top);
        star.Margin(Thickness{0, 14, 18, 0});
        g.Children().Append(star);
    }
    Image glow;
    glow.Source(ImageFromFile(Svc().ThemeAsset(t.textures.cursor)));
    glow.Stretch(Stretch::Fill);
    glow.NineGrid(Thickness{60, 60, 60, 60});
    glow.Margin(Thickness{-w * 0.05, -h * 0.08, -w * 0.05, -h * 0.08});
    glow.Opacity(0);
    glow.OpacityTransition(ScalarTransition());
    glow.IsHitTestVisible(false);
    g.Children().Append(glow);
    tile.Content(g);

    // Depth + focus motion via XAML's implicit transitions.
    tile.Shadow(shadow_);
    tile.Translation(float3{0, 0, static_cast<float>(t.style.tile_depth * 2)});
    tile.CenterPoint(float3{static_cast<float>(w / 2), static_cast<float>(h / 2), 0});
    Vector3Transition scale_tr;
    scale_tr.Duration(Ms(180));
    tile.ScaleTransition(scale_tr);
    Vector3Transition move_tr;
    move_tr.Duration(Ms(180));
    tile.TranslationTransition(move_tr);

    const GameEntry entry = game;
    auto weak = get_weak();
    tile.GotFocus([weak, tile, entry](auto&&, auto&&) {
        if (auto self = weak.get()) self->OnTileFocused(tile, entry, true);
    });
    tile.LostFocus([weak, tile, entry](auto&&, auto&&) {
        if (auto self = weak.get()) self->OnTileFocused(tile, entry, false);
    });
    tile.Click([weak, entry](auto&&, auto&&) {
        if (auto self = weak.get()) self->OpenGame(entry);
    });
    tile.Tag(box_value(kit::H(game.path)));
    return tile;
}

UIElement MainPage::MakeEmptySlot(double w, double h) {
    Image img;
    img.Source(ImageFromFile(Svc().ThemeAsset(Svc().CurrentTheme().textures.tile_empty)));
    img.Stretch(Stretch::Fill);
    img.NineGrid(Thickness{48, 48, 48, 48});
    img.Width(w);
    img.Height(h);
    img.HorizontalAlignment(HorizontalAlignment::Center);
    img.VerticalAlignment(VerticalAlignment::Center);
    return img;
}

void MainPage::OnTileFocused(Button const& tile, const GameEntry& game, bool focused) {
    const Theme& t = Svc().CurrentTheme();
    const float s = focused ? static_cast<float>(t.style.hover_scale) : 1.0f;
    tile.Scale(float3{s, s, 1});
    tile.Translation(float3{0, 0, static_cast<float>(t.style.tile_depth * (focused ? 5 : 2))});
    Canvas::SetZIndex(tile, focused ? 10 : 0);
    if (auto g = tile.Content().try_as<Grid>()) {
        auto children = g.Children();
        if (children.Size()) children.GetAt(children.Size() - 1).Opacity(focused ? 1.0 : 0.0);
    }
    if (!focused) return;
    focused_path_ = game.path;
    Svc().SetSelectedGame(game);
    Svc().PlaySfx("move");
    InfoText().Text(kit::H(game.DisplayTitle() + "   ·   " + FormatLastPlayed(game.last_played) +
                           (game.play_seconds ? "   ·   " + FormatPlayTime(game.play_seconds) : "")));
    // Parallax follows the cursor across the grid.
    const int per_page = columns_ * rows_;
    int index = 0;
    for (int i = 0; i < static_cast<int>(games_.size()); ++i)
        if (games_[i].path == game.path) index = i % per_page;
    const float nx = columns_ > 1 ? (index % columns_) / float(columns_ - 1) * 2 - 1 : 0;
    const float ny = rows_ > 1 ? (index / columns_) / float(rows_ - 1) * 2 - 1 : 0;
    MoveParallax(nx, ny);
}

void MainPage::ChangePage(int delta) {
    const int per_page = columns_ * rows_;
    const int pages = std::max(1, static_cast<int>((games_.size() + per_page - 1) / per_page));
    if (pages <= 1) return;
    page_ = (page_ + delta + pages) % pages;
    focused_path_.clear();
    Svc().PlaySfx("move");
    // Slide the grid in from the side we are moving toward.
    auto grid = ChannelGrid();
    grid.Opacity(0);
    grid.OpacityTransition(ScalarTransition());
    grid.Translation(float3{delta * 60.0f, 0, 0});
    grid.TranslationTransition(Vector3Transition());
    BuildPage();
    grid.Opacity(1);
    grid.Translation(float3{0, 0, 0});
}

// ---------------------------------------------------------------------------
// Empty state, bar buttons, clock

void MainPage::ShowEmptyState() {
    auto host = EmptyState();
    host.Children().Clear();
    host.Visibility(Visibility::Visible);
    const auto drives = RemovableDriveRoots();
    StackPanel body;
    body.Spacing(18);
    body.Children().Append(kit::Text(Svc().Scanning() ? "Looking for games..." : "No games yet", 44, true));
    body.Children().Append(kit::Text(
        drives.empty()
            ? "Plug in a USB drive with your decrypted 3DS games (.3ds, .cci, .cia, .3dsx), then "
              "choose its games folder."
            : "Put your decrypted 3DS games on the USB drive. \"Set up USB drive\" creates an ONYX3DS "
              "folder with places for games, updates & DLC, textures, mods, cheats and more.",
        24));
    StackPanel buttons;
    buttons.Orientation(Orientation::Horizontal);
    buttons.Spacing(16);
    if (!drives.empty()) {
        buttons.Children().Append(kit::ActionButton("Set up USB drive", kit::glyph::Usb, [this, drives] {
            auto& cfg = Svc().Config();
            cfg.folders.ApplyDriveLayout(drives.front());
            for (const auto& sub : FolderConfig::DriveLayoutSubfolders())
                Svc().Fs().CreateDirs(JoinPath(JoinPath(drives.front(), "ONYX3DS"), sub));
            Svc().SaveSettings();
            Svc().Toast("Created " + drives.front() + "/ONYX3DS. Copy your games into the Roms folder.");
            auto weak = get_weak();
            Svc().RescanLibrary([weak](size_t) {
                if (auto self = weak.get()) self->Rebuild(false);
            });
        }, true));
    }
    buttons.Children().Append(kit::ActionButton("Choose games folder", kit::glyph::Folder, [this] {
        Frame().Navigate(xaml_typename<ONYX3DS::FolderPage>(), box_value(L"roms"));
    }, drives.empty()));
    buttons.Children().Append(kit::ActionButton("Rescan", kit::glyph::Refresh, [this] {
        auto weak = get_weak();
        Svc().RescanLibrary([weak](size_t) {
            if (auto self = weak.get()) self->Rebuild(false);
        });
    }));
    body.Children().Append(buttons);
    host.Children().Append(kit::Card(body, 40));
    if (auto first = buttons.Children().GetAt(0).try_as<Control>()) first.Focus(FocusState::Programmatic);
}

void MainPage::BuildBarButtons() {
    const Theme& t = Svc().CurrentTheme();
    auto make = [&](const wchar_t* glyph, const std::string& label, std::function<void()> click) {
        Button b;
        b.Style(Application::Current().Resources().Lookup(box_value(L"ChannelButtonStyle")).as<winrt::Windows::UI::Xaml::Style>());
        StackPanel col;
        col.Spacing(6);
        Grid circle;
        circle.Width(112);
        circle.Height(112);
        Image bg;
        bg.Source(ImageFromFile(Svc().ThemeAsset(t.textures.button)));
        circle.Children().Append(bg);
        auto g = kit::Glyph(glyph, 40, t.colors.accent);
        g.HorizontalAlignment(HorizontalAlignment::Center);
        g.VerticalAlignment(VerticalAlignment::Center);
        g.Margin(Thickness{0, 0, 6, 8});
        circle.Children().Append(g);
        col.Children().Append(circle);
        auto text = kit::Text(label, 20, true, t.colors.bar_text);
        text.HorizontalAlignment(HorizontalAlignment::Center);
        col.Children().Append(text);
        b.Content(col);
        b.CenterPoint(float3{56, 56, 0});
        b.ScaleTransition(Vector3Transition());
        b.GotFocus([b](auto&&, auto&&) {
            b.Scale(float3{1.12f, 1.12f, 1});
            AppServices::Get().PlaySfx("move");
        });
        b.LostFocus([b](auto&&, auto&&) { b.Scale(float3{1, 1, 1}); });
        b.Click([click](auto&&, auto&&) {
            AppServices::Get().PlaySfx("select");
            click();
        });
        return b;
    };
    LeftButtonHost().Children().Clear();
    RightButtonHost().Children().Clear();
    LeftButtonHost().Children().Append(make(kit::glyph::Settings, "Settings", [this] {
        Frame().Navigate(xaml_typename<ONYX3DS::SettingsPage>());
    }));
    RightButtonHost().Children().Append(make(kit::glyph::Grid, "Tools", [this] { ShowToolsMenu(); }));
}

void MainPage::UpdateClock() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char clock[16], date[32];
    if (Svc().Config().qol.clock_24h) {
        std::snprintf(clock, sizeof(clock), "%02d:%02d", st.wHour, st.wMinute);
    } else {
        const int h12 = st.wHour % 12 == 0 ? 12 : st.wHour % 12;
        std::snprintf(clock, sizeof(clock), "%d:%02d", h12, st.wMinute);
    }
    static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    std::snprintf(date, sizeof(date), "%s %d/%d", days[st.wDayOfWeek], st.wMonth, st.wDay);
    ClockText().Text(kit::H(clock));
    DateText().Text(kit::H(date));
}

// ---------------------------------------------------------------------------
// Actions

void MainPage::OpenGame(const GameEntry& game) {
    Svc().PlaySfx("select");
    focused_path_ = game.path;
    if (Svc().Config().qol.quick_launch) {
        Svc().StopMusic();
        Svc().PlaySfx("launch");
        Frame().Navigate(xaml_typename<ONYX3DS::EmulationPage>(), box_value(kit::H(game.path)));
    } else {
        Frame().Navigate(xaml_typename<ONYX3DS::GamePage>(), box_value(kit::H(game.path)));
    }
}

void MainPage::ShowToolsMenu() {
    MenuFlyout menu;
    auto item = [&](const std::string& text, const wchar_t* glyph, std::function<void()> click) {
        MenuFlyoutItem i;
        i.Text(kit::H(text));
        FontIcon icon;
        icon.Glyph(glyph);
        i.Icon(icon);
        i.Click([click](auto&&, auto&&) { click(); });
        menu.Items().Append(i);
    };
    auto weak = get_weak();
    item("Rescan games", kit::glyph::Refresh, [weak] {
        Svc().Toast("Scanning your folders...");
        Svc().RescanLibrary([weak](size_t n) {
            if (auto self = weak.get()) {
                self->Rebuild();
                Svc().Toast(std::to_string(n) + " files in your library");
            }
        });
    });
    item("Download missing artwork", kit::glyph::Picture, [weak] {
        for (const auto& g : Svc().Library().GridView(Svc().Config().qol)) {
            if (!g.grid_art.empty()) continue;
            Svc().ScrapeArt(g, false, [weak](bool ok) {
                if (auto self = weak.get(); self && ok) self->Rebuild();
            });
        }
    });
    MenuFlyoutSubItem sort;
    sort.Text(L"Sort games by");
    for (auto [mode, label] : {std::pair{SortMode::RecentlyPlayed, "Recently played"},
                               std::pair{SortMode::Title, "Title"},
                               std::pair{SortMode::MostPlayed, "Most played"},
                               std::pair{SortMode::Publisher, "Publisher"},
                               std::pair{SortMode::Region, "Region"}}) {
        ToggleMenuFlyoutItem s;
        s.Text(kit::H(label));
        s.IsChecked(Svc().Config().qol.sort == mode);
        s.Click([weak, mode = mode](auto&&, auto&&) {
            Svc().Config().qol.sort = mode;
            Svc().SaveSettings();
            if (auto self = weak.get()) self->Rebuild(false);
        });
        sort.Items().Append(s);
    }
    menu.Items().Append(sort);
    MenuFlyoutSubItem themes;
    themes.Text(L"Theme");
    for (const auto& th : Svc().Themes()) {
        ToggleMenuFlyoutItem s;
        s.Text(kit::H(th.name));
        s.IsChecked(th.id == Svc().CurrentTheme().id);
        s.Click([weak, id = th.id](auto&&, auto&&) {
            Svc().SetTheme(id);
            if (auto self = weak.get()) {
                self->ApplyTheme();
                self->BuildBarButtons();
                self->Rebuild();
            }
        });
        themes.Items().Append(s);
    }
    menu.Items().Append(themes);
    item("Updates & DLC", kit::glyph::Package, [this] {
        Frame().Navigate(xaml_typename<ONYX3DS::SettingsPage>(), box_value(L"updates"));
    });
    item("System check", kit::glyph::Info, [this] {
        Frame().Navigate(xaml_typename<ONYX3DS::SettingsPage>(), box_value(L"system"));
    });
    menu.ShowAt(RightButtonHost());
}

void MainPage::HandleKeyDown(IInspectable const&, KeyRoutedEventArgs const& e) {
    auto focused = FocusManager::GetFocusedElement().try_as<Button>();
    std::optional<GameEntry> game;
    int slot = -1;
    if (focused && focused.Tag()) {
        const std::string path = kit::S(unbox_value<hstring>(focused.Tag()));
        for (size_t i = 0; i < games_.size(); ++i)
            if (games_[i].path == path) {
                game = games_[i];
                slot = static_cast<int>(i) % (columns_ * rows_);
            }
    }
    switch (e.Key()) {
    case VirtualKey::GamepadRightShoulder:
        ChangePage(+1);
        e.Handled(true);
        break;
    case VirtualKey::GamepadLeftShoulder:
        ChangePage(-1);
        e.Handled(true);
        break;
    case VirtualKey::GamepadDPadRight:
    case VirtualKey::GamepadLeftThumbstickRight:
        // Wii style: walking off the right edge turns the page.
        if (game && slot % columns_ == columns_ - 1) {
            ChangePage(+1);
            e.Handled(true);
        }
        break;
    case VirtualKey::GamepadDPadLeft:
    case VirtualKey::GamepadLeftThumbstickLeft:
        if (game && slot % columns_ == 0 && page_ > 0) {
            ChangePage(-1);
            e.Handled(true);
        }
        break;
    case VirtualKey::GamepadY:
        if (game) {
            GameEntry g = *game;
            g.favorite = !g.favorite;
            Svc().Library().Update(g);
            Svc().Toast(g.favorite ? g.DisplayTitle() + " added to favourites"
                                   : g.DisplayTitle() + " removed from favourites");
            Rebuild();
            e.Handled(true);
        }
        break;
    case VirtualKey::GamepadX:
        if (game) {
            Frame().Navigate(xaml_typename<ONYX3DS::GamePage>(), box_value(kit::H(game->path)));
            e.Handled(true);
        }
        break;
    case VirtualKey::GamepadMenu:
        ShowToolsMenu();
        e.Handled(true);
        break;
    case VirtualKey::GamepadView:
        Frame().Navigate(xaml_typename<ONYX3DS::SettingsPage>());
        e.Handled(true);
        break;
    default:
        break;
    }
}

} // namespace winrt::ONYX3DS::implementation

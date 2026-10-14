// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "MainPage.h"
#if __has_include("MainPage.g.cpp")
#include "MainPage.g.cpp"
#endif

#include "Platform/Imaging.h"
#include "Platform/Log.h"
#include "Ui/AppServices.h"
#include "Ui/SystemList.h"
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
constexpr double kAreaHeight = 688;
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
    if (sparkles_) sparkles_.Begin();
    animate_entrance_ = true;
    // Back from a game: undo the launch flourish.
    launching_ = false;
    backdrop_game_.clear(); // settings may have changed; the next focus re-asks
    ContentArea().OpacityTransition(nullptr);
    BottomBar().OpacityTransition(nullptr);
    ContentArea().Opacity(1);
    BottomBar().Opacity(1);
    Svc().StartMusic();

    if (!initialised_) {
        initialised_ = true;
        if (!EmulatorSession::CoreAvailable(Svc().HomeSystem())) Svc().SetHomeSystem(GameSystem::N3DS);
        games_ = Svc().LibraryFor(Svc().HomeSystem()).GridView(Svc().Config().qol);
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
        // A restart after a crash reopens the game it was playing.
        if (const std::string resume = Svc().TakeResumePath(); !resume.empty()) {
            if (auto g = Svc().FindGame(resume)) {
                ONYX_INFO("Reopening %s after the restart", g->title.c_str());
                Svc().SetHomeSystem(g->system);
                Frame().Navigate(xaml_typename<ONYX3DS::EmulationPage>(), box_value(kit::H(g->path)));
                return;
            }
        }
        if (cfg.qol.resume_last_game && !cfg.last_played_path.empty()) {
            if (auto g = Svc().FindGame(cfg.last_played_path)) {
                Svc().SetHomeSystem(g->system);
                Frame().Navigate(xaml_typename<ONYX3DS::EmulationPage>(), box_value(kit::H(g->path)));
            }
        }
    }
}

void MainPage::OnNavigatedFrom(NavigationEventArgs const&) {
    clock_.Stop();
    if (sparkles_) sparkles_.Stop(); // nothing animates behind a running game
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
    const auto font = Svc().ThemeFont(false);
    for (auto tb : {InfoText(), DateText(), HintText()}) tb.FontFamily(font);
    // Title and clock use the heading face (a real bold file when the theme ships one).
    for (auto tb : {TitleText(), ClockText()}) tb.FontFamily(Svc().ThemeFont(true));
    TitleText().Foreground(Svc().ThemeBrush(t.colors.text));
    InfoText().Foreground(Svc().ThemeBrush(t.colors.text_muted));
    ClockText().Foreground(Svc().ThemeBrush(t.colors.bar_text));
    DateText().Foreground(Svc().ThemeBrush(t.colors.bar_text));
    HintText().Foreground(Svc().ThemeBrush(t.colors.bar_text));
    ClockText().Visibility(t.style.show_clock && Svc().Config().qol.show_clock ? Visibility::Visible
                                                                             : Visibility::Collapsed);
    HintText().Text(L"A Open    Y Favourite    X Details    LB / RB Pages    Menu Tools    View Settings");
    std::vector<std::pair<std::string, std::string>> hints = {
        {"A", "Open"}, {"Y", "Favourite"}, {"X", "Details"},
        {"LB/RB", "Pages"}, {"Menu", "Tools"}, {"View", "Settings"}};
    if (AvailableSystems().size() > 1) {
        std::string names;
        for (const GameSystem sys : AvailableSystems()) names += (names.empty() ? "" : " / ") + std::string(SystemLabel(sys));
        hints.insert(hints.begin() + 4, std::make_pair(std::string("LT/RT"), names));
    }
    HintHost().Child(kit::HintBar(hints, t.colors.bar_text));
    MarkHost().Child(kit::OnyxMark(56));
    for (auto tb : {GreetingText(), InfoText()}) tb.FontFamily(font);
    SelectedText().FontFamily(Svc().ThemeFont(true));
    GreetingText().Foreground(Svc().ThemeBrush(t.colors.text_muted));
    SelectedText().Foreground(Svc().ThemeBrush(t.colors.text));
    BuildSparkles();

    // Depth: one shared ThemeShadow, cast onto the layer behind the grid.
    shadow_ = ThemeShadow();
    shadow_.Receivers().Append(ShadowReceiver());
    StartAmbientMotion();
}

void MainPage::StartAmbientMotion() {
    // Plain RenderTransforms only: XAML refuses RenderTransform on an element
    // that also uses the Translation/TranslationTransition facade (that combo
    // threw while building the page and crashed the app on launch).
    // Each layer: [drift (slow loop), parallax (follows the stick)].
    const Theme& t = Svc().CurrentTheme();
    const bool motion = Svc().Config().qol.background_parallax && t.style.parallax > 0;
    auto setup = [&](Image const& img, TranslateTransform& par, double dx, double dy, int seconds) {
        TranslateTransform drift;
        par = TranslateTransform();
        TransformGroup group;
        group.Children().Append(drift);
        group.Children().Append(par);
        img.RenderTransform(group);
        if (!motion) return;
        Storyboard sb;
        for (auto [prop, to] : {std::pair{L"X", dx}, std::pair{L"Y", dy}}) {
            DoubleAnimation a;
            a.From(-to);
            a.To(to);
            a.Duration(DurationHelper::FromTimeSpan(std::chrono::seconds(seconds)));
            a.AutoReverse(true);
            a.RepeatBehavior(RepeatBehaviorHelper::Forever());
            SineEase ease;
            ease.EasingMode(EasingMode::EaseInOut);
            a.EasingFunction(ease);
            Storyboard::SetTarget(a, drift);
            Storyboard::SetTargetProperty(a, prop);
            sb.Children().Append(a);
        }
        sb.Begin();
    };
    // The near layer moves further, which is what sells the depth.
    setup(FarLayer(), far_par_, 24 * t.style.parallax, 10 * t.style.parallax, 23);
    setup(NearLayer(), near_par_, -40 * t.style.parallax, -26 * t.style.parallax, 17);
}

void MainPage::MoveParallax(float nx, float ny) {
    const Theme& t = Svc().CurrentTheme();
    if (!far_par_ || !Svc().Config().qol.background_parallax || t.style.parallax <= 0) return;
    const double p = t.style.parallax;
    // Ease toward the new offset rather than jumping.
    Storyboard sb;
    auto ease_to = [&](TranslateTransform const& tr, double x, double y) {
        for (auto [prop, to] : {std::pair{L"X", x}, std::pair{L"Y", y}}) {
            DoubleAnimation a;
            a.To(to);
            a.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(1200)));
            CubicEase ease;
            ease.EasingMode(EasingMode::EaseOut);
            a.EasingFunction(ease);
            Storyboard::SetTarget(a, tr);
            Storyboard::SetTargetProperty(a, prop);
            sb.Children().Append(a);
        }
    };
    ease_to(far_par_, -nx * 18 * p, -ny * 10 * p);
    ease_to(near_par_, -nx * 42 * p, -ny * 24 * p);
    sb.Begin();
}

// ---------------------------------------------------------------------------
// Grid

void MainPage::Rebuild(bool keep_focus) {
    if (!keep_focus) focused_path_.clear();
    if (theme_applied_ != Svc().CurrentTheme().id) ApplyTheme();
    if (!EmulatorSession::CoreAvailable(Svc().HomeSystem())) Svc().SetHomeSystem(GameSystem::N3DS);
    games_ = Svc().LibraryFor(Svc().HomeSystem()).GridView(Svc().Config().qol);
    if (Xmb() && xmb_special_ != 0) {
        std::vector<GameEntry> all;
        for (const GameSystem s : AvailableSystems()) {
            auto part = Svc().LibraryFor(s).GridView(Svc().Config().qol);
            all.insert(all.end(), part.begin(), part.end());
        }
        games_.clear();
        for (auto& g : all)
            if (xmb_special_ == 2 ? g.favorite : g.last_played > 0) games_.push_back(g);
        if (xmb_special_ == 1) {
            std::stable_sort(games_.begin(), games_.end(),
                             [](const GameEntry& a, const GameEntry& b) { return a.last_played > b.last_played; });
            if (games_.size() > 24) games_.resize(24);
        }
    }
    UpdateSystemTabs();
    columns_ = std::clamp(Svc().Config().qol.grid_columns, 3, 6);
    rows_ = 3;
    if (Xmb()) {
        columns_ = 1;
        rows_ = std::max(1, static_cast<int>(games_.size()));
    }
    const int per_page = columns_ * rows_;
    const int pages = std::max(1, static_cast<int>((games_.size() + per_page - 1) / per_page));
    // Keep the focused game's page after a rescan.
    if (!focused_path_.empty()) {
        for (size_t i = 0; i < games_.size(); ++i)
            if (games_[i].path == focused_path_) page_ = static_cast<int>(i) / per_page;
    }
    page_ = std::clamp(page_, 0, pages - 1);
    BuildPage();

    // Box art on: fetch what is missing for the whole library in the background, then redraw once.
    if (BoxArtOn() && !box_prefetch_started_) {
        box_prefetch_started_ = true;
        std::vector<GameEntry> all;
        for (const GameSystem s : AvailableSystems()) {
            auto part = Svc().LibraryFor(s).GridView(Svc().Config().qol);
            all.insert(all.end(), part.begin(), part.end());
        }
        Svc().PrefetchBoxArt(std::move(all), [weak = get_weak()](int fetched) {
            auto self = weak.get();
            if (!self) return;
            self->box_prefetch_started_ = false;
            if (fetched > 0) self->Rebuild();
        });
    }
}

// ---------------------------------------------------------------------------
// 3DS | NES | GBA tabs

UIElement MainPage::MakeSystemTabs() {
    StackPanel tabs;
    tabs.Orientation(Orientation::Horizontal);
    tabs.Spacing(10);
    const GameSystem current = Svc().HomeSystem();
    auto weak = get_weak();
    for (const GameSystem system : AvailableSystems()) {
        auto chip = kit::Chip(SystemLabel(system), nullptr, system == current);
        chip.Tapped([weak, system](auto&&, auto&&) {
            if (auto self = weak.get()) self->SwitchSystem(system);
        });
        tabs.Children().Append(chip);
    }
    return tabs;
}

void MainPage::UpdateSystemTabs() {
    const bool show = AvailableSystems().size() > 1 && !Xmb();
    SystemTabsHost().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
    if (show) SystemTabsHost().Child(MakeSystemTabs());
}

void MainPage::SwitchSystem(GameSystem system) {
    if (system == Svc().HomeSystem() && xmb_special_ == 0) return;
    if (!EmulatorSession::CoreAvailable(system)) return;
    xmb_special_ = 0;
    Svc().SetHomeSystem(system);
    Svc().PlaySfx("move");
    page_ = 0;
    focused_path_.clear();
    animate_entrance_ = true;
    Rebuild(false);
}

void MainPage::BuildPage() {
    auto grid = ChannelGrid();
    grid.Children().Clear();
    grid.RowDefinitions().Clear();
    grid.ColumnDefinitions().Clear();

    if (games_.empty() && !(Xmb() && xmb_special_ != 0)) {
        ShowEmptyState();
        ContentArea().Visibility(Visibility::Collapsed);
        return;
    }
    EmptyState().Visibility(Visibility::Collapsed);
    ContentArea().Visibility(Visibility::Visible);
    if (Xmb()) {
        BuildXmb();
        return;
    }

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
        // Tiles fade in one after another (opacity only: tiles use the Translation
        // and Scale properties, which can't be mixed with a RenderTransform).
        if (animate_entrance_) kit::Entrance(element, 0, 0, slot * 28, 260);
    }
    animate_entrance_ = false;
    UpdateGreeting();

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
    const std::string box = BoxArtOn() ? Svc().BoxArtPath(game) : std::string{};
    const bool portrait_tile = w <= h * 1.1;
    auto fill_gradient = [&] {
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
    };
    if (!box.empty() && portrait_tile) {
        // A tall tile made for the box: the cover fills it.
        ImageBrush brush;
        brush.ImageSource(ImageFromFile(box, static_cast<int>(h * 1.2)));
        brush.Stretch(Stretch::UniformToFill);
        art.Background(brush);
    } else if (!box.empty()) {
        // A wide tile (the channel grid): the box stands in the middle of the card.
        fill_gradient();
        Image cover;
        cover.Source(ImageFromFile(box, static_cast<int>(h * 1.2)));
        cover.Stretch(Stretch::Uniform);
        cover.Margin(Thickness{h * 0.05, h * 0.05, h * 0.05, h * 0.05});
        art.Child(cover);
    } else if (!game.grid_art.empty() && CachedExists(game.grid_art)) {
        ImageBrush brush;
        brush.ImageSource(ImageFromFile(game.grid_art, static_cast<int>(w * 1.3)));
        brush.Stretch(Stretch::UniformToFill);
        art.Background(brush);
    } else {
        fill_gradient();
        StackPanel inner;
        inner.HorizontalAlignment(HorizontalAlignment::Center);
        inner.VerticalAlignment(VerticalAlignment::Center);
        inner.Spacing(8);
        if (!game.icon_path.empty()) {
            Image icon;
            icon.Width(h * 0.42);
            icon.Height(h * 0.42);
            LoadIconInto(icon, game.icon_path, 48, 48);
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
    if (Xmb()) {
        // The whole row nudges toward the panel while it is the one in focus.
        if (const auto row = VisualTreeHelper::GetParent(tile).try_as<StackPanel>())
            row.Translation(float3{focused ? 26.f : 0.f, 0.f, 0.f});
    }
    if (!focused) return;
    focused_path_ = game.path;
    if (Xmb()) {
        UpdateXmbPanel(game);
        ScrollXmbTo(tile);
    }
    ShowBackdrop(game);
    Svc().SetSelectedGame(game);
    Svc().PlaySfx("move");
    SelectedText().Text(kit::H(game.DisplayTitle()));
    InfoText().Text(kit::H(FormatLastPlayed(game.last_played) +
                           (game.play_seconds ? "   ·   " + FormatPlayTime(game.play_seconds) : "") +
                           (game.favorite ? "   ·   Favourite" : "")));
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
    animate_entrance_ = true;
    BuildPage();
    grid.Opacity(1);
    grid.Translation(float3{0, 0, 0});
}

// ---------------------------------------------------------------------------
// Empty state, bar buttons, clock

void MainPage::ShowEmptyState() {
    if (Svc().HomeSystem() != GameSystem::N3DS) {
        ShowRetroEmptyState();
        return;
    }
    auto host = EmptyState();
    host.Children().Clear();
    host.Visibility(Visibility::Visible);
    const auto drives = RemovableDriveRoots();
    StackPanel body;
    body.Spacing(18);
    // The grid (and its tabs) is hidden while empty: keep the way over to the other lists.
    if (AvailableSystems().size() > 1) body.Children().Append(MakeSystemTabs());
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

void MainPage::ShowRetroEmptyState() {
    auto host = EmptyState();
    host.Children().Clear();
    host.Visibility(Visibility::Visible);
    const auto drives = RemovableDriveRoots();
    const bool gba = Svc().HomeSystem() == GameSystem::Gba;
    const std::string label = gba ? "GBA" : "NES";
    StackPanel body;
    body.Spacing(18);
    body.Children().Append(MakeSystemTabs());
    body.Children().Append(kit::Text(Svc().Scanning() ? "Looking for games..." : "No " + label + " games yet", 44, true));
    const std::string folder = gba ? Svc().Config().folders.gba_roms : Svc().Config().folders.nes_roms;
    body.Children().Append(kit::Text(
        folder.empty()
            ? "Choose the folder with your " + label + " games (" + (gba ? ".gba, .agb; ROM hacks too" : ".nes, .unf") +
                  "). \"Set up USB drive\" creates a Roms/" + label + " folder inside ONYX3DS on the drive."
            : std::string(gba ? "No .gba or .agb files found in " : "No .nes or .unf files found in ") + folder +
                  ". Copy your games there and rescan.",
        24));
    StackPanel buttons;
    buttons.Orientation(Orientation::Horizontal);
    buttons.Spacing(16);
    if (!drives.empty() && folder.empty()) {
        buttons.Children().Append(kit::ActionButton("Set up USB drive", kit::glyph::Usb, [this, drives, label] {
            auto& cfg = Svc().Config();
            cfg.folders.ApplyDriveLayout(drives.front());
            for (const auto& sub : FolderConfig::DriveLayoutSubfolders())
                Svc().Fs().CreateDirs(JoinPath(JoinPath(drives.front(), "ONYX3DS"), sub));
            Svc().SaveSettings();
            Svc().Toast("Created " + drives.front() + "/ONYX3DS/Roms/" + label + ". Copy your " + label + " games into it.");
            auto weak = get_weak();
            Svc().RescanLibrary([weak](size_t) {
                if (auto self = weak.get()) self->Rebuild(false);
            });
        }, true));
    }
    buttons.Children().Append(kit::ActionButton("Choose " + label + " folder", kit::glyph::Folder, [this, gba] {
        Frame().Navigate(xaml_typename<ONYX3DS::FolderPage>(), box_value(gba ? L"gba_roms" : L"nes_roms"));
    }, buttons.Children().Size() == 0));
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

bool MainPage::Xmb() const { return Svc().Config().qol.xmb_layout; }

// Categories left to right: Recent, Favourites, then each system.
void MainPage::StepCategory(int delta) {
    const auto systems = AvailableSystems();
    const int count = 2 + static_cast<int>(systems.size());
    int current = xmb_special_ != 0 ? xmb_special_ - 1 : 2;
    if (xmb_special_ == 0)
        for (size_t i = 0; i < systems.size(); ++i)
            if (systems[i] == Svc().HomeSystem()) current = 2 + static_cast<int>(i);
    const int next = (current + delta + count) % count;
    Svc().PlaySfx("move");
    focused_path_.clear();
    animate_entrance_ = true;
    if (next < 2) {
        xmb_special_ = next + 1;
        Rebuild(false);
    } else {
        xmb_special_ = 0;
        const GameSystem s = systems[next - 2];
        if (s != Svc().HomeSystem()) Svc().SetHomeSystem(s);
        Rebuild(false);
    }
}

// Title (or logo), facts and, after a short pause so that scrolling past games costs nothing, the
// critics' score, description and release details from the web. The picture of the game itself
// is the faded backdrop of the whole page (ShowBackdrop), not a banner in here.
static void SetCover(Border const& host, const std::string& path, ThemeShadow const& shadow) {
    Image img;
    img.Source(ImageFromFile(path, 700));
    img.Stretch(Stretch::Uniform);
    img.MaxHeight(430);
    img.HorizontalAlignment(HorizontalAlignment::Left);
    img.Shadow(shadow);
    img.Translation(float3{0.f, 0.f, 32.f});
    host.Child(img);
    host.Visibility(Visibility::Visible);
}

void MainPage::UpdateXmbPanel(const GameEntry& game) {
    if (!xmb_panel_ || xmb_shown_ == game.path) return;
    xmb_shown_ = game.path;
    const Theme& t = Svc().CurrentTheme();
    xmb_panel_.Children().Clear();

    StackPanel layout;
    layout.Orientation(Orientation::Horizontal);
    layout.Spacing(40);
    layout.VerticalAlignment(VerticalAlignment::Center);

    xmb_cover_ = Border();
    xmb_cover_.VerticalAlignment(VerticalAlignment::Top);
    xmb_cover_.Visibility(Visibility::Collapsed);
    xmb_cover_.MaxWidth(300);
    if (BoxArtOn()) {
        if (const std::string box = Svc().BoxArtPath(game); !box.empty()) SetCover(xmb_cover_, box, shadow_);
    }
    layout.Children().Append(xmb_cover_);

    StackPanel column;
    column.Spacing(14);
    column.MaxWidth(BoxArtOn() ? 620 : 800);
    column.VerticalAlignment(VerticalAlignment::Center);
    if (!game.logo_art.empty() && CachedExists(game.logo_art)) {
        Image logo;
        logo.Source(ImageFromFile(game.logo_art, 700));
        logo.MaxWidth(520);
        logo.MaxHeight(150);
        logo.Stretch(Stretch::Uniform);
        logo.HorizontalAlignment(HorizontalAlignment::Left);
        column.Children().Append(logo);
    } else {
        auto title = kit::Text(game.DisplayTitle(), 54, true);
        title.TextTrimming(TextTrimming::CharacterEllipsis);
        title.MaxLines(2);
        title.TextWrapping(TextWrapping::Wrap);
        column.Children().Append(title);
    }

    std::string facts = SystemLabel(game.system);
    if (game.play_seconds) facts += "  ·  " + FormatPlayTime(game.play_seconds);
    else if (game.last_played) facts += "  ·  " + FormatLastPlayed(game.last_played);
    column.Children().Append(kit::Text(facts, 20, false, kit::WithAlpha(t.colors.text_muted, 0xF0)));

    xmb_info_ = StackPanel();
    xmb_info_.Spacing(14);
    xmb_info_.Opacity(0);
    xmb_info_.OpacityTransition(ScalarTransition());
    column.Children().Append(xmb_info_);
    layout.Children().Append(column);
    xmb_panel_.Children().Append(layout);
    kit::Entrance(layout, 30, 0, 0, 260);
    if (!Svc().Config().qol.xmb_info) return;

    xmb_pending_ = game;
    if (!info_timer_) {
        info_timer_ = DispatcherTimer();
        info_timer_.Interval(Ms(350));
        auto weak = get_weak();
        info_timer_.Tick([weak](auto&&, auto&&) {
            auto self = weak.get();
            if (!self) return;
            self->info_timer_.Stop();
            const GameEntry entry = self->xmb_pending_;
            if (self->xmb_shown_ != entry.path) return;
            Svc().LoadGameInfo(entry, [weak, entry](GameInfo info) {
                if (auto self = weak.get())
                    if (self->xmb_shown_ == entry.path) self->FillXmbInfo(entry, info);
            });
        });
    }
    info_timer_.Stop();
    info_timer_.Start();
}

// Critics' stars, e.g. four and a half.
static std::string StarText(double stars) {
    const int full = static_cast<int>(stars);
    const bool half = stars - full >= 0.5;
    std::string out;
    for (int i = 0; i < full; ++i) out += "\xE2\x98\x85"; // black star
    if (half) out += "\xC2\xBD";
    for (int i = full + (half ? 1 : 0); i < 5; ++i) out += "\xE2\x98\x86"; // white star
    return out;
}

void MainPage::FillXmbInfo(const GameEntry&, const GameInfo& info) {
    if (!xmb_info_ || !info.found) return;
    const Theme& t = Svc().CurrentTheme();
    xmb_info_.Children().Clear();
    if (BoxArtOn() && !info.boxart.empty() && xmb_cover_ && !xmb_cover_.Child())
        SetCover(xmb_cover_, info.boxart, shadow_);

    if (info.score > 0) {
        StackPanel row;
        row.Orientation(Orientation::Horizontal);
        row.Spacing(16);
        auto stars = kit::Text(StarText(info.Stars()), 32, false, t.colors.accent);
        stars.VerticalAlignment(VerticalAlignment::Center);
        auto score = kit::Text(std::to_string(info.score), 28, true);
        score.VerticalAlignment(VerticalAlignment::Center);
        auto from = kit::Text("Critics' average, " + info.score_source, 16, false, kit::WithAlpha(t.colors.text_muted, 0xE0));
        from.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(stars);
        row.Children().Append(score);
        row.Children().Append(from);
        xmb_info_.Children().Append(row);
    }
    if (!info.genre.empty()) {
        StackPanel chips;
        chips.Orientation(Orientation::Horizontal);
        chips.Spacing(10);
        std::string rest = info.genre;
        int shown = 0;
        while (!rest.empty() && shown < 3) {
            const auto comma = rest.find(',');
            std::string one = rest.substr(0, comma);
            rest = comma == std::string::npos ? std::string{} : rest.substr(comma + 1);
            const auto first = one.find_first_not_of(' ');
            if (first == std::string::npos) continue;
            chips.Children().Append(kit::Chip(one.substr(first)));
            ++shown;
        }
        if (shown) xmb_info_.Children().Append(chips);
    }
    if (!info.summary.empty()) {
        auto text = kit::Text(info.summary, 21);
        text.TextWrapping(TextWrapping::Wrap);
        text.MaxLines(6);
        text.TextTrimming(TextTrimming::CharacterEllipsis);
        text.LineHeight(31);
        xmb_info_.Children().Append(text);
    }
    std::string details;
    auto add = [&](const char* label, const std::string& v) {
        if (v.empty()) return;
        details += (details.empty() ? "" : "     ") + std::string(label) + "  " + v;
    };
    add("Released", info.released);
    add("Developer", info.developer);
    add("Publisher", info.publisher);
    if (!details.empty()) {
        auto line = kit::Text(details, 17, false, kit::WithAlpha(t.colors.text_muted, 0xF0));
        line.TextWrapping(TextWrapping::Wrap);
        line.MaxLines(2);
        xmb_info_.Children().Append(line);
    }
    std::string credit = "Details from Wikipedia, pictures from the libretro thumbnail project";
    if (BoxArtOn() && !info.boxart.empty()) credit += ". Box art (c) Nintendo";
    xmb_info_.Children().Append(kit::Text(credit, 13, false, kit::WithAlpha(t.colors.text_muted, 0xA0)));
    xmb_info_.Opacity(1);
}

// ---------------------------------------------------------------------------
// The focused game's picture, faded behind the whole page

bool MainPage::BoxArtOn() const { return Svc().Config().qol.box_art; }

void MainPage::HideBackdrop() {
    BackdropA().Opacity(0);
    BackdropB().Opacity(0);
    BackdropScrim().Opacity(0);
    backdrop_file_.clear();
}

void MainPage::SetBackdropImage(const std::string& path) {
    if (path == backdrop_file_) return;
    backdrop_file_ = path;
    backdrop_front_ = !backdrop_front_;
    Image in = backdrop_front_ ? BackdropA() : BackdropB();
    Image out = backdrop_front_ ? BackdropB() : BackdropA();
    in.Source(ImageFromFile(path, 1280));
    // Snap back to full size unseen, then creep outwards slowly. A composition scale: it costs the
    // UI thread nothing per frame.
    in.ScaleTransition(nullptr);
    in.Scale(float3{1.0f, 1.0f, 1.0f});
    in.CenterPoint(float3{static_cast<float>(Root().ActualWidth() / 2), static_cast<float>(Root().ActualHeight() / 2), 0.f});
    Vector3Transition zoom;
    zoom.Duration(std::chrono::seconds(14));
    in.ScaleTransition(zoom);
    in.Scale(float3{1.07f, 1.07f, 1.0f});
    in.Opacity(0.6);
    out.Opacity(0);
    BackdropScrim().Opacity(1);
}

// Asks for the picture a moment after the cursor settles, so moving quickly through a list costs
// nothing and the old picture stays until the new one is ready.
void MainPage::ShowBackdrop(const GameEntry& game) {
    if (!Svc().Config().qol.game_backdrop) {
        if (!backdrop_file_.empty()) HideBackdrop();
        return;
    }
    if (backdrop_game_ == game.path) return;
    backdrop_game_ = game.path;
    backdrop_entry_ = game;
    if (!backdrop_timer_) {
        backdrop_timer_ = DispatcherTimer();
        backdrop_timer_.Interval(Ms(240));
        auto weak = get_weak();
        backdrop_timer_.Tick([weak](auto&&, auto&&) {
            auto self = weak.get();
            if (!self) return;
            self->backdrop_timer_.Stop();
            const GameEntry entry = self->backdrop_entry_;
            Svc().LoadGameInfo(entry, [weak, entry](GameInfo info) {
                auto self = weak.get();
                if (!self || self->backdrop_game_ != entry.path) return;
                // A gameplay screenshot first; else the game's SteamGridDB hero, else its cover.
                std::string pic = !info.shots.empty() ? info.shots.front() : std::string{};
                if (pic.empty() && !entry.hero_art.empty() && CachedExists(entry.hero_art)) pic = entry.hero_art;
                if (pic.empty() && !entry.grid_art.empty() && CachedExists(entry.grid_art)) pic = entry.grid_art;
                if (pic.empty()) self->HideBackdrop();
                else self->SetBackdropImage(pic);
            });
        });
    }
    backdrop_timer_.Stop();
    backdrop_timer_.Start();
}

// Keeps the focused row a little below the top and glides there.
void MainPage::ScrollXmbTo(Button const& tile) {
    if (!xmb_scroll_) return;
    const auto content = xmb_scroll_.Content().try_as<UIElement>();
    if (!content) return;
    try {
        const auto pt = tile.TransformToVisual(content).TransformPoint(Point{0.f, 0.f});
        xmb_scroll_.ChangeView(nullptr, std::max(0.0, static_cast<double>(pt.Y) - 150.0), nullptr, false);
    } catch (...) {
    }
}

// The crossbar: category names across the top, the chosen category's games down the page.
void MainPage::BuildXmb() {
    const Theme& t = Svc().CurrentTheme();
    auto grid = ChannelGrid();
    RowDefinition bar_row;
    bar_row.Height(GridLengthHelper::FromPixels(86));
    grid.RowDefinitions().Append(bar_row);
    RowDefinition list_row;
    list_row.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    grid.RowDefinitions().Append(list_row);

    std::vector<std::pair<std::string, bool>> cats;
    cats.push_back({"Recent", xmb_special_ == 1});
    cats.push_back({"Favourites", xmb_special_ == 2});
    for (const GameSystem s : AvailableSystems())
        cats.push_back({SystemLabel(s), xmb_special_ == 0 && s == Svc().HomeSystem()});
    StackPanel bar;
    bar.Orientation(Orientation::Horizontal);
    bar.Spacing(46);
    bar.VerticalAlignment(VerticalAlignment::Center);
    bar.Margin(Thickness{40, 0, 0, 0});
    int index = 0;
    auto weak = get_weak();
    for (const auto& [label, on] : cats) {
        auto text = kit::Text(label, on ? 38 : 26, on, on ? std::string{} : kit::WithAlpha(t.colors.text_muted, 0xC0));
        text.VerticalAlignment(VerticalAlignment::Center);
        const int target = index++;
        text.Tapped([weak, target](auto&&, auto&&) {
            if (auto self = weak.get()) {
                self->xmb_special_ = target < 2 ? target + 1 : 0;
                if (target >= 2) Svc().SetHomeSystem(AvailableSystems()[target - 2]);
                self->focused_path_.clear();
                self->animate_entrance_ = true;
                self->Rebuild(false);
            }
        });
        bar.Children().Append(text);
    }
    Grid::SetRow(bar, 0);
    grid.Children().Append(bar);

    // The glowing line under the bar
    Border line;
    line.Height(2);
    line.VerticalAlignment(VerticalAlignment::Bottom);
    line.Margin(Thickness{40, 0, 40, 0});
    line.Background(Svc().ThemeBrush(kit::WithAlpha(t.colors.accent, 0x90)));
    Grid::SetRow(line, 0);
    grid.Children().Append(line);

    Grid content;
    ColumnDefinition list_col;
    list_col.Width(GridLengthHelper::FromPixels(690));
    ColumnDefinition panel_col;
    panel_col.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    content.ColumnDefinitions().Append(list_col);
    content.ColumnDefinitions().Append(panel_col);
    Grid::SetRow(content, 1);
    grid.Children().Append(content);

    // The hover panel on the right: banner, details and screenshots of the focused game.
    xmb_panel_ = Grid();
    xmb_panel_.Margin(Thickness{30, 14, 0, 20});
    Grid::SetColumn(xmb_panel_, 1);
    content.Children().Append(xmb_panel_);
    xmb_shown_.clear();

    ScrollViewer scroll;
    scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Hidden);
    scroll.HorizontalScrollMode(ScrollMode::Disabled);
    scroll.VerticalScrollMode(ScrollMode::Enabled);
    scroll.Margin(Thickness{40, 14, 0, 0});
    scroll.BringIntoViewOnFocusChange(false); // ScrollXmbTo glides there instead
    xmb_scroll_ = scroll;
    StackPanel list;
    list.Spacing(14);
    list.Padding(Thickness{0, 12, 0, 40});
    Button focus_target{nullptr};
    const double th_wide = 118, tw = th_wide / 0.5625;
    if (games_.empty()) {
        list.Children().Append(kit::Text(xmb_special_ == 2 ? "Press Y on a game to add it here."
                                                           : "Games you play show up here.",
                                         24, false, kit::WithAlpha(t.colors.text_muted, 0xE0)));
    }
    int slot = 0;
    for (const auto& game : games_) {
        StackPanel row;
        row.Orientation(Orientation::Horizontal);
        row.Spacing(26);
        // With box art on, a game that has its box gets a tall tile; the rest keep the wide one.
        const bool boxed = BoxArtOn() && !Svc().BoxArtPath(game).empty();
        const double th = boxed ? 156 : th_wide;
        auto tile = MakeTile(game, boxed ? th * 0.86 : tw, th);
        if (game.path == focused_path_ || (!focus_target && focused_path_.empty())) focus_target = tile;
        row.Children().Append(tile);
        StackPanel words;
        words.VerticalAlignment(VerticalAlignment::Center);
        auto name = kit::Text(game.DisplayTitle(), 28, true);
        name.TextWrapping(TextWrapping::Wrap);
        name.MaxLines(2);
        name.TextTrimming(TextTrimming::CharacterEllipsis);
        words.MaxWidth(400);
        words.Children().Append(name);
        std::string sub = SystemLabel(game.system);
        if (game.play_seconds) sub += "  ·  " + FormatPlayTime(game.play_seconds);
        else if (game.last_played) sub += "  ·  " + FormatLastPlayed(game.last_played);
        words.Children().Append(kit::Text(sub, 18, false, kit::WithAlpha(t.colors.text_muted, 0xE0)));
        row.Children().Append(words);
        Vector3Transition glide;
        glide.Duration(Ms(240));
        row.TranslationTransition(glide);
        if (animate_entrance_) kit::Entrance(row, 0, 0, std::min(slot, 10) * 30, 280);
        list.Children().Append(row);
        ++slot;
    }
    animate_entrance_ = false;
    scroll.Content(list);
    Grid::SetColumn(scroll, 0);
    content.Children().Append(scroll);

    UpdateGreeting();
    PageDots().Children().Clear();
    if (focus_target) focus_target.Focus(FocusState::Programmatic);
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
    UpdateGreeting();
}

void MainPage::UpdateGreeting() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    const char* part = st.wHour < 5 ? "Up late" : st.wHour < 12 ? "Good morning"
                       : st.wHour < 18 ? "Good afternoon" : "Good evening";
    const size_t n = games_.size();
    const GameSystem home = Svc().HomeSystem();
    const std::string noun = home == GameSystem::N3DS ? std::string() : std::string(" ") + SystemLabel(home);
    const std::string text = std::string(part) + "   ·   " + std::to_string(n) + noun + (n == 1 ? " game" : " games");
    if (kit::S(GreetingText().Text()) != text) GreetingText().Text(kit::H(text));
}

void MainPage::BuildSparkles() {
    // A dozen soft motes drifting up and pulsing: plain RenderTransforms on
    // shapes in a Canvas (independent animations, run by the compositor).
    auto canvas = Sparkles();
    if (sparkles_) sparkles_.Stop();
    sparkles_ = nullptr;
    canvas.Children().Clear();
    const Theme& t = Svc().CurrentTheme();
    if (!Svc().Config().qol.background_parallax || t.style.parallax <= 0) return;
    Storyboard sb;
    uint32_t seed = 0x9E3779B9u;
    auto rnd = [&seed](double lo, double hi) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return lo + (hi - lo) * ((seed & 0xFFFF) / 65535.0);
    };
    for (int i = 0; i < 14; ++i) {
        Shapes::Ellipse dot;
        const double size = rnd(6, 18);
        dot.Width(size);
        dot.Height(size);
        dot.Fill(Svc().ThemeBrush(kit::WithAlpha(i % 3 ? t.colors.tile_glow : t.colors.accent, 0x90)));
        dot.Stroke(Svc().ThemeBrush(kit::WithAlpha(t.colors.tile_glow, 0x30)));
        dot.StrokeThickness(size * 0.35);
        Canvas::SetLeft(dot, rnd(40, 1880));
        Canvas::SetTop(dot, rnd(120, 900));
        TranslateTransform tr;
        dot.RenderTransform(tr);
        dot.Opacity(0);
        canvas.Children().Append(dot);

        const auto dur = std::chrono::milliseconds(static_cast<int>(rnd(9000, 16000)));
        const auto delay = std::chrono::milliseconds(static_cast<int>(rnd(0, 8000)));
        DoubleAnimation rise;
        rise.From(0.0);
        rise.To(-rnd(120, 260));
        rise.Duration(DurationHelper::FromTimeSpan(dur));
        rise.BeginTime(TimeSpan{delay});
        rise.RepeatBehavior(RepeatBehaviorHelper::Forever());
        Storyboard::SetTarget(rise, tr);
        Storyboard::SetTargetProperty(rise, L"Y");
        sb.Children().Append(rise);
        DoubleAnimationUsingKeyFrames fade;
        fade.Duration(DurationHelper::FromTimeSpan(dur));
        fade.BeginTime(TimeSpan{delay});
        fade.RepeatBehavior(RepeatBehaviorHelper::Forever());
        for (auto [at, value] : {std::pair{0.0, 0.0}, std::pair{0.3, 0.8}, std::pair{0.7, 0.6},
                                 std::pair{1.0, 0.0}}) {
            LinearDoubleKeyFrame k;
            k.KeyTime(KeyTimeHelper::FromTimeSpan(TimeSpan{std::chrono::milliseconds(
                static_cast<long long>(dur.count() * at))}));
            k.Value(value);
            fade.KeyFrames().Append(k);
        }
        Storyboard::SetTarget(fade, dot);
        Storyboard::SetTargetProperty(fade, L"Opacity");
        sb.Children().Append(fade);
    }
    sparkles_ = sb;
    sb.Begin();
}

// ---------------------------------------------------------------------------
// Actions

void MainPage::OpenGame(const GameEntry& game) {
    if (launching_) return;
    launching_ = true;
    Svc().PlaySfx("select");
    focused_path_ = game.path;
    const bool quick = Svc().Config().qol.quick_launch;
    if (quick) {
        Svc().StopMusic();
        Svc().PlaySfx("launch");
    }
    // The page fades away over the game's picture for a beat, then the game opens.
    ScalarTransition fade;
    fade.Duration(Ms(170));
    ContentArea().OpacityTransition(fade);
    BottomBar().OpacityTransition(fade);
    ContentArea().Opacity(0);
    BottomBar().Opacity(0);
    auto timer = DispatcherTimer();
    timer.Interval(Ms(170));
    const std::string path = game.path;
    timer.Tick([weak = get_weak(), quick, path](IInspectable const& sender, auto&&) {
        sender.as<DispatcherTimer>().Stop();
        auto self = weak.get();
        if (!self) return;
        if (quick) self->Frame().Navigate(xaml_typename<ONYX3DS::EmulationPage>(), box_value(kit::H(path)));
        else self->Frame().Navigate(xaml_typename<ONYX3DS::GamePage>(), box_value(kit::H(path)));
    });
    timer.Start();
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
        for (const auto& g : Svc().LibraryFor(Svc().HomeSystem()).GridView(Svc().Config().qol)) {
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
    // LT / RT: step through the 3DS, NES and GBA lists.
    case VirtualKey::GamepadLeftTrigger:
        if (Xmb()) { StepCategory(-1); e.Handled(true); break; }
        SwitchSystem(NeighbourSystem(Svc().HomeSystem(), -1));
        e.Handled(true);
        break;
    case VirtualKey::GamepadRightTrigger:
        if (Xmb()) { StepCategory(+1); e.Handled(true); break; }
        SwitchSystem(NeighbourSystem(Svc().HomeSystem(), +1));
        e.Handled(true);
        break;
    case VirtualKey::GamepadRightShoulder:
        if (Xmb()) { StepCategory(+1); e.Handled(true); break; }
        ChangePage(+1);
        e.Handled(true);
        break;
    case VirtualKey::GamepadLeftShoulder:
        if (Xmb()) { StepCategory(-1); e.Handled(true); break; }
        ChangePage(-1);
        e.Handled(true);
        break;
    case VirtualKey::GamepadDPadRight:
    case VirtualKey::GamepadLeftThumbstickRight:
        if (Xmb()) { StepCategory(+1); e.Handled(true); break; }
        // Wii style: walking off the right edge turns the page.
        if (game && slot % columns_ == columns_ - 1) {
            ChangePage(+1);
            e.Handled(true);
        }
        break;
    case VirtualKey::GamepadDPadLeft:
    case VirtualKey::GamepadLeftThumbstickLeft:
        if (Xmb()) { StepCategory(-1); e.Handled(true); break; }
        if (game && slot % columns_ == 0 && page_ > 0) {
            ChangePage(-1);
            e.Handled(true);
        }
        break;
    case VirtualKey::GamepadY:
        if (game) {
            GameEntry g = *game;
            g.favorite = !g.favorite;
            Svc().LibraryFor(g.system).Update(g);
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

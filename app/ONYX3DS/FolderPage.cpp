// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "FolderPage.h"
#if __has_include("FolderPage.g.cpp")
#include "FolderPage.g.cpp"
#endif

#include "Emu/EmulatorSession.h"
#include "Ui/AppServices.h"
#include "Ui/UiKit.h"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Navigation;
using namespace onyx;
using namespace onyx::app;
namespace kit = onyx::app::ui;

namespace winrt::ONYX3DS::implementation {

namespace {
AppServices& Svc() {
    return AppServices::Get();
}

std::string ParentDir(const std::string& dir) {
    std::string d = NormalizeSlashes(dir);
    while (!d.empty() && d.back() == '/') d.pop_back();
    const auto slash = d.find_last_of('/');
    if (slash == std::string::npos) return {}; // "E:" -> drive list
    return d.substr(0, slash);
}
} // namespace

void FolderPage::InitializeComponent() {
    FolderPageT::InitializeComponent();
    Entries().ItemClick([weak = get_weak()](auto&&, ItemClickEventArgs const& e) {
        auto self = weak.get();
        if (!self) return;
        Svc().PlaySfx("select");
        if (auto item = e.ClickedItem().try_as<StackPanel>(); item && item.Tag()) {
            self->Browse(kit::S(unbox_value<hstring>(item.Tag())));
        }
    });
}

void FolderPage::OnNavigatedTo(NavigationEventArgs const& e) {
    const std::string key = kit::S(unbox_value_or<hstring>(e.Parameter(), L"roms"));
    for (int k = 0; k < static_cast<int>(FolderKind::Count); ++k)
        if (key == FolderKindKey(static_cast<FolderKind>(k))) kind_ = static_cast<FolderKind>(k);

    const Theme& t = Svc().CurrentTheme();
    Root().Background(Svc().ThemeBrush(t.colors.background_bottom));
    ListCard().Background(Svc().ThemeBrush(t.colors.panel));
    TitleText().Foreground(Svc().ThemeBrush(t.colors.text));
    PathText().Foreground(Svc().ThemeBrush(t.colors.text_muted));
    TitleText().Text(kit::H(std::string("Choose a folder: ") + FolderKindLabel(kind_)));

    // B goes up a level before leaving the page.
    auto weak = get_weak();
    Svc().SetBackOverride([weak] {
        auto self = weak.get();
        if (!self || self->current_.empty()) return false;
        Svc().PlaySfx("back");
        self->Browse(ParentDir(self->current_));
        return true;
    });

    auto buttons = Buttons();
    buttons.Children().Clear();
    buttons.Children().Append(kit::ActionButton("Use this folder", kit::glyph::Check, [weak] {
        if (auto self = weak.get()) self->Choose();
    }, true));
    buttons.Children().Append(kit::ActionButton("Up", kit::glyph::Back, [weak] {
        if (auto self = weak.get()) self->Browse(ParentDir(self->current_));
    }));
    buttons.Children().Append(kit::ActionButton("New folder here", kit::glyph::Folder, [weak] {
        auto self = weak.get();
        if (!self || self->current_.empty()) return;
        // Name it after what it is for, e.g. "Textures", "Textures 2"...
        std::string base = FolderKindLabel(self->kind_);
        std::erase_if(base, [](char c) { return std::strchr("&()", c) != nullptr; });
        std::string name = Trim(base);
        for (int i = 2; Svc().Fs().Exists(JoinPath(self->current_, name)); ++i)
            name = Trim(base) + " " + std::to_string(i);
        Svc().Fs().CreateDirs(JoinPath(self->current_, name));
        self->Browse(JoinPath(self->current_, name));
    }));
    buttons.Children().Append(kit::ActionButton("Cancel", nullptr, [weak] {
        if (auto self = weak.get()) self->Frame().GoBack();
    }));

    // Start in the current setting if it still exists, else on the drive list.
    const std::string existing = Svc().Config().folders.Get(kind_);
    Browse(!existing.empty() && Svc().Fs().IsDirectory(existing) ? existing : "");
}

void FolderPage::Browse(const std::string& dir) {
    current_ = dir;
    auto list = Entries();
    list.Items().Clear();
    const Theme& t = Svc().CurrentTheme();
    auto entry = [&](const wchar_t* glyph, const std::string& label, const std::string& detail,
                     const std::string& target) {
        StackPanel row;
        row.Orientation(Orientation::Horizontal);
        row.Spacing(16);
        row.Padding(Thickness{8, 10, 8, 10});
        row.Children().Append(kit::Glyph(glyph, 26));
        row.Children().Append(kit::Text(label, 24, true));
        if (!detail.empty()) {
            auto d = kit::Text(detail, 18, false, t.colors.text_muted);
            d.VerticalAlignment(VerticalAlignment::Center);
            row.Children().Append(d);
        }
        row.Tag(box_value(kit::H(target)));
        list.Items().Append(row);
    };

    if (dir.empty()) {
        PathText().Text(L"Drives");
        const auto drives = RemovableDriveRoots();
        for (const auto& d : drives) entry(kit::glyph::Usb, d, "External drive", d + "/");
        if (drives.empty())
            entry(kit::glyph::Warning, "No USB drive found", "Plug one in, then press Up to refresh", "");
        entry(kit::glyph::Folder, "Console storage (LocalState)", "Inside the app's own folder",
              Paths().local_state);
    } else {
        PathText().Text(kit::H(dir));
        int files = 0;
        for (const auto& e : Svc().Fs().List(dir)) {
            if (e.is_dir) entry(kit::glyph::Folder, e.name, "", JoinPath(dir, e.name));
            else if (kind_ == FolderKind::NesRoms ? IsNesRomExtension(Extension(e.name))
                     : (kind_ != FolderKind::Roms || n3ds::IsRomExtension(Extension(e.name)))) ++files;
        }
        if (files) {
            StackPanel info;
            info.Children().Append(kit::Text(std::to_string(files) +
                                                 (kind_ == FolderKind::Roms || kind_ == FolderKind::NesRoms
                                                      ? " games in this folder"
                                                      : " files in this folder"),
                                             18, false, t.colors.text_muted));
            list.Items().Append(info);
        }
    }
    if (list.Items().Size()) {
        list.SelectedIndex(-1);
        list.Focus(FocusState::Programmatic);
    }
}

void FolderPage::Choose() {
    if (current_.empty()) {
        Svc().Toast("Open a drive or folder first");
        return;
    }
    auto& cfg = Svc().Config();
    std::string dir = NormalizeSlashes(current_);
    while (dir.size() > 3 && dir.back() == '/') dir.pop_back();
    cfg.folders.Set(kind_, dir);
    Svc().SaveSettings();
    EmulatorSession::Get().UpdateFolders(cfg.folders);
    if (kind_ == FolderKind::Themes) Svc().ReloadThemes();
    if (kind_ == FolderKind::Music) {
        Svc().StopMusic(false);
        Svc().StartMusic();
    }
    if (kind_ == FolderKind::Roms || kind_ == FolderKind::UpdatesDlc || kind_ == FolderKind::NesRoms)
        Svc().RescanLibrary(nullptr);
    Svc().Toast(std::string(FolderKindLabel(kind_)) + ": " + dir);
    Frame().GoBack();
}

} // namespace winrt::ONYX3DS::implementation

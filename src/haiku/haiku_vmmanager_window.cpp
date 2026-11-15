/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the VM manager's window
 *          (qt_vmmanager_mainwindow.cpp, qt_vmmanager_main.cpp,
 *          qt_vmmanager_model.cpp and qt_vmmanager_listviewdelegate.cpp):
 *          the tool bar, the searchable list of machines, the details
 *          panel and the status bar.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <Entry.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ListItem.h>
#include <ListView.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <Node.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <SplitView.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <thread>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "haiku_dialogs.hpp"
#include "haiku_icons.hpp"
#include "haiku_tooldialogs.hpp"
#include "haiku_vmmanager.hpp"
#include "haiku_vmmanager_config.hpp"
#include "haiku_vmmanager_system.hpp"
#include "haiku_vmmanager_ui.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/config.h>
#include <86box/version.h>
}

/* ------------------------------------------------------------ file helpers */

static bool
path_exists(const std::string &path)
{
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

static bool
make_path(const std::string &path)
{
    if (path.empty())
        return false;
    std::string partial;
    size_t      start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        partial            = path.substr(0, (slash == std::string::npos) ? path.size() : slash);
        if (!partial.empty() && !path_exists(partial) && (mkdir(partial.c_str(), 0755) != 0) && (errno != EEXIST))
            return false;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    struct stat st;
    return (stat(path.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
}

/* QDir::removeRecursively(): true if nothing is left. */
static bool
remove_tree(const std::string &path)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path.c_str()) == 0;

    bool ok     = true;
    DIR *handle = opendir(path.c_str());
    if (handle != nullptr) {
        struct dirent *entry;
        while ((entry = readdir(handle)) != nullptr) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            ok = remove_tree(path + "/" + entry->d_name) && ok;
        }
        closedir(handle);
    }
    return (rmdir(path.c_str()) == 0) && ok;
}

static bool
copy_file(const std::string &source, const std::string &target)
{
    std::ifstream in(source, std::ios::binary);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!in.is_open() || !out.is_open())
        return false;
    out << in.rdbuf();
    return out.good() || in.eof();
}

/* copyPath() of qt_vmmanager_main.cpp */
static bool
copy_tree(const std::string &source, const std::string &target)
{
    if (!make_path(target))
        return false;
    DIR *handle = opendir(source.c_str());
    if (handle == nullptr)
        return false;

    bool           ok = true;
    struct dirent *entry;
    while ((entry = readdir(handle)) != nullptr) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        const std::string from = source + "/" + entry->d_name;
        const std::string to   = target + "/" + entry->d_name;
        struct stat       st;
        if (stat(from.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            ok = copy_tree(from, to) && ok;
        else if (S_ISREG(st.st_mode))
            ok = copy_file(from, to) && ok;
    }
    closedir(handle);
    return ok;
}

/* Moves a folder to the Trash of its volume, remembering where it was so
   Tracker can restore it. */
static bool
move_to_trash(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;
    char trash[B_PATH_NAME_LENGTH];
    if (find_directory(B_TRASH_DIRECTORY, st.st_dev, true, trash, sizeof(trash)) != B_OK)
        return false;

    const size_t      slash  = path.rfind('/');
    const std::string name   = (slash == std::string::npos) ? path : path.substr(slash + 1);
    std::string       target = std::string(trash) + "/" + name;
    for (int i = 1; path_exists(target); i++)
        target = std::string(trash) + "/" + name + " " + std::to_string(i);
    if (rename(path.c_str(), target.c_str()) != 0)
        return false;

    BNode node(target.c_str());
    if (node.InitCheck() == B_OK)
        node.WriteAttr("_trk/original_path", B_STRING_TYPE, 0, path.c_str(), path.size() + 1);
    return true;
}

static void
alert(const char *title, const std::string &text, alert_type type = B_STOP_ALERT)
{
    BAlert *box = new BAlert(title, text.c_str(), "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, type);
    box->SetFlags(box->Flags() | B_CLOSE_ON_ESCAPE);
    box->Go();
}

static bool
ask(const char *title, const std::string &text)
{
    BAlert *box = new BAlert(title, text.c_str(), "No", "Yes", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    box->SetShortcut(0, B_ESCAPE);
    return box->Go() == 1;
}

static std::string
lower(std::string text)
{
    for (auto &c : text)
        c = (char) tolower((unsigned char) c);
    return text;
}

/* ------------------------------------------------------------- list item */

/* A row of the machine list (VMManagerListViewDelegate): icon, name, and
   the process status with its icon. */
class VmmListItem : public BListItem {
public:
    explicit VmmListItem(VmmSystem *system)
        : system(system)
        , name_height(0)
    {
    }

    void Update(BView *owner, const BFont *font) override
    {
        BListItem::Update(owner, font);
        font_height fh;
        font->GetHeight(&fh);
        name_height = ceilf(fh.ascent + fh.descent);
        BFont small(*font);
        small.SetSize(font->Size() * 0.9f);
        small.GetHeight(&fh);
        const float status_height = std::max(16.0f, ceilf(fh.ascent + fh.descent));
        SetHeight(MARGIN_TOP + std::max(ICON_SIZE, name_height + SPACING_V + status_height) + MARGIN_BOTTOM);
    }

    void DrawItem(BView *owner, BRect frame, bool) override
    {
        const rgb_color background = ui_color(B_LIST_BACKGROUND_COLOR);
        const rgb_color selected   = ui_color(B_LIST_SELECTED_BACKGROUND_COLOR);
        const rgb_color text       = ui_color(IsSelected() ? B_LIST_SELECTED_ITEM_TEXT_COLOR : B_LIST_ITEM_TEXT_COLOR);

        owner->SetHighColor(background);
        owner->FillRect(frame);
        if (IsSelected()) {
            owner->SetHighColor(selected);
            owner->FillRect(BRect(frame.left, frame.top, frame.right, frame.bottom - 2));
        }
        owner->SetHighColor(tint_color(background, B_DARKEN_2_TINT));
        owner->StrokeLine(BPoint(frame.left + MARGIN_LEFT, frame.bottom), BPoint(frame.right, frame.bottom));

        owner->SetDrawingMode(B_OP_ALPHA);
        owner->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        const BRect icon_rect(frame.left + MARGIN_LEFT, frame.top + MARGIN_TOP, frame.left + MARGIN_LEFT + ICON_SIZE - 1,
                              frame.top + MARGIN_TOP + ICON_SIZE - 1);
        if (BBitmap *custom = vmm_machine_icon(system->icon))
            owner->DrawBitmap(custom, custom->Bounds(), icon_rect, B_FILTER_BITMAP_BILINEAR);
        else if (BBitmap *icon = haiku_icon("86Box-gray"))
            owner->DrawBitmap(icon, icon->Bounds(), icon_rect);

        const char *status_icon = nullptr;
        switch (system->status) {
            case VmmStatus::Running:
            case VmmStatus::RunningWaiting:
                status_icon = "run";
                break;
            case VmmStatus::Stopped:
                status_icon = "acpi_shutdown";
                break;
            case VmmStatus::Paused:
            case VmmStatus::PausedWaiting:
                status_icon = "pause";
                break;
            default:
                break;
        }
        const float x = frame.left + MARGIN_LEFT + ICON_SIZE + SPACING_H;
        const float status_top = frame.top + MARGIN_TOP + name_height + SPACING_V;
        if (BBitmap *icon = (status_icon != nullptr) ? haiku_icon(status_icon) : nullptr)
            owner->DrawBitmap(icon, BPoint(x, status_top));
        owner->SetDrawingMode(B_OP_OVER);

        BFont font;
        owner->GetFont(&font);
        font_height fh;
        font.GetHeight(&fh);
        owner->SetHighColor(text);
        BString name(system->display_name.c_str());
        font.TruncateString(&name, B_TRUNCATE_END, frame.right - x - 4);
        owner->DrawString(name.String(), BPoint(x, frame.top + MARGIN_TOP + ceilf(fh.ascent)));

        BFont small(font);
        small.SetSize(font.Size() * 0.9f);
        small.GetHeight(&fh);
        owner->SetFont(&small);
        owner->SetHighColor(IsSelected() ? text : tint_color(text, B_LIGHTEN_1_TINT));
        const float baseline = status_top + floorf((16 - (fh.ascent + fh.descent)) / 2 + fh.ascent);
        owner->DrawString(system->StatusString().c_str(), BPoint(x + 16 + 4, baseline));
        owner->SetFont(&font);
        owner->SetDrawingMode(B_OP_COPY);
    }

    VmmSystem *system;

private:
    static constexpr float ICON_SIZE     = 32;
    static constexpr float MARGIN_LEFT   = 4;
    static constexpr float MARGIN_TOP    = 10;
    static constexpr float MARGIN_BOTTOM = 12;
    static constexpr float SPACING_H     = 8;
    static constexpr float SPACING_V     = 4;

    float name_height;
};

/* The list: the folder as tool tip, a context menu on the secondary button. */
class VmmListView : public BListView {
public:
    VmmListView()
        : BListView("machines", B_SINGLE_SELECTION_LIST)
    {
    }

    bool GetToolTipAt(BPoint point, BToolTip **_tip) override
    {
        const int32  index = IndexOf(point);
        VmmListItem *item  = dynamic_cast<VmmListItem *>(ItemAt(index));
        if (item == nullptr)
            return false;
        SetToolTip(item->system->shortened_dir.c_str());
        *_tip = ToolTip();
        return *_tip != nullptr;
    }

    void MouseDown(BPoint where) override
    {
        int32 buttons = 0;
        if (Window()->CurrentMessage() != nullptr)
            Window()->CurrentMessage()->FindInt32("buttons", &buttons);
        if (buttons & B_SECONDARY_MOUSE_BUTTON) {
            const int32 index = IndexOf(where);
            if (index >= 0)
                Select(index);
            BMessage message(MSG_VMM_CONTEXT);
            message.AddPoint("where", ConvertToScreen(where));
            message.AddBool("item", index >= 0);
            Window()->PostMessage(&message);
            return;
        }
        BListView::MouseDown(where);
    }
};

/* --------------------------------------------------------------- window */

class VmmWindow : public BWindow {
public:
    VmmWindow();
    ~VmmWindow() override;

    bool QuitRequested() override;
    void MessageReceived(BMessage *message) override;
    void MenusBeginning() override;

private:
    struct Pending {
        VmmSystem  *system;
        std::string name;
        std::string display_name;
        std::string dir;
    };

    void BuildMenus();
    void FillMachineMenu(BMenu *menu, bool popup);
    void LoadSettings();
    void SaveSettings();

    void LoadFromCache(const std::string &select_name);
    void StartDiscovery();
    void Discovered(BMessage *message);
    void Reload();

    void AddSystem(VmmSystem *system, bool select);
    void RemoveSystem(VmmSystem *system);
    VmmSystem *Find(int32 id) const;
    VmmSystem *Selected() const;
    void SelectSystem(VmmSystem *system);
    void RebuildList();
    bool Matches(const VmmSystem *system) const;
    void InvalidateSystem(VmmSystem *system);

    void SelectionChanged();
    void StateChanged();
    void UpdateStatusRight();
    int  ActiveMachineCount() const;

    void ClientMessage(BMessage *message);
    void ProcessExited(BMessage *message);
    void PendingExited(size_t index, int32 code, bool crashed);

    void Start(VmmSystem *system, bool settings);
    void NewMachine();
    void AddNewSystem(const std::string &name, const std::string &dir, const std::string &display_name,
                      const std::string &configuration);
    void Rename(VmmSystem *system);
    void ChooseIcon(VmmSystem *system);
    void Clone(VmmSystem *system);
    void Kill(VmmSystem *system);
    void WipeNvram(VmmSystem *system);
    void Delete(VmmSystem *system);
    void ShowPreferences(bool manager);
    void UpdateGlobalSettings();
    void PreferencesUpdated();
    void Search(const std::string &text);

    std::vector<VmmSystem *> systems;
    std::vector<Pending>     pending;
    VmmSystem               *selected;

    BMenuBar       *menu_bar;
    BMenu          *machine_menu;
    BMenuItem      *hide_toolbar_item;
    BGroupView     *toolbar;
    VmmToolButton  *start_pause_button;
    VmmToolButton  *reset_button;
    VmmToolButton  *stop_button;
    VmmToolButton  *cad_button;
    VmmToolButton  *settings_button;
    BTextControl   *search;
    VmmListView    *list;
    BSplitView     *split;
    VmmDetailsView *details;
    BStringView    *status_left;
    BStringView    *status_right;
    BFilePanel     *icon_panel;

    bool        toolbar_hidden;
    bool        quitting;
    bool        regex_search;
    std::string filter_text;
    std::regex  filter_regex;
    bool        filter_is_regex;
};

static BMenuItem *
add_item(BMenu *menu, const char *label, uint32 what, char shortcut = 0, bool enabled = true)
{
    BMenuItem *item = new BMenuItem(label, new BMessage(what), shortcut);
    item->SetEnabled(enabled);
    menu->AddItem(item);
    return item;
}

VmmWindow::VmmWindow()
    : BWindow(BRect(100, 100, 999, 699), EMU_NAME " VM Manager", B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS)
    , selected(nullptr)
    , icon_panel(nullptr)
    , toolbar_hidden(false)
    , quitting(false)
    , regex_search(vmm_regex_search != 0)
    , filter_is_regex(false)
{
    BuildMenus();

    const char *new_icon = (haiku_icon("new_vm") != nullptr) ? "new_vm" : "new";
    start_pause_button   = new VmmToolButton("start_pause", "run", "Start", "Start", new BMessage(MSG_VMM_START_PAUSE), true);
    reset_button         = new VmmToolButton("reset", "hard_reset", "Hard reset", "Hard reset", new BMessage(MSG_VMM_RESET), true);
    stop_button          = new VmmToolButton("stop", "acpi_shutdown", "Force shutdown", "Force shutdown", new BMessage(MSG_VMM_FORCE_SHUTDOWN), true);
    cad_button           = new VmmToolButton("cad", "send_cad", "Ctrl+Alt+Del", "Ctrl+Alt+Del", new BMessage(MSG_VMM_CAD), true);
    settings_button      = new VmmToolButton("settings", "settings", "Settings", "Settings…", new BMessage(MSG_VMM_SETTINGS), true);

    toolbar = new BGroupView(B_VERTICAL, 0);
    BLayoutBuilder::Group<>(toolbar)
        .AddGroup(B_HORIZONTAL, 2)
            .SetInsets(B_USE_SMALL_SPACING, 2, B_USE_SMALL_SPACING, 2)
            .Add(new VmmToolButton("new", new_icon, "New machine", "New machine…", new BMessage(MSG_VMM_NEW), true))
            .Add(new BSeparatorView(B_VERTICAL))
            .Add(start_pause_button)
            .Add(reset_button)
            .Add(stop_button)
            .Add(cad_button)
            .Add(settings_button)
            .AddGlue()
        .End()
        .Add(new BSeparatorView(B_HORIZONTAL));

    search = new BTextControl("search", "Search:", "", nullptr);
    search->SetModificationMessage(new BMessage(MSG_VMM_SEARCH));

    list = new VmmListView();
    list->SetSelectionMessage(new BMessage(MSG_VMM_SELECTION));
    list->SetInvocationMessage(new BMessage(MSG_VMM_INVOKE));
    BScrollView *list_scroll = new BScrollView("machines_scroll", list, 0, false, true);
    list_scroll->SetExplicitMinSize(BSize(220, 200));

    details = new VmmDetailsView();

    BGroupView *left = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
    BLayoutBuilder::Group<>(left)
        .SetInsets(B_USE_SMALL_SPACING, B_USE_SMALL_SPACING, 0, B_USE_SMALL_SPACING)
        .Add(search)
        .Add(list_scroll);

    split = new BSplitView(B_HORIZONTAL, B_USE_SMALL_SPACING);
    split->AddChild(left, 1.0f);
    split->AddChild(details, 2.5f);
    split->SetCollapsible(false);

    status_left  = new BStringView("status_left", "");
    status_right = new BStringView("status_right", "");
    status_left->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    status_right->SetAlignment(B_ALIGN_RIGHT);
    status_right->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .Add(menu_bar)
        .Add(toolbar)
        .Add(split, 1.0f)
        .Add(new BSeparatorView(B_HORIZONTAL))
        .AddGroup(B_HORIZONTAL)
            .SetInsets(B_USE_SMALL_SPACING, 2, B_USE_SMALL_SPACING, 2)
            .Add(status_left, 1.0f)
            .Add(status_right, 1.0f)
        .End();

    LoadSettings();
    StateChanged();
    UpdateStatusRight();
}

VmmWindow::~VmmWindow()
{
    delete icon_panel;
    for (VmmSystem *system : systems)
        delete system;
    for (auto &entry : pending)
        delete entry.system;
}

void
VmmWindow::BuildMenus()
{
    menu_bar = new BMenuBar("menu_bar");

    BMenu *file = new BMenu("File");
    add_item(file, "New machine…", MSG_VMM_NEW, 'N');
    file->AddSeparatorItem();
    add_item(file, "Exit", B_QUIT_REQUESTED, 'Q');
    menu_bar->AddItem(file);

    machine_menu = new BMenu("Machine");
    FillMachineMenu(machine_menu, false);
    menu_bar->AddItem(machine_menu);

    BMenu *view       = new BMenu("View");
    hide_toolbar_item = add_item(view, "Hide toolbar", MSG_VMM_HIDE_TOOLBAR);
    menu_bar->AddItem(view);

    BMenu *tools = new BMenu("Tools");
    add_item(tools, "Preferences…", MSG_VMM_GENERAL_PREFS, ',');
    add_item(tools, "VM manager preferences…", MSG_VMM_PREFERENCES);
    menu_bar->AddItem(tools);

    BMenu *help = new BMenu("Help");
    add_item(help, "Documentation…", MSG_VMM_DOCS);
    add_item(help, "About 86Box…", MSG_VMM_ABOUT);
    menu_bar->AddItem(help);
}

/* The context menu of qt_vmmanager_main.cpp, also the Machine menu. */
void
VmmWindow::FillMachineMenu(BMenu *menu, bool popup)
{
    while (BMenuItem *item = menu->RemoveItem((int32) 0))
        delete item;

    VmmSystem *system   = Selected();
    const bool valid    = system != nullptr;
    const bool running  = valid && system->IsRunning();
    const bool obscured = valid && system->window_obscured;

    if (!popup || !running)
        add_item(menu, "Start", MSG_VMM_START, 0, valid && !running);
    if (!popup || running)
        add_item(menu, (running && (system->status != VmmStatus::Running)) ? "Resume" : "Pause", MSG_VMM_PAUSE, 0, running);
    add_item(menu, "Hard reset", MSG_VMM_RESET, 0, running);
    add_item(menu, "Force shutdown", MSG_VMM_FORCE_SHUTDOWN, 0, running);
    add_item(menu, "Ctrl+Alt+Del", MSG_VMM_CAD, 0, running);
    menu->AddSeparatorItem();
    add_item(menu, "Settings…", MSG_VMM_SETTINGS, 0, valid);
    add_item(menu, "Change display name…", MSG_VMM_RENAME, 0, valid && !obscured);
    add_item(menu, "Set icon…", MSG_VMM_SET_ICON, 0, valid && !obscured);
    add_item(menu, "Reset icon", MSG_VMM_RESET_ICON, 0, valid && !obscured && !system->icon.empty());
    menu->AddSeparatorItem();
    add_item(menu, "Clone…", MSG_VMM_CLONE, 0, valid);
    add_item(menu, "Kill", MSG_VMM_KILL, 0, running);
    add_item(menu, "Wipe NVRAM", MSG_VMM_WIPE_NVRAM, 0, valid && !running);
    add_item(menu, "Delete", MSG_VMM_DELETE, 0, valid && !running);
    menu->AddSeparatorItem();
    add_item(menu, "Open folder…", MSG_VMM_OPEN_FOLDER, 0, valid);
    add_item(menu, "Open printer tray…", MSG_VMM_OPEN_PRINTER, 0, valid);
    add_item(menu, "Open screenshots folder…", MSG_VMM_OPEN_SCREENSHOTS, 0, valid);
    add_item(menu, "Show config file", MSG_VMM_SHOW_CONFIG, 0, valid);
    menu->SetTargetForItems(this);
}

void
VmmWindow::MenusBeginning()
{
    FillMachineMenu(machine_menu, false);
    hide_toolbar_item->SetMarked(toolbar_hidden);
    BWindow::MenusBeginning();
}

/* The window part of VMManagerMainWindow's constructor. */
void
VmmWindow::LoadSettings()
{
    const VmmConfig config;
    toolbar_hidden = atoi(config.GetString("hide_tool_bar").c_str()) != 0;
    if (toolbar_hidden)
        toolbar->Hide();

    if (!vmm_window_remember) {
        config.SetString("window_coordinates", "");
        config.SetString("window_maximized", "");
        config.SetString("window_splitter", "");
        return;
    }

    int x, y, w, h;
    if (sscanf(config.GetString("window_coordinates").c_str(), "%d , %d , %d , %d", &x, &y, &w, &h) == 4) {
        MoveTo(x, y);
        ResizeTo(std::max(w, 300) - 1, std::max(h, 200) - 1);
    }
    int a, b;
    if ((sscanf(config.GetString("window_splitter").c_str(), "%d , %d", &a, &b) == 2) && (a > 0) && (b > 0)) {
        split->SetItemWeight(0, (float) a, false);
        split->SetItemWeight(1, (float) b, true);
    }
}

/* VMManagerMainWindow::saveSettings() */
void
VmmWindow::SaveSettings()
{
    const VmmConfig config;
    config.SetString("last_selection", (selected != nullptr) ? selected->config_name : std::string());
    config.SetString("hide_tool_bar", toolbar_hidden ? "1" : "0");
    if (vmm_window_remember) {
        const BRect frame = Frame();
        char        text[96];
        snprintf(text, sizeof(text), "%d, %d, %d, %d", (int) frame.left, (int) frame.top, (int) frame.Width() + 1,
                 (int) frame.Height() + 1);
        config.SetString("window_coordinates", text);
        config.SetString("window_maximized", "");
        const BView *left  = split->ChildAt(0);
        const BView *right = split->ChildAt(1);
        if ((left != nullptr) && (right != nullptr)) {
            snprintf(text, sizeof(text), "%d, %d", (int) left->Frame().Width() + 1, (int) right->Frame().Width() + 1);
            config.SetString("window_splitter", text);
        }
    } else {
        config.SetString("window_coordinates", "");
        config.SetString("window_maximized", "");
        config.SetString("window_splitter", "");
    }
}

bool
VmmWindow::QuitRequested()
{
    if (quitting)
        return true;

    const int running = ActiveMachineCount();
    if (running > 0) {
        const std::string text = std::to_string(running)
            + ((running == 1) ? " machine is" : " machines are")
            + " currently active. Are you sure you want to exit the VM manager anyway?";
        if (!ask(EMU_NAME " VM Manager", text))
            return false;
    }

    details->SaveNotes();
    SaveSettings();
    quitting = true;
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
}

/* ------------------------------------------------------------- the model */

VmmSystem *
VmmWindow::Find(int32 id) const
{
    for (VmmSystem *system : systems) {
        if (system->id == id)
            return system;
    }
    return nullptr;
}

/* currentSelectionIsValid() */
VmmSystem *
VmmWindow::Selected() const
{
    return ((selected != nullptr) && selected->IsValid()) ? selected : nullptr;
}

int
VmmWindow::ActiveMachineCount() const
{
    int running = 0;
    for (const VmmSystem *system : systems) {
        if (system->status != VmmStatus::Stopped)
            running++;
    }
    return running;
}

bool
VmmWindow::Matches(const VmmSystem *system) const
{
    if (filter_text.empty())
        return true;
    for (const auto &term : system->search_terms) {
        if (filter_is_regex) {
            if (std::regex_search(term, filter_regex))
                return true;
        } else if (lower(term).find(filter_text) != std::string::npos)
            return true;
    }
    return false;
}

/* The list, sorted by name without case, filtered by the search. */
void
VmmWindow::RebuildList()
{
    std::vector<VmmSystem *> shown;
    for (VmmSystem *system : systems) {
        if (Matches(system))
            shown.push_back(system);
    }
    std::stable_sort(shown.begin(), shown.end(), [](const VmmSystem *a, const VmmSystem *b) {
        return strcasecmp(a->display_name.c_str(), b->display_name.c_str()) < 0;
    });

    list->SetSelectionMessage(nullptr);
    while (BListItem *item = list->RemoveItem((int32) 0))
        delete item;
    int32 select = -1;
    for (VmmSystem *system : shown) {
        if (system == selected)
            select = list->CountItems();
        list->AddItem(new VmmListItem(system));
    }
    if (select >= 0) {
        list->Select(select);
        list->ScrollToSelection();
    }
    list->SetSelectionMessage(new BMessage(MSG_VMM_SELECTION));
}

void
VmmWindow::InvalidateSystem(VmmSystem *system)
{
    for (int32 i = 0; i < list->CountItems(); i++) {
        VmmListItem *item = static_cast<VmmListItem *>(list->ItemAt(i));
        if (item->system == system) {
            list->InvalidateItem(i);
            return;
        }
    }
}

/* Makes `system` the current machine at once (also when the search hides
   it); the list's own selection message follows. */
void
VmmWindow::SelectSystem(VmmSystem *system)
{
    for (int32 i = 0; i < list->CountItems(); i++) {
        if (static_cast<VmmListItem *>(list->ItemAt(i))->system == system) {
            list->Select(i);
            list->ScrollToSelection();
            break;
        }
    }
    if (system != selected) {
        details->SaveNotes();
        selected = system;
        details->SetSystem(system);
    }
    StateChanged();
}

/* VMManagerModel::addConfigToModel() */
void
VmmWindow::AddSystem(VmmSystem *system, bool select)
{
    systems.push_back(system);
    RebuildList();
    if (select || (selected == nullptr))
        SelectSystem(system);
    UpdateStatusRight();
}

/* VMManagerModel::removeConfigFromModel() */
void
VmmWindow::RemoveSystem(VmmSystem *system)
{
    const auto it = std::find(systems.begin(), systems.end(), system);
    if (it == systems.end())
        return;

    int32 index = -1;
    for (int32 i = 0; i < list->CountItems(); i++) {
        if (static_cast<VmmListItem *>(list->ItemAt(i))->system == system)
            index = i;
    }
    systems.erase(it);
    if (selected == system) {
        selected = nullptr;
        details->Reset();
    }
    RebuildList();
    delete system;

    if ((selected == nullptr) && (list->CountItems() > 0)) {
        const int32 next = std::min(std::max(index, (int32) 0), list->CountItems() - 1);
        SelectSystem(static_cast<VmmListItem *>(list->ItemAt(next))->system);
    }
    StateChanged();
    UpdateStatusRight();
}

/* VMManagerModel::loadFromCache() */
void
VmmWindow::LoadFromCache(const std::string &select_name)
{
    std::set<std::string> seen;
    for (VmmSystem *system : systems)
        seen.insert(VmmSystem::Canonical(system->config_file));

    for (const auto &path : VmmSystem::CachedConfigPaths()) {
        const std::string key = VmmSystem::Canonical(path);
        if (seen.count(key))
            continue;
        VmmSystem *system = new VmmSystem(path, BMessenger(this));
        if (!system->IsValid()) {
            delete system;
            continue;
        }
        seen.insert(key);
        AddSystem(system, !select_name.empty() && (system->config_name == select_name));
    }
}

/* VMManagerModel::startBackgroundDiscovery(): the scan runs on a thread,
   the machines are made here. */
void
VmmWindow::StartDiscovery()
{
    const std::string root = vmm_path;
    const BMessenger  messenger(this);
    std::thread([root, messenger]() {
        if (!root.empty() && !path_exists(root))
            make_path(root);
        BMessage message(MSG_VMM_DISCOVERED);
        message.AddString("root", root.c_str());
        for (const auto &path : VmmSystem::ScanForConfigs(root))
            message.AddString("path", path.c_str());
        BMessenger(messenger).SendMessage(&message);
    }).detach();
}

void
VmmWindow::Discovered(BMessage *message)
{
    const char *root = nullptr;
    if ((message->FindString("root", &root) != B_OK) || strcmp(root, vmm_path) != 0)
        return;

    std::set<std::string> existing;
    for (VmmSystem *system : systems)
        existing.insert(VmmSystem::Canonical(system->config_file));

    const char *path;
    for (int32 i = 0; message->FindString("path", i, &path) == B_OK; i++) {
        const std::string key = VmmSystem::Canonical(path);
        if (key.empty() || existing.count(key))
            continue;
        VmmSystem *system = new VmmSystem(path, BMessenger(this));
        if (system->IsValid()) {
            existing.insert(key);
            AddSystem(system, false);
        } else
            delete system;
    }

    /* Remove machines whose configuration file is gone. */
    std::vector<VmmSystem *> gone;
    for (VmmSystem *system : systems) {
        if (!path_exists(system->config_file) && !system->IsRunning())
            gone.push_back(system);
    }
    for (VmmSystem *system : gone)
        RemoveSystem(system);
}

/* VMManagerMain::reload(): the machine folder changed. */
void
VmmWindow::Reload()
{
    details->SaveNotes();
    selected = nullptr;
    details->Reset();
    while (BListItem *item = list->RemoveItem((int32) 0))
        delete item;
    for (VmmSystem *system : systems)
        delete system;
    systems.clear();

    const VmmConfig config;
    LoadFromCache(config.GetString("last_selection"));
    StartDiscovery();
    StateChanged();
    UpdateStatusRight();
}

/* --------------------------------------------------------------- status */

void
VmmWindow::SelectionChanged()
{
    const int32  index = list->CurrentSelection();
    VmmListItem *item  = dynamic_cast<VmmListItem *>(list->ItemAt(index));
    if ((item == nullptr) || (item->system == selected))
        return;

    details->SaveNotes();
    selected = item->system;
    details->SetSystem(selected);
    StateChanged();
}

/* VMManagerMainWindow::vmmStateChanged() */
void
VmmWindow::StateChanged()
{
    VmmSystem *system = Selected();
    if (system == nullptr) {
        start_pause_button->SetEnabled(false);
        settings_button->SetEnabled(false);
        reset_button->SetEnabled(false);
        stop_button->SetEnabled(false);
        cad_button->SetEnabled(false);
        return;
    }

    const bool running = system->IsRunning();
    if (running && (system->status == VmmStatus::Running)) {
        start_pause_button->SetIcon("pause");
        start_pause_button->SetLabelAndTip("Pause", "Pause");
    } else {
        start_pause_button->SetIcon("run");
        start_pause_button->SetLabelAndTip(running ? "Continue" : "Start", running ? "Continue" : "Start");
    }

    start_pause_button->SetEnabled(!system->window_obscured);
    settings_button->SetEnabled(!system->window_obscured);
    reset_button->SetEnabled(!system->window_obscured && running);
    stop_button->SetEnabled(!system->window_obscured && running);
    cad_button->SetEnabled(!system->window_obscured && running);
}

/* VMManagerMain::modelDataChange() and machineCountString() */
void
VmmWindow::UpdateStatusRight()
{
    int running = 0;
    int paused  = 0;
    int waiting = 0;
    for (const VmmSystem *system : systems) {
        switch (system->status) {
            case VmmStatus::Running:
                running++;
                break;
            case VmmStatus::Paused:
                paused++;
                break;
            case VmmStatus::PausedWaiting:
            case VmmStatus::RunningWaiting:
                waiting++;
                break;
            default:
                break;
        }
    }

    std::string states;
    if (running > 0)
        states += std::to_string(running) + " running, ";
    if (paused > 0)
        states += std::to_string(paused) + " paused, ";
    if (waiting > 0)
        states += std::to_string(waiting) + " waiting, ";
    states += std::to_string(systems.size()) + " total";
    status_right->SetText(("VMs: " + states).c_str());
}

void
VmmWindow::ClientMessage(BMessage *message)
{
    const int32 id   = message->GetInt32("vm", 0);
    const char *json = message->GetString("json", "");

    VmmSystem *system = Find(id);
    if (system == nullptr) {
        for (auto &entry : pending) {
            if (entry.system->id == id)
                entry.system->HandleClientMessage(json);
        }
        return;
    }

    const uint32 changed = system->HandleClientMessage(json);
    if (changed & (VmmSystem::CHANGED_STATUS | VmmSystem::CHANGED_CONFIG)) {
        InvalidateSystem(system);
        if (system == selected) {
            if (changed & VmmSystem::CHANGED_CONFIG)
                details->UpdateConfig();
            details->UpdateProcessStatus();
            StateChanged();
        }
        UpdateStatusRight();
    }
    if (changed & VmmSystem::CHANGED_GLOBAL)
        UpdateGlobalSettings();
}

/* QProcess::finished in launch86Box() and addNewSystem(). */
void
VmmWindow::ProcessExited(BMessage *message)
{
    const int32 id      = message->GetInt32("vm", 0);
    const int32 pid     = message->GetInt32("pid", -1);
    const int32 code    = message->GetInt32("code", 0);
    const bool  crashed = message->GetBool("crashed", false);

    for (size_t i = 0; i < pending.size(); i++) {
        if ((pending[i].system->id == id) && pending[i].system->ProcessExited(pid)) {
            PendingExited(i, code, crashed);
            return;
        }
    }

    VmmSystem *system = Find(id);
    if ((system == nullptr) || !system->ProcessExited(pid))
        return;

    const bool killed = system->killed;
    system->killed    = false;
    InvalidateSystem(system);
    if (system == selected) {
        details->UpdateProcessStatus();
        StateChanged();
    }
    UpdateStatusRight();

    if (!killed && (crashed || (code != 0))) {
        char number[48];
        snprintf(number, sizeof(number), "%d (0x%x)", (int) code, (unsigned) code);
        alert("Virtual machine crash", "The virtual machine \"" + system->display_name
                                           + "\"'s process has unexpectedly terminated with exit code " + number + ".");
        return;
    }

    /* configurationChangeReceived() */
    system->ReloadConfig();
    InvalidateSystem(system);
    if (system == selected)
        details->UpdateConfig();
}

void
VmmWindow::PendingExited(size_t index, int32 code, bool crashed)
{
    Pending entry = pending[index];
    pending.erase(pending.begin() + index);

    bool fail = false;
    if (crashed || (code != 0)) {
        const std::string shown = !entry.display_name.empty() ? entry.display_name : entry.name;
        alert("Error adding system", "The virtual machine \"" + shown + "\"'s process has unexpectedly terminated with exit code "
                                         + std::to_string(code) + ".\n\nThe system will not be added.");
        fail = true;
    }
    if (!path_exists(entry.system->config_file)) {
        /* No configuration: the settings were cancelled. */
        if (rmdir(entry.dir.c_str()) != 0)
            pclog("VMM: could not remove %s\n", entry.dir.c_str());
        fail = true;
    }
    const std::string path = entry.system->config_file;
    delete entry.system;
    if (fail)
        return;

    VmmSystem *added = new VmmSystem(path, BMessenger(this));
    if (!added->IsValid()) {
        delete added;
        return;
    }
    if (!entry.display_name.empty())
        added->SetDisplayName(entry.display_name);
    AddSystem(added, true);
    StartDiscovery();
}

/* -------------------------------------------------------------- actions */

void
VmmWindow::Start(VmmSystem *system, bool settings)
{
    if (system == nullptr)
        return;
    std::string error;
    if (!system->Launch(settings, error)) {
        alert(EMU_NAME " VM Manager", error);
        return;
    }
    InvalidateSystem(system);
    if (system == selected) {
        details->UpdateProcessStatus();
        StateChanged();
    }
    UpdateStatusRight();
}

/* VMManagerMain::newMachineWizard() */
void
VmmWindow::NewMachine()
{
    VmmNewMachine result;
    if (vmm_add_machine_wizard(this, result))
        AddNewSystem(result.name, vmm_path, result.display_name, result.configuration);
}

/* VMManagerMain::addNewSystem() */
void
VmmWindow::AddNewSystem(const std::string &name, const std::string &dir, const std::string &display_name,
                        const std::string &configuration)
{
    std::string base = dir;
    while ((base.size() > 1) && (base[base.size() - 1] == '/'))
        base.erase(base.size() - 1);
    const std::string directory   = base + "/" + name;
    const std::string config_path = directory + "/" CONFIG_FILE;

    if (path_exists(config_path) || path_exists(directory)) {
        alert("Directory in use", "The selected directory is already in use. Please select a different directory.");
        return;
    }
    if (mkdir(directory.c_str(), 0755) != 0) {
        alert("Create directory failed", "Unable to create the directory for the new system");
        return;
    }
    if (!configuration.empty()) {
        std::ofstream file(config_path, std::ios::trunc);
        if (!file.is_open()) {
            alert("Configuration write failed", "Unable to open the configuration file at " + config_path + " for writing");
            return;
        }
        file << configuration;
    }

    VmmSystem  *system = new VmmSystem(config_path, BMessenger(this));
    std::string error;
    if (!system->Launch(true, error)) {
        delete system;
        if (configuration.empty())
            rmdir(directory.c_str());
        alert("Error adding system", error);
        return;
    }
    pending.push_back({ system, name, display_name, directory });
}

/* VMManagerMain::updateDisplayName() */
void
VmmWindow::Rename(VmmSystem *system)
{
    std::string name = system->display_name;
    if (!vmm_display_name_dialog(this, name))
        return;
    system->SetDisplayName(name);
    RebuildList();
    if (system == selected)
        details->SetSystem(system);
}

void
VmmWindow::ChooseIcon(VmmSystem *system)
{
    if (icon_panel == nullptr) {
        BMessenger messenger(this);
        icon_panel = new BFilePanel(B_OPEN_PANEL, &messenger, nullptr, B_FILE_NODE, false, nullptr, nullptr, true, true);
        icon_panel->Window()->SetTitle("Select an icon");
    }
    BMessage message(MSG_VMM_ICON_CHOSEN);
    message.AddInt32("vm", system->id);
    icon_panel->SetMessage(&message);
    icon_panel->Show();
}

/* The "Clone" action of the context menu. */
struct CloneJob {
    std::string source;
    std::string target;
};

static bool
clone_work(void *data)
{
    const CloneJob *job = static_cast<CloneJob *>(data);
    remove_tree(job->target);
    return copy_tree(job->source, job->target);
}

void
VmmWindow::Clone(VmmSystem *system)
{
    std::string name;
    if (!vmm_clone_dialog(this, system, name))
        return;

    const std::string target = VmmSystem::Canonical(vmm_path) + "/" + name;
    if (!make_path(target)) {
        alert("Clone", "Failed to create directory for cloned VM");
        return;
    }

    CloneJob job { system->config_dir, target };
    if (!vmm_run_with_progress(this, "Clone", "Cloning the virtual machine…", clone_work, &job)) {
        remove_tree(target);
        alert("Clone", "Failed to clone VM.");
        return;
    }

    const std::string config_path = target + "/" CONFIG_FILE;
    if (!path_exists(config_path)) {
        remove_tree(target);
        alert("Clone", "Failed to clone VM.");
        return;
    }
    VmmSystem *added = new VmmSystem(config_path, BMessenger(this));
    if (added->IsValid()) {
        added->SetDisplayName(name);
        AddSystem(added, true);
    } else
        delete added;
    StartDiscovery();
}

void
VmmWindow::Kill(VmmSystem *system)
{
    if (ask("Warning", "Killing a virtual machine can cause data loss. Only do this if the 86Box process gets stuck.\n\n"
                       "Do you really wish to kill the virtual machine \"" + system->display_name + "\"?"))
        system->Kill();
}

void
VmmWindow::WipeNvram(VmmSystem *system)
{
    if (!ask("Warning", "This will delete all NVRAM (and related) files of the virtual machine located in the \"nvr\" "
                        "subdirectory. You'll have to reconfigure the BIOS (and possibly other devices inside the VM) "
                        "settings again if applicable.\n\nAre you sure you want to wipe all NVRAM contents of the "
                        "virtual machine \"" + system->display_name + "\"?"))
        return;
    if (remove_tree(system->config_dir + "/nvr"))
        alert("Success", "Successfully wiped the NVRAM contents of the virtual machine \"" + system->display_name + "\"",
              B_INFO_ALERT);
    else
        alert("Error", "An error occurred trying to wipe the NVRAM contents of the virtual machine \"" + system->display_name + "\"");
}

/* VMManagerMain::deleteSystem() */
void
VmmWindow::Delete(VmmSystem *system)
{
    std::string text = "Do you really want to delete the virtual machine \"" + system->display_name + "\" and all its files?";
    if (!vmm_delete_to_trash)
        text += " This action cannot be undone!";
    if (!ask("Warning", text))
        return;

    const bool removed = vmm_delete_to_trash ? move_to_trash(system->config_dir) : remove_tree(system->config_dir);
    if (!removed) {
        alert("Remove directory failed", "Some files in the machine's directory were unable to be deleted. Please delete them manually.");
        return;
    }
    system->RemoveFromCache();
    RemoveSystem(system);
}

/* VMManagerMainWindow::preferencesTriggered() */
void
VmmWindow::ShowPreferences(bool manager)
{
    const bool machines_running = ActiveMachineCount() > 0;
    /* Another instance may have changed the preferences since. */
    config_load_global();
    const std::string old_path = vmm_path_cfg;

    bool accepted = false;
    if (manager) {
        accepted = vmm_preferences_dialog(this, machines_running);
        if (accepted)
            config_save_global();
    } else
        haiku_preferences_dialog(this);

    /* Applied even when cancelled, as they were reloaded. */
    PreferencesUpdated();

    if (accepted && !machines_running && (old_path != vmm_path_cfg)) {
        pclog("VMM: machine path changed from %s to %s\n", old_path.c_str(), vmm_path_cfg);
        strncpy(vmm_path, vmm_path_cfg, sizeof(vmm_path) - 1);
        vmm_path[sizeof(vmm_path) - 1] = '\0';
        Reload();
    }
}

/* VMManagerMainWindow::updateSettings() */
void
VmmWindow::UpdateGlobalSettings()
{
    config_load_global();
    PreferencesUpdated();
}

/* VMManagerMain::onPreferencesUpdated() */
void
VmmWindow::PreferencesUpdated()
{
    const bool old_regex = regex_search;
    regex_search         = vmm_regex_search != 0;
    if (old_regex != regex_search)
        search->SetText("");

    for (VmmSystem *system : systems) {
        if (system->status != VmmStatus::Stopped)
            system->SendGlobalConfigurationChanged();
    }
}

/* VMManagerMain::searchSystems() */
void
VmmWindow::Search(const std::string &text)
{
    if (regex_search) {
        try {
            filter_regex    = std::regex(text, std::regex::ECMAScript | std::regex::icase);
            filter_is_regex = true;
        } catch (const std::regex_error &) {
            /* Skipped, as in Qt: an invalid expression. */
            return;
        }
    } else
        filter_is_regex = false;
    filter_text = filter_is_regex ? text : lower(text);
    RebuildList();
}

/* -------------------------------------------------------------- messages */

void
VmmWindow::MessageReceived(BMessage *message)
{
    VmmSystem *system = Selected();

    switch (message->what) {
        case MSG_VMM_LOAD:
            {
                const VmmConfig config;
                LoadFromCache(config.GetString("last_selection"));
                if ((selected == nullptr) && (list->CountItems() > 0))
                    SelectSystem(static_cast<VmmListItem *>(list->ItemAt(0))->system);
                StartDiscovery();
                break;
            }
        case MSG_VMM_DISCOVERED:
            Discovered(message);
            break;
        case MSG_VMM_CLIENT:
            ClientMessage(message);
            break;
        case MSG_VMM_EXITED:
            ProcessExited(message);
            break;

        case MSG_VMM_SELECTION:
            SelectionChanged();
            break;
        case MSG_VMM_INVOKE:
        case MSG_VMM_START:
            /* Brings a running machine to the front. */
            Start(system, false);
            break;
        case MSG_VMM_START_PAUSE:
            if (system == nullptr)
                break;
            if (system->IsRunning())
                system->Pause();
            else
                Start(system, false);
            break;
        case MSG_VMM_PAUSE:
            if (system != nullptr)
                system->Pause();
            break;
        case MSG_VMM_RESET:
            if (system != nullptr)
                system->Reset();
            break;
        case MSG_VMM_FORCE_SHUTDOWN:
            if (system != nullptr)
                system->ForceShutdown();
            break;
        case MSG_VMM_CAD:
            if (system != nullptr)
                system->CtrlAltDel();
            break;
        case MSG_VMM_SETTINGS:
            Start(system, true);
            break;
        case MSG_VMM_NEW:
            NewMachine();
            break;
        case MSG_VMM_RENAME:
            if (system != nullptr)
                Rename(system);
            break;
        case MSG_VMM_SET_ICON:
            if (system != nullptr)
                ChooseIcon(system);
            break;
        case MSG_VMM_RESET_ICON:
            if (system != nullptr) {
                system->SetIcon("");
                InvalidateSystem(system);
            }
            break;
        case MSG_VMM_ICON_CHOSEN:
            {
                VmmSystem *target = Find(message->GetInt32("vm", 0));
                entry_ref  ref;
                if ((target != nullptr) && (message->FindRef("refs", &ref) == B_OK)) {
                    BPath path(&ref);
                    if (path.Path() != nullptr) {
                        if (vmm_machine_icon(path.Path()) == nullptr)
                            alert("Select an icon", std::string("The file is not an image: ") + path.Path());
                        else {
                            target->SetIcon(path.Path());
                            InvalidateSystem(target);
                        }
                    }
                }
                break;
            }
        case MSG_VMM_CLONE:
            if (system != nullptr)
                Clone(system);
            break;
        case MSG_VMM_KILL:
            if ((system != nullptr) && system->IsRunning())
                Kill(system);
            break;
        case MSG_VMM_WIPE_NVRAM:
            if ((system != nullptr) && !system->IsRunning())
                WipeNvram(system);
            break;
        case MSG_VMM_DELETE:
            if ((system != nullptr) && !system->IsRunning())
                Delete(system);
            break;
        case MSG_VMM_OPEN_FOLDER:
            if (system != nullptr)
                haiku_open_folder(system->config_dir.c_str());
            break;
        case MSG_VMM_OPEN_PRINTER:
            if (system != nullptr)
                haiku_open_folder((system->config_dir + "/printer").c_str());
            break;
        case MSG_VMM_OPEN_SCREENSHOTS:
            if (system != nullptr)
                haiku_open_folder((system->config_dir + "/screenshots").c_str());
            break;
        case MSG_VMM_SHOW_CONFIG:
            if (system != nullptr)
                vmm_show_text_file(this, system->display_name, VmmSystem::Canonical(system->config_file));
            break;
        case MSG_VMM_CONTEXT:
            {
                BPoint where;
                if (message->FindPoint("where", &where) != B_OK)
                    break;
                BPopUpMenu *menu = new BPopUpMenu("context", false, false);
                if (message->GetBool("item", false))
                    FillMachineMenu(menu, true);
                else {
                    add_item(menu, "New machine…", MSG_VMM_NEW);
                    menu->SetTargetForItems(this);
                }
                menu->SetAsyncAutoDestruct(true);
                menu->Go(where, true, false, true);
                break;
            }

        case MSG_VMM_SHOT_NEXT:
            details->NextScreenshot();
            break;
        case MSG_VMM_SHOT_PREVIOUS:
            details->PreviousScreenshot();
            break;
        case MSG_VMM_NOTES:
            details->SaveNotes();
            break;
        case MSG_VMM_SEARCH:
            Search(search->Text());
            break;

        case MSG_VMM_HIDE_TOOLBAR:
            toolbar_hidden = !toolbar_hidden;
            if (toolbar_hidden)
                toolbar->Hide();
            else
                toolbar->Show();
            VmmConfig().SetString("hide_tool_bar", toolbar_hidden ? "1" : "0");
            break;
        case MSG_VMM_PREFERENCES:
            ShowPreferences(true);
            break;
        case MSG_VMM_GENERAL_PREFS:
            ShowPreferences(false);
            break;
        case MSG_VMM_DOCS:
            haiku_open_url(EMU_DOCS_URL);
            break;
        case MSG_VMM_ABOUT:
            haiku_about_dialog(this);
            break;

        default:
            BWindow::MessageReceived(message);
            break;
    }
}

/* ------------------------------------------------------------ interface */

BWindow *
haiku_vmmanager_create_window(void)
{
    VmmConfig::MigratePreferences();

    VmmWindow *window = new VmmWindow();
    window->Show();
    /* The machines are loaded once the window is up, as in Qt. */
    window->PostMessage(MSG_VMM_LOAD);
    return window;
}

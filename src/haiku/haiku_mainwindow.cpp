/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the main window.
 *
 *          The menus, tool bar and status bar follow the Qt main window
 *          (qt_mainwindow.ui / qt_mainwindow.cpp); the handlers do what the
 *          Qt ones do. Left out, as there is nothing behind them here: the
 *          renderer choice and its options (one software renderer), the
 *          on-screen display, HiDPI scaling and non-primary monitors.
 *
 *          Accelerators (acc_keys[], the same table and defaults as Qt)
 *          work as in Qt: as shortcuts while the mouse is free, only
 *          "release mouse" while it is captured, and all of them in
 *          fullscreen.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Alert.h>
#include <Application.h>
#include <InterfaceDefs.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <Path.h>
#include <Screen.h>
#include <StringView.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <string>
#include <unordered_map>

#include "haiku_dialogs.hpp"
#include "haiku_frontend.hpp"
#include "haiku_keyboardgrab.hpp"
#include "haiku_machinestatus.hpp"
#include "haiku_mainwindow.hpp"
#include "haiku_monitorwindow.hpp"
#include "haiku_iconmenuitem.hpp"
#include "haiku_icons.hpp"
#include "haiku_osd.hpp"

#ifdef MTR_ENABLED
#    include <minitrace/minitrace.h>
/* Whether a Chrome trace is being recorded (Tools -> Begin trace). */
static bool tracing = false;
#endif
#include "haiku_mediamenu.hpp"
#include "haiku_memoryviewer.hpp"
#include "haiku_renderers.hpp"
#include "haiku_rendererview.hpp"
#include "haiku_settings.hpp"
#include "haiku_settings_common.hpp"
#include "haiku_toolbar.hpp"
#include "haiku_tooldialogs.hpp"
#include "haiku_vmmanager.hpp"

/* Haiku raw keycode -> XT scancode table shared with the Qt frontend. */
#include "be_keyboard.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/nvr.h>
#include <86box/acpi.h>
#include <86box/config.h>
#include <86box/cpu_events.h>
#include <86box/keyboard.h>
#include <86box/machine.h>
#include <86box/midi.h>
#include <86box/mouse.h>
#include <86box/plat.h>
#include <86box/renderdefs.h>
#include <86box/runctl.h>
#include <86box/sound.h>
#include <86box/ui.h>
#include <86box/version.h>
#include <86box/video.h>
#ifdef USE_RTMIDI
#    include <86box/midi_rtmidi.h>
#endif

extern void nmi_raise(void);
extern int  vid_resize;
}

MainWindow *main_window              = nullptr;
bool        MainWindow::app_quitting = false;

/* Messages only the main window's menus send. */
enum : uint32 {
    MSG_FULLSCREEN          = 'mwfs',
    MSG_FAST_FORWARD        = 'mwff',
    MSG_FAST_FORWARD_START  = 'mwfx',
    MSG_MOUSE_EVERY_FRAME   = 'mwmf',
    MSG_INPUT_DEVICE        = 'mwid', /* int32 "mode" */
    MSG_TABLET_TOOL         = 'mwtt', /* int32 "tool" */
    MSG_HIDE_TOOL_BAR       = 'mwht',
    MSG_RESIZABLE           = 'mwrs',
    MSG_REMEMBER_SIZE       = 'mwrm',
    MSG_SECOND_MONITORS     = 'mwnp',
    MSG_TOGGLE_OSD          = 'mwod',
    MSG_BEGIN_TRACE         = 'mwtb',
    MSG_END_TRACE           = 'mwte',
    MSG_SPECIFY_DIMENSIONS  = 'mwsd',
    MSG_FORCE_43            = 'mw43',
    MSG_FORCE_DEVICE_ASPECT = 'mwda',
    MSG_STRETCH_MODE        = 'mwst', /* int32 "mode" */
    MSG_STRETCH_MAXIMIZED   = 'mwsm',
    MSG_UPDATE_ICONS        = 'mwui',
    MSG_SCREENSHOT_KIND     = 'mwss', /* int32 "kind" */
    MSG_COPY_SCREENSHOT     = 'mwcp',
    MSG_MIDI_OUT            = 'mwmo', /* int32 "device" */
    MSG_MIDI_RESTART        = 'mwmr',
    MSG_MIDI_COMMAND        = 'mwmc', /* int32 "command" */
    MSG_OPEN_SCREENSHOTS    = 'mwos',
    MSG_OPEN_PRINTER_TRAY   = 'mwop',
    MSG_DOCUMENTATION       = 'mwdc',
    MSG_PREFERENCES         = 'mwpf',
    MSG_MCA_DEVICES         = 'mwmd',
    MSG_CGA_SETTINGS        = 'mwcg',
    MSG_MEMORY_VIEWER       = 'mwmv',
    MSG_RENDERER            = 'mwrn', /* int32 "api" */
    MSG_RENDERER_OPTIONS    = 'mwro',
    MSG_GL_STRETCH          = 'mwgs', /* int32 "mode" */
    MSG_GL_SCALE            = 'mwgc', /* int32 "scale" (x2) */
};

enum {
    SHOT_NORMAL,
    SHOT_RAW,
    SHOT_COPY,
    SHOT_COPY_RAW,
};

/* Window scale factors, indexed by the `scale` setting (as in Qt). */
static const double scale_factors[] = { 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0 };
static const char  *scale_names[]   = { "0.5x", "1x", "1.5x", "2x", "3x", "4x", "5x", "6x", "7x", "8x" };

/* Raw keycodes of the modifier keys. Haiku reports them only through
   B_MODIFIERS_CHANGED, so their state is read from get_key_info(). */
static const uint8_t modifier_keys[] = {
    0x4b, /* left shift */
    0x56, /* right shift */
    0x5c, /* left control */
    0x60, /* right control */
    0x5d, /* left alt */
    0x5f, /* right alt */
    0x66, /* left windows */
    0x67, /* right windows */
};

static inline bool
key_is_down(const uint8_t *states, uint8_t key)
{
    return (states[key >> 3] & (1 << (7 - (key & 7)))) != 0;
}

static BMenuItem *
add_item(BMenu *menu, const char *label, uint32 what, char shortcut = 0)
{
    BMenuItem *item = new BMenuItem(label, new BMessage(what), shortcut);
    menu->AddItem(item);
    return item;
}

/* An item with one of Qt's menu icons (by its .ico name). */
static IconMenuItem *
add_icon_item(BMenu *menu, const char *label, uint32 what, const char *icon)
{
    IconMenuItem *item = new IconMenuItem(label, new BMessage(what), haiku_icon(icon));
    menu->AddItem(item);
    return item;
}

static IconMenuItem *
add_icon_item_int(BMenu *menu, const char *label, uint32 what, const char *field, int32 value, const char *icon)
{
    BMessage *msg = new BMessage(what);
    msg->AddInt32(field, value);
    IconMenuItem *item = new IconMenuItem(label, msg, haiku_icon(icon));
    menu->AddItem(item);
    return item;
}

static BMenuItem *
add_item_int(BMenu *menu, const char *label, uint32 what, const char *field, int32 value)
{
    BMessage *msg = new BMessage(what);
    msg->AddInt32(field, value);
    BMenuItem *item = new BMenuItem(label, msg);
    menu->AddItem(item);
    return item;
}

/* Marks the item whose message carries `value` in `field`. */
static void
mark_by_value(BMenu *menu, const char *field, int32 value)
{
    for (int32 i = 0; i < menu->CountItems(); i++) {
        BMenuItem *item = menu->ItemAt(i);
        if (item->Message() != nullptr)
            item->SetMarked(item->Message()->GetInt32(field, -1) == value);
    }
}

/* ---------------------------------------------------------- accelerators */

/* The XT scancode for a key name in a Qt key sequence ("F12", "PgUp", "G"). */
static uint16_t
accelerator_key_code(const char *name)
{
    static const char    letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const uint8_t letter_codes[] = { 0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                            0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c };
    static const struct {
        const char *name;
        uint16_t    code;
    } named[] = {
        { "Esc", 0x01 },     { "Escape", 0x01 }, { "Tab", 0x0f },    { "Backspace", 0x0e }, { "Return", 0x1c },
        { "Enter", 0x1c },   { "Space", 0x39 },  { "Ins", 0x152 },   { "Insert", 0x152 },   { "Del", 0x153 },
        { "Delete", 0x153 }, { "Home", 0x147 },  { "End", 0x14f },   { "PgUp", 0x149 },     { "PgDown", 0x151 },
        { "Up", 0x148 },     { "Down", 0x150 },  { "Left", 0x14b },  { "Right", 0x14d },    { "Pause", 0x145 },
        { "Print", 0x137 },  { "ScrollLock", 0x46 },
    };

    if ((name[0] != '\0') && (name[1] == '\0')) {
        const char c = name[0];
        if (const char *p = strchr(letters, (c >= 'a' && c <= 'z') ? (c - 32) : c))
            return letter_codes[p - letters];
        if (c >= '1' && c <= '9')
            return 0x02 + (c - '1');
        if (c == '0')
            return 0x0b;
    }
    if ((name[0] == 'F') && (name[1] >= '1') && (name[1] <= '9')) {
        const int n = atoi(name + 1);
        if (n >= 1 && n <= 10)
            return 0x3b + (n - 1);
        if (n == 11)
            return 0x57;
        if (n == 12)
            return 0x58;
    }
    for (const auto &entry : named) {
        if (!strcasecmp(entry.name, name))
            return entry.code;
    }
    return 0;
}

/* Whether the key just pressed (XT code), with the modifiers now held down,
   is the accelerator's sequence. */
bool
MainWindow::MatchAccelerator(const char *name, uint16_t code) const
{
    const int id = FindAccelerator(name);
    if ((id < 0) || (acc_keys[id].seq[0] == '\0'))
        return false;

    bool     want_ctrl  = false;
    bool     want_shift = false;
    bool     want_alt   = false;
    uint16_t want_key   = 0;

    std::string seq = acc_keys[id].seq;
    size_t      pos = 0;
    while (pos <= seq.size()) {
        size_t end = seq.find('+', pos);
        /* A '+' key itself shows up as an empty part; it isn't bindable. */
        std::string part = seq.substr(pos, (end == std::string::npos) ? std::string::npos : end - pos);
        if (part == "Ctrl")
            want_ctrl = true;
        else if (part == "Shift")
            want_shift = true;
        else if (part == "Alt")
            want_alt = true;
        else if (!part.empty())
            want_key = accelerator_key_code(part.c_str());
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }

    if ((want_key == 0) || (want_key != code))
        return false;

    key_info info;
    if (get_key_info(&info) != B_OK)
        return false;
    const bool ctrl  = key_is_down(info.key_states, 0x5c) || key_is_down(info.key_states, 0x60);
    const bool shift = key_is_down(info.key_states, 0x4b) || key_is_down(info.key_states, 0x56);
    const bool alt   = key_is_down(info.key_states, 0x5d) || key_is_down(info.key_states, 0x5f);

    return (ctrl == want_ctrl) && (shift == want_shift) && (alt == want_alt);
}

/* Runs the accelerator for a key press, if it is one. Returns true when
   the key was used. */
bool
MainWindow::HandleAccelerators(uint16_t code)
{
    static const struct {
        const char *name;
        uint32      what;
        int32       kind;
    } actions[] = {
        { "screenshot", MSG_SCREENSHOT_KIND, SHOT_NORMAL },
        { "raw_screenshot", MSG_SCREENSHOT_KIND, SHOT_RAW },
        { "copy_screenshot", MSG_SCREENSHOT_KIND, SHOT_COPY },
        { "copy_raw_screenshot", MSG_SCREENSHOT_KIND, SHOT_COPY_RAW },
        { "fullscreen", MSG_FULLSCREEN, 0 },
        { "hard_reset", MSG_HARD_RESET, 0 },
        { "fast_forward", MSG_FAST_FORWARD, 0 },
        { "send_ctrl_alt_del", MSG_CTRL_ALT_DEL, 0 },
        { "send_ctrl_alt_esc", MSG_CTRL_ALT_ESC, 0 },
        { "nmi", MSG_NMI, 0 },
        { "pause", MSG_PAUSE, 0 },
        { "mute", MSG_MUTE, 0 },
        { "force_interpretation", MSG_FORCE_INTERPRETER, 0 },
        { "toggle_osd", MSG_TOGGLE_OSD, 0 },
        { "exit", B_QUIT_REQUESTED, 0 },
    };

    if (video_fullscreen && MatchAccelerator("toggle_ui_fullscreen", code)) {
        ToggleFullscreenUI();
        return true;
    }

    for (const auto &action : actions) {
        if (MatchAccelerator(action.name, code)) {
            BMessage msg(action.what);
            msg.AddInt32("kind", action.kind);
            PostMessage(&msg);
            return true;
        }
    }
    return false;
}

/* --------------------------------------------------------------- window */

MainWindow::MainWindow()
    : BWindow(BRect(100, 100, 739, 579), EMU_NAME, B_TITLED_WINDOW,
              B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS)
    , media(nullptr)
    , settings_window(nullptr)
    , emu_w(640)
    , emu_h(480)
    , fullscreen_ui(false)
    , zoomed(false)
    , dialog_depth(0)
    , dialog_had_capture(false)
{
    main_window = this;
    memset(key_states, 0, sizeof(key_states));

    media = new MediaMenu(this);
    BuildMenus();

    tool_bar = new ToolBarView(BRect(0, menu_height, Bounds().Width(), menu_height + ToolBarView::PreferredHeight() - 1));
    tool_bar->AddButton("pause", "Pause execution", MSG_PAUSE);
    tool_bar->AddButton("fast_forward", "Fast forward", MSG_FAST_FORWARD);
    tool_bar->AddButton("hard_reset", "Hard reset", MSG_HARD_RESET);
    tool_bar->AddButton("acpi_shutdown", "ACPI shutdown", MSG_ACPI_SHUTDOWN);
    tool_bar->AddSeparator();
    tool_bar->AddButton("send_cad", "Send Control+Alt+Delete", MSG_CTRL_ALT_DEL);
    tool_bar->AddButton("send_cae", "Send Control+Alt+Escape", MSG_CTRL_ALT_ESC);
    tool_bar->AddSeparator();
    tool_bar->AddButton("settings", "Settings" B_UTF8_ELLIPSIS, MSG_SETTINGS);
    tool_bar->AddSeparator();
    tool_bar->AddButton("take_screenshot", "Take screenshot", MSG_SCREENSHOT);
    tool_bar->AddButton("copy_screenshot", "Copy screenshot", MSG_COPY_SCREENSHOT);
    AddChild(tool_bar);
    tool_height = ToolBarView::PreferredHeight();

    font_height fh;
    be_plain_font->GetHeight(&fh);
    status_height = ceilf(fh.ascent + fh.descent + fh.leading) + 8;

    renderer = new RendererView(BRect(0, menu_height + tool_height, 639, menu_height + tool_height + 479));
    AddChild(renderer);

    status_icons = new MachineStatusView(BRect(0, 0, 10, 10));
    AddChild(status_icons);
    status_locks = new MachineStatusView(BRect(0, 0, 10, 10), true);
    AddChild(status_locks);

    status_text = new BStringView(BRect(0, 0, 10, 10), "status_text", "", B_FOLLOW_LEFT | B_FOLLOW_BOTTOM);
    status_hz   = new BStringView(BRect(0, 0, 10, 10), "status_hz", "", B_FOLLOW_RIGHT | B_FOLLOW_BOTTOM);
    status_hz->SetAlignment(B_ALIGN_RIGHT);
    status_hz->SetToolTip("Refresh rate");
    AddChild(status_text);
    AddChild(status_hz);

    char title[1200];
    snprintf(title, sizeof(title), "%s - %s %s", vm_name, EMU_NAME, EMU_VERSION_FULL);
    SetTitle(title);
    ui_window_title(title);

    if (scale < 0 || scale > 9)
        scale = 1;
    renderer->SetLinearFilter(video_filter_method != 0);

    if (vid_resize == 1)
        SetResizable(true);
    if (window_remember && (window_x || window_y))
        MoveTo(window_x, window_y);
    if ((vid_resize == 1) && window_remember && (window_w > 0) && (window_h > 0))
        ResizeTo(window_w - 1, menu_height + (hide_tool_bar ? 0 : tool_height) + window_h
                 + (hide_status_bar ? 0 : status_height) - 1);
    else if (vid_resize == 2)
        ResizeContents(fixed_size_x, fixed_size_y);
    else
        ResizeContents(640, 480);
    LayoutViews();
    UpdatePauseState();

    video_setblit(haiku_blit);
}

MainWindow::~MainWindow()
{
    delete media;
    main_window = nullptr;
}

/* ---------------------------------------------------------------- menus */

void
MainWindow::BuildMenus()
{
    menu_bar = new BMenuBar(BRect(0, 0, Bounds().Width(), 0), "menu_bar");

    /* Action (Qt's menuAction, in its order) */
    BMenu *action = new BMenu("Action");
    add_item(action, "Keyboard requires capture", MSG_KBD_CAPTURE);
    add_item(action, "Right CTRL is left ALT", MSG_RCTRL_IS_LALT);
    action->AddSeparatorItem();
    add_item(action, "Update mouse every CPU frame", MSG_MOUSE_EVERY_FRAME);
    input_device_menu = new BMenu("Input device");
    input_device_menu->SetRadioMode(true);
    add_item_int(input_device_menu, "Mouse", MSG_INPUT_DEVICE, "mode", 0);
    add_item_int(input_device_menu, "Tablet", MSG_INPUT_DEVICE, "mode", 1);
    add_item_int(input_device_menu, "Tablet (Crosshair)", MSG_INPUT_DEVICE, "mode", 2);
    action->AddItem(input_device_menu);
    tablet_tool_menu = new BMenu("Tablet tool");
    tablet_tool_menu->SetRadioMode(true);
    add_item_int(tablet_tool_menu, "Pen", MSG_TABLET_TOOL, "tool", 1);
    add_item_int(tablet_tool_menu, "Cursor/Puck", MSG_TABLET_TOOL, "tool", 0);
    action->AddItem(tablet_tool_menu);
    action->AddSeparatorItem();
    item_pause = add_icon_item(action, "Pause", MSG_PAUSE, "pause");
    action->AddSeparatorItem();
    add_item(action, "Fullscreen", MSG_FULLSCREEN);
    add_item(action, "Toggle on-screen display", MSG_TOGGLE_OSD);
    action->AddSeparatorItem();
    add_icon_item(action, "Fast forward", MSG_FAST_FORWARD, "fast_forward");
    add_item(action, "Fast forward at startup", MSG_FAST_FORWARD_START);
    item_force_interpreter = add_icon_item(action, "Force interpretation", MSG_FORCE_INTERPRETER, "interpreter");
    action->AddSeparatorItem();
    add_icon_item(action, "Hard reset", MSG_HARD_RESET, "hard_reset");
    add_icon_item(action, "Ctrl+Alt+Del", MSG_CTRL_ALT_DEL, "send_cad");
    action->AddSeparatorItem();
    add_icon_item(action, "Ctrl+Alt+Esc", MSG_CTRL_ALT_ESC, "send_cae");
    add_item(action, "Non-maskable interrupt", MSG_NMI);
    action->AddSeparatorItem();
    add_icon_item(action, "ACPI shutdown", MSG_ACPI_SHUTDOWN, "acpi_shutdown");
    add_item(action, "Exit", B_QUIT_REQUESTED, 'Q');
    menu_bar->AddItem(action);

    /* View (Qt's menuView, in its order; no HiDPI scaling or Vulkan) */
    BMenu *view = new BMenu("View");
    add_item(view, "Hide toolbar", MSG_HIDE_TOOL_BAR);
    add_item(view, "Hide status bar", MSG_HIDE_STATUS_BAR);
    add_item(view, "Show non-primary monitors", MSG_SECOND_MONITORS);
    view->AddSeparatorItem();
    add_item(view, "Resizeable window", MSG_RESIZABLE);
    add_item(view, "Remember size & position", MSG_REMEMBER_SIZE);
    add_item(view, "Specify dimensions" B_UTF8_ELLIPSIS, MSG_SPECIFY_DIMENSIONS);
    add_item(view, "Force 4:3 display ratio", MSG_FORCE_43);
    add_item(view, "Force machine display ratio", MSG_FORCE_DEVICE_ASPECT);
    view->AddSeparatorItem();
    stretch_menu = new BMenu("Fullscreen stretch mode");
    stretch_menu->SetRadioMode(true);
    add_item_int(stretch_menu, "Full screen stretch", MSG_STRETCH_MODE, "mode", FULLSCR_SCALE_FULL);
    add_item_int(stretch_menu, "4:3", MSG_STRETCH_MODE, "mode", FULLSCR_SCALE_43);
    add_item_int(stretch_menu, "Square pixels (Keep ratio)", MSG_STRETCH_MODE, "mode", FULLSCR_SCALE_KEEPRATIO);
    add_item_int(stretch_menu, "Integer scale", MSG_STRETCH_MODE, "mode", FULLSCR_SCALE_INT);
    add_item_int(stretch_menu, "4:3 Integer scale", MSG_STRETCH_MODE, "mode", FULLSCR_SCALE_INT43);
    add_item_int(stretch_menu, "4:3 Integer scale (vertical)", MSG_STRETCH_MODE, "mode", FULLSCR_SCALE_INT43_VERT);
    add_item_int(stretch_menu, "Center", MSG_STRETCH_MODE, "mode", FULLSCR_SCALE_CENTER);
    add_item_int(stretch_menu, "4:3 Center", MSG_STRETCH_MODE, "mode", FULLSCR_SCALE_CENTER43);
    view->AddItem(stretch_menu);
    add_item(view, "Apply fullscreen stretch mode when maximized", MSG_STRETCH_MAXIMIZED);
    add_item(view, "CGA composite settings" B_UTF8_ELLIPSIS, MSG_CGA_SETTINGS);
    view->AddSeparatorItem();
    scale_menu = new BMenu("Window scale factor");
    scale_menu->SetRadioMode(true);
    for (int i = 0; i < 10; i++)
        add_item_int(scale_menu, scale_names[i], MSG_SCALE, "scale", i);
    view->AddItem(scale_menu);
    view->AddSeparatorItem();
    filter_menu = new BMenu("Filter method");
    filter_menu->SetRadioMode(true);
    add_item_int(filter_menu, "Nearest", MSG_FILTER, "filter", 0);
    add_item_int(filter_menu, "Linear", MSG_FILTER, "filter", 1);
    view->AddItem(filter_menu);
    view->AddSeparatorItem();
    renderer_menu = new BMenu("Renderer");
    renderer_menu->SetRadioMode(true);
    for (int i = 0; i < haiku_renderer_count(); i++) {
        const haiku_renderer_choice choice = haiku_renderer_at(i);
        add_item_int(renderer_menu, choice.name, MSG_RENDERER, "api", choice.vid_api);
    }
    view->AddItem(renderer_menu);
    add_item(view, "Renderer options" B_UTF8_ELLIPSIS, MSG_RENDERER_OPTIONS);
    gl_stretch_menu = new BMenu("OpenGL input stretch mode");
    gl_stretch_menu->SetRadioMode(true);
    add_item_int(gl_stretch_menu, "Full screen stretch", MSG_GL_STRETCH, "mode", FULLSCR_SCALE_FULL);
    add_item_int(gl_stretch_menu, "4:3", MSG_GL_STRETCH, "mode", FULLSCR_SCALE_43);
    add_item_int(gl_stretch_menu, "Square pixels (Keep ratio)", MSG_GL_STRETCH, "mode", FULLSCR_SCALE_KEEPRATIO);
    add_item_int(gl_stretch_menu, "Integer scale", MSG_GL_STRETCH, "mode", FULLSCR_SCALE_INT);
    add_item_int(gl_stretch_menu, "4:3 Integer scale", MSG_GL_STRETCH, "mode", FULLSCR_SCALE_INT43);
    view->AddItem(gl_stretch_menu);
    gl_scale_menu = new BMenu("OpenGL input scale");
    gl_scale_menu->SetRadioMode(true);
    /* The value is the scale times two, so 0.5x and 1.5x fit in an int. */
    static const int gl_scales_x2[] = { 1, 2, 3, 4, 6, 8, 10, 12, 14, 16 };
    for (int i = 0; i < 10; i++)
        add_item_int(gl_scale_menu, scale_names[i], MSG_GL_SCALE, "scale", gl_scales_x2[i]);
    view->AddItem(gl_scale_menu);
    menu_bar->AddItem(view);

    /* Media: filled by MediaMenu when the menu bar opens. */
    media_menu = new BMenu("Media");
    menu_bar->AddItem(media_menu);

    /* Tools (Qt's menuTools, in its order) */
    BMenu *tools = new BMenu("Tools");
    add_icon_item(tools, "Settings" B_UTF8_ELLIPSIS, MSG_SETTINGS, "settings");
    add_item(tools, "Update status bar icons", MSG_UPDATE_ICONS);
    tools->AddSeparatorItem();
    add_icon_item_int(tools, "Take screenshot", MSG_SCREENSHOT_KIND, "kind", SHOT_NORMAL, "take_screenshot");
    add_icon_item_int(tools, "Take raw screenshot", MSG_SCREENSHOT_KIND, "kind", SHOT_RAW, "take_raw_screenshot");
    add_icon_item_int(tools, "Copy screenshot", MSG_SCREENSHOT_KIND, "kind", SHOT_COPY, "copy_screenshot");
    add_icon_item_int(tools, "Copy raw screenshot", MSG_SCREENSHOT_KIND, "kind", SHOT_COPY_RAW, "copy_raw_screenshot");
    tools->AddSeparatorItem();
    BMenu *sound = new BMenu("Sound");
    item_mute = add_item(sound, "Mute", MSG_MUTE);
    sound->AddSeparatorItem();
    add_item(sound, "Sound gain" B_UTF8_ELLIPSIS, MSG_SOUND_GAIN);
    sound->AddSeparatorItem();
    midi_menu = new BMenu("MIDI output device");
    midi_menu->SetRadioMode(false);
    sound->AddItem(midi_menu);
    midi_command_menu = new BMenu("Send MIDI command");
    add_item_int(midi_command_menu, "GM System On", MSG_MIDI_COMMAND, "command", MIDI_CMD_GM_ON);
    add_item_int(midi_command_menu, "GS Reset", MSG_MIDI_COMMAND, "command", MIDI_CMD_GS_RESET);
    add_item_int(midi_command_menu, "XG System On", MSG_MIDI_COMMAND, "command", MIDI_CMD_XG_ON);
    add_item_int(midi_command_menu, "MT-32 Reset", MSG_MIDI_COMMAND, "command", MIDI_CMD_MT32_RESET);
    midi_command_menu->AddSeparatorItem();
    add_item_int(midi_command_menu, "All notes off", MSG_MIDI_COMMAND, "command", MIDI_CMD_ALL_NOTES_OFF);
    sound->AddItem(midi_command_menu);
    tools->AddItem(sound);
    tools->AddSeparatorItem();
    add_item(tools, "Preferences" B_UTF8_ELLIPSIS, MSG_PREFERENCES);
    tools->AddSeparatorItem();
#ifdef MTR_ENABLED
    /* Chrome tracing, in MINITRACE builds only (as in Qt). */
    add_item(tools, "Begin trace", MSG_BEGIN_TRACE, 'T');
    add_item(tools, "End trace", MSG_END_TRACE);
    tools->AddSeparatorItem();
#endif
    add_item(tools, "MCA devices" B_UTF8_ELLIPSIS, MSG_MCA_DEVICES);
    add_item(tools, "Memory viewer", MSG_MEMORY_VIEWER);
    tools->AddSeparatorItem();
    add_item(tools, "Open printer tray" B_UTF8_ELLIPSIS, MSG_OPEN_PRINTER_TRAY);
    add_item(tools, "Open screenshots folder" B_UTF8_ELLIPSIS, MSG_OPEN_SCREENSHOTS);
    menu_bar->AddItem(tools);

    /* Help */
    BMenu *help = new BMenu("Help");
    add_item(help, "Documentation" B_UTF8_ELLIPSIS, MSG_DOCUMENTATION);
    add_item(help, "About " EMU_NAME B_UTF8_ELLIPSIS, MSG_ABOUT);
    menu_bar->AddItem(help);

    AddChild(menu_bar);
    menu_height = menu_bar->Frame().Height() + 1;
}

static void
set_marked(BMenu *root, uint32 what, bool marked)
{
    for (int32 i = 0; i < root->CountItems(); i++) {
        BMenuItem *item = root->ItemAt(i);
        if (item->Submenu() != nullptr)
            set_marked(item->Submenu(), what, marked);
        else if ((item->Message() != nullptr) && (item->Message()->what == what))
            item->SetMarked(marked);
    }
}

static void
set_enabled(BMenu *root, uint32 what, bool enabled)
{
    for (int32 i = 0; i < root->CountItems(); i++) {
        BMenuItem *item = root->ItemAt(i);
        if (item->Submenu() != nullptr)
            set_enabled(item->Submenu(), what, enabled);
        else if ((item->Message() != nullptr) && (item->Message()->what == what))
            item->SetEnabled(enabled);
    }
}

void
MainWindow::MenusBeginning()
{
    BMenu *root = menu_bar;

    set_marked(root, MSG_KBD_CAPTURE, kbd_req_capture);
    set_marked(root, MSG_RCTRL_IS_LALT, rctrl_is_lalt);
    set_marked(root, MSG_MOUSE_EVERY_FRAME, force_constant_mouse);
    set_marked(root, MSG_FAST_FORWARD, runctl_mode() == RUN_MODE_FAST);
    set_marked(root, MSG_FAST_FORWARD_START, fast_forward_on_start);
    set_marked(root, MSG_HIDE_TOOL_BAR, hide_tool_bar);
    set_marked(root, MSG_HIDE_STATUS_BAR, hide_status_bar);
    set_marked(root, MSG_SECOND_MONITORS, show_second_monitors);
#ifdef MTR_ENABLED
    set_enabled(root, MSG_BEGIN_TRACE, !tracing);
    set_enabled(root, MSG_END_TRACE, tracing);
#endif
    set_marked(root, MSG_RESIZABLE, vid_resize == 1);
    set_marked(root, MSG_REMEMBER_SIZE, window_remember);
    set_marked(root, MSG_FORCE_43, force_43);
    set_marked(root, MSG_FORCE_DEVICE_ASPECT, force_device_aspect);
    set_marked(root, MSG_STRETCH_MAXIMIZED, video_fullscreen_scale_maximized);
    set_marked(root, MSG_UPDATE_ICONS, update_icons);
    set_enabled(root, MSG_RESIZABLE, vid_resize != 2);
    scale_menu->SetEnabled(vid_resize == 0);

    /* Qt updateUiPauseState() and forceInterpretationCompleted. */
    item_pause->SetLabel((runctl_state() == RUN_PAUSED) ? "Resume" : "Pause");
    item_pause->SetIcon(haiku_icon((runctl_state() == RUN_PAUSED) ? "run" : "pause"));
    item_force_interpreter->SetLabel(cpu_force_interpreter ? "Allow recompilation" : "Force interpretation");
    item_force_interpreter->SetIcon(haiku_icon(cpu_force_interpreter ? "recompiler" : "interpreter"));

    /* As MainWindow::refreshMediaMenu() / updatePowerOffAction() in Qt
       (items Qt hides are disabled here). */
    const bool soft_power = acpi_enabled || device_has_power_button();
    for (int32 i = 0; BMenuItem *item = menu_bar->SubmenuAt(0)->ItemAt(i); i++) {
        if ((item->Message() != nullptr) && (item->Message()->what == MSG_ACPI_SHUTDOWN))
            item->SetLabel(soft_power ? "Power off (soft)" : ((confirm_exit && confirm_exit_cmdl) ? "Power off" B_UTF8_ELLIPSIS : "Power off"));
    }
    set_enabled(root, MSG_MCA_DEVICES, machine_has_bus(machine, MACHINE_BUS_MCA));
    bool composite = false;
    for (int i = 0; i < MONITORS_NUM; i++)
        composite |= monitors[i].mon_composite != 0;
    set_enabled(root, MSG_CGA_SETTINGS, composite);
    set_enabled(root, MSG_FORCE_DEVICE_ASPECT, (monitors[0].mon_device_aspect_x > 0) && (monitors[0].mon_device_aspect_y > 0));
    item_mute->SetLabel(sound_muted ? "Unmute" : "Mute");

    mark_by_value(scale_menu, "scale", scale);
    mark_by_value(filter_menu, "filter", video_filter_method ? 1 : 0);
    mark_by_value(stretch_menu, "mode", video_fullscreen_scale);
    mark_by_value(input_device_menu, "mode", mouse_input_mode);

    /* Renderer and the OpenGL submenus (enabled for OpenGL only, as Qt). */
    const int api = renderer->CurrentRenderer();
    mark_by_value(renderer_menu, "api", api);
    gl_stretch_menu->SetEnabled(api == RENDERER_OPENGL3);
    gl_scale_menu->SetEnabled(api == RENDERER_OPENGL3);
    mark_by_value(gl_stretch_menu, "mode", video_gl_input_scale_mode);
    {
        /* Qt marks the range the value falls in. */
        static const double bounds[] = { 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0 };
        static const int    x2[]     = { 1, 2, 3, 4, 6, 8, 10, 12, 14, 16 };
        int i = 0;
        while ((i < 9) && (video_gl_input_scale >= bounds[i]))
            i++;
        mark_by_value(gl_scale_menu, "scale", x2[i]);
    }
    set_enabled(root, MSG_RENDERER_OPTIONS, haiku_renderer_options_available(api));
    mark_by_value(tablet_tool_menu, "tool", tablet_tool_type);

    RefreshMidiMenu();
    midi_command_menu->SetEnabled(midi_out_device_is_open());

    media->Rebuild(media_menu);
}

/* Like MainWindow::refreshMidiOutMenu() in Qt: the MIDI output devices
   (one entry per host port for System MIDI), then "Restart device". */
void
MainWindow::RefreshMidiMenu()
{
    while (BMenuItem *item = midi_menu->RemoveItem((int32) 0))
        delete item;

    for (int c = 0;; c++) {
        const device_t   *dev  = midi_out_device_getdevice(c);
        const std::string name = settings_device_name(dev, midi_out_device_get_internal_name(c), 0);
        if (name.empty())
            break;
        if (!midi_out_device_available(c))
            continue;

        BMenuItem *item = add_item_int(midi_menu, name.c_str(), MSG_MIDI_OUT, "device", c);
        item->SetMarked(c == midi_output_device_current);
    }

    midi_menu->AddSeparatorItem();
    add_item(midi_menu, "Restart device", MSG_MIDI_RESTART)->SetEnabled(midi_output_device_current > 0);
}

void
MainWindow::SwitchMidiOut(int device)
{
    const bool changed = (device != midi_output_device_current);

    runctl_pause_wait(RUN_PAUSE_SYNC);
    midi_output_device_current = device;
    midi_config_changed();
    if (changed)
        config_save();
    runctl_resume(RUN_PAUSE_SYNC);
}

/* --------------------------------------------------------------- layout */

/* Size the window so the emulated screen is w x h times the scale factor
   (or the fixed size), as Qt's resizeContents() does. */
void
MainWindow::ResizeContents(int w, int h)
{
    emu_w = w;
    emu_h = h;

    if (video_fullscreen || (vid_resize == 1)) {
        renderer->UpdateDestination();
        return;
    }

    float vw;
    float vh;
    if (vid_resize == 2) {
        vw = fixed_size_x;
        vh = fixed_size_y;
    } else {
        const double factor = scale_factors[(scale >= 0 && scale <= 9) ? scale : 1];
        vw                  = roundf(w * factor);
        vh                  = roundf(h * factor);
    }

    const float extra = menu_height + (hide_tool_bar ? 0 : tool_height) + (hide_status_bar ? 0 : status_height);
    ResizeTo(vw - 1, vh + extra - 1);
    LayoutViews();
}

void
MainWindow::LayoutViews()
{
    const BRect bounds  = Bounds();
    const bool  menus   = !video_fullscreen || fullscreen_ui;
    const bool  tools   = menus && !hide_tool_bar;
    const bool  status  = menus && !hide_status_bar;
    float       top     = 0;

    auto show = [](BView *v, bool on) {
        if (on && v->IsHidden())
            v->Show();
        else if (!on && !v->IsHidden())
            v->Hide();
    };

    show(menu_bar, menus);
    if (menus)
        top += menu_height;

    show(tool_bar, tools);
    if (tools) {
        tool_bar->MoveTo(0, top);
        tool_bar->ResizeTo(bounds.Width(), tool_height - 1);
        top += tool_height;
    }

    const float bottom = status ? (bounds.bottom - status_height) : bounds.bottom;
    renderer->MoveTo(0, top);
    renderer->ResizeTo(bounds.Width(), bottom - top);

    show(status_icons, status);
    show(status_text, status);
    show(status_hz, status);
    show(status_locks, status);
    if (status) {
        /* [drive icons][message][refresh rate][lock LEDs], as Qt's status
           bar with the refresh rate and the LEDs as permanent widgets. */
        const float row   = bounds.bottom - status_height + 1;
        const float icons = status_icons->ContentWidth();
        const float locks = status_locks->ContentWidth();
        const float hz_w  = status_hz->StringWidth("000.00 Hz") + 8;
        status_icons->MoveTo(0, row);
        status_icons->ResizeTo(icons > 0 ? icons - 1 : 0, status_height - 1);
        status_locks->MoveTo(bounds.Width() - locks + 1, row);
        status_locks->ResizeTo(locks > 0 ? locks - 1 : 0, status_height - 1);
        status_hz->MoveTo(bounds.Width() - locks - hz_w - 4, row);
        status_hz->ResizeTo(hz_w, status_height - 1);
        status_text->MoveTo(icons + 4, row);
        status_text->ResizeTo(bounds.Width() - icons - locks - hz_w - 12, status_height - 1);
    }
}

void
MainWindow::FrameResized(float width, float height)
{
    BWindow::FrameResized(width, height);
    LayoutViews();

    if ((vid_resize == 1) && !video_fullscreen) {
        window_w = renderer->Bounds().IntegerWidth() + 1;
        window_h = renderer->Bounds().IntegerHeight() + 1;
    }
}

void
MainWindow::FrameMoved(BPoint where)
{
    BWindow::FrameMoved(where);
    if (window_remember && !video_fullscreen) {
        window_x = static_cast<int>(where.x);
        window_y = static_cast<int>(where.y);
    }
}

void
MainWindow::Zoom(BPoint origin, float width, float height)
{
    BWindow::Zoom(origin, width, height);
    zoomed = !zoomed;
    renderer->SetMaximized(zoomed);
}

void
MainWindow::SetResizable(bool resizable)
{
    if (resizable)
        SetFlags(Flags() & ~(B_NOT_RESIZABLE | B_NOT_ZOOMABLE));
    else
        SetFlags(Flags() | B_NOT_RESIZABLE | B_NOT_ZOOMABLE);
}

/* After a setting that changes how the picture is placed (Qt's
   video_toggle_option()). */
void
MainWindow::VideoOptionChanged()
{
    config_save();
    reset_screen_size();
    device_force_redraw();
    for (int i = 0; i < MONITORS_NUM; i++) {
        if (monitors[i].target_buffer)
            video_force_resize_set_monitor(1, i);
    }
    renderer->UpdateDestination();
    haiku_monitors_update();
}

/* Qt MainWindow::on_actionFullscreen_triggered(). */
void
MainWindow::ToggleFullscreen()
{
    if (video_fullscreen) {
        video_fullscreen = 0;
        fullscreen_ui    = false;
        SetLook(B_TITLED_WINDOW_LOOK);
        SetFlags(Flags() & ~B_NOT_MOVABLE);
        MoveTo(windowed_frame.LeftTop());
        ResizeTo(windowed_frame.Width(), windowed_frame.Height());
        LayoutViews();
        if (vid_resize != 1)
            ResizeContents((vid_resize == 2) ? fixed_size_x : monitors[0].mon_scrnsz_x,
                           (vid_resize == 2) ? fixed_size_y : monitors[0].mon_scrnsz_y);
    } else {
        if ((mouse_type != MOUSE_TYPE_NONE) || machine_has_mouse())
            plat_mouse_capture(1);
        windowed_frame   = Frame();
        video_fullscreen = 1;
        fullscreen_ui    = false;
        BScreen screen(this);
        const BRect sf = screen.Frame();
        SetLook(B_NO_BORDER_WINDOW_LOOK);
        SetFlags(Flags() | B_NOT_MOVABLE);
        MoveTo(sf.LeftTop());
        ResizeTo(sf.Width(), sf.Height());
        LayoutViews();
    }
    renderer->UpdateDestination();
    device_force_redraw();
}

/* Qt MainWindow::toggleFullscreenUI(): the menus in fullscreen. */
void
MainWindow::ToggleFullscreenUI()
{
    static bool was_captured = false;

    if (!video_fullscreen)
        return;

    fullscreen_ui = !fullscreen_ui;
    if (fullscreen_ui) {
        was_captured = mouse_capture != 0;
        if (was_captured)
            plat_mouse_capture(0);
    } else if (was_captured)
        plat_mouse_capture(1);

    LayoutViews();
    renderer->UpdateDestination();
}

void
MainWindow::SpecifyDimensions()
{
    int  w    = renderer->Bounds().IntegerWidth() + 1;
    int  h    = renderer->Bounds().IntegerHeight() + 1;
    bool lock = vid_resize == 2;

    BeginDialog();
    const bool accepted = haiku_specify_dimensions_dialog(this, &w, &h, &lock);
    EndDialog();
    if (!accepted)
        return;

    if (lock) {
        vid_resize      = 2;
        window_remember = 0;
        fixed_size_x    = w;
        fixed_size_y    = h;
        SetResizable(false);
        ResizeContents(fixed_size_x, fixed_size_y);
    } else {
        vid_resize      = 1;
        window_remember = 1;
        window_w        = w;
        window_h        = h;
        SetResizable(true);
        ResizeTo(w - 1, menu_height + (hide_tool_bar ? 0 : tool_height) + h + (hide_status_bar ? 0 : status_height) - 1);
        LayoutViews();
    }
    renderer->UpdateDestination();
    config_save();
}

void
MainWindow::TakeScreenshot(int kind)
{
    startblit();
    for (auto &monitor : monitors) {
        switch (kind) {
            case SHOT_NORMAL:
                ++monitor.mon_screenshots;
                break;
            case SHOT_RAW:
                ++monitor.mon_screenshots_raw;
                break;
            case SHOT_COPY:
                ++monitor.mon_screenshots_clipboard;
                break;
            case SHOT_COPY_RAW:
                ++monitor.mon_screenshots_raw_clipboard;
                break;
        }
    }
    endblit();
    device_force_redraw();
}

/* Qt MainWindow::on_actionToggle_OSD_triggered(). */
void
MainWindow::ToggleOsd()
{
    haiku_osd_toggle();
    renderer->OsdChanged();
}

void
MainWindow::BeginDialog()
{
    if (dialog_depth++ > 0)
        return;

    /* Qt closes the OSD when a dialog blocks the window. */
    if (haiku_osd_is_visible())
        ToggleOsd();
    dialog_had_capture = mouse_capture != 0;
    if (dialog_had_capture)
        SetMouseCapture(false);
    if (do_auto_dialog_pause)
        runctl_pause(RUN_PAUSE_DIALOG);
}

void
MainWindow::EndDialog()
{
    if ((dialog_depth == 0) || (--dialog_depth > 0))
        return;

    if (do_auto_dialog_pause)
        runctl_resume(RUN_PAUSE_DIALOG);
    if (dialog_had_capture && IsActive())
        SetMouseCapture(true);
}

void
MainWindow::UpdatePauseState()
{
    const bool paused = runctl_state() == RUN_PAUSED;
    tool_bar->SetButtonIcon(MSG_PAUSE, paused ? "run" : "pause", paused ? "Resume execution" : "Pause execution");
    tool_bar->SetButtonMarked(MSG_FAST_FORWARD, runctl_mode() == RUN_MODE_FAST);

    /* The VM manager shows the state; "waiting" while a dialog is open
       (Qt: VMManagerProtocol::RunningState). */
    static int last_state = -1;
    const int  state      = (paused ? 1 : 0) + ((settings_window != nullptr) ? 2 : 0);
    if (state != last_state) {
        last_state = state;
        haiku_vmm_client_running_state(state);
    }
}

/* -------------------------------------------------------------- actions */

void
MainWindow::HardReset()
{
    if (confirm_reset) {
        BAlert *alert = new BAlert(EMU_NAME, "Are you sure you want to hard reset the emulated machine?",
                                   "No", "Yes", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
        alert->SetShortcut(0, B_ESCAPE);
        BeginDialog();
        const int32 answer = alert->Go();
        EndDialog();
        if (answer != 1)
            return;
    }

    config_changed = 2;
    pc_reset_hard();
}

bool
MainWindow::ConfirmExit()
{
    if (!confirm_exit || !confirm_exit_cmdl)
        return true;

    BAlert *alert = new BAlert(EMU_NAME, "Are you sure you want to exit " EMU_NAME "?",
                               "No", "Yes", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    alert->SetShortcut(0, B_ESCAPE);
    BeginDialog();
    const bool yes = alert->Go() == 1;
    EndDialog();
    return yes;
}

void
MainWindow::AcpiShutdown()
{
    if (acpi_enabled) {
        acpi_pwrbut_press();
        return;
    }

    if (device_has_power_button()) {
        runctl_call_wait([](void *) { device_power_button(); }, nullptr);
        return;
    }

    /* Without a soft-power method, powering off means exiting 86Box. */
    if (confirm_exit && confirm_exit_cmdl) {
        BAlert *alert = new BAlert(EMU_NAME,
                                   "Powering off the emulated machine may cause data loss. "
                                   "Are you sure you want to continue?",
                                   "No", "Yes", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
        alert->SetShortcut(0, B_ESCAPE);
        BeginDialog();
        const int32 answer = alert->Go();
        EndDialog();
        if (answer != 1)
            return;
    }

    app_quitting = true;
    be_app->PostMessage(B_QUIT_REQUESTED);
}

/* Like MainWindow::on_actionSettings_triggered() in Qt: the emulation is
   paused while the window is open. */
void
MainWindow::OpenSettings()
{
    if (settings_window != nullptr) {
        settings_window->Activate();
        return;
    }

    if (mouse_capture)
        plat_mouse_capture(0);

    runctl_pause_wait(RUN_PAUSE_SYNC);
    settings_window = new SettingsWindow(this);
    settings_window->Show();
    UpdatePauseState();
}

/* The window has written the new settings to the core: hard reset the
   machine with them, as Qt does, then resume. */
void
MainWindow::SettingsClosed(bool apply)
{
    settings_window = nullptr;

    if (apply) {
        config_changed = 2;
        pc_reset_hard();
        config_save();
        reset_screen_size();
        device_force_redraw();
        for (int i = 0; i < MONITORS_NUM; i++) {
            if (monitors[i].target_buffer)
                video_force_resize_set_monitor(1, i);
        }
        status_icons->Refresh();
        status_locks->Refresh();
        LayoutViews();
        haiku_vmm_client_configuration_changed();
    }

    runctl_resume(RUN_PAUSE_SYNC);
    UpdatePauseState();
}

void
MainWindow::SetMouseCapture(bool on)
{
    mouse_capture = on ? 1 : 0;
    renderer->SetCapture(on);
    /* While captured, the keys Haiku keeps for itself go to the guest. */
    haiku_keyboard_grab(on);
    ui_emu_status(runctl_speed());
}

/* --------------------------------------------------------------- input */

/* Send the guest the modifier keys that changed since last time. */
void
MainWindow::SyncModifierKeys()
{
    key_info info;
    if (get_key_info(&info) != B_OK)
        return;

    for (uint8_t key : modifier_keys) {
        const bool now  = key_is_down(info.key_states, key);
        const bool then = key_is_down(key_states, key);
        if (now == then)
            continue;

        if (now)
            key_states[key >> 3] |= (1 << (7 - (key & 7)));
        else
            key_states[key >> 3] &= ~(1 << (7 - (key & 7)));

        ProcessKey(key, now);
    }
}

void
MainWindow::ProcessKey(uint8_t key, bool down)
{
    const auto found = be_keycodes.find(key);
    if (found == be_keycodes.end())
        return;

    uint16_t code = found->second;

    if (rctrl_is_lalt && (code == 0x11d))
        code = 0x038;

    if (down && mouse_capture && MatchAccelerator("release_mouse", code)) {
        keyboard_all_up();
        plat_mouse_capture(0);
        return;
    }

    keyboard_input(down, code);

    /* Haiku never sends a release for the Menu key. */
    if (down && (key == 0x68))
        keyboard_input(0, code);
}

/* Returns true when the key was used (by the guest or an accelerator). */
bool
MainWindow::HandleKeyMessage(BMessage *message)
{
    if (message->what == B_MODIFIERS_CHANGED) {
        if (haiku_osd_is_visible())
            return false;
        if (!kbd_req_capture || mouse_capture)
            SyncModifierKeys();
        return false;
    }

    int32 key       = 0;
    int32 modifiers = 0;
    message->FindInt32("key", &key);
    message->FindInt32("modifiers", &modifiers);

    const bool down = (message->what == B_KEY_DOWN) || (message->what == B_UNMAPPED_KEY_DOWN);

    /* While the OSD is open it gets every key, except its own toggle
       accelerator, so that one still closes it (as in Qt). */
    if (haiku_osd_is_visible()) {
        const auto found = be_keycodes.find(static_cast<uint8_t>(key));
        if (down && (found != be_keycodes.end()) && MatchAccelerator("toggle_osd", found->second)
            && (message->GetInt32("be:key_repeat", 0) == 0)) {
            ToggleOsd();
            return true;
        }
        if (haiku_osd_key(message, down))
            return true;
    }

    /* Accelerators: all of them in fullscreen, as shortcuts while the
       mouse is free; while captured only "release mouse" (ProcessKey). */
    if (down && (video_fullscreen || !mouse_capture)) {
        const auto found = be_keycodes.find(static_cast<uint8_t>(key));
        if ((found != be_keycodes.end()) && HandleAccelerators(found->second))
            return true;
    }

    /* Without capture, Command shortcuts belong to the menus, and with
       "Keyboard requires capture" so does everything else. */
    if (!video_fullscreen && !mouse_capture && (kbd_req_capture || (down && (modifiers & B_COMMAND_KEY))))
        return false;

    /* A modifier key arrives here as a plain key when its modifier role is
       unassigned (the Command keys while captured): record it, so the next
       B_MODIFIERS_CHANGED does not send it to the guest a second time. */
    for (uint8_t mod : modifier_keys) {
        if (mod != key)
            continue;
        if (down)
            key_states[mod >> 3] |= (1 << (7 - (mod & 7)));
        else
            key_states[mod >> 3] &= ~(1 << (7 - (mod & 7)));
    }

    ProcessKey(static_cast<uint8_t>(key), down);
    return true;
}

void
MainWindow::DispatchMessage(BMessage *message, BHandler *handler)
{
    /* Keys typed into another window of ours (a file panel, say) are not
       the guest's. */
    switch (message->what) {
        case B_KEY_DOWN:
        case B_KEY_UP:
        case B_UNMAPPED_KEY_DOWN:
        case B_UNMAPPED_KEY_UP:
            if (!dopause && HandleKeyMessage(message))
                return;
            break;

        case B_MODIFIERS_CHANGED:
            HandleKeyMessage(message);
            break;

        default:
            break;
    }

    BWindow::DispatchMessage(message, handler);
}

void
MainWindow::WindowActivated(bool active)
{
    BWindow::WindowActivated(active);

    if (active)
        haiku_vmm_client_window_activated();
    else {
        /* Keys released while another window has the focus never reach us. */
        keyboard_all_up();
        memset(key_states, 0, sizeof(key_states));
        if (mouse_capture)
            plat_mouse_capture(0);
    }
}

/* ------------------------------------------------------------- messages */

void
MainWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_PAUSE:
            runctl_pause_user_toggle();
            break;
        case MSG_FAST_FORWARD:
            runctl_fast_forward_toggle();
            UpdatePauseState();
            break;
        case MSG_FAST_FORWARD_START:
            fast_forward_on_start ^= 1;
            config_save();
            break;
        case MSG_FORCE_INTERPRETER:
            cpu_force_interpreter ^= 1;
            ui_update_force_interpreter();
            break;
        case MSG_HARD_RESET:
            HardReset();
            break;
        case MSG_CTRL_ALT_DEL:
            pc_send_cad();
            break;
        case MSG_CTRL_ALT_ESC:
            pc_send_cae();
            break;
        case MSG_NMI:
            nmi_raise();
            break;
        case MSG_ACPI_SHUTDOWN:
            AcpiShutdown();
            break;
        case MSG_FULLSCREEN:
            ToggleFullscreen();
            break;
        case MSG_KBD_CAPTURE:
            kbd_req_capture ^= 1;
            config_save();
            break;
        case MSG_RCTRL_IS_LALT:
            rctrl_is_lalt ^= 1;
            config_save();
            break;
        case MSG_MOUSE_EVERY_FRAME:
            force_constant_mouse ^= 1;
            mouse_update_sample_rate();
            config_save();
            break;
        case MSG_INPUT_DEVICE:
            mouse_input_mode         = message->GetInt32("mode", 0);
            mouse_input_mode_initial = mouse_input_mode;
            config_save();
            break;
        case MSG_TABLET_TOOL:
            tablet_tool_type = message->GetInt32("tool", 1);
            config_save();
            break;

        case MSG_SCALE:
            scale = message->GetInt32("scale", 1);
            reset_screen_size();
            device_force_redraw();
            for (int i = 0; i < MONITORS_NUM; i++) {
                if (monitors[i].target_buffer)
                    video_force_resize_set_monitor(1, i);
            }
            ResizeContents(emu_w, emu_h);
            config_save();
            break;
        case MSG_FILTER:
            video_filter_method = message->GetInt32("filter", 0);
            renderer->SetLinearFilter(video_filter_method != 0);
            config_save();
            break;
        case MSG_STRETCH_MODE:
            video_fullscreen_scale = message->GetInt32("mode", FULLSCR_SCALE_FULL);
            renderer->UpdateDestination();
            device_force_redraw();
            config_save();
            break;
        case MSG_STRETCH_MAXIMIZED:
            video_fullscreen_scale_maximized ^= 1;
            renderer->UpdateDestination();
            device_force_redraw();
            config_save();
            break;
        case MSG_FORCE_43:
            if (!force_43)
                force_device_aspect = 0;
            startblit();
            force_43 ^= 1;
            endblit();
            VideoOptionChanged();
            break;
        case MSG_FORCE_DEVICE_ASPECT:
            if (!force_device_aspect)
                force_43 = 0;
            startblit();
            force_device_aspect ^= 1;
            endblit();
            VideoOptionChanged();
            break;
        case MSG_HIDE_STATUS_BAR:
            hide_status_bar ^= 1;
            if (vid_resize == 1) {
                LayoutViews();
                renderer->UpdateDestination();
            } else
                ResizeContents(emu_w, emu_h);
            config_save();
            break;
        case MSG_HIDE_TOOL_BAR:
            hide_tool_bar ^= 1;
            if (vid_resize == 1) {
                LayoutViews();
                renderer->UpdateDestination();
            } else
                ResizeContents(emu_w, emu_h);
            ui_emu_status(runctl_speed());
            config_save();
            break;
        case MSG_RESIZABLE:
            vid_resize = (vid_resize == 1) ? 0 : 1;
            SetResizable(vid_resize == 1);
            ResizeContents(monitors[0].mon_scrnsz_x, monitors[0].mon_scrnsz_y);
            renderer->UpdateDestination();
            config_save();
            break;
#ifdef MTR_ENABLED
        case MSG_BEGIN_TRACE:
            if (!tracing) {
                mtr_init("trace.json");
                mtr_start();
                tracing = true;
            }
            break;
        case MSG_END_TRACE:
            if (tracing) {
                mtr_stop();
                mtr_shutdown();
                tracing = false;
            }
            break;
#endif
        case MSG_TOGGLE_OSD:
            ToggleOsd();
            break;
        case MSG_SECOND_MONITORS:
            /* Qt on_actionShow_non_primary_monitors_triggered(). */
            show_second_monitors ^= 1;
            haiku_monitors_show(show_second_monitors);
            config_save();
            break;
        case MSG_REMEMBER_SIZE:
            window_remember ^= 1;
            if (!video_fullscreen) {
                window_w = renderer->Bounds().IntegerWidth() + 1;
                window_h = renderer->Bounds().IntegerHeight() + 1;
                window_x = static_cast<int>(Frame().left);
                window_y = static_cast<int>(Frame().top);
            }
            config_save();
            break;
        case MSG_SPECIFY_DIMENSIONS:
            SpecifyDimensions();
            break;

        case MSG_UPDATE_ICONS:
            update_icons ^= 1;
            config_save();
            break;
        case MSG_SCREENSHOT:
            TakeScreenshot(SHOT_NORMAL);
            break;
        case MSG_COPY_SCREENSHOT:
            TakeScreenshot(SHOT_COPY);
            break;
        case MSG_SCREENSHOT_KIND:
            TakeScreenshot(message->GetInt32("kind", SHOT_NORMAL));
            break;
        case MSG_MUTE:
            sound_muted ^= 1;
            config_save();
            break;
        case MSG_SOUND_GAIN:
            BeginDialog();
            haiku_sound_gain_dialog(this);
            EndDialog();
            break;
        case MSG_MIDI_OUT:
            {
                const int device = message->GetInt32("device", 0);
                if (device != midi_output_device_current)
                    SwitchMidiOut(device);
                break;
            }
        case MSG_MIDI_RESTART:
            SwitchMidiOut(midi_output_device_current);
            break;
        case MSG_MIDI_COMMAND:
            runctl_pause_wait(RUN_PAUSE_SYNC);
            midi_out_send_command(message->GetInt32("command", MIDI_CMD_ALL_NOTES_OFF));
            runctl_resume(RUN_PAUSE_SYNC);
            break;
        case MSG_OPEN_SCREENSHOTS:
        case MSG_OPEN_PRINTER_TRAY:
            {
                char path[1200];
                snprintf(path, sizeof(path), "%s%s%s", usr_path,
                         (usr_path[0] && usr_path[strlen(usr_path) - 1] == '/') ? "" : "/",
                         (message->what == MSG_OPEN_SCREENSHOTS) ? "screenshots" : "printer");
                haiku_open_folder(path);
                break;
            }
        case MSG_DOCUMENTATION:
            haiku_open_url(EMU_DOCS_URL);
            break;
        case MSG_ABOUT:
            BeginDialog();
            haiku_about_dialog(this);
            EndDialog();
            break;
        case MSG_RENDERER:
            vid_api = message->GetInt32("api", vid_api);
            renderer->SwitchRenderer(vid_api);
            haiku_monitors_switch_renderer(vid_api);
            config_save();
            break;
        case MSG_RENDERER_OPTIONS:
            BeginDialog();
            haiku_renderer_options_dialog(this, renderer->CurrentRenderer());
            EndDialog();
            break;
        case MSG_GL_STRETCH:
            video_gl_input_scale_mode = message->GetInt32("mode", FULLSCR_SCALE_FULL);
            renderer->UpdateDestination();
            haiku_monitors_update();
            config_save();
            break;
        case MSG_GL_SCALE:
            video_gl_input_scale = message->GetInt32("scale", 2) / 2.0;
            renderer->UpdateDestination();
            haiku_monitors_update();
            config_save();
            break;
        case MSG_PREFERENCES:
            haiku_preferences_dialog(this);
            break;
        case MSG_MEMORY_VIEWER:
            haiku_memory_viewer_open(this);
            break;
        case MSG_MCA_DEVICES:
            haiku_mca_devices_dialog(this);
            break;
        case MSG_CGA_SETTINGS:
            haiku_cga_settings_dialog(this);
            break;

        case MSG_RESIZE:
            {
                int32 w = 0;
                int32 h = 0;
                if ((message->FindInt32("w", &w) == B_OK) && (message->FindInt32("h", &h) == B_OK)
                    && (w > 0) && (h > 0))
                    ResizeContents(w, h);
                break;
            }
        case MSG_EMU_STATUS:
            {
                const char *text = nullptr;
                if (message->FindString("text", &text) == B_OK)
                    tool_bar->SetText(text);
                const char *hz = nullptr;
                if (message->FindString("hz", &hz) == B_OK)
                    status_hz->SetText(hz);
                UpdatePauseState();
                break;
            }
        case MSG_STATUS_TEXT:
            {
                const char *text = nullptr;
                if (message->FindString("text", &text) == B_OK)
                    status_text->SetText(text);
                break;
            }
        case MSG_TITLE:
            {
                const char *text = nullptr;
                if (message->FindString("text", &text) == B_OK)
                    SetTitle(text);
                UpdatePauseState();
                break;
            }
        case MSG_MOUSE_CAPTURE:
            SetMouseCapture(message->GetBool("on", false));
            break;
        case MSG_SB_PANES:
            status_icons->Refresh();
            status_locks->Refresh();
            LayoutViews();
            break;

        case MSG_SETTINGS:
            OpenSettings();
            break;
        case MSG_SETTINGS_CLOSED:
            SettingsClosed(message->GetBool("apply", false));
            break;

        default:
            if (!media->HandleMessage(message))
                BWindow::MessageReceived(message);
            break;
    }
}

bool
MainWindow::QuitRequested()
{
    if (app_quitting)
        return true;

    /* The VM manager's "force shutdown" skips the question. */
    const BMessage *request = CurrentMessage();
    const bool      force   = (request != nullptr) && request->GetBool("force", false);
    if (!force && !ConfirmExit())
        return false;

    /* The application stops the emulation, then closes the window. */
    app_quitting = true;
    be_app->PostMessage(B_QUIT_REQUESTED);
    return false;
}

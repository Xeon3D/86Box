/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Preferences window (qt_preferences.cpp
 *          and its pages qt_preferencesgeneral.cpp, qt_preferencesemulator.cpp,
 *          qt_preferencesinput.cpp, qt_preferenceskeybindings.cpp) and the
 *          key binder (qt_keybind.cpp, qt_singlekeyseqedit.cpp).
 *
 *          Key sequences are written the way Qt writes them in its portable
 *          format ("Ctrl+Alt+Shift+F12", "PgUp", "G"), using only the key
 *          names the main window's accelerator parser understands. Keys are
 *          named by their position (raw Haiku key code), which is also how
 *          the main window matches them.
 *
 *          The native UI has no translations and no VM manager, so the
 *          language list only offers the system default and the VM manager
 *          page is left out; the colour scheme and multimedia keys options
 *          are Windows-only in Qt and hidden here as well.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Alert.h>
#include <Button.h>
#include <CheckBox.h>
#include <InterfaceDefs.h>
#include <LayoutBuilder.h>
#include <ListItem.h>
#include <ListView.h>
#include <ScrollView.h>
#include <Slider.h>
#include <String.h>
#include <StringView.h>
#include <TabView.h>
#include <Window.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "haiku_dialogs.hpp"
#include "haiku_keyboardgrab.hpp"
#include "haiku_settings_common.hpp"
#include "haiku_tooldialogs.hpp"

/* The core headers are not self-contained: keep this order. */
extern "C" {
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/plat.h>
#include <86box/runctl.h>
}

/* ------------------------------------------------------------- modality */

/* A window that releases a semaphore when it goes away, so its creator can
   wait for it with haiku_run_modal() (as in haiku_dialogs.cpp). */
class PrefsModalWindow : public BWindow {
public:
    PrefsModalWindow(BWindow *parent, const char *title)
        : BWindow(BRect(0, 0, 100, 100), title, B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
                  B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS
                      | B_CLOSE_ON_ESCAPE)
        , done(create_sem(0, "86Box modal dialog"))
        , parent(parent)
    {
        if (parent != nullptr)
            AddToSubset(parent);
    }

    ~PrefsModalWindow() override { release_sem(done); }

    void Go()
    {
        if (parent != nullptr)
            CenterIn(parent->Frame());
        haiku_run_modal(this, parent, done);
    }

protected:
    sem_id   done;
    BWindow *parent;
};

enum : uint32 {
    MSG_PF_OK            = 'pfok',
    MSG_PF_CANCEL        = 'pfcn',
    MSG_PF_LANGUAGE      = 'pfln',
    MSG_PF_LANG_DEFAULT  = 'pfld',
    MSG_PF_SENSITIVITY   = 'pfse',
    MSG_PF_SENS_DEFAULT  = 'pfsd',
    MSG_PF_KEY_SELECTED  = 'pfks',
    MSG_PF_KEY_INVOKED   = 'pfki',
    MSG_PF_KEY_BIND      = 'pfkb',
    MSG_PF_KEY_CLEAR     = 'pfkc',
};

/* ------------------------------------------------------------- key names */

/* The name of a key (raw Haiku key code) in a Qt portable key sequence, or
   nullptr for the keys the accelerator parser doesn't know (keypad,
   punctuation, modifiers, ...). */
static const char *
raw_key_name(uint32 raw)
{
    static const char *const fkeys[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
    static const char *const digits[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" };
    static const char *const row_q[]  = { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" };
    static const char *const row_a[]  = { "A", "S", "D", "F", "G", "H", "J", "K", "L" };
    static const char *const row_z[]  = { "Z", "X", "C", "V", "B", "N", "M" };

    if ((raw >= B_F1_KEY) && (raw <= B_F12_KEY))
        return fkeys[raw - B_F1_KEY];
    if ((raw >= 0x12) && (raw <= 0x1b))
        return digits[raw - 0x12];
    if ((raw >= 0x27) && (raw <= 0x30))
        return row_q[raw - 0x27];
    if ((raw >= 0x3c) && (raw <= 0x44))
        return row_a[raw - 0x3c];
    if ((raw >= 0x4c) && (raw <= 0x52))
        return row_z[raw - 0x4c];

    switch (raw) {
        case 0x01:
            return "Esc";
        case B_PRINT_KEY:
        case 0x7e: /* SysRq */
            return "Print";
        case B_SCROLL_KEY:
            return "ScrollLock";
        case B_PAUSE_KEY:
        case 0x7f: /* Break */
            return "Pause";
        case 0x1e:
            return "Backspace";
        case 0x1f:
            return "Ins";
        case 0x20:
            return "Home";
        case 0x21:
            return "PgUp";
        case 0x26:
            return "Tab";
        case 0x34:
            return "Del";
        case 0x35:
            return "End";
        case 0x36:
            return "PgDown";
        case 0x47:
            return "Return";
        case 0x57:
            return "Up";
        case 0x5e:
            return "Space";
        case 0x61:
            return "Left";
        case 0x62:
            return "Down";
        case 0x63:
            return "Right";
        default:
            return nullptr;
    }
}

static bool
raw_key_down(const key_info &info, uint8 raw)
{
    return (info.key_states[raw >> 3] & (1 << (7 - (raw & 7)))) != 0;
}

/* ------------------------------------------------------------ key binder */

/* Qt's singleKeySequenceEdit: shows a key sequence, and replaces it with
   the next key pressed together with the modifiers then held down. */
class KeySequenceView : public BView {
public:
    explicit KeySequenceView(const char *sequence)
        : BView("sequence", B_WILL_DRAW | B_NAVIGABLE | B_FULL_UPDATE_ON_RESIZE)
        , sequence(sequence)
    {
        SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
        font_height fh;
        be_plain_font->GetHeight(&fh);
        const float height = ceilf(fh.ascent + fh.descent + fh.leading) + 10;
        SetExplicitMinSize(BSize(240, height));
        SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, height));
    }

    const std::string &Sequence() const { return sequence; }

    /* A key went down while the view has the focus. */
    void Capture(BMessage *message)
    {
        int32 raw = 0;
        if (message->FindInt32("key", &raw) != B_OK)
            return;
        const char *name = raw_key_name(static_cast<uint32>(raw));
        if (name == nullptr)
            return;

        /* The same keys the main window checks for its accelerators. */
        key_info info;
        if (get_key_info(&info) != B_OK)
            return;
        const bool ctrl  = raw_key_down(info, 0x5c) || raw_key_down(info, 0x60);
        const bool alt   = raw_key_down(info, 0x5d) || raw_key_down(info, 0x5f);
        const bool shift = raw_key_down(info, 0x4b) || raw_key_down(info, 0x56);

        /* Qt's portable order: Ctrl, Alt, Shift, then the key. */
        std::string text;
        if (ctrl)
            text += "Ctrl+";
        if (alt)
            text += "Alt+";
        if (shift)
            text += "Shift+";
        text += name;
        sequence = text;
        Invalidate();
    }

    void Draw(BRect) override
    {
        BRect bounds = Bounds();
        SetHighUIColor(B_PANEL_BACKGROUND_COLOR, B_DARKEN_2_TINT);
        StrokeRect(bounds);
        if (IsFocus() && Window()->IsActive()) {
            SetHighUIColor(B_KEYBOARD_NAVIGATION_COLOR);
            StrokeRect(bounds.InsetByCopy(1, 1));
        }

        font_height fh;
        GetFontHeight(&fh);
        SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
        SetLowUIColor(B_DOCUMENT_BACKGROUND_COLOR);
        const float baseline = bounds.top + ((bounds.Height() + fh.ascent - fh.descent) / 2.0f);
        DrawString(sequence.c_str(), BPoint(bounds.left + 5, baseline));
    }

    void MakeFocus(bool focus) override
    {
        BView::MakeFocus(focus);
        Invalidate();
    }

    void WindowActivated(bool) override { Invalidate(); }

    void MouseDown(BPoint) override { MakeFocus(true); }

private:
    std::string sequence;
};

/* Qt's KeyBinder ("Bind Key"). */
class KeyBinderWindow : public PrefsModalWindow {
public:
    KeyBinderWindow(BWindow *parent, const char *current, bool *accepted, std::string *result)
        : PrefsModalWindow(parent, "Bind Key")
        , accepted(accepted)
        , result(result)
    {
        /* Every key goes to the sequence, Escape included, as in Qt. */
        SetFlags(Flags() & ~B_CLOSE_ON_ESCAPE);

        edit = new KeySequenceView(current);
        BButton *ok = new BButton("ok", "OK", new BMessage(MSG_PF_OK));
        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_WINDOW_SPACING)
            .Add(new BStringView("label", "Enter key combo:"))
            .Add(edit)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(new BButton("cancel", "Cancel", new BMessage(MSG_PF_CANCEL)))
                .Add(ok)
            .End();
        edit->MakeFocus(true);
    }

    /* Key presses reach the sequence before the window's shortcuts and
       keyboard navigation can take them. */
    void DispatchMessage(BMessage *message, BHandler *handler) override
    {
        if (((message->what == B_KEY_DOWN) || (message->what == B_UNMAPPED_KEY_DOWN)) && (CurrentFocus() == edit)) {
            edit->Capture(message);
            return;
        }
        if (((message->what == B_KEY_UP) || (message->what == B_UNMAPPED_KEY_UP)) && (CurrentFocus() == edit))
            return;
        PrefsModalWindow::DispatchMessage(message, handler);
    }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
            case MSG_PF_OK:
                *result   = edit->Sequence();
                *accepted = true;
                Quit();
                break;
            case MSG_PF_CANCEL:
                Quit();
                break;
            default:
                PrefsModalWindow::MessageReceived(message);
                break;
        }
    }

private:
    KeySequenceView *edit;
    bool            *accepted;
    std::string     *result;
};

/* Qt's KeyBinder::BindKey(): false on Cancel. */
static bool
bind_key(BWindow *parent, const char *current, std::string *result)
{
    bool accepted = false;
    (new KeyBinderWindow(parent, current, &accepted, result))->Go();
    return accepted;
}

/* ------------------------------------------------- the key bindings list */

static const float action_share = 0.62f;

static void
draw_two_columns(BView *view, const BRect &frame, const char *first, const char *second, float left, float width)
{
    font_height fh;
    view->GetFontHeight(&fh);
    const float baseline = frame.top + ((frame.Height() + fh.ascent - fh.descent) / 2.0f);
    const float split    = left + (width * action_share);

    BString text(first);
    view->TruncateString(&text, B_TRUNCATE_END, split - left - 8);
    view->DrawString(text.String(), BPoint(left + 4, baseline));
    text = second;
    view->TruncateString(&text, B_TRUNCATE_END, left + width - split - 8);
    view->DrawString(text.String(), BPoint(split + 4, baseline));
}

class KeyBindItem : public BListItem {
public:
    std::string action;
    std::string keybind;

    void DrawItem(BView *owner, BRect frame, bool) override
    {
        const rgb_color background = ui_color(IsSelected() ? B_LIST_SELECTED_BACKGROUND_COLOR : B_LIST_BACKGROUND_COLOR);
        owner->SetLowColor(background);
        owner->FillRect(frame, B_SOLID_LOW);
        owner->SetHighColor(ui_color(IsSelected() ? B_LIST_SELECTED_ITEM_TEXT_COLOR : B_LIST_ITEM_TEXT_COLOR));
        draw_two_columns(owner, frame, action.c_str(), keybind.c_str(), frame.left, frame.Width());
    }

    void Update(BView *owner, const BFont *font) override
    {
        BListItem::Update(owner, font);
        font_height fh;
        font->GetHeight(&fh);
        SetHeight(ceilf(fh.ascent + fh.descent + fh.leading) + 6);
    }
};

/* The titles above the list ("Action", "Keybind"). */
class KeyBindHeader : public BView {
public:
    KeyBindHeader()
        : BView("header", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE)
    {
        SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
        font_height fh;
        be_plain_font->GetHeight(&fh);
        const float height = ceilf(fh.ascent + fh.descent + fh.leading) + 6;
        SetExplicitMinSize(BSize(B_SIZE_UNSET, height));
        SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, height));
    }

    void Draw(BRect) override
    {
        const BRect bounds = Bounds();
        SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
        SetHighUIColor(B_PANEL_TEXT_COLOR);
        /* The list sits in a scroll view: a border, and the scroll bar. */
        draw_two_columns(this, bounds, "Action", "Keybind", 1, bounds.Width() - B_V_SCROLL_BAR_WIDTH - 2);
        SetHighUIColor(B_PANEL_BACKGROUND_COLOR, B_DARKEN_2_TINT);
        StrokeLine(BPoint(bounds.left, bounds.bottom), BPoint(bounds.right, bounds.bottom));
    }
};

/* ------------------------------------------------------------ Preferences */

class PreferencesWindow : public PrefsModalWindow {
public:
    explicit PreferencesWindow(BWindow *parent)
        : PrefsModalWindow(parent, "Preferences")
        , language(new Choice("language", "Language:", MSG_PF_LANGUAGE))
        , sensitivity_value(mouse_sensitivity)
    {
        BTabView *tabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);
        tabs->AddTab(CreateGeneralPage());
        tabs->AddTab(CreateEmulatorPage());
        tabs->AddTab(CreateInputPage());
        tabs->AddTab(CreateKeyBindingsPage());

        BButton *ok = new BButton("ok", "OK", new BMessage(MSG_PF_OK));
        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_WINDOW_SPACING)
            .Add(tabs)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(new BButton("cancel", "Cancel", new BMessage(MSG_PF_CANCEL)))
                .Add(ok)
            .End();
        SetDefaultButton(ok);
    }

    ~PreferencesWindow() override { delete language; }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
            case MSG_PF_LANG_DEFAULT:
                language->Select(0);
                break;
            case MSG_PF_SENSITIVITY:
                sensitivity_value = static_cast<double>(sensitivity->Value()) / 100.0;
                break;
            case MSG_PF_SENS_DEFAULT:
                sensitivity_value = 1.0;
                sensitivity->SetValue(100);
                break;
            case MSG_PF_KEY_SELECTED:
                UpdateKeyButtons();
                break;
            case MSG_PF_KEY_INVOKED:
            case MSG_PF_KEY_BIND:
                EditBinding();
                break;
            case MSG_PF_KEY_CLEAR:
                ClearBinding();
                break;
            case MSG_PF_OK:
                /* Qt's Preferences::accept(). */
                Save();
                config_save_global();
                Quit();
                break;
            case MSG_PF_CANCEL:
                Quit();
                break;
            default:
                PrefsModalWindow::MessageReceived(message);
                break;
        }
    }

private:
    /* --- General (qt_preferencesgeneral.cpp) --- */
    BView *CreateGeneralPage()
    {
        BView *view = new BView("General", 0);
        view->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

        /* The native UI is not translated: the core keeps lang_id 0 (the
           system default) on this platform. */
        language->Add("(System Default)", 0);
        language->Select(lang_id);

        BLayoutBuilder::Group<>(view, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .AddGroup(B_HORIZONTAL)
                .Add(language->field)
                .Add(new BButton("lang_default", "Default", new BMessage(MSG_PF_LANG_DEFAULT)))
            .End()
            .AddGlue();
        return view;
    }

    /* --- Emulator (qt_preferencesemulator.cpp) --- */
    BView *CreateEmulatorPage()
    {
        BView *view = new BView("Emulator", 0);
        view->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

        open_dir_usr_path_box = NewCheckBox("open_dir", "Select media images from program working directory",
                                            open_dir_usr_path > 0);
        open_dir_usr_path_box->SetToolTip("When selecting media images (CD-ROM, floppy, etc.) the open dialog will "
                                          "start in the same directory as the 86Box configuration file. This "
                                          "setting will likely only make a difference on macOS.");
        auto_pause_box        = NewCheckBox("auto_pause", "Auto-pause on focus loss", do_auto_pause);
        auto_dialog_pause_box = NewCheckBox("auto_dialog_pause", "Auto-pause on dialog boxes", do_auto_dialog_pause);
        confirm_save_box      = NewCheckBox("confirm_save", "Ask for confirmation before saving settings", confirm_save);
        confirm_exit_box      = NewCheckBox("confirm_exit", "Ask for confirmation before quitting", confirm_exit);
        confirm_reset_box     = NewCheckBox("confirm_reset", "Ask for confirmation before hard resetting", confirm_reset);
        chd_precache_box      = NewCheckBox("chd_precache", "Precache CHD files into memory", chd_precache_level > 0);
        fast_forward_box      = NewCheckBox("fast_forward", "Start new machines in fast forward", fast_forward_new_machines);
        fast_forward_box->SetToolTip("Only affects machines created from now on. Each machine can be changed in its "
                                     "settings, under Machine > Performance.");

        BLayoutBuilder::Group<>(view, B_VERTICAL, 0)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .Add(open_dir_usr_path_box)
            .Add(auto_pause_box)
            .Add(auto_dialog_pause_box)
            .Add(confirm_save_box)
            .Add(confirm_exit_box)
            .Add(confirm_reset_box)
            .Add(chd_precache_box)
            .Add(fast_forward_box)
            .AddGlue();
        return view;
    }

    /* --- Input (qt_preferencesinput.cpp) --- */
    BView *CreateInputPage()
    {
        BView *view = new BView("Input", 0);
        view->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

        /* Qt's slider: 10 to 200 (hundredths), steps of 10. */
        sensitivity = new BSlider("sensitivity", "Mouse sensitivity:", new BMessage(MSG_PF_SENSITIVITY), 10, 200,
                                  B_HORIZONTAL);
        sensitivity->SetModificationMessage(new BMessage(MSG_PF_SENSITIVITY));
        sensitivity->SetKeyIncrementValue(10);
        sensitivity->SetHashMarks(B_HASH_MARKS_BOTTOM);
        sensitivity->SetHashMarkCount(20);
        sensitivity->SetLimitLabels("0.1", "2.0");
        sensitivity->SetValue(static_cast<int32>(lround(mouse_sensitivity * 100.0)));

        /* Haiku only: how the keys Haiku keeps for itself (Command+key,
           workspace switching) reach the guest while the mouse is captured. */
        system_grab_box = NewCheckBox("system_grab", "Use Haiku's keyboard grab (patched system)",
                                      haiku_keyboard_grab_use_system());
        BStringView *grab_note = new BStringView("grab_note",
            haiku_keyboard_grab_system_available()
                ? "This system has the keyboard grab."
                : "This system has no keyboard grab: the Command keys are unassigned while the mouse is captured.");
        grab_note->SetExplicitMinSize(BSize(10, B_SIZE_UNSET));

        BLayoutBuilder::Group<>(view, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .AddGroup(B_HORIZONTAL)
                .Add(sensitivity, 1)
                .Add(new BButton("sens_default", "Default", new BMessage(MSG_PF_SENS_DEFAULT)))
            .End()
            .Add(system_grab_box)
            .Add(grab_note)
            .AddGlue();
        return view;
    }

    /* --- Key bindings (qt_preferenceskeybindings.cpp) --- */
    BView *CreateKeyBindingsPage()
    {
        BView *view = new BView("Key bindings", 0);
        view->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

        /* A working copy of acc_keys, so the conflicts are checked against
           the bindings as the user left them. */
        for (int x = 0; x < NUM_ACCELS; x++)
            snprintf(acc_keys_t[x], sizeof(acc_keys_t[x]), "%s", acc_keys[x].seq);

        keys = new BListView("keys", B_SINGLE_SELECTION_LIST,
                             B_WILL_DRAW | B_FRAME_EVENTS | B_NAVIGABLE | B_FULL_UPDATE_ON_RESIZE);
        keys->SetSelectionMessage(new BMessage(MSG_PF_KEY_SELECTED));
        keys->SetInvocationMessage(new BMessage(MSG_PF_KEY_INVOKED));
        for (int x = 0; x < NUM_ACCELS; x++)
            keys->AddItem(new KeyBindItem());
        RefreshInputList();
        BScrollView *scroll = new BScrollView("keys_scroll", keys, 0, false, true);
        scroll->SetExplicitMinSize(BSize(440, 280));

        clear_button = new BButton("clear", "Clear binding", new BMessage(MSG_PF_KEY_CLEAR));
        bind_button  = new BButton("bind", "Bind", new BMessage(MSG_PF_KEY_BIND));

        BLayoutBuilder::Group<>(view, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .AddGroup(B_VERTICAL, 0)
                .Add(new KeyBindHeader())
                .Add(scroll, 1)
            .End()
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(clear_button)
                .Add(bind_button)
            .End();

        keys->Select(0);
        UpdateKeyButtons();
        return view;
    }

    static BCheckBox *NewCheckBox(const char *name, const char *label, bool on)
    {
        BCheckBox *box = new BCheckBox(name, label, nullptr);
        box->SetValue(on ? B_CONTROL_ON : B_CONTROL_OFF);
        return box;
    }

    void RefreshInputList()
    {
        for (int x = 0; x < NUM_ACCELS; x++) {
            KeyBindItem *item = static_cast<KeyBindItem *>(keys->ItemAt(x));
            item->action      = acc_keys[x].desc;
            item->keybind     = acc_keys_t[x];
        }
        keys->Invalidate();
    }

    void UpdateKeyButtons()
    {
        const bool selected = keys->CurrentSelection() >= 0;
        bind_button->SetEnabled(selected);
        clear_button->SetEnabled(selected);
    }

    /* Qt's on_treeViewKeys_doubleClicked(). */
    void EditBinding()
    {
        const int row = keys->CurrentSelection();
        if ((row < 0) || (row >= NUM_ACCELS))
            return;

        std::string sequence;
        if (!bind_key(this, acc_keys_t[row], &sequence) || sequence.empty())
            return;

        /* No change. */
        if (sequence == acc_keys_t[row])
            return;

        for (int x = 0; x < NUM_ACCELS; x++) {
            if (sequence == acc_keys_t[x]) {
                BAlert *alert = new BAlert("Bind conflict", "This key combo is already in use.", "OK", nullptr,
                                           nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
                alert->SetShortcut(0, B_ESCAPE);
                alert->Go();
                return;
            }
        }

        snprintf(acc_keys_t[row], sizeof(acc_keys_t[row]), "%s", sequence.c_str());
        RefreshInputList();
    }

    /* Qt's on_pushButtonClearBind_clicked(). */
    void ClearBinding()
    {
        const int row = keys->CurrentSelection();
        if ((row < 0) || (row >= NUM_ACCELS))
            return;
        acc_keys_t[row][0] = '\0';
        RefreshInputList();
    }

    /* Qt's Preferences::save(): each page's save(). */
    void Save()
    {
        lang_id = language->Value(0);

        open_dir_usr_path         = Checked(open_dir_usr_path_box);
        do_auto_pause             = Checked(auto_pause_box);
        do_auto_dialog_pause      = Checked(auto_dialog_pause_box);
        confirm_exit              = Checked(confirm_exit_box);
        confirm_save              = Checked(confirm_save_box);
        confirm_reset             = Checked(confirm_reset_box);
        chd_precache_level        = Checked(chd_precache_box);
        fast_forward_new_machines = Checked(fast_forward_box);

        mouse_sensitivity = sensitivity_value;

        haiku_keyboard_grab_set_use_system(Checked(system_grab_box) != 0);

        for (int x = 0; x < NUM_ACCELS; x++)
            snprintf(acc_keys[x].seq, sizeof(acc_keys[x].seq), "%s", acc_keys_t[x]);
    }

    static int Checked(const BCheckBox *box) { return (box->Value() == B_CONTROL_ON) ? 1 : 0; }

    Choice    *language;
    BCheckBox *open_dir_usr_path_box;
    BCheckBox *auto_pause_box;
    BCheckBox *auto_dialog_pause_box;
    BCheckBox *confirm_save_box;
    BCheckBox *confirm_exit_box;
    BCheckBox *confirm_reset_box;
    BCheckBox *chd_precache_box;
    BCheckBox *fast_forward_box;
    BCheckBox *system_grab_box;
    BSlider   *sensitivity;
    double     sensitivity_value;
    BListView *keys;
    BButton   *bind_button;
    BButton   *clear_button;
    char       acc_keys_t[NUM_ACCELS][sizeof(acc_keys[0].seq)];
};

/* Like MainWindow::on_actionPreferences_triggered() in Qt: the global
   configuration is reloaded first, since another instance may have changed
   it. A dialog over the main window pauses the emulation when "Auto-pause
   on dialog boxes" is set (MainWindow::eventFilter() there). The main
   window reads acc_keys[] on every key press, so new bindings take effect
   at once. */
void
haiku_preferences_dialog(BWindow *parent)
{
    config_load_global();

    if (do_auto_dialog_pause > 0)
        runctl_pause(RUN_PAUSE_DIALOG);
    (new PreferencesWindow(parent))->Go();
    runctl_resume(RUN_PAUSE_DIALOG);
}

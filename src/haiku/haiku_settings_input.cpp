/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Input devices Settings page (qt_settingsinput.cpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Button.h>
#include <GroupView.h>
#include <LayoutBuilder.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "haiku_deviceconfig.hpp"
#include "haiku_joystickconfig.hpp"
#include "haiku_settings_common.hpp"
#include "haiku_settings_display.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/video.h>
#include <86box/keyboard.h>
#include <86box/mouse.h>
#include <86box/gameport.h>
}

/* The joystick number is added to MSG_SI_JOYSTICK_CFG. */
enum : uint32 {
    MSG_SI_KEYBOARD     = 'sikb',
    MSG_SI_KEYBOARD_CFG = 'sikc',
    MSG_SI_MOUSE        = 'simo',
    MSG_SI_MOUSE_CFG    = 'simc',
    MSG_SI_TABLET       = 'sitb',
    MSG_SI_TABLET_CFG   = 'sitc',
    MSG_SI_JOYSTICK     = 'sijs',
    MSG_SI_JOYSTICK_CFG = 'sij0',
};

/* The joystick mappings as they were when the window opened: the dialog
   writes joystick_state directly, so this tells Changed() what moved and
   lets Cancel put it back (Qt's org_joystick_state). */
static joystick_state_t org_joystick_state[GAMEPORT_MAX][MAX_JOYSTICKS];

static bool
has_joystick_state_changed(int gameport_nr, int joystick_nr)
{
    const joystick_state_t &now = joystick_state[gameport_nr][joystick_nr];
    const joystick_state_t &org = org_joystick_state[gameport_nr][joystick_nr];
    bool                    has_changed = (now.plat_joystick_nr != org.plat_joystick_nr);

    for (int axis_nr = 0; axis_nr < MAX_JOY_AXES; axis_nr++)
        has_changed |= (now.axis_mapping[axis_nr] != org.axis_mapping[axis_nr]);
    for (int button_nr = 0; button_nr < MAX_JOY_BUTTONS; button_nr++)
        has_changed |= (now.button_mapping[button_nr] != org.button_mapping[button_nr]);
    for (int pov_nr = 0; pov_nr < MAX_JOY_POVS; pov_nr++) {
        has_changed |= (now.pov_mapping[pov_nr][0] != org.pov_mapping[pov_nr][0]);
        has_changed |= (now.pov_mapping[pov_nr][1] != org.pov_mapping[pov_nr][1]);
    }

    return has_changed;
}

/* --- Input (qt_settingsinput.cpp) ------------------------------------- */

class InputPage : public SettingsPage {
public:
    InputPage()
        : SettingsPage("Input devices")
        , keyboard(new Choice("keyboard", "Keyboard:", MSG_SI_KEYBOARD))
        , keyboard_button(new BButton("keyboard_cfg", "Configure", new BMessage(MSG_SI_KEYBOARD_CFG)))
        , mouse(new Choice("mouse", "Mouse:", MSG_SI_MOUSE))
        , mouse_button(new BButton("mouse_cfg", "Configure", new BMessage(MSG_SI_MOUSE_CFG)))
        , tablet(new Choice("tablet", "Tablet:", MSG_SI_TABLET))
        , tablet_button(new BButton("tablet_cfg", "Configure", new BMessage(MSG_SI_TABLET_CFG)))
        , joystick(new Choice("joystick0", "Joystick:", MSG_SI_JOYSTICK))
        , kbd_config_changed(false)
        , mouse_config_changed(false)
        , tablet_config_changed(false)
        , saved(false)
    {
        memcpy(org_joystick_state, joystick_state, sizeof(org_joystick_state));

        for (int i = 0; i < MAX_JOYSTICKS; i++) {
            char name[32];
            char label[32];
            snprintf(name, sizeof(name), "joystick0%i", i + 1);
            snprintf(label, sizeof(label), "Joystick %i…", i + 1);
            joystick_button[i] = new BButton(name, label, new BMessage(MSG_SI_JOYSTICK_CFG + i));
        }
        BGroupView *joystick_buttons = new BGroupView(B_HORIZONTAL, B_USE_SMALL_SPACING);
        for (int i = 0; i < MAX_JOYSTICKS; i++)
            joystick_buttons->AddChild(joystick_button[i]);

        BLayoutBuilder::Grid<> grid(this, B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
        grid.SetInsets(B_USE_DEFAULT_SPACING)
            .AddMenuField(keyboard->field, 0, 0)
            .Add(keyboard_button, 2, 0)
            .AddMenuField(mouse->field, 0, 1)
            .Add(mouse_button, 2, 1)
            .AddMenuField(tablet->field, 0, 2)
            .Add(tablet_button, 2, 2)
            .AddMenuField(joystick->field, 0, 3, B_ALIGN_HORIZONTAL_UNSET, 1, 2)
            .Add(joystick_buttons, 0, 4, 3)
            .AddGlue(0, 5, 3);
        grid.Layout()->SetColumnWeight(1, 1.0f);

        /* The joystick types do not depend on the machine. */
        for (int i = 0;; i++) {
            const char *name = joystick_get_name(i);
            if (name == nullptr)
                break;
            joystick->Add(name, i);
        }
        joystick->Select(joystick_type[0]);
        joystick->SetEnabled(true);
        UpdateJoystickButtons();
    }

    ~InputPage() override
    {
        /* Cancel: the dialog's changes to the mappings are dropped. */
        if (!saved) {
            for (int i = 0; i < GAMEPORT_MAX; i++) {
                for (int j = 0; j < MAX_JOYSTICKS; j++) {
                    joystick_state_t       &now = joystick_state[i][j];
                    const joystick_state_t &org = org_joystick_state[i][j];
                    now.plat_joystick_nr        = org.plat_joystick_nr;
                    memcpy(now.axis_mapping, org.axis_mapping, sizeof(now.axis_mapping));
                    memcpy(now.button_mapping, org.button_mapping, sizeof(now.button_mapping));
                    memcpy(now.pov_mapping, org.pov_mapping, sizeof(now.pov_mapping));
                }
            }
        }

        delete keyboard;
        delete mouse;
        delete tablet;
        delete joystick;
    }

    void MachineChanged(int m) override
    {
        const int cur_keyboard = keyboard->Count() ? keyboard->Value(keyboard_type) : keyboard_type;
        const int cur_mouse    = mouse->Count() ? mouse->Value(mouse_type) : mouse_type;
        const int has_int_kbd  = !!machine_has_flags(m, MACHINE_KEYBOARD);

        keyboard->Clear();
        for (int i = 0; i < keyboard_get_ndev(); i++) {
            const device_t *dev           = keyboard_get_device(i);
            const int       internal      = (i == KEYBOARD_TYPE_INTERNAL);
            const bool      pc5086_filter = strstr(keyboard_get_internal_name(i), "ps")
                && (machines[m].init == machine_xt_pc5086_init);
            if ((internal != has_int_kbd) || !device_is_valid(dev, m) || pc5086_filter)
                continue;
            keyboard->Add(settings_device_name(dev, keyboard_get_internal_name(i), 0).c_str(), i);
        }
        keyboard->Select(cur_keyboard);
        /* Select() disables a single choice; the internal one stays fixed too. */
        if (has_int_kbd)
            keyboard->SetEnabled(false);

        mouse->Clear();
        for (int i = 0; i < mouse_get_ndev(); i++) {
            const device_t *dev = mouse_get_device(i);
            if ((i == MOUSE_TYPE_INTERNAL) && !machine_has_flags(m, MACHINE_MOUSE))
                continue;
            if (!device_is_valid(dev, m))
                continue;
            mouse->Add(settings_device_name(dev, mouse_get_internal_name(i), 0).c_str(), i);
        }
        mouse->Select(cur_mouse);

        FillTablets(m);
        UpdateConfigButtons();
    }

    void Show() override
    {
        FillTablets(CurrentMachine());
        UpdateConfigButtons();
        SettingsPage::Show();
    }

    bool Handle(BMessage *message) override
    {
        const uint32 what = message->what;

        switch (what) {
            case MSG_SI_KEYBOARD:
            case MSG_SI_MOUSE:
            case MSG_SI_TABLET:
                UpdateConfigButtons();
                return true;
            case MSG_SI_JOYSTICK:
                UpdateJoystickButtons();
                return true;
            case MSG_SI_KEYBOARD_CFG:
                kbd_config_changed |= haiku_device_configure(Window(), keyboard_get_device(keyboard->Value(0)));
                return true;
            case MSG_SI_MOUSE_CFG:
                mouse_config_changed |= haiku_device_configure(Window(), mouse_get_device(mouse->Value(0)));
                return true;
            case MSG_SI_TABLET_CFG:
                tablet_config_changed |= haiku_device_configure(Window(), tablet_get_device(tablet->Value(0)));
                return true;
            default:
                break;
        }

        if ((what >= MSG_SI_JOYSTICK_CFG) && (what < MSG_SI_JOYSTICK_CFG + MAX_JOYSTICKS)) {
            haiku_joystick_configure(Window(), joystick->Value(0), 0, what - MSG_SI_JOYSTICK_CFG);
            return true;
        }
        return false;
    }

    bool Changed() override
    {
        bool changed = false;

        changed |= (keyboard_type != keyboard->Value(keyboard_type)) || kbd_config_changed;
        changed |= (mouse_type != mouse->Value(mouse_type)) || mouse_config_changed;
        changed |= (tablet_type != tablet->Value(tablet_type)) || tablet_config_changed;
        changed |= (joystick_type[0] != joystick->Value(joystick_type[0]));
        for (int i = 0; i < MAX_JOYSTICKS; i++)
            changed |= has_joystick_state_changed(0, i);

        return changed;
    }

    void Save() override
    {
        keyboard_type    = keyboard->Value(keyboard_type);
        mouse_type       = mouse->Value(mouse_type);
        tablet_type      = tablet->Value(tablet_type);
        joystick_type[0] = joystick->Value(joystick_type[0]);
        saved            = true;
    }

private:
    /* The tablets, with the CGA light pen only while the Display page has
       a card it fits (Qt refills this when the video card changes; here it
       is refilled whenever the page is shown). */
    void FillTablets(int m)
    {
        const int  cur_tablet  = tablet->Count() ? tablet->Value(tablet_type) : tablet_type;
        const bool has_cga_pen = settings_display_light_pen_usable();

        tablet->Clear();
        for (int i = 0; i < tablet_get_ndev(); i++) {
            const device_t *dev = tablet_get_device(i);
            if ((i == TABLET_TYPE_INTERNAL) && (machine_get_tablet_device(m) == nullptr))
                continue;
            if (!device_is_valid(dev, m))
                continue;
            if (!has_cga_pen && !strcmp(tablet_get_internal_name(i), "cga_lightpen"))
                continue;
            tablet->Add(settings_device_name(dev, tablet_get_internal_name(i), 0).c_str(), i);
        }
        tablet->Select(cur_tablet);
    }

    /* on_comboBox{Keyboard,Mouse,Tablet}_currentIndexChanged(). */
    void UpdateConfigButtons()
    {
        keyboard_button->SetEnabled((keyboard->Count() > 0) && (keyboard_has_config(keyboard->Value(0)) > 0));
        mouse_button->SetEnabled((mouse->Count() > 0) && (mouse_has_config(mouse->Value(0)) > 0));
        tablet_button->SetEnabled((tablet->Count() > 0) && (tablet_has_config(tablet->Value(0)) > 0));
    }

    /* on_comboBoxJoystick0_currentIndexChanged(). */
    void UpdateJoystickButtons()
    {
        const int type = joystick->Value(0);
        for (int i = 0; i < MAX_JOYSTICKS; i++)
            joystick_button[i]->SetEnabled(joystick_get_max_joysticks(type) > i);
    }

    Choice  *keyboard;
    BButton *keyboard_button;
    Choice  *mouse;
    BButton *mouse_button;
    Choice  *tablet;
    BButton *tablet_button;
    Choice  *joystick;
    BButton *joystick_button[MAX_JOYSTICKS];
    bool     kbd_config_changed;
    bool     mouse_config_changed;
    bool     tablet_config_changed;
    bool     saved;
};


SettingsPage *
create_input_page()
{
    return new InputPage();
}

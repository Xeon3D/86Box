/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the joystick configuration dialog
 *          (qt_joystickconfiguration.cpp, and updateJoystickConfig() from
 *          qt_settingsinput.cpp). The host joysticks come from the platform
 *          joystick layer (plat_joystick_state / joysticks_present).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Button.h>
#include <GridView.h>
#include <LayoutBuilder.h>
#include <OS.h>
#include <StringView.h>
#include <Window.h>

#include <cstdio>
#include <string>
#include <vector>

#include "haiku_joystickconfig.hpp"
#include "haiku_settings_common.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/device.h>
#include <86box/gameport.h>
}

enum : uint32 {
    MSG_JC_DEVICE = 'jcdv',
    MSG_JC_OK     = 'jcok',
    MSG_JC_CANCEL = 'jccn',
};

/* What the dialog hands back: the pop-up indexes, as Qt's selected*(). */
struct JoystickDialogState {
    int    type        = 0;
    int    gameport_nr = 0;
    int    joystick_nr = 0;
    bool   accepted    = false;
    int    device      = 0;
    int    axis[MAX_JOY_AXES] {};
    int    button[MAX_JOY_BUTTONS] {};
    int    pov[MAX_JOY_POVS * 2] {};
    sem_id done = -1;
};

static std::string
pov_axis_name(const char *name, bool y)
{
    return std::string(name) + (y ? " (Y axis)" : " (X axis)");
}

/* ---------------------------------------------------------- the window */

class JoystickConfigWindow : public BWindow {
public:
    JoystickConfigWindow(BWindow *parent, JoystickDialogState *state);
    ~JoystickConfigWindow() override;

    void MessageReceived(BMessage *message) override;

private:
    void Rebuild();
    void Accept();

    JoystickDialogState  *state;
    Choice               *device;
    BGridView            *ct;
    std::vector<BView *>  widgets;
    std::vector<Choice *> axes;
    std::vector<Choice *> buttons;
    std::vector<Choice *> povs;
};

JoystickConfigWindow::JoystickConfigWindow(BWindow *parent, JoystickDialogState *s)
    : BWindow(BRect(0, 0, 100, 100), "Joystick configuration", B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
              B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS
                  | B_ASYNCHRONOUS_CONTROLS | B_CLOSE_ON_ESCAPE)
    , state(s)
    , device(new Choice("device", "Device", MSG_JC_DEVICE))
    , ct(new BGridView("mapping", B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING))
{
    /* A modal parent (the Settings window) cannot be put in a subset:
       be modal to the whole application then. */
    if (parent != nullptr) {
        if (AddToSubset(parent) != B_OK)
            SetFeel(B_MODAL_APP_WINDOW_FEEL);
    }

    device->Add("None", 0);
    for (int c = 0; c < joysticks_present; c++)
        device->Add(plat_joystick_state[c].name, c + 1);
    device->Select(joystick_state[state->gameport_nr][state->joystick_nr].plat_joystick_nr);

    BButton *ok     = new BButton("ok", "OK", new BMessage(MSG_JC_OK));
    BButton *cancel = new BButton("cancel", "Cancel", new BMessage(MSG_JC_CANCEL));

    BLayoutBuilder::Group<>(this, B_VERTICAL)
        .SetInsets(B_USE_WINDOW_SPACING)
        .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
            .AddMenuField(device->field, 0, 0)
        .End()
        .Add(ct)
        .AddGlue()
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel)
            .Add(ok)
        .End();

    SetDefaultButton(ok);
    Rebuild();

    if (parent != nullptr)
        CenterIn(parent->Frame());
    else
        CenterOnScreen();
}

JoystickConfigWindow::~JoystickConfigWindow()
{
    for (Choice *c : axes)
        delete c;
    for (Choice *c : buttons)
        delete c;
    for (Choice *c : povs)
        delete c;
    delete device;

    /* The caller waits for this; nothing of the window is used after it. */
    release_sem(state->done);
}

/* on_comboBoxDevice_currentIndexChanged(): one pop-up per axis, button and
   POV direction of the emulated joystick, offering the host joystick's. */
void
JoystickConfigWindow::Rebuild()
{
    for (BView *w : widgets) {
        w->RemoveSelf();
        delete w;
    }
    widgets.clear();
    for (Choice *c : axes)
        delete c;
    for (Choice *c : buttons)
        delete c;
    for (Choice *c : povs)
        delete c;
    axes.clear();
    buttons.clear();
    povs.clear();

    const int index = device->Value(0);
    if (index > 0) {
        const plat_joystick_state_t &host    = plat_joystick_state[index - 1];
        const joystick_state_t      &current = joystick_state[state->gameport_nr][state->joystick_nr];
        const int                    type    = state->type;
        BGridLayout                 *grid    = ct->GridLayout();
        int                          row     = 0;

        auto add_row = [&](const std::string &label, Choice *choice) {
            BStringView *text = new BStringView(nullptr, label.c_str());
            grid->AddView(text, 0, row);
            grid->AddView(choice->field, 1, row);
            widgets.push_back(text);
            widgets.push_back(choice->field);
            row++;
        };

        for (int c = 0; c < joystick_get_axis_count(type); c++) {
            Choice *choice = new Choice(nullptr, nullptr, MSG_SETTINGS_IGNORE);
            int     n      = 0;
            for (int d = 0; d < host.nr_axes; d++)
                choice->Add(host.axis[d].name, n++);
            for (int d = 0; d < host.nr_povs; d++) {
                choice->Add(pov_axis_name(host.pov[d].name, false).c_str(), n++);
                choice->Add(pov_axis_name(host.pov[d].name, true).c_str(), n++);
            }

            const int mapping = current.axis_mapping[c];
            if (mapping & POV_X)
                choice->Select(host.nr_axes + (mapping & 3) * 2);
            else if (mapping & POV_Y)
                choice->Select(host.nr_axes + (mapping & 3) * 2 + 1);
            else
                choice->Select(mapping);
            choice->SetEnabled(true);

            axes.push_back(choice);
            add_row(joystick_get_axis_name(type, c), choice);
        }

        for (int c = 0; c < joystick_get_button_count(type); c++) {
            Choice *choice = new Choice(nullptr, nullptr, MSG_SETTINGS_IGNORE);
            for (int d = 0; d < host.nr_buttons; d++)
                choice->Add(host.button[d].name, d);
            choice->Select(current.button_mapping[c]);
            choice->SetEnabled(true);

            buttons.push_back(choice);
            add_row(joystick_get_button_name(type, c), choice);
        }

        for (int c = 0; c < joystick_get_pov_count(type) * 2; c++) {
            Choice *choice = new Choice(nullptr, nullptr, MSG_SETTINGS_IGNORE);
            int     n      = 0;
            for (int d = 0; d < host.nr_povs; d++) {
                choice->Add(pov_axis_name(host.pov[d].name, false).c_str(), n++);
                choice->Add(pov_axis_name(host.pov[d].name, true).c_str(), n++);
            }
            for (int d = 0; d < host.nr_axes; d++)
                choice->Add(host.axis[d].name, n++);

            const int mapping = current.pov_mapping[c / 2][c & 1];
            if (mapping & POV_X)
                choice->Select((mapping & 3) * 2);
            else if (mapping & POV_Y)
                choice->Select((mapping & 3) * 2 + 1);
            else
                choice->Select(mapping + host.nr_povs * 2);
            choice->SetEnabled(true);

            povs.push_back(choice);
            add_row(pov_axis_name(joystick_get_pov_name(type, c / 2), c & 1), choice);
        }
    }

    /* Fixed size, like Qt's SetFixedSize constraint. */
    if (BLayout *layout = GetLayout()) {
        layout->InvalidateLayout(true);
        const BSize size = layout->PreferredSize();
        ResizeTo(size.width, size.height);
    }
}

void
JoystickConfigWindow::Accept()
{
    state->device = device->Value(0);
    for (size_t i = 0; (i < axes.size()) && (i < MAX_JOY_AXES); i++)
        state->axis[i] = axes[i]->Value(0);
    for (size_t i = 0; (i < buttons.size()) && (i < MAX_JOY_BUTTONS); i++)
        state->button[i] = buttons[i]->Value(0);
    for (size_t i = 0; (i < povs.size()) && (i < MAX_JOY_POVS * 2); i++)
        state->pov[i] = povs[i]->Value(0);
    state->accepted = true;
}

void
JoystickConfigWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_JC_DEVICE:
            Rebuild();
            break;

        case MSG_JC_OK:
            Accept();
            Quit();
            break;

        case MSG_JC_CANCEL:
            Quit();
            break;

        case MSG_SETTINGS_IGNORE:
            break;

        default:
            BWindow::MessageReceived(message);
            break;
    }
}

/* --------------------------------------------------------- the interface */

/* get_axis() / get_pov() of qt_settingsinput.cpp: a pop-up index back to
   the core's mapping (an axis number, or POV_X / POV_Y with the hat). */
static int
get_axis(const JoystickDialogState &s, int axis)
{
    int       axis_sel = s.axis[axis];
    const int nr_axes  = plat_joystick_state[joystick_state[s.gameport_nr][s.joystick_nr].plat_joystick_nr - 1].nr_axes;

    if (axis_sel < nr_axes)
        return axis_sel;

    axis_sel -= nr_axes;
    if (axis_sel & 1)
        return POV_Y | (axis_sel >> 1);
    else
        return POV_X | (axis_sel >> 1);
}

static int
get_pov(const JoystickDialogState &s, int pov)
{
    const int pov_sel = s.pov[pov];
    const int nr_povs = plat_joystick_state[joystick_state[s.gameport_nr][s.joystick_nr].plat_joystick_nr - 1].nr_povs * 2;

    if (pov_sel < nr_povs) {
        if (pov_sel & 1)
            return POV_Y | (pov_sel >> 1);
        else
            return POV_X | (pov_sel >> 1);
    }

    return pov_sel - nr_povs;
}

bool
haiku_joystick_configure(BWindow *parent, int type, int gameport_nr, int joystick_nr)
{
    if ((gameport_nr < 0) || (gameport_nr >= GAMEPORT_MAX) || (joystick_nr < 0) || (joystick_nr >= MAX_JOYSTICKS))
        return false;

    JoystickDialogState state;
    state.type        = type;
    state.gameport_nr = gameport_nr;
    state.joystick_nr = joystick_nr;

    state.done = create_sem(0, "joystick config");
    if (state.done < B_OK)
        return false;

    JoystickConfigWindow *window = new JoystickConfigWindow(parent, &state);
    window->Show();

    /* Like BAlert::Go(): wait, keeping the parent window redrawn. */
    status_t err;
    if (parent != nullptr) {
        do {
            err = acquire_sem_etc(state.done, 1, B_RELATIVE_TIMEOUT, 50000);
            if ((err == B_TIMED_OUT) || (err == B_INTERRUPTED))
                parent->UpdateIfNeeded();
        } while ((err == B_TIMED_OUT) || (err == B_INTERRUPTED));
    } else {
        do {
            err = acquire_sem(state.done);
        } while (err == B_INTERRUPTED);
    }
    delete_sem(state.done);

    if (!state.accepted)
        return false;

    joystick_state_t &js = joystick_state[gameport_nr][joystick_nr];
    js.plat_joystick_nr  = state.device;
    if (js.plat_joystick_nr) {
        for (int axis_nr = 0; axis_nr < joystick_get_axis_count(type); axis_nr++)
            js.axis_mapping[axis_nr] = get_axis(state, axis_nr);

        for (int button_nr = 0; button_nr < joystick_get_button_count(type); button_nr++)
            js.button_mapping[button_nr] = state.button[button_nr];

        for (int pov_nr = 0; pov_nr < joystick_get_pov_count(type); pov_nr++) {
            js.pov_mapping[pov_nr][0] = get_pov(state, pov_nr * 2);     /* X axis */
            js.pov_mapping[pov_nr][1] = get_pov(state, pov_nr * 2 + 1); /* Y axis */
        }
    }

    return true;
}

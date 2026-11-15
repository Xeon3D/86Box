/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: send the guest the keys Haiku keeps for
 *          itself while the mouse is captured.
 *
 *          Haiku has no keyboard grab. app_server switches workspaces on
 *          Command+F1..F12, and input_server handles Command+Tab and
 *          Ctrl+Command+Del, before any window sees them; with the default
 *          PC keymap Command is the Alt key, so Alt+F4 in the guest would
 *          change the host's workspace. As a stopgap, the Command keys are
 *          unassigned while the mouse is captured, so Alt becomes a plain key
 *          that reaches the window, and assigned again when it is released.
 *
 *          set_modifier_key() changes the system keymap and input_server
 *          saves it, so the original keys are also written to a file first:
 *          if 86Box dies while holding them, the next start restores them.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <FindDirectory.h>
#include <InterfaceDefs.h>
#include <OS.h>
#include <Path.h>
#include <Window.h>

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include "haiku_frontend.hpp"
#include "haiku_keyboardgrab.hpp"
#include "haiku_mainwindow.hpp"

/* How the keyboard is held while grabbed. */
enum grab_method {
    GRAB_NONE,
    GRAB_COMMAND_KEYS, /* Command keys unassigned (any Haiku) */
    GRAB_SYSTEM,       /* BWindow::SetKeyboardGrab() (patched Haiku) */
};

static grab_method grab_used = GRAB_NONE;

/* BWindow::SetKeyboardGrab(bool), looked up at run time so the same binary
   runs on Haiku without it. A non-virtual member function: `this` is the
   first argument. */
typedef status_t (*set_keyboard_grab_fn)(BWindow *window, bool grab);

static set_keyboard_grab_fn
system_grab_function(void)
{
    static bool                 looked = false;
    static set_keyboard_grab_fn fn     = nullptr;

    if (!looked) {
        looked = true;
        fn     = reinterpret_cast<set_keyboard_grab_fn>(dlsym(RTLD_DEFAULT, "_ZN7BWindow15SetKeyboardGrabEb"));
    }
    return fn;
}

bool
haiku_keyboard_grab_system_available(void)
{
    return system_grab_function() != nullptr;
}

/* ~/config/settings/86Box/haiku-frontend: "keyboard_grab=system|command_keys". */
static bool
frontend_settings_path(BPath *path)
{
    if (find_directory(B_USER_SETTINGS_DIRECTORY, path, true) != B_OK)
        return false;
    path->Append("86Box");
    mkdir(path->Path(), 0755);
    path->Append("haiku-frontend");
    return true;
}

static int use_system = -1; /* not read yet */

bool
haiku_keyboard_grab_use_system(void)
{
    if (use_system < 0) {
        use_system = 0;
        BPath path;
        FILE *f = frontend_settings_path(&path) ? fopen(path.Path(), "r") : nullptr;
        if (f != nullptr) {
            char line[128];
            while (fgets(line, sizeof(line), f) != nullptr) {
                if (!strncmp(line, "keyboard_grab=system", 20))
                    use_system = 1;
            }
            fclose(f);
        }
    }
    return use_system == 1;
}

void
haiku_keyboard_grab_set_use_system(bool use)
{
    use_system = use ? 1 : 0;

    BPath path;
    FILE *f = frontend_settings_path(&path) ? fopen(path.Path(), "w") : nullptr;
    if (f == nullptr)
        return;
    fprintf(f, "keyboard_grab=%s\n", use ? "system" : "command_keys");
    fclose(f);
}

static uint32 saved_left_cmd    = 0;
static uint32 saved_right_cmd   = 0;

/* ~/config/settings/86Box/haiku-command-keys while the keys are held. */
static bool
marker_path(BPath *path)
{
    if (find_directory(B_USER_SETTINGS_DIRECTORY, path, true) != B_OK)
        return false;
    path->Append("86Box");
    mkdir(path->Path(), 0755);
    path->Append("haiku-command-keys");
    return true;
}

static void
write_marker(uint32 left, uint32 right)
{
    BPath path;
    if (!marker_path(&path))
        return;

    FILE *f = fopen(path.Path(), "w");
    if (f == nullptr)
        return;
    fprintf(f, "%u %u\n", (unsigned) left, (unsigned) right);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static bool
read_marker(uint32 *left, uint32 *right)
{
    BPath path;
    if (!marker_path(&path))
        return false;

    FILE *f = fopen(path.Path(), "r");
    if (f == nullptr)
        return false;

    unsigned l = 0;
    unsigned r = 0;
    const bool ok = (fscanf(f, "%u %u", &l, &r) == 2);
    fclose(f);

    *left  = l;
    *right = r;
    return ok;
}

static void
remove_marker(void)
{
    BPath path;
    if (marker_path(&path))
        unlink(path.Path());
}

void
haiku_keyboard_grab(bool on)
{
    if (on == (grab_used != GRAB_NONE))
        return;

    if (on) {
        /* The system's grab, when there is one and the user chose it. */
        set_keyboard_grab_fn grab = system_grab_function();
        if ((grab != nullptr) && haiku_keyboard_grab_use_system() && (main_window != nullptr)
            && (grab(main_window, true) == B_OK)) {
            grab_used = GRAB_SYSTEM;
            return;
        }

        uint32 left  = 0;
        uint32 right = 0;
        get_modifier_key(B_LEFT_COMMAND_KEY, &left);
        get_modifier_key(B_RIGHT_COMMAND_KEY, &right);
        if ((left == 0) && (right == 0))
            return;

        saved_left_cmd  = left;
        saved_right_cmd = right;
        write_marker(left, right);

        set_modifier_key(B_LEFT_COMMAND_KEY, 0);
        set_modifier_key(B_RIGHT_COMMAND_KEY, 0);
        grab_used = GRAB_COMMAND_KEYS;
    } else {
        if (grab_used == GRAB_SYSTEM) {
            /* app_server drops the grab by itself when the window loses
               focus or closes; release it here only from the window's own
               thread, so quitting cannot wait on the window's lock. */
            set_keyboard_grab_fn grab = system_grab_function();
            if ((grab != nullptr) && (main_window != nullptr) && (find_thread(nullptr) == main_window->Thread()))
                grab(main_window, false);
        } else {
            set_modifier_key(B_LEFT_COMMAND_KEY, saved_left_cmd);
            set_modifier_key(B_RIGHT_COMMAND_KEY, saved_right_cmd);
            remove_marker();
        }
        grab_used = GRAB_NONE;
    }
}

void
haiku_keyboard_restore_after_crash(void)
{
    uint32 left  = 0;
    uint32 right = 0;

    if (!read_marker(&left, &right))
        return;

    if (left != 0)
        set_modifier_key(B_LEFT_COMMAND_KEY, left);
    if (right != 0)
        set_modifier_key(B_RIGHT_COMMAND_KEY, right);
    remove_marker();
}

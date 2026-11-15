/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the ui_* and window-related plat_*
 *          functions the core calls (the Qt frontend keeps these in
 *          qt_ui.cpp). Most are called from the emulation or video threads,
 *          so they only post messages to the main window.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Alert.h>
#include <Message.h>
#include <OS.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "haiku_frontend.hpp"
#include "haiku_mainwindow.hpp"
#include "haiku_monitorwindow.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/ui.h>
#include <86box/mouse.h>
#include <86box/machine.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/fdd.h>
#include <86box/hdc.h>
#include <86box/scsi.h>
#include <86box/scsi_device.h>
#include <86box/cartridge.h>
#include <86box/cassette.h>
#include <86box/cdrom.h>
#include <86box/rdisk.h>
#include <86box/mo.h>
#include <86box/scsi_tape.h>
#include <86box/hdd.h>
#include <86box/network.h>
#include <86box/machine_status.h>
#include <86box/runctl.h>
#include <86box/version.h>
#include <86box/video.h>
}

extern "C" int vid_resize;

static std::mutex  text_lock;
static std::string sb_text;
static std::string sb_buguitext;
static std::string sb_mt32lcdtext;
static char        window_title[512] = EMU_NAME;

static void
post_text(uint32 what, const char *text)
{
    if (main_window == nullptr)
        return;

    BMessage msg(what);
    msg.AddString("text", text ? text : "");
    main_window->PostMessage(&msg);
}

extern "C" {

/* ----------------------------------------------------- window and title */

/* Used by ui_notify() in src/unix/sdl_plat.c to mark the title while
   paused: with no argument it returns the current title. */
char *
ui_window_title(char *str)
{
    if (str != nullptr) {
        strlcpy(window_title, str, sizeof(window_title));
        post_text(MSG_TITLE, window_title);
    }
    return window_title;
}

void
ui_emu_status(int speed_percent)
{
    char text[256];

    if ((mouse_type == MOUSE_TYPE_NONE) || (mouse_input_mode >= 1))
        snprintf(text, sizeof(text), "%i%%", speed_percent);
    else if (mouse_capture) {
        /* Qt MainWindow::updateMouseStrings(): the "release mouse" binding
           and the button that also releases it. */
        const int   id      = FindAccelerator("release_mouse");
        const char *seq     = ((id >= 0) && acc_keys[id].seq[0]) ? acc_keys[id].seq : "?";
        const int   release = mouse_get_release_buttons();
        const char *button  = (release & MOUSE_RELEASE_MIDDLE) ? " or middle button"
                                                               : ((release & MOUSE_RELEASE_THUMB) ? " or side button" : "");
        snprintf(text, sizeof(text), "%i%% - Press %s%s to release mouse", speed_percent, seq, button);
    } else
        snprintf(text, sizeof(text), "%i%% - Click to capture mouse", speed_percent);

    /* A sample taken just before a pause may arrive after it. */
    if (runctl_state() == RUN_PAUSED)
        strlcat(text, " - PAUSED", sizeof(text));

    /* The refresh rate shown at the right of the status bar, once a second
       like Qt's frameRateTimer. */
    char hz[64];
    if (monitors[0].mon_dpms)
        strlcpy(hz, "Monitor in sleep mode", sizeof(hz));
    else
        snprintf(hz, sizeof(hz), "%i Hz%s", static_cast<int>(monitors[0].mon_actualrenderedframes.load()),
                 monitors[0].mon_interlace ? "i" : "");

    if (main_window != nullptr) {
        BMessage msg(MSG_EMU_STATUS);
        msg.AddString("text", text);
        msg.AddString("hz", hz);
        main_window->PostMessage(&msg);
    }
}

void
ui_hard_reset_completed(void)
{
    ui_emu_status(runctl_speed());
}

void
plat_resize_request(int w, int h, int monitor_index)
{
    if (video_fullscreen || is_quit)
        return;

    if (vid_resize & 2)
        plat_resize(fixed_size_x, fixed_size_y, monitor_index);
    else
        plat_resize(w, h, monitor_index);
}

void
plat_resize(int w, int h, int monitor_index)
{
    if (monitor_index != 0) {
        haiku_monitor_resize(monitor_index, w, h);
        return;
    }
    if (main_window == nullptr)
        return;

    BMessage msg(MSG_RESIZE);
    msg.AddInt32("w", w);
    msg.AddInt32("h", h);
    main_window->PostMessage(&msg);
}

void
plat_mouse_capture(int on)
{
    if (!kbd_req_capture && (mouse_type == MOUSE_TYPE_NONE) && !machine_has_mouse())
        return;

    mouse_capture = (on > 0) ? 1 : 0;

    if (main_window != nullptr) {
        BMessage msg(MSG_MOUSE_CAPTURE);
        msg.AddBool("on", on > 0);
        main_window->PostMessage(&msg);
    }
}

void
ui_init_monitor(int monitor_index)
{
    haiku_monitor_init(monitor_index);
}

void
ui_deinit_monitor(int monitor_index)
{
    haiku_monitor_deinit(monitor_index);
}

void
ui_update_force_interpreter(void)
{
}

/* --------------------------------------------------------- message boxes */

int
ui_msgbox_header(int flags, char *header, char *message)
{
    const char *title = header;
    if ((title == nullptr) || (title[0] == '\0')) {
        if (flags & MBX_FATAL)
            title = "Fatal error";
        else if (flags & MBX_ERROR)
            title = "Error";
        else
            title = EMU_NAME;
    }

    alert_type type = B_INFO_ALERT;
    if (flags & (MBX_ERROR | MBX_FATAL))
        type = B_STOP_ALERT;
    else if (flags & MBX_WARNING)
        type = B_WARNING_ALERT;

    /* Yes returns 1 and No 0, with No the default, like the Qt UI. */
    if (flags & MBX_QUESTION_YN) {
        BAlert *alert = new BAlert(title, message ? message : "", "No", "Yes", nullptr,
                                   B_WIDTH_AS_USUAL, (flags & MBX_WARNING) ? B_WARNING_ALERT : B_IDEA_ALERT);
        alert->SetShortcut(0, B_ESCAPE);
        return (alert->Go() == 1) ? 1 : 0;
    }

    BAlert *alert = new BAlert(title, message ? message : "", "OK", nullptr, nullptr,
                               B_WIDTH_AS_USUAL, type);
    /* Before the window exists, or for fatal errors, wait for the user. */
    if ((main_window == nullptr) || (flags & MBX_FATAL))
        alert->Go();
    else
        alert->Go(nullptr);
    return 0;
}

int
ui_msgbox(int flags, char *message)
{
    return ui_msgbox_header(flags, nullptr, message);
}

/* snprintf returns the length it would have written, even when truncated. */
static void
ui_msg_append(char *message, size_t size, size_t *len, const char *format, ...)
{
    va_list args;
    int     written;

    if (*len >= size - 1)
        return;

    va_start(args, format);
    written = vsnprintf(message + *len, size - *len, format, args);
    va_end(args);

    if (written > 0)
        *len = ((size_t) written < size - *len) ? *len + written : size - 1;
}

int
ui_confirm_unsupported_hardware(const ui_unsupported_hardware_t *items, int count, int machine_missing)
{
    char   message[2048];
    size_t len = 0;

    if (count <= 0)
        return 1;

    message[0] = '\0';
    ui_msg_append(message, sizeof(message), &len, "%s\n\n", plat_get_string(STRING_UNSUPPORTED_TEXT));
    /* Show six names in full; for larger lists show five and the remainder. */
    const int listed = count <= 6 ? count : 5;
    for (int i = 0; i < listed; i++) {
        ui_msg_append(message, sizeof(message), &len, plat_get_string(items[i].kind), items[i].name);
        ui_msg_append(message, sizeof(message), &len, "\n");
    }
    if (count > listed) {
        ui_msg_append(message, sizeof(message), &len, plat_get_string(STRING_UNSUPPORTED_OTHERS), count - listed);
        ui_msg_append(message, sizeof(message), &len, "\n");
    }

    const int explanation = !machine_missing ? STRING_UNSUPPORTED_REMOVE :
                            count > 1 ? STRING_UNSUPPORTED_REPLACE_REMOVE : STRING_UNSUPPORTED_REPLACE;
    ui_msg_append(message, sizeof(message), &len, "\n%s\n\n%s", plat_get_string(explanation),
                  plat_get_string(STRING_UNSUPPORTED_CONTINUE));

    return ui_msgbox_header(MBX_WARNING | MBX_QUESTION_YN, plat_get_string(STRING_UNSUPPORTED_TITLE), message) == 1;
}

/* Waits until poll() reaches total. A progress window is not shown yet. */
void
ui_progress_wait(const char *, int total, int (*poll)(void *arg), void *arg)
{
    while (poll(arg) < total)
        snooze(50000);
}

/* ------------------------------------------------------------ status bar */

/* The MT-32 LCD takes the field alone; otherwise the diagnostic-card text
   comes first and the status text after it, as in the Qt UI. */
void
ui_sb_update_text(void)
{
    std::string msg;
    {
        std::lock_guard<std::mutex> guard(text_lock);
        if (!sb_mt32lcdtext.empty())
            msg = sb_mt32lcdtext;
        else if (sb_buguitext.empty())
            msg = sb_text;
        else if (sb_text.empty())
            msg = sb_buguitext;
        else
            msg = sb_buguitext + "    " + sb_text;
    }
    post_text(MSG_STATUS_TEXT, msg.c_str());
}

void
ui_sb_mt32lcd(char *str)
{
    {
        std::lock_guard<std::mutex> guard(text_lock);
        sb_mt32lcdtext = str ? str : "";
    }
    ui_sb_update_text();
}

void
ui_sb_set_text(char *str)
{
    {
        std::lock_guard<std::mutex> guard(text_lock);
        sb_text = str ? str : "";
    }
    ui_sb_update_text();
}

void
ui_sb_bugui(char *str)
{
    {
        std::lock_guard<std::mutex> guard(text_lock);
        sb_buguitext = str ? str : "";
    }
    ui_sb_update_text();
}

void
ui_sb_set_ready(int ready)
{
    if (ready == 0) {
        ui_sb_bugui(nullptr);
        ui_sb_set_text(nullptr);
    }
}

/* Tooltips are built when the pointer is over an icon, so they are always
   current. */
void
ui_sb_update_tip(int)
{
}

/* The set of drives may have changed: rebuild the status-bar icons. */
void
ui_sb_update_panes(void)
{
    if (main_window != nullptr)
        main_window->PostMessage(MSG_SB_PANES);
}

/* The icon functions record the drive state in machine_status, as the Qt
   UI does, so a status bar can draw it from there. */
void
ui_sb_update_icon_wp(int tag, int state)
{
    const int category = static_cast<int>(static_cast<unsigned int>(tag) & 0xfffffff0);
    const int item     = tag & 0xf;
    const bool on      = state > 0;

    switch (category) {
        case SB_CASSETTE:
            machine_status.cassette.write_prot = on;
            break;
        case SB_FLOPPY:
            machine_status.fdd[item].write_prot = on;
            break;
        case SB_RDISK:
            machine_status.rdisk[item].write_prot = on;
            break;
        case SB_MO:
            machine_status.mo[item].write_prot = on;
            break;
        case SB_TAPE:
            machine_status.tape[item].write_prot = on;
            break;
        default:
            break;
    }
}

void
ui_sb_update_icon_state(int tag, int state)
{
    const int category = static_cast<int>(static_cast<unsigned int>(tag) & 0xfffffff0);
    const int item     = tag & 0xf;
    const bool on      = state > 0;

    switch (category) {
        case SB_CASSETTE:
            machine_status.cassette.empty = on;
            break;
        case SB_CARTRIDGE:
            machine_status.cartridge[item].empty = on;
            break;
        case SB_FLOPPY:
            machine_status.fdd[item].empty = on;
            break;
        case SB_CDROM:
            machine_status.cdrom[item].empty = on;
            break;
        case SB_RDISK:
            machine_status.rdisk[item].empty = on;
            break;
        case SB_MO:
            machine_status.mo[item].empty = on;
            break;
        case SB_TAPE:
            machine_status.tape[item].empty = on;
            break;
        case SB_NETWORK:
            machine_status.net[item].empty = on;
            break;
        default:
            break;
    }
}

void
ui_sb_update_icon(int tag, int active)
{
    const int category = static_cast<int>(static_cast<unsigned int>(tag) & 0xfffffff0);
    const int item     = tag & 0xf;
    const bool on      = active > 0;

    switch (category) {
        case SB_FLOPPY:
            machine_status.fdd[item].active = on;
            break;
        case SB_CDROM:
            machine_status.cdrom[item].active = on;
            break;
        case SB_RDISK:
            machine_status.rdisk[item].active = on;
            break;
        case SB_MO:
            machine_status.mo[item].active = on;
            break;
        case SB_TAPE:
            machine_status.tape[item].active = on;
            break;
        case SB_HDD:
            machine_status.hdd[item].active = on;
            break;
        case SB_NETWORK:
            machine_status.net[item].active = on;
            break;
        default:
            break;
    }
}

void
ui_sb_update_icon_write(int tag, int write)
{
    const int category = static_cast<int>(static_cast<unsigned int>(tag) & 0xfffffff0);
    const int item     = tag & 0xf;
    const bool on      = write > 0;

    switch (category) {
        case SB_FLOPPY:
            machine_status.fdd[item].write_active = on;
            break;
        case SB_CDROM:
            machine_status.cdrom[item].write_active = on;
            break;
        case SB_RDISK:
            machine_status.rdisk[item].write_active = on;
            break;
        case SB_MO:
            machine_status.mo[item].write_active = on;
            break;
        case SB_TAPE:
            machine_status.tape[item].write_active = on;
            break;
        case SB_HDD:
            machine_status.hdd[item].write_active = on;
            break;
        case SB_NETWORK:
            machine_status.net[item].write_active = on;
            break;
        default:
            break;
    }
}

} /* extern "C" */

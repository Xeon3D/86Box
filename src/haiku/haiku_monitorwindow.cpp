/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the window of a non-primary monitor.
 *
 *          Created and destroyed by the main window when the core calls
 *          ui_init_monitor() / ui_deinit_monitor() (Qt's
 *          initRendererMonitorSlot / destroyRendererMonitorSlot), shown only
 *          with View -> Show non-primary monitors.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Message.h>
#include <Messenger.h>

#include <cmath>
#include <cstdio>
#include <mutex>

#include "haiku_frontend.hpp"
#include "haiku_mainwindow.hpp"
#include "haiku_monitorwindow.hpp"
#include "haiku_rendererview.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/video.h>

extern int vid_resize;
}

/* Window scale factors, indexed by the `scale` setting (as in Qt). */
static const double scale_factors[] = { 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0 };

enum {
    MSG_MONITOR_SHOW   = 'mosh', /* bool "show" */
    MSG_MONITOR_CLOSE  = 'mocl',
    MSG_MONITOR_UPDATE = 'moup',
    MSG_MONITOR_API    = 'moap', /* int32 "api" */
};

/* The windows of monitors 1 and up, by messenger: a window that already
   went away just makes a post fail. */
static BMessenger monitor_windows[MONITORS_NUM];
static std::mutex monitor_lock;

void
haiku_monitor_init(int monitor_index)
{
    if ((monitor_index < 1) || (monitor_index >= MONITORS_NUM))
        return;

    /* A BWindow can be built on any thread; its own thread starts with the
       first Show(), and Hide() first keeps it hidden. */
    MonitorWindow *window = new MonitorWindow(monitor_index);
    window->Hide();
    window->Show();

    {
        std::lock_guard<std::mutex> guard(monitor_lock);
        monitor_windows[monitor_index] = BMessenger(window);
    }

    if (show_second_monitors) {
        BMessage msg(MSG_MONITOR_SHOW);
        msg.AddBool("show", true);
        BMessenger(window).SendMessage(&msg);
    }
}

void
haiku_monitor_deinit(int monitor_index)
{
    if ((monitor_index < 1) || (monitor_index >= MONITORS_NUM))
        return;

    BMessenger target;
    {
        std::lock_guard<std::mutex> guard(monitor_lock);
        target                         = monitor_windows[monitor_index];
        monitor_windows[monitor_index] = BMessenger();
    }
    target.SendMessage(MSG_MONITOR_CLOSE);
}

void
haiku_monitor_resize(int monitor_index, int w, int h)
{
    if ((monitor_index < 1) || (monitor_index >= MONITORS_NUM))
        return;

    BMessage msg(MSG_RESIZE);
    msg.AddInt32("w", w);
    msg.AddInt32("h", h);
    std::lock_guard<std::mutex> guard(monitor_lock);
    monitor_windows[monitor_index].SendMessage(&msg);
}

void
haiku_monitors_show(bool show)
{
    BMessage msg(MSG_MONITOR_SHOW);
    msg.AddBool("show", show);
    std::lock_guard<std::mutex> guard(monitor_lock);
    for (int i = 1; i < MONITORS_NUM; i++)
        monitor_windows[i].SendMessage(&msg);
}

void
haiku_monitors_update(void)
{
    std::lock_guard<std::mutex> guard(monitor_lock);
    for (int i = 1; i < MONITORS_NUM; i++)
        monitor_windows[i].SendMessage(MSG_MONITOR_UPDATE);
}

MonitorWindow::MonitorWindow(int monitor_index)
    : BWindow(BRect(120, 120, 759, 599), "", B_TITLED_WINDOW,
              ((vid_resize == 1) ? 0 : (B_NOT_RESIZABLE | B_NOT_ZOOMABLE)) | B_ASYNCHRONOUS_CONTROLS)
    , closing(false)
    , monitor(monitor_index)
{
    char title[64];
    snprintf(title, sizeof(title), "86Box Monitor #%i", monitor_index + 1);
    SetTitle(title);

    renderer = new RendererView(Bounds(), monitor_index);
    AddChild(renderer);

    if (vid_resize == 2)
        ResizeContents(fixed_size_x, fixed_size_y);
    else
        ResizeContents(640, 480);
}

bool
MonitorWindow::QuitRequested()
{
    /* The close button hides the window; it goes away with the monitor. */
    if (!closing && !MainWindow::app_quitting) {
        RememberGeometry();
        Hide();
        return false;
    }
    return true;
}

void
MonitorWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_MONITOR_SHOW:
            if (message->GetBool("show", true))
                ShowRemembered();
            else if (!IsHidden()) {
                RememberGeometry();
                Hide();
            }
            break;
        case MSG_MONITOR_CLOSE:
            RememberGeometry();
            closing = true;
            Quit();
            break;
        case MSG_MONITOR_API:
            renderer->SwitchRenderer(message->GetInt32("api", vid_api));
            break;
        case MSG_MONITOR_UPDATE:
            renderer->UpdateDestination();
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
        default:
            BWindow::MessageReceived(message);
            break;
    }
}

void
MonitorWindow::ResizeContents(int w, int h)
{
    if (vid_resize == 1) {
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
    ResizeTo(vw - 1, vh - 1);
    renderer->UpdateDestination();
}

void
MonitorWindow::ShowRemembered()
{
    if (window_remember) {
        const auto &ms = monitor_settings[monitor];
        /* Qt keeps the window at least 120 pixels in and at most 2048 big. */
        MoveTo((ms.mon_window_x < 120) ? 120 : ms.mon_window_x, (ms.mon_window_y < 120) ? 120 : ms.mon_window_y);
        if ((vid_resize == 1) && (ms.mon_window_w > 0) && (ms.mon_window_h > 0))
            ResizeTo(((ms.mon_window_w > 2048) ? 2048 : ms.mon_window_w) - 1,
                     ((ms.mon_window_h > 2048) ? 2048 : ms.mon_window_h) - 1);
    }
    if (IsHidden())
        Show();
}

void
MonitorWindow::RememberGeometry()
{
    if (!window_remember || IsHidden())
        return;

    const BRect frame                         = Frame();
    monitor_settings[monitor].mon_window_x = static_cast<int>(frame.left);
    monitor_settings[monitor].mon_window_y = static_cast<int>(frame.top);
    monitor_settings[monitor].mon_window_w = frame.IntegerWidth() + 1;
    monitor_settings[monitor].mon_window_h = frame.IntegerHeight() + 1;
}

void
MonitorWindow::FrameMoved(BPoint where)
{
    BWindow::FrameMoved(where);
    RememberGeometry();
}

void
MonitorWindow::FrameResized(float width, float height)
{
    BWindow::FrameResized(width, height);
    RememberGeometry();
}

void
haiku_monitors_switch_renderer(int api)
{
    BMessage msg(MSG_MONITOR_API);
    msg.AddInt32("api", api);
    std::lock_guard<std::mutex> guard(monitor_lock);
    for (int i = 1; i < MONITORS_NUM; i++)
        monitor_windows[i].SendMessage(&msg);
}

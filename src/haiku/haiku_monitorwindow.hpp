/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the window of a non-primary monitor (a
 *          second video card), like Qt's secondary RendererStack windows.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_MONITORWINDOW_HPP
#define HAIKU_MONITORWINDOW_HPP

#include <Window.h>

class RendererView;

class MonitorWindow : public BWindow {
public:
    explicit MonitorWindow(int monitor_index);

    bool QuitRequested() override;
    void MessageReceived(BMessage *message) override;
    void FrameMoved(BPoint where) override;
    void FrameResized(float width, float height) override;

    /* Size the window for a w x h picture, by the main window's rules. */
    void ResizeContents(int w, int h);

    /* Show and place it as the remembered geometry says. */
    void ShowRemembered();

    /* Record the geometry in monitor_settings (when remembering). */
    void RememberGeometry();

    RendererView *Renderer() const { return renderer; }

    /* Set when the monitor goes away, so closing really closes. */
    bool closing;

private:
    int           monitor;
    RendererView *renderer;
};

/* Called from any thread (the core's ui_init_monitor, ui_deinit_monitor
   and plat_resize for monitors 1 and up); they only post messages. */
void haiku_monitor_init(int monitor_index);
void haiku_monitor_deinit(int monitor_index);
void haiku_monitor_resize(int monitor_index, int w, int h);
/* Show or hide every non-primary monitor window (show_second_monitors). */
void haiku_monitors_show(bool show);
/* Recompute the pictures' placement after a video option changed. */
void haiku_monitors_update(void);
/* Switch every monitor window to another renderer (vid_api changed). */
void haiku_monitors_switch_renderer(int vid_api);

#endif

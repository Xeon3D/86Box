/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the renderer choices offered by the main
 *          window's View > Renderer menu (Qt's actionSoftware_Renderer,
 *          actionOpenGL, actionVulkan and actionVNC) and "Renderer
 *          options...".
 *
 *          The vid_api values are the core's RENDERER_* (renderdefs.h),
 *          which are also Qt's RendererStack::Renderer values.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_RENDERERS_HPP
#define HAIKU_RENDERERS_HPP

#include <SupportDefs.h>

class BWindow;

struct haiku_renderer_choice {
    int         vid_api; /* RENDERER_SOFTWARE, RENDERER_OPENGL3, RENDERER_VNC */
    const char *name;    /* menu label */
};

/* The renderers this build offers, in menu order. */
int                   haiku_renderer_count(void);
haiku_renderer_choice haiku_renderer_at(int i);

/* Whether "Renderer options..." applies to a renderer (only OpenGL 3 has
   options: Qt's shader manager), and the dialog itself. On OK the options
   are saved and the renderer is reloaded. Call from the parent window's
   thread. */
bool haiku_renderer_options_available(int vid_api);
void haiku_renderer_options_dialog(BWindow *parent, int vid_api);

/* Recreate the current renderer so that changed options take effect (Qt's
   MainWindow::reloadAllRenderers()). Safe from any thread. */
void haiku_renderer_reload(void);

#endif

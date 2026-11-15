/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: glue for the shared on-screen display
 *          (src/osd, Dear ImGui), like qt_osd.cpp. Everything here runs on
 *          the main window's thread.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_OSD_HPP
#define HAIKU_OSD_HPP

#include <SupportDefs.h>

class BBitmap;
class BMessage;

bool haiku_osd_is_visible(void);

/* Visible, or the core still has an overlay (indicators, a message) to
   draw. Gates drawing only; input follows haiku_osd_is_visible(). */
bool haiku_osd_needs_render(void);

void haiku_osd_toggle(void);

/* Layout scale for a picture of w x h pixels. */
void haiku_osd_set_layout_scale_for(int w, int h);

/* Drop everything, also the OpenGL objects: call with the GL context
   current (or none in use), like qt_osd_shutdown(). */
void haiku_osd_shutdown(void);

/* Draw into the current OpenGL context's default framebuffer. */
void haiku_osd_render_gl(int output_w, int output_h, int gl_major, int gl_minor);

/* Draw into a transparent B_RGBA32 bitmap of w x h, to draw over the
   picture with B_OP_ALPHA. */
const BBitmap *haiku_osd_render_software(int w, int h);

/* A B_KEY_DOWN / B_KEY_UP (or unmapped) message: true when the OSD took it. */
bool haiku_osd_key(BMessage *message, bool down);

void haiku_osd_mouse_pos(float x, float y);
/* Haiku button bits: B_PRIMARY_MOUSE_BUTTON etc. */
void haiku_osd_mouse_buttons(int32 buttons);
void haiku_osd_mouse_wheel(float dx, float dy);

#endif

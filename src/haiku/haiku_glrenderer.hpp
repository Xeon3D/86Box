/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the OpenGL 3 renderer (Qt's
 *          OpenGLRenderer in a BGLView).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_GLRENDERER_HPP
#define HAIKU_GLRENDERER_HPP

class RendererBackend;
class RendererView;

/* A new OpenGL backend for `owner`, not yet attached. */
RendererBackend *haiku_gl_create(RendererView *owner);

/* Shows an error without blocking (Qt's MainWindow::showMessage(MBX_ERROR)
   in the GL code). */
void haiku_gl_message(const char *title, const char *text);

#endif

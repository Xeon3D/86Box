/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: Dear ImGui's OpenGL 3 backend for the OSD,
 *          built against Haiku's GL headers. ImGui's own loader looks for
 *          glXGetProcAddressARB, which Haiku's libGL does not have; its GL 3
 *          functions are exported directly, as haiku_glrenderer.cpp uses them.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#define IMGUI_IMPL_OPENGL_LOADER_CUSTOM
#include "imgui_impl_opengl3.cpp"

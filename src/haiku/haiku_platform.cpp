/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: platform globals and helpers that the
 *          reused src/unix platform layer does not provide (the Qt frontend
 *          keeps these in qt_platform.cpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <cstdint>
#include <chrono>
#include <mutex>
#include <strings.h>

extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/renderdefs.h>
#include <86box/runctl.h>
}

/* Globals the core expects the frontend to define (see qt_platform.cpp). */
extern "C" {
volatile int cpu_thread_run  = 1;
int          mouse_capture   = 0;
int          fixed_size_x    = 640;
int          fixed_size_y    = 480;
int          rctrl_is_lalt   = 0;
int          update_icons    = 1;
int          kbd_req_capture = 0;
int          hide_status_bar = 0;
int          hide_tool_bar   = 0;

/* The Vulkan shader list the Qt Vulkan renderer defines. The native UI has
   no Vulkan renderer; config.c still loads and saves the list, so it
   survives a run of the native UI unchanged. (gl3_shader_file is defined
   by the OpenGL renderer, haiku_glrenderer.cpp.) */
char vk_shader_file[20][512];
}

/* One recursive lock serialises renderer changes against the emulation,
   exactly like the Qt frontend's blitmx. */
static std::recursive_timed_mutex blit_lock;

extern "C" void
startblit(void)
{
    blit_lock.lock();
}

extern "C" void
endblit(void)
{
    blit_lock.unlock();
}

/* startblit() that gives up after timeout_ms; returns 1 when locked (the
   Qt frontend's, used by the memory viewer). */
extern "C" int
startblit_timeout(int timeout_ms)
{
    return blit_lock.try_lock_for(std::chrono::milliseconds(timeout_ms)) ? 1 : 0;
}

/* The emulation thread is started by haiku_main.cpp; do_stop() is called by
   the core to end it. */
extern "C" void
do_start(void)
{
}

extern "C" void
do_stop(void)
{
    runctl_request_stop();
}

/* The renderer names in the configuration are Qt's (renderdefs.h), so a
   configuration moves between the two UIs. Vulkan has no Haiku surface
   support and VNC needs a VNC build: both start the software renderer. */
extern "C" int
plat_vidapi(const char *api)
{
    if (!strcasecmp(api, RENDERER_NAME_QT_OPENGL) || !strcasecmp(api, RENDERER_NAME_QT_OPENGLES)
        || !strcasecmp(api, RENDERER_NAME_QT_OPENGL3))
        return RENDERER_OPENGL3;
#ifdef USE_VNC
    if (!strcasecmp(api, RENDERER_NAME_VNC))
        return RENDERER_VNC;
#endif

    return RENDERER_SOFTWARE;
}

extern "C" char *
plat_vidapi_name(int api)
{
    switch (api) {
        case RENDERER_OPENGL3:
            return (char *) RENDERER_NAME_QT_OPENGL3;
        case RENDERER_VULKAN:
            return (char *) RENDERER_NAME_QT_VULKAN;
        case RENDERER_VNC:
            return (char *) RENDERER_NAME_VNC;
        case RENDERER_SOFTWARE:
        default:
            return (char *) RENDERER_NAME_QT_SOFTWARE;
    }
}

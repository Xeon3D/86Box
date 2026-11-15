/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: glue for the shared on-screen display
 *          core, a port of qt_osd.cpp and qt_osd_raster.cpp. The software
 *          renderer gets the overlay rasterised into a B_RGBA32 bitmap; the
 *          OpenGL 3 renderer draws it with ImGui's OpenGL 3 backend.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Bitmap.h>
#include <Clipboard.h>
#include <InterfaceDefs.h>
#include <Message.h>
#include <String.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include "osd_core.hpp"

#include "haiku_dialogs.hpp"
#include "haiku_frontend.hpp"
#include "haiku_mainwindow.hpp"
#include "haiku_osd.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/keyboard.h>
}

/* The main window's MSG_FULLSCREEN (haiku_mainwindow.cpp). */
static const uint32 kMsgFullscreen = 'mwfs';

namespace {

using Clock = std::chrono::steady_clock;

bool              g_visible            = false;
bool              g_ctx_ready          = false;
bool              g_gl_ready           = false;
bool              g_fonts_ready        = false;
bool              g_mouse_was_captured = false;
int               g_font_pixel_size    = 0;
int32             g_buttons            = 0;
Clock::time_point g_last_frame;
BBitmap          *g_surface = nullptr;
BString           g_clipboard;

void
osd_toggle_fullscreen(void)
{
    if (main_window != nullptr)
        main_window->PostMessage(kMsgFullscreen);
}

void
osd_request_exit(void)
{
    g_visible            = false;
    g_mouse_was_captured = false;
    plat_mouse_capture(0);
    if (main_window != nullptr)
        main_window->PostMessage(B_QUIT_REQUESTED);
}

const osd_host_t haiku_osd_host = {
    osd_toggle_fullscreen,
    osd_request_exit
};

void
ensure_font_size(float scale)
{
    const float raster_scale = std::max(1.0f, scale);
    const int   base_size    = osd_core_default_font_size();
    const int   pixel_size   = std::max(base_size, (int) std::lround((float) base_size * raster_scale));

    if (pixel_size == g_font_pixel_size)
        return;

    osd_core_rebuild_default_font(pixel_size);
    g_font_pixel_size = pixel_size;
    g_fonts_ready     = false;
}

void
ensure_context(void)
{
    if (g_ctx_ready)
        return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; /* don't save layout */

    ImGuiPlatformIO &platform_io            = ImGui::GetPlatformIO();
    platform_io.Platform_SetClipboardTextFn = [](ImGuiContext *, const char *text) {
        if ((text == nullptr) || !be_clipboard->Lock())
            return;
        be_clipboard->Clear();
        BMessage *clip = be_clipboard->Data();
        if (clip != nullptr) {
            clip->AddData("text/plain", B_MIME_TYPE, text, strlen(text));
            be_clipboard->Commit();
        }
        be_clipboard->Unlock();
    };
    platform_io.Platform_GetClipboardTextFn = [](ImGuiContext *) -> const char * {
        g_clipboard.Truncate(0);
        if (be_clipboard->Lock()) {
            BMessage   *clip = be_clipboard->Data();
            const char *text = nullptr;
            ssize_t     len  = 0;
            if ((clip != nullptr) && (clip->FindData("text/plain", B_MIME_TYPE, (const void **) &text, &len) == B_OK))
                g_clipboard.SetTo(text, len);
            be_clipboard->Unlock();
        }
        return g_clipboard.Length() ? g_clipboard.String() : nullptr;
    };
    platform_io.Platform_SetImeDataFn  = nullptr;
    platform_io.Platform_OpenInShellFn = [](ImGuiContext *, const char *url) {
        haiku_open_url(url);
        return true;
    };

    osd_core_set_layout_scale(1.0f);
    osd_core_setup_style();
    ensure_font_size(1.0f);
    osd_core_set_host(&haiku_osd_host);
    osd_core_install_log_hook();

    g_ctx_ready = true;
}

void
ensure_fonts(void)
{
    if (g_fonts_ready)
        return;

    unsigned char *px = nullptr;
    int            w  = 0;
    int            h  = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    g_fonts_ready = true;
}

void
advance_frame_time(ImGuiIO &io)
{
    const auto  now = Clock::now();
    const float dt  = std::chrono::duration<float>(now - g_last_frame).count();
    g_last_frame    = now;
    io.DeltaTime    = (dt > 0.0f) ? dt : (1.0f / 60.0f);
}

void
build_frame(void)
{
    ImGui::NewFrame();
    if (g_visible) {
        if (!osd_core_build_ui()) {
            g_visible = false;
            if (g_mouse_was_captured)
                plat_mouse_capture(1);
            g_mouse_was_captured = false;
        }
    }
    osd_core_draw_indicators();
    osd_core_draw_message();
    ImGui::Render();
}

/* Haiku's raw_char for the keys the OSD navigates with (as qt_key_to_imgui). */
ImGuiKey
raw_char_to_imgui(int32 c)
{
    switch (c) {
        case B_TAB:
            return ImGuiKey_Tab;
        case B_LEFT_ARROW:
            return ImGuiKey_LeftArrow;
        case B_RIGHT_ARROW:
            return ImGuiKey_RightArrow;
        case B_UP_ARROW:
            return ImGuiKey_UpArrow;
        case B_DOWN_ARROW:
            return ImGuiKey_DownArrow;
        case B_PAGE_UP:
            return ImGuiKey_PageUp;
        case B_PAGE_DOWN:
            return ImGuiKey_PageDown;
        case B_HOME:
            return ImGuiKey_Home;
        case B_END:
            return ImGuiKey_End;
        case B_INSERT:
            return ImGuiKey_Insert;
        case B_DELETE:
            return ImGuiKey_Delete;
        case B_BACKSPACE:
            return ImGuiKey_Backspace;
        case B_SPACE:
            return ImGuiKey_Space;
        case B_ENTER:
            return ImGuiKey_Enter;
        default:
            break;
    }
    if ((c >= '0') && (c <= '9'))
        return (ImGuiKey) (ImGuiKey_0 + (c - '0'));
    if ((c >= 'a') && (c <= 'z'))
        return (ImGuiKey) (ImGuiKey_A + (c - 'a'));
    if ((c >= 'A') && (c <= 'Z'))
        return (ImGuiKey) (ImGuiKey_A + (c - 'A'));
    return ImGuiKey_None;
}

void
osd_open(void)
{
    ensure_context();
    g_visible = true;
    if ((main_window != nullptr) && main_window->LockLooper()) {
        osd_core_set_title(main_window->Title());
        main_window->UnlockLooper();
    } else
        osd_core_set_title("");
    osd_core_reset_to_menu();
    ImGui::GetIO().ClearInputKeys();
    g_last_frame = Clock::now();

    /* Free the pointer so the user can drive the menu. */
    g_mouse_was_captured = (mouse_capture != 0);
    if (g_mouse_was_captured)
        plat_mouse_capture(0);

    /* Release any guest-held keys so the toggle modifiers do not stay stuck. */
    keyboard_all_up();
}

void
osd_close(void)
{
    g_visible = false;
    if (g_mouse_was_captured)
        plat_mouse_capture(1);
    g_mouse_was_captured = false;
}

/* ---------------------------------------------------- software raster */

struct Atlas {
    const unsigned char *pixels = nullptr; /* RGBA8, row-major */
    int                  w      = 0;
    int                  h      = 0;
};

inline float
edge(float ax, float ay, float bx, float by, float cx, float cy)
{
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

inline int
clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Blend a straight-alpha source over a premultiplied B_RGBA32 pixel. */
inline void
blend_pixel(uint32_t *dst, int sr, int sg, int sb, int sa)
{
    if (sa <= 0)
        return;

    const int pr = sr * sa / 255;
    const int pg = sg * sa / 255;
    const int pb = sb * sa / 255;

    const uint32_t d   = *dst;
    const int      da  = (d >> 24) & 0xff;
    const int      dr  = (d >> 16) & 0xff;
    const int      dg  = (d >> 8) & 0xff;
    const int      db  = d & 0xff;
    const int      inv = 255 - sa;

    const int oa  = sa + da * inv / 255;
    const int orr = pr + dr * inv / 255;
    const int og  = pg + dg * inv / 255;
    const int ob  = pb + db * inv / 255;

    *dst = ((uint32_t) clampi(oa, 0, 255) << 24) | ((uint32_t) clampi(orr, 0, 255) << 16)
        | ((uint32_t) clampi(og, 0, 255) << 8) | (uint32_t) clampi(ob, 0, 255);
}

void
raster_triangle(uint32_t *base, int stride, const Atlas &atlas,
                const ImDrawVert &a, const ImDrawVert &b, const ImDrawVert &c,
                int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    const float ax = a.pos.x, ay = a.pos.y;
    const float bx = b.pos.x, by = b.pos.y;
    const float cx = c.pos.x, cy = c.pos.y;

    const float area = edge(ax, ay, bx, by, cx, cy);
    if (area == 0.0f)
        return;
    const float inv_area = 1.0f / area;

    int min_x = std::max((int) std::floor(std::min({ ax, bx, cx })), clip_x0);
    int max_x = std::min((int) std::ceil(std::max({ ax, bx, cx })), clip_x1);
    int min_y = std::max((int) std::floor(std::min({ ay, by, cy })), clip_y0);
    int max_y = std::min((int) std::ceil(std::max({ ay, by, cy })), clip_y1);
    if (min_x >= max_x || min_y >= max_y)
        return;

    const int aw = atlas.w;
    const int ah = atlas.h;

    /* ImU32 is 0xAABBGGRR. */
    const int ar = a.col & 0xff, ag = (a.col >> 8) & 0xff, ab = (a.col >> 16) & 0xff, aa = (a.col >> 24) & 0xff;
    const int br = b.col & 0xff, bg = (b.col >> 8) & 0xff, bb = (b.col >> 16) & 0xff, ba = (b.col >> 24) & 0xff;
    const int cr = c.col & 0xff, cg = (c.col >> 8) & 0xff, cb = (c.col >> 16) & 0xff, ca = (c.col >> 24) & 0xff;

    for (int y = min_y; y < max_y; y++) {
        uint32_t   *row = base + (size_t) y * stride;
        const float py  = y + 0.5f;
        for (int x = min_x; x < max_x; x++) {
            const float px = x + 0.5f;

            const float w0 = edge(bx, by, cx, cy, px, py) * inv_area;
            const float w1 = edge(cx, cy, ax, ay, px, py) * inv_area;
            const float w2 = edge(ax, ay, bx, by, px, py) * inv_area;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                continue;

            const int col_r = (int) (w0 * ar + w1 * br + w2 * cr);
            const int col_g = (int) (w0 * ag + w1 * bg + w2 * cg);
            const int col_b = (int) (w0 * ab + w1 * bb + w2 * cb);
            const int col_a = (int) (w0 * aa + w1 * ba + w2 * ca);

            int tr = 255, tg = 255, tb = 255, ta = 255;
            if (atlas.pixels && aw > 0 && ah > 0) {
                const float          u     = w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x;
                const float          v     = w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y;
                const int            sx    = clampi((int) (u * aw), 0, aw - 1);
                const int            sy    = clampi((int) (v * ah), 0, ah - 1);
                const unsigned char *texel = atlas.pixels + ((size_t) sy * aw + sx) * 4;
                tr                         = texel[0];
                tg                         = texel[1];
                tb                         = texel[2];
                ta                         = texel[3];
            }

            blend_pixel(&row[x], col_r * tr / 255, col_g * tg / 255, col_b * tb / 255, col_a * ta / 255);
        }
    }
}

void
raster_render(ImDrawData *draw_data, BBitmap *target)
{
    if (!draw_data)
        return;

    Atlas          atlas;
    unsigned char *px = nullptr;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &atlas.w, &atlas.h);
    atlas.pixels = px;

    const int tw     = target->Bounds().IntegerWidth() + 1;
    const int th     = target->Bounds().IntegerHeight() + 1;
    const int stride = target->BytesPerRow() / 4;
    auto     *base   = static_cast<uint32_t *>(target->Bits());

    for (int n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList *cmds  = draw_data->CmdLists[n];
        const ImDrawVert *verts = cmds->VtxBuffer.Data;
        const ImDrawIdx  *idx   = cmds->IdxBuffer.Data;

        for (int ci = 0; ci < cmds->CmdBuffer.Size; ci++) {
            const ImDrawCmd &cmd = cmds->CmdBuffer[ci];
            if (cmd.UserCallback) {
                cmd.UserCallback(cmds, &cmd);
                continue;
            }

            const int clip_x0 = clampi((int) cmd.ClipRect.x, 0, tw);
            const int clip_y0 = clampi((int) cmd.ClipRect.y, 0, th);
            const int clip_x1 = clampi((int) std::ceil(cmd.ClipRect.z), 0, tw);
            const int clip_y1 = clampi((int) std::ceil(cmd.ClipRect.w), 0, th);
            if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1)
                continue;

            const ImDrawIdx *tri = idx + cmd.IdxOffset;
            for (unsigned int e = 0; e + 2 < cmd.ElemCount; e += 3) {
                raster_triangle(base, stride, atlas,
                                verts[cmd.VtxOffset + tri[e + 0]],
                                verts[cmd.VtxOffset + tri[e + 1]],
                                verts[cmd.VtxOffset + tri[e + 2]],
                                clip_x0, clip_y0, clip_x1, clip_y1);
            }
        }
    }

    /* B_OP_ALPHA with B_ALPHA_OVERLAY takes straight alpha. */
    for (int y = 0; y < th; y++) {
        uint32_t *row = base + (size_t) y * stride;
        for (int x = 0; x < tw; x++) {
            const uint32_t p = row[x];
            const int      a = p >> 24;
            if ((a == 0) || (a == 255))
                continue;
            const int r = std::min(255, (int) ((p >> 16) & 0xff) * 255 / a);
            const int g = std::min(255, (int) ((p >> 8) & 0xff) * 255 / a);
            const int b = std::min(255, (int) (p & 0xff) * 255 / a);
            row[x]      = ((uint32_t) a << 24) | ((uint32_t) r << 16) | ((uint32_t) g << 8) | (uint32_t) b;
        }
    }
}

} /* namespace */

bool
haiku_osd_is_visible(void)
{
    return g_visible;
}

bool
haiku_osd_needs_render(void)
{
    return g_visible || (g_ctx_ready && osd_core_needs_render());
}

void
haiku_osd_toggle(void)
{
    if (g_visible)
        osd_close();
    else
        osd_open();
}

void
haiku_osd_set_layout_scale_for(int w, int h)
{
    ensure_context();
    float scale = osd_core_layout_scale_for_output(std::max(1, w), std::max(1, h));
    if (scale <= 0.0f)
        scale = 1.0f;
    osd_core_set_layout_scale(scale);
    ensure_font_size(scale);
}

void
haiku_osd_shutdown(void)
{
    if (!g_ctx_ready)
        return;

    if (g_gl_ready) {
        ImGui_ImplOpenGL3_Shutdown();
        g_gl_ready = false;
    }
    osd_core_remove_log_hook();
    ImGui::DestroyContext();
    g_ctx_ready       = false;
    g_fonts_ready     = false;
    g_font_pixel_size = 0;
    g_visible         = false;
    delete g_surface;
    g_surface = nullptr;
}

void
haiku_osd_render_gl(int output_w, int output_h, int gl_major, int gl_minor)
{
    ensure_context();
    if (!g_gl_ready) {
        /* GLSL 1.50 needs OpenGL 3.2; Haiku's Mesa may give 3.0 or 3.1. */
        const bool glsl150 = (gl_major > 3) || ((gl_major == 3) && (gl_minor >= 2));
        if (!ImGui_ImplOpenGL3_Init(glsl150 ? "#version 150" : "#version 130"))
            return;
        g_gl_ready = true;
    }

    ImGuiIO &io                = ImGui::GetIO();
    io.DisplaySize             = ImVec2((float) output_w, (float) output_h);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    advance_frame_time(io);

    ImGui_ImplOpenGL3_NewFrame();
    build_frame();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

const BBitmap *
haiku_osd_render_software(int w, int h)
{
    ensure_context();
    ensure_fonts();

    w = std::max(1, w);
    h = std::max(1, h);
    if ((g_surface == nullptr) || (g_surface->Bounds().IntegerWidth() + 1 != w)
        || (g_surface->Bounds().IntegerHeight() + 1 != h)) {
        delete g_surface;
        g_surface = new BBitmap(BRect(0, 0, w - 1, h - 1), B_RGBA32);
    }
    memset(g_surface->Bits(), 0, g_surface->BitsLength());

    ImGuiIO &io                = ImGui::GetIO();
    io.DisplaySize             = ImVec2((float) w, (float) h);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    advance_frame_time(io);

    build_frame();
    raster_render(ImGui::GetDrawData(), g_surface);
    return g_surface;
}

bool
haiku_osd_key(BMessage *message, bool down)
{
    if (!g_visible)
        return false;

    int32 raw = 0;
    message->FindInt32("raw_char", &raw);

    /* Escape steps back a view or closes the OSD. */
    if (raw == B_ESCAPE) {
        if (!down && osd_core_escape())
            osd_close();
        return true;
    }

    const ImGuiKey ik = raw_char_to_imgui(raw);
    if (ik != ImGuiKey_None)
        ImGui::GetIO().AddKeyEvent(ik, down);

    if (down) {
        const char *bytes = nullptr;
        if ((message->FindString("bytes", &bytes) == B_OK) && (bytes != nullptr)
            && (static_cast<unsigned char>(bytes[0]) >= 0x20) && (bytes[0] != 0x7f))
            ImGui::GetIO().AddInputCharactersUTF8(bytes);
    }

    return true; /* swallow all input while the OSD is open */
}

void
haiku_osd_mouse_pos(float x, float y)
{
    if (g_visible)
        ImGui::GetIO().AddMousePosEvent(x, y);
}

void
haiku_osd_mouse_buttons(int32 buttons)
{
    if (!g_visible)
        return;

    static const int32 bits[3] = { B_PRIMARY_MOUSE_BUTTON, B_SECONDARY_MOUSE_BUTTON, B_TERTIARY_MOUSE_BUTTON };
    for (int i = 0; i < 3; i++) {
        if ((buttons ^ g_buttons) & bits[i])
            ImGui::GetIO().AddMouseButtonEvent(i, (buttons & bits[i]) != 0);
    }
    g_buttons = buttons;
}

void
haiku_osd_mouse_wheel(float dx, float dy)
{
    if (g_visible)
        ImGui::GetIO().AddMouseWheelEvent(dx, dy);
}

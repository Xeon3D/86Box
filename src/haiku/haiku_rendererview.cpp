/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the emulated screen.
 *
 *          Frames arrive on the video blit thread through haiku_blit(). The
 *          changed rows are copied into a B_RGB32 BBitmap, whose layout
 *          matches the core's 0x00RRGGBB pixels, and the window is asked to
 *          redraw. The destination rectangle follows RendererCommon::
 *          onResize() in Qt (the fullscreen stretch modes, forced 4:3 and
 *          machine aspect ratios). A backend draws the frame there: the
 *          software one scales the bitmap with DrawBitmap() (Qt's
 *          SoftwareRenderer), the OpenGL one (haiku_glrenderer.cpp) runs
 *          Qt's GLSL shader pipeline in a child BGLView.
 *
 *          Mouse input follows RendererStack: relative motion while
 *          captured, absolute positions in the tablet modes.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Application.h>
#include <Clipboard.h>
#include <Cursor.h>
#include <InterfaceDefs.h>
#include <Region.h>
#include <Message.h>
#include <Messenger.h>
#include <Window.h>
#include <WindowScreen.h> /* set_mouse_position(), from libgame */

#include <cmath>
#include <cstring>

#include "haiku_frontend.hpp"
#include "haiku_glrenderer.hpp"
#include "haiku_renderer_backend.hpp"
#include "haiku_renderers.hpp"
#include <MessageRunner.h>

#include "haiku_osd.hpp"
#include "haiku_rendererview.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/mouse.h>
#include <86box/plat.h>
#include <86box/renderdefs.h>
#include <86box/video.h>
#ifdef USE_VNC
#    include <86box/vnc.h>
#endif
}

extern "C" int vid_resize;

/* The largest frame the core produces (bitmap_t has 2112 lines; widths stay
   within 2048). */
static const int MAX_W = 2048;
static const int MAX_H = 2112;

/* The view of each monitor; the lock keeps a view from going away while a
   blit thread draws into it. */
static RendererView *renderer_views[MONITORS_NUM] = { nullptr };
static std::mutex     views_lock;

enum : uint32 {
    MSG_RENDERER_RELOAD = 'rrld',
};

/* ------------------------------------------------------------ renderers */

static const haiku_renderer_choice renderer_choices[] = {
    { RENDERER_SOFTWARE, "Software" },
    { RENDERER_OPENGL3, "OpenGL" },
#ifdef USE_VNC
    { RENDERER_VNC, "VNC" },
#endif
};

int
haiku_renderer_count(void)
{
    return sizeof(renderer_choices) / sizeof(renderer_choices[0]);
}

haiku_renderer_choice
haiku_renderer_at(int i)
{
    if ((i < 0) || (i >= haiku_renderer_count()))
        return renderer_choices[0];

    return renderer_choices[i];
}

void
haiku_renderer_reload(void)
{
    std::lock_guard<std::mutex> guard(views_lock);
    for (RendererView *view : renderer_views) {
        if (view != nullptr)
            view->Messenger().SendMessage(MSG_RENDERER_RELOAD);
    }
}

/* The software backend: RendererView draws the bitmap itself. */
class SoftwareBackend : public RendererBackend {
public:
    explicit SoftwareBackend(RendererView *owner)
        : owner(owner)
    {
    }

    int VidApi() const override { return RENDERER_SOFTWARE; }

    void FrameReady(bool resized) override
    {
        /* A resize has already redrawn everything (UpdateDestination). */
        if (!resized)
            owner->Invalidate(owner->destination);
    }

    void Refresh() override { owner->Invalidate(); }

    void DrawInto(BRect update) override { owner->DrawSoftware(update); }

private:
    RendererView *owner;
};

/* The blit hook. It must always end with video_blit_complete_monitor(), or
   the emulation waits forever for the buffer. */
extern "C" void
haiku_blit(int x, int y, int w, int h, int monitor_index)
{
    std::lock_guard<std::mutex> guard(views_lock);

    RendererView *view = ((monitor_index >= 0) && (monitor_index < MONITORS_NUM)) ? renderer_views[monitor_index] : nullptr;
    if ((view == nullptr) || (w <= 0) || (h <= 0) || (monitors[monitor_index].target_buffer == nullptr)) {
        video_blit_complete_monitor(monitor_index);
        return;
    }

    view->Blit(x, y, w, h);
}

/* Copy a frame to the clipboard as an archived BBitmap, the form ShowImage
   and WonderBrush paste. */
void
haiku_copy_to_clipboard(const BBitmap *frame, const BRect &area)
{
    BBitmap copy(BRect(0, 0, area.IntegerWidth(), area.IntegerHeight()), B_RGB32);
    const int32 src_bpr = frame->BytesPerRow();
    const int32 dst_bpr = copy.BytesPerRow();
    const int   left    = static_cast<int>(area.left);
    const int   top     = static_cast<int>(area.top);
    for (int y = 0; y <= area.IntegerHeight(); y++)
        memcpy(static_cast<uint8 *>(copy.Bits()) + (y * dst_bpr),
               static_cast<const uint8 *>(frame->Bits()) + ((top + y) * src_bpr) + (left * 4),
               (area.IntegerWidth() + 1) * 4);

    BMessage archive;
    if (copy.Archive(&archive) != B_OK)
        return;

    if (be_clipboard->Lock()) {
        be_clipboard->Clear();
        BMessage *clip = be_clipboard->Data();
        clip->AddMessage("image/bitmap", &archive);
        clip->AddString("class", "BBitmap");
        be_clipboard->Commit();
        be_clipboard->Unlock();
    }
}

static void
integer_scale(double *d, double *g)
{
    double ratio;

    if (*d > *g) {
        ratio = std::floor(*d / *g);
        *d    = *g * ratio;
    } else {
        ratio = std::ceil(*d / *g);
        *d    = *g / ratio;
    }
}

RendererView::RendererView(BRect frame, int monitor_index)
    : BView(frame, "renderer", B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS)
    , monitor(monitor_index)
    , frame_pending(false)
    , bitmap(new BBitmap(BRect(0, 0, MAX_W - 1, MAX_H - 1), B_RGB32))
    , source(0, 0, 639, 479)
    , destination(0, 0, 639, 479)
    , linear(false)
    , maximized(false)
    , buttons(0)
    , primary_down_free(false)
    , backend(nullptr)
    , pending_api(-1)
    , vnc_active(false)
    , backend_screenshots(false)
    , last_what(0)
    , last_when(-1)
    , osd_runner(nullptr)
{
    SetViewColor(B_TRANSPARENT_COLOR);
    memset(bitmap->Bits(), 0, bitmap->BitsLength());

    std::lock_guard<std::mutex> guard(views_lock);
    renderer_views[monitor] = this;
}

RendererView::~RendererView()
{
    delete osd_runner;

    {
        std::lock_guard<std::mutex> guard(views_lock);
        if (renderer_views[monitor] == this)
            renderer_views[monitor] = nullptr;
    }

    /* The backend's view is deleted with the backend, not by ~BView. */
    if (backend != nullptr) {
        BView *child = backend->ChildView();
        if (child != nullptr)
            RemoveChild(child);
        delete backend;
        backend = nullptr;
    }

    std::lock_guard<std::mutex> guard(bitmap_lock);
    delete bitmap;
    bitmap = nullptr;
}

void
RendererView::AttachedToWindow()
{
    BView::AttachedToWindow();
    messenger = BMessenger(this, Window());
    UpdateDestination();
    SwitchRenderer((pending_api >= 0) ? pending_api : vid_api);
    pending_api = -1;
}

int
RendererView::CurrentRenderer() const
{
    if (vnc_active)
        return RENDERER_VNC;

    return (backend != nullptr) ? backend->VidApi() : RENDERER_SOFTWARE;
}

void
RendererView::SwitchRenderer(int api)
{
    if (Window() == nullptr) {
        pending_api = api;
        return;
    }
    if ((backend != nullptr) && (api == CurrentRenderer()))
        return;

#ifdef USE_VNC
    if (vnc_active && (api != RENDERER_VNC)) {
        startblit();
        vnc_close();
        video_setblit(haiku_blit);
        vnc_active = false;
        endblit();
    }
#endif

    RendererBackend *next = nullptr;
    switch (api) {
        case RENDERER_OPENGL3:
            next = haiku_gl_create(this);
            break;
#ifdef USE_VNC
        case RENDERER_VNC:
            /* The picture goes to the VNC clients; the window keeps the
               last frame, as with Qt's software renderer. */
            startblit();
            vnc_active = !!vnc_init(nullptr);
            endblit();
            break;
#endif
        default:
            break;
    }

    SetBackend(next);
}

/* Replace the backend (nullptr: software) and show the current frame. */
void
RendererView::SetBackend(RendererBackend *next)
{
    startblit();

    {
        std::lock_guard<std::mutex> guard(bitmap_lock);
        backend_screenshots = false;
    }

    if (backend != nullptr) {
        BView *child = backend->ChildView();
        if (child != nullptr)
            RemoveChild(child);
        delete backend;
        backend = nullptr;
    }

    if (next != nullptr) {
        BView *child = next->ChildView();
        if (child != nullptr) {
            child->MoveTo(0, 0);
            child->ResizeTo(Bounds().Width(), Bounds().Height());
            AddChild(child);
        }
        if (!next->Init()) {
            if (child != nullptr)
                RemoveChild(child);
            delete next;
            next = nullptr;
        }
    }
    if (next == nullptr)
        next = new SoftwareBackend(this);

    backend = next;
    {
        std::lock_guard<std::mutex> guard(bitmap_lock);
        backend_screenshots = backend->TakesScreenshots();
    }

    endblit();

    UpdateDestination();
    backend->FrameReady(true);
}

void
RendererView::Blit(int x, int y, int w, int h)
{
    bool size_changed = false;

    {
        std::lock_guard<std::mutex> guard(bitmap_lock);

        if (bitmap != nullptr) {
            /* The frame stays where it is in the core's buffer (it usually
               starts after the overscan), as in Qt; the source rectangle
               says which part to show. */
            if ((x < 0) || (y < 0) || (x >= MAX_W) || (y >= MAX_H))
                x = y = w = h = 0;
            if (x + w > MAX_W)
                w = MAX_W - x;
            if (y + h > MAX_H)
                h = MAX_H - y;

            const int32 bpr  = bitmap->BytesPerRow();
            uint8      *bits = static_cast<uint8 *>(bitmap->Bits());
            bitmap_t   *src  = monitors[monitor].target_buffer;

            for (int row = y; row < y + h; row++)
                memcpy(bits + (row * bpr) + (x * 4), &src->line[row][x], w * sizeof(uint32_t));

            if ((w > 0) && (h > 0)) {
                const BRect frame(x, y, x + w - 1, y + h - 1);
                size_changed = (frame != source);
                source       = frame;
            }

            /* The software renderer has no scaled output to save, so its
               screenshots are all taken from the frame as the machine made
               it, like Qt's raw ones (video_screenshot_monitor() counts the
               raw requests). The OpenGL renderer saves the scaled ones
               itself, as in Qt. */
            const bool scaled = !backend_screenshots;
            if (scaled && (monitors[monitor].mon_screenshots > 0)) {
                monitors[monitor].mon_screenshots--;
                monitors[monitor].mon_screenshots_raw++;
            }
            while (monitors[monitor].mon_screenshots_raw > 0)
                video_screenshot_monitor(reinterpret_cast<uint32_t *>(bits), x, y, bpr / 4, monitor);
            const bool clip     = scaled && monitors[monitor].mon_screenshots_clipboard;
            const bool raw_clip = monitors[monitor].mon_screenshots_raw_clipboard;
            if (clip || raw_clip) {
                haiku_copy_to_clipboard(bitmap, source);
                if (clip)
                    monitors[monitor].mon_screenshots_clipboard--;
                if (raw_clip)
                    monitors[monitor].mon_screenshots_raw_clipboard--;
            }
        }
    }

    /* The frame is copied; let the emulation reuse its buffer. */
    video_blit_complete_monitor(0);

    if (size_changed || !frame_pending.exchange(true)) {
        BMessage msg(MSG_FRAME);
        msg.AddBool("resized", size_changed);
        messenger.SendMessage(&msg);
    }
}

/* RendererCommon::onResize() in Qt, with a pixel ratio of 1. */
void
RendererView::UpdateDestination()
{
    const BRect bounds = Bounds();
    const int   width  = bounds.IntegerWidth() + 1;
    const int   height = bounds.IntegerHeight() + 1;
    BRect       src;
    {
        std::lock_guard<std::mutex> guard(bitmap_lock);
        src = source;
    }

    const bool apply_scale = video_fullscreen || (video_fullscreen_scale_maximized && maximized);
    const auto &mon        = monitors[monitor];
    const bool  unscaled   = !apply_scale && !dpi_scale && (vid_resize == 0) && (width >= mon.mon_scrnsz_x)
        && ((width - mon.mon_scrnsz_x) <= 1) && (height >= mon.mon_scrnsz_y)
        && ((height - mon.mon_scrnsz_y) <= 1);

    BRect dest;
    if (unscaled)
        dest.Set(0, 0, mon.mon_scrnsz_x - 1, mon.mon_scrnsz_y - 1);
    else if (!force_43 && force_device_aspect && (mon.mon_device_aspect_x > 0) && (mon.mon_device_aspect_y > 0)) {
        int dw = width;
        int dh = static_cast<int>(lround(static_cast<double>(width) * mon.mon_device_aspect_y / mon.mon_device_aspect_x));
        if (dh > height) {
            dh = height;
            dw = static_cast<int>(lround(static_cast<double>(height) * mon.mon_device_aspect_x / mon.mon_device_aspect_y));
        }
        dest.Set((width - dw) / 2, (height - dh) / 2, ((width - dw) / 2) + dw - 1, ((height - dh) / 2) + dh - 1);
    } else if (!apply_scale && !(force_43 && vid_resize))
        dest.Set(0, 0, width - 1, height - 1);
    else {
        int    mode = video_fullscreen_scale;
        double dx;
        double dy;
        double dw;
        double dh;
        double gsr;
        double hw  = width;
        double hh  = height;
        double gw  = src.Width() + 1;
        double gh  = src.Height() + 1;
        double hsr = hw / hh;
        double r43 = 4.0 / 3.0;

        if (force_43 && vid_resize && !apply_scale)
            mode = FULLSCR_SCALE_43;

        switch (mode) {
            case FULLSCR_SCALE_INT:
            case FULLSCR_SCALE_INT43:
                gsr = gw / gh;
                if (mode == FULLSCR_SCALE_INT43) {
                    gh  = gw / r43;
                    gsr = r43;
                }
                if (gsr <= hsr) {
                    dw = hh * gsr;
                    dh = hh;
                } else {
                    dw = hw;
                    dh = hw / gsr;
                }
                integer_scale(&dw, &gw);
                integer_scale(&dh, &gh);
                dx = (hw - dw) / 2.0;
                dy = (hh - dh) / 2.0;
                break;
            case FULLSCR_SCALE_INT43_VERT:
                dh = (r43 <= hsr) ? hh : (hw / r43);
                integer_scale(&dh, &gh);
                dw = std::round(dh * r43);
                dx = (hw - dw) / 2.0;
                dy = (hh - dh) / 2.0;
                break;
            case FULLSCR_SCALE_43:
            case FULLSCR_SCALE_KEEPRATIO:
                gsr = (mode == FULLSCR_SCALE_43) ? r43 : (gw / gh);
                if (gsr <= hsr) {
                    dw = hh * gsr;
                    dh = hh;
                } else {
                    dw = hw;
                    dh = hw / gsr;
                }
                dx = (hw - dw) / 2.0;
                dy = (hh - dh) / 2.0;
                break;
            case FULLSCR_SCALE_CENTER:
            case FULLSCR_SCALE_CENTER43:
                if (mode == FULLSCR_SCALE_CENTER43)
                    gh = std::round(gw / r43);
                dx = std::floor((hw - gw) / 2.0);
                dy = std::floor((hh - gh) / 2.0);
                dw = gw;
                dh = gh;
                break;
            case FULLSCR_SCALE_FULL:
            default:
                dx = 0;
                dy = 0;
                dw = hw;
                dh = hh;
                break;
        }
        dest.Set(static_cast<int>(dx), static_cast<int>(dy), static_cast<int>(dx) + static_cast<int>(dw) - 1,
                 static_cast<int>(dy) + static_cast<int>(dh) - 1);
    }

    if (dest.Width() < 1)
        dest.right = dest.left + 255;
    if (dest.Height() < 1)
        dest.bottom = dest.top + 255;

    destination = dest;

    /* Mouse movement is scaled by the size of the picture. */
    monitors[monitor].mon_picture_x = dest.IntegerWidth() + 1;
    monitors[monitor].mon_picture_y = dest.IntegerHeight() + 1;

    if (backend != nullptr)
        backend->Refresh();
    else
        Invalidate();
}

void
RendererView::FrameResized(float width, float height)
{
    BView::FrameResized(width, height);
    UpdateDestination();
}

void
RendererView::SetMaximized(bool on)
{
    maximized = on;
    UpdateDestination();
}

void
RendererView::Draw(BRect update)
{
    if (backend != nullptr)
        backend->DrawInto(update);
    else
        DrawSoftware(update);
}

void
RendererView::DrawSoftware(BRect update)
{
    /* Black around the picture. */
    SetHighColor(0, 0, 0);
    BRegion outside(update);
    outside.Exclude(destination);
    FillRegion(&outside);

    std::lock_guard<std::mutex> guard(bitmap_lock);

    if (bitmap == nullptr)
        return;

    DrawBitmap(bitmap, source, destination, linear ? B_FILTER_BITMAP_BILINEAR : 0);

    /* The OSD over the picture, at the view's resolution. */
    if ((monitor == 0) && haiku_osd_needs_render()) {
        haiku_osd_set_layout_scale_for(destination.IntegerWidth() + 1, destination.IntegerHeight() + 1);
        const BBitmap *osd = haiku_osd_render_software(Bounds().IntegerWidth() + 1, Bounds().IntegerHeight() + 1);
        if (osd != nullptr) {
            SetDrawingMode(B_OP_ALPHA);
            SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
            DrawBitmap(osd, BPoint(0, 0));
            SetDrawingMode(B_OP_COPY);
        }
    }
}

void
RendererView::OsdChanged()
{
    if (monitor != 0)
        return;

    if (osd_runner == nullptr) {
        BMessage tick(MSG_OSD_TICK);
        osd_runner = new BMessageRunner(BMessenger(this), &tick, 16000);
    }
    if (backend != nullptr)
        backend->Refresh();
    else
        Invalidate();
}

void
RendererView::SetLinearFilter(bool on)
{
    linear = on;
    if (backend != nullptr)
        backend->Refresh();
    else
        Invalidate();
}

/* Whether the current mouse message was already handled: while the mouse
   is captured, the message reaches this view as a pointer listener and the
   backend's child view, which forwards it here, as the view under the
   pointer. */
bool
RendererView::Duplicate()
{
    BMessage *message = (Window() != nullptr) ? Window()->CurrentMessage() : nullptr;
    int64     when    = 0;

    if ((message == nullptr) || (message->FindInt64("when", &when) != B_OK))
        return false;
    if ((message->what == last_what) && (when == last_when))
        return true;

    last_what = message->what;
    last_when = when;
    return false;
}

void
RendererView::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_FRAME:
            {
                frame_pending      = false;
                const bool resized = message->GetBool("resized", false);
                if ((monitor == 0) && (osd_runner == nullptr) && haiku_osd_needs_render())
                    OsdChanged();
                if (resized)
                    UpdateDestination();
                if (backend != nullptr)
                    backend->FrameReady(resized);
                else if (!resized)
                    Invalidate(destination);
                break;
            }

        case MSG_OSD_TICK:
            {
                /* One more redraw after the OSD is gone clears it. */
                const bool needed = haiku_osd_needs_render();
                if (backend != nullptr)
                    backend->Refresh();
                else
                    Invalidate();
                if (!needed) {
                    delete osd_runner;
                    osd_runner = nullptr;
                }
                break;
            }

        case MSG_RENDERER_RELOAD:
            if (CurrentRenderer() == RENDERER_OPENGL3) {
                SetBackend(nullptr);
                SwitchRenderer(RENDERER_OPENGL3);
            }
            break;

        case B_MOUSE_WHEEL_CHANGED:
            {
                if (Duplicate())
                    break;
                if ((monitor == 0) && haiku_osd_is_visible()) {
                    float wy = 0.0f;
                    float wx = 0.0f;
                    message->FindFloat("be:wheel_delta_y", &wy);
                    message->FindFloat("be:wheel_delta_x", &wx);
                    haiku_osd_mouse_wheel(-wx, -wy);
                    break;
                }
                if (!mouse_capture && (mouse_input_mode < 1))
                    break;

                float dy = 0.0f;
                float dx = 0.0f;
                message->FindFloat("be:wheel_delta_y", &dy);
                message->FindFloat("be:wheel_delta_x", &dx);
                /* Haiku counts notches, positive downwards; the core takes
                   120 per notch, positive upwards, like Qt's angleDelta. */
                if (dy != 0.0f)
                    mouse_set_z(static_cast<int>(-dy * 120.0f));
                if (dx != 0.0f)
                    mouse_set_w(static_cast<int>(-dx * 120.0f));
                break;
            }

        default:
            BView::MessageReceived(message);
            break;
    }
}

BPoint
RendererView::ScreenCenter()
{
    BRect bounds = Bounds();
    return ConvertToScreen(BPoint(floorf((bounds.left + bounds.right) / 2),
                                  floorf((bounds.top + bounds.bottom) / 2)));
}

void
RendererView::CenterPointer()
{
    BPoint center = ScreenCenter();
    set_mouse_position(static_cast<int32>(center.x), static_cast<int32>(center.y));
}

void
RendererView::SetCapture(bool on)
{
    if (on) {
        /* Keep getting pointer events when the pointer leaves the view. */
        SetEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);
        buttons = 0;
        be_app->HideCursor();
        CenterPointer();
    } else {
        SetEventMask(0, 0);
        mouse_set_buttons_ex(0);
        buttons = 0;
        be_app->ShowCursor();
    }
}

/* Haiku's button bits (primary 1, secondary 2, tertiary 4, then 8 and 16)
   are the ones mouse_set_buttons_ex() takes. */
void
RendererView::UpdateButtons(int32 now)
{
    const int32 pressed = now & ~buttons;

    buttons = now;

    if (mouse_capture && (pressed & mouse_get_release_buttons())) {
        plat_mouse_capture(0);
        return;
    }

    mouse_set_buttons_ex(now & 0x1f);
}

/* The pointer position as the 0..1 fractions of the picture that the
   tablet modes report (Qt RendererStack::event). */
void
RendererView::UpdateAbsolute(BPoint where)
{
    const double w = destination.Width() + 1;
    const double h = destination.Height() + 1;
    double       x = (where.x - destination.left) / w;
    double       y = (where.y - destination.top) / h;

    mouse_x_abs = (x < 0) ? 0 : ((x > 1) ? 1 : x);
    mouse_y_abs = (y < 0) ? 0 : ((y > 1) ? 1 : y);
    mouse_tablet_in_proximity = 1;
}

void
RendererView::MouseDown(BPoint where)
{
    if (monitor != 0)
        return;

    if (Duplicate())
        return;

    int32 now = 0;
    Window()->CurrentMessage()->FindInt32("buttons", &now);

    if (haiku_osd_is_visible()) {
        haiku_osd_mouse_pos(where.x, where.y);
        haiku_osd_mouse_buttons(now);
        return;
    }

    if (mouse_input_mode >= 1) {
        UpdateAbsolute(where);
        UpdateButtons(now);
        return;
    }
    if (mouse_capture)
        UpdateButtons(now);
    else if (now & B_PRIMARY_MOUSE_BUTTON)
        primary_down_free = true;
}

void
RendererView::MouseUp(BPoint where)
{
    if (monitor != 0)
        return;

    if (Duplicate())
        return;

    int32 now = 0;
    Window()->CurrentMessage()->FindInt32("buttons", &now);

    if (haiku_osd_is_visible()) {
        haiku_osd_mouse_pos(where.x, where.y);
        haiku_osd_mouse_buttons(now);
        return;
    }

    if (mouse_input_mode >= 1) {
        UpdateAbsolute(where);
        UpdateButtons(now);
        return;
    }
    if (mouse_capture) {
        UpdateButtons(now);
        return;
    }

    /* Only releasing the primary button, pressed over the screen while the
       mouse was free, captures it (Qt RendererStack::mouseReleaseEvent), so
       letting go of the middle or side button that just released the mouse
       does not capture it again. */
    const bool primary_released = primary_down_free && !(now & B_PRIMARY_MOUSE_BUTTON);
    if (!(now & B_PRIMARY_MOUSE_BUTTON))
        primary_down_free = false;
    if (primary_released && !dopause && Bounds().Contains(where))
        plat_mouse_capture(1);
}

void
RendererView::MouseMoved(BPoint where, uint32 transit, const BMessage *)
{
    if (monitor != 0)
        return;

    if (Duplicate())
        return;

    if (haiku_osd_is_visible()) {
        haiku_osd_mouse_pos(where.x, where.y);
        return;
    }

    if (mouse_input_mode >= 1) {
        if ((transit == B_EXITED_VIEW) || (transit == B_OUTSIDE_VIEW)) {
            mouse_tablet_in_proximity = 0;
            be_app->SetCursor(B_CURSOR_SYSTEM_DEFAULT);
        } else {
            if (transit == B_ENTERED_VIEW && mouse_input_mode == 2) {
                BCursor cross(B_CURSOR_ID_CROSS_HAIR);
                be_app->SetCursor(&cross);
            }
            UpdateAbsolute(where);
        }
        return;
    }

    if (!mouse_capture)
        return;

    const BPoint center = ScreenCenter();
    const BPoint pos    = ConvertToScreen(where);
    const int    dx     = static_cast<int>(pos.x - center.x);
    const int    dy     = static_cast<int>(pos.y - center.y);

    if ((dx == 0) && (dy == 0))
        return;

    mouse_scale(dx, dy);
    CenterPointer();
}

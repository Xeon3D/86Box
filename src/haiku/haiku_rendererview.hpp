/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the emulated screen (mouse input and the
 *          destination rectangle, like the Qt RendererStack, with a
 *          pluggable drawing backend: software or OpenGL 3).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_RENDERERVIEW_HPP
#define HAIKU_RENDERERVIEW_HPP

#include <Bitmap.h>
#include <Messenger.h>
#include <View.h>

#include <atomic>
#include <mutex>

class BMessageRunner;
class RendererBackend;

class RendererView : public BView {
public:
    /* One view per monitor; monitor 0 is the main window's. */
    explicit RendererView(BRect frame, int monitor_index = 0);
    ~RendererView() override;

    void AttachedToWindow() override;
    void Draw(BRect update) override;
    void FrameResized(float width, float height) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage *drag) override;
    void MessageReceived(BMessage *message) override;

    /* Called on the video blit thread: copies the changed area of the
       emulated screen and asks the window to redraw. */
    void Blit(int x, int y, int w, int h);

    void SetCapture(bool on);
    void SetLinearFilter(bool linear);
    /* Whether the window is zoomed (for "apply fullscreen stretch mode
       when maximized"). */
    void SetMaximized(bool on);
    /* Recompute where the picture goes, after a setting that affects it
       (also after changing the OpenGL input scale or stretch mode). */
    void UpdateDestination();

    /* Switch the drawing backend (RENDERER_* from renderdefs.h), keeping
       the current frame, like RendererStack::switchRenderer(). An OpenGL
       backend that fails to start falls back to software, as in Qt, with
       vid_api left as it is. Before the view is attached this only
       records the choice; AttachedToWindow() starts vid_api's renderer. */
    void SwitchRenderer(int vid_api);
    /* The RENDERER_* value of the backend in use. */
    int CurrentRenderer() const;

    /* The OSD opened, closed or has something to show: keep redrawing
       while it does, also when the machine is paused. */
    void OsdChanged();

    int         Monitor() const { return monitor; }
    BMessenger &Messenger() { return messenger; }

    /* For the drawing backends (window thread). The frame is read under
       FrameLock(). */
    std::mutex    &FrameLock() { return bitmap_lock; }
    const BBitmap *Frame() const { return bitmap; }
    BRect          Source() const { return source; }
    BRect          Destination() const { return destination; }

private:
    friend class SoftwareBackend;

    void   DrawSoftware(BRect update);
    bool   Duplicate();
    void   CenterPointer();
    void   UpdateButtons(int32 buttons);
    void   UpdateAbsolute(BPoint where);
    BPoint ScreenCenter();
    void   SetBackend(RendererBackend *next);

    int              monitor;
    BMessenger       messenger;
    std::atomic_bool frame_pending; /* one MSG_FRAME in flight at most */
    BBitmap         *bitmap;
    std::mutex       bitmap_lock;
    BRect            source;      /* area of the bitmap holding the current frame */
    BRect            destination; /* where the frame is drawn in the view */
    bool             linear;
    bool             maximized;
    int32            buttons;     /* last host buttons seen */
    /* The primary button went down over the view while the mouse was free
       (Qt's isMouseDown & 1): only its release captures the mouse. */
    bool             primary_down_free;
    RendererBackend *backend;
    int              pending_api; /* renderer to start once attached */
    bool             vnc_active;
    /* Whether the backend saves the scaled screenshots; read by Blit(). */
    std::atomic_bool backend_screenshots;
    /* The last mouse message handled: a child view's forwarded copy and the
       one delivered to this view as a pointer listener are the same event. */
    uint32 last_what;
    int64  last_when;
    /* Redraws the OSD about 60 times a second while it needs drawing. */
    BMessageRunner *osd_runner;
};

#endif

/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the interface between RendererView and
 *          its drawing backends (Qt's RendererCommon).
 *
 *          RendererView owns the frame (a B_RGB32 BBitmap filled on the
 *          blit thread), the destination rectangle and all input; a backend
 *          only draws. Every backend method runs on the window thread with
 *          the window locked.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_RENDERER_BACKEND_HPP
#define HAIKU_RENDERER_BACKEND_HPP

#include <Rect.h>

class BBitmap;
class BView;

class RendererBackend {
public:
    virtual ~RendererBackend() = default;

    /* The RENDERER_* value this backend implements. */
    virtual int VidApi() const = 0;

    /* A view to lay over the whole RendererView (it must forward mouse
       input to its parent), or nullptr to draw in RendererView::Draw(). */
    virtual BView *ChildView() { return nullptr; }

    /* Called once the child view is attached. Returning false makes
       RendererView fall back to the software backend. */
    virtual bool Init() { return true; }

    /* Whether the backend saves the screenshots and clipboard copies taken
       of the scaled picture (mon_screenshots / mon_screenshots_clipboard);
       otherwise they are taken from the raw frame. */
    virtual bool TakesScreenshots() const { return false; }

    /* A new frame is in the bitmap; `resized` if its size changed. */
    virtual void FrameReady(bool resized) = 0;

    /* The destination rectangle or a drawing option (filtering, input
       scale) changed. */
    virtual void Refresh() = 0;

    /* For backends without a child view. */
    virtual void DrawInto(BRect) { }
};

/* Copies the part `area` of a B_RGB32 frame to the clipboard as an archived
   BBitmap (haiku_rendererview.cpp). */
void haiku_copy_to_clipboard(const BBitmap *frame, const BRect &area);

#endif

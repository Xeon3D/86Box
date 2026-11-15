/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the tool bar.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <Bitmap.h>
#include <Control.h>
#include <ControlLook.h>
#include <String.h>
#include <Window.h>

#include <cmath>

#include "haiku_icons.hpp"
#include "haiku_toolbar.hpp"

static const float BUTTON_SIZE = 24;
static const float ICON_SIZE   = 16;
static const float SPACING     = 2;

/* A flat button showing an icon, like a QToolButton in a QToolBar. */
class IconButton : public BControl {
public:
    IconButton(BRect frame, const char *icon, const char *tip, uint32 what)
        : BControl(frame, icon, nullptr, new BMessage(what), B_FOLLOW_LEFT | B_FOLLOW_TOP, B_WILL_DRAW)
        , what(what)
        , bitmap(haiku_icon(icon))
        , inside(false)
        , pressed(false)
        , marked(false)
    {
        SetToolTip(tip);
    }

    void AttachedToWindow() override
    {
        BControl::AttachedToWindow();
        AdoptParentColors();
        SetTarget(Window());
    }

    void Draw(BRect) override
    {
        BRect      rect = Bounds();
        rgb_color  base = ViewColor();
        uint32     flags = 0;

        if (pressed || marked)
            flags |= BControlLook::B_ACTIVATED;
        if (!IsEnabled())
            flags |= BControlLook::B_DISABLED;

        if (inside || pressed || marked)
            be_control_look->DrawButtonFrame(this, rect, rect, base, base, flags);
        if (pressed || marked)
            be_control_look->DrawButtonBackground(this, rect, rect, base, flags);

        if (bitmap != nullptr) {
            const float x = floorf((Bounds().Width() - ICON_SIZE) / 2);
            const float y = floorf((Bounds().Height() - ICON_SIZE) / 2);
            SetDrawingMode(B_OP_ALPHA);
            SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
            DrawBitmap(bitmap, BPoint(x, y));
            SetDrawingMode(B_OP_COPY);
        }
    }

    void MouseDown(BPoint) override
    {
        if (!IsEnabled())
            return;
        pressed = true;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        Invalidate();
    }

    void MouseUp(BPoint where) override
    {
        if (pressed) {
            pressed = false;
            Invalidate();
            if (Bounds().Contains(where))
                Invoke();
        }
    }

    void MouseMoved(BPoint where, uint32 transit, const BMessage *) override
    {
        const bool now = (transit != B_EXITED_VIEW) && (transit != B_OUTSIDE_VIEW) && Bounds().Contains(where);
        if (now != inside) {
            inside = now;
            Invalidate();
        }
    }

    void SetIcon(const char *icon)
    {
        bitmap = haiku_icon(icon);
        Invalidate();
    }

    void SetMarked(bool on)
    {
        if (on != marked) {
            marked = on;
            Invalidate();
        }
    }

    uint32 what;

private:
    BBitmap *bitmap;
    bool     inside;
    bool     pressed;
    bool     marked;
};

ToolBarView::ToolBarView(BRect frame)
    : BView(frame, "tool_bar", B_FOLLOW_LEFT_RIGHT | B_FOLLOW_TOP, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE)
    , next_x(SPACING + 2)
{
}

float
ToolBarView::PreferredHeight()
{
    return BUTTON_SIZE + (2 * SPACING) + 1;
}

void
ToolBarView::AttachedToWindow()
{
    BView::AttachedToWindow();
    SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
    SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
    SetHighUIColor(B_PANEL_TEXT_COLOR);
}

void
ToolBarView::AddButton(const char *icon, const char *tip, uint32 what)
{
    IconButton *button = new IconButton(BRect(next_x, SPACING, next_x + BUTTON_SIZE - 1, SPACING + BUTTON_SIZE - 1),
                                        icon, tip, what);
    AddChild(button);
    buttons.push_back(button);
    next_x += BUTTON_SIZE + SPACING;
}

void
ToolBarView::AddSeparator()
{
    separators.push_back(next_x + 2);
    next_x += 6;
}

void
ToolBarView::SetButtonIcon(uint32 what, const char *icon, const char *tip)
{
    for (IconButton *button : buttons) {
        if (button->what == what) {
            button->SetIcon(icon);
            button->SetToolTip(tip);
        }
    }
}

void
ToolBarView::SetButtonMarked(uint32 what, bool marked)
{
    for (IconButton *button : buttons) {
        if (button->what == what)
            button->SetMarked(marked);
    }
}

void
ToolBarView::SetText(const char *str)
{
    text = str ? str : "";
    Invalidate();
}

void
ToolBarView::FrameResized(float width, float height)
{
    BView::FrameResized(width, height);
    Invalidate();
}

void
ToolBarView::Draw(BRect)
{
    const BRect bounds = Bounds();

    /* Separators between button groups. */
    SetHighUIColor(B_PANEL_BACKGROUND_COLOR, B_DARKEN_2_TINT);
    for (float x : separators)
        StrokeLine(BPoint(x, SPACING + 3), BPoint(x, SPACING + BUTTON_SIZE - 4));

    /* A line along the bottom, as under a menu bar. */
    StrokeLine(BPoint(bounds.left, bounds.bottom), BPoint(bounds.right, bounds.bottom));

    /* The status text, after the buttons, cut to fit. */
    if (!text.empty()) {
        SetHighUIColor(B_PANEL_TEXT_COLOR);
        BFont font;
        GetFont(&font);
        font_height fh;
        font.GetHeight(&fh);

        const float left  = next_x + 8;
        const float width = bounds.right - 6 - left;
        if (width > 10) {
            BString str(text.c_str());
            font.TruncateString(&str, B_TRUNCATE_END, width);
            const float y = floorf((bounds.Height() + fh.ascent - fh.descent) / 2);
            DrawString(str.String(), BPoint(left, y));
        }
    }
}

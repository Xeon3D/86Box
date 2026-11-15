/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: a menu item with an icon.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <Bitmap.h>
#include <Menu.h>

#include <cmath>

#include "haiku_icons.hpp"
#include "haiku_iconmenuitem.hpp"

static const float kIconSize = 16.0f;
static const float kIconGap  = 6.0f;

IconMenuItem::IconMenuItem(const char *label, BMessage *message, BBitmap *bitmap, char shortcut, uint32 modifiers)
    : BMenuItem(label, message, shortcut, modifiers)
    , icon(bitmap)
{
}

IconMenuItem::IconMenuItem(BMenu *submenu, BBitmap *bitmap)
    : BMenuItem(submenu)
    , icon(bitmap)
{
}

void
IconMenuItem::SetIcon(BBitmap *bitmap)
{
    if (bitmap == icon)
        return;
    icon = bitmap;
    if (Menu() != nullptr)
        Menu()->Invalidate(Frame());
}

void
IconMenuItem::GetContentSize(float *width, float *height)
{
    BMenuItem::GetContentSize(width, height);
    *width += kIconSize + kIconGap;
    if (*height < kIconSize)
        *height = kIconSize;
}

void
IconMenuItem::DrawContent()
{
    BMenu *menu  = Menu();
    BPoint where = ContentLocation();

    if (icon != nullptr) {
        /* Disabled items get the faded copy, as Qt draws them. */
        BBitmap     *shown = IsEnabled() ? icon : haiku_icon_disabled(icon);
        const BRect  frame = Frame();
        const BPoint at(where.x, floorf(frame.top + ((frame.Height() - kIconSize + 1) / 2)));

        const drawing_mode mode = menu->DrawingMode();
        menu->SetDrawingMode(B_OP_ALPHA);
        menu->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        menu->DrawBitmapAsync(shown != nullptr ? shown : icon, at);
        menu->SetDrawingMode(mode);
    }

    /* The label after the icon's room. */
    menu->MovePenTo(where.x + kIconSize + kIconGap, where.y);
    BMenuItem::DrawContent();
}

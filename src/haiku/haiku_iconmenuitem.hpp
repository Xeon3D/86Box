/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: a menu item with a 16x16 icon before its
 *          label, for the icons Qt shows in its menus (BMenuItem has none).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_ICONMENUITEM_HPP
#define HAIKU_ICONMENUITEM_HPP

#include <MenuItem.h>

class BBitmap;

class IconMenuItem : public BMenuItem {
public:
    /* The icon is not owned (icons come from haiku_icon() and friends,
       which keep them); nullptr leaves the icon's room empty. */
    IconMenuItem(const char *label, BMessage *message, BBitmap *icon, char shortcut = 0, uint32 modifiers = 0);
    IconMenuItem(BMenu *submenu, BBitmap *icon);

    void SetIcon(BBitmap *icon);

protected:
    void GetContentSize(float *width, float *height) override;
    void DrawContent() override;

private:
    BBitmap *icon;
};

#endif

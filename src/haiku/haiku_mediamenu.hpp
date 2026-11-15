/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Media menu, like the Qt MediaMenu
 *          (qt_mediamenu.cpp): one submenu per cassette, cartridge, floppy,
 *          CD-ROM, removable disk, MO and tape drive, with new/existing
 *          image, recent images, eject and the other actions Qt offers;
 *          one per network card, USB device and USB port.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_MEDIAMENU_HPP
#define HAIKU_MEDIAMENU_HPP

#include <SupportDefs.h>

class BMenu;
class BMessage;
class BPoint;
class BWindow;

/* Media types, also used by the status bar. */
enum haiku_media_type {
    HAIKU_MEDIA_CASSETTE,
    HAIKU_MEDIA_CARTRIDGE,
    HAIKU_MEDIA_FLOPPY,
    HAIKU_MEDIA_CDROM,
    HAIKU_MEDIA_RDISK,
    HAIKU_MEDIA_MO,
    HAIKU_MEDIA_TAPE,
};

class MediaMenu {
public:
    /* `window` is the main window: menu items and file panels send their
       messages there, and it passes them to HandleMessage(). */
    explicit MediaMenu(BWindow *window);
    ~MediaMenu();

    /* Refill the top-level Media menu from the configured drives (called
       when the menu bar opens and after a settings change). */
    void Rebuild(BMenu *media_menu);

    /* The submenu for one drive, as a pop-up at a screen point (clicking a
       status-bar icon). */
    void PopUp(haiku_media_type type, int id, BPoint screen_where);

    /* The working USB ports, one submenu each, as a pop-up at a screen
       point (clicking the status bar's USB icon). Nothing opens when no
       port works. */
    void PopUpUsb(BPoint screen_where);

    /* A network card's submenu ("Connected"), as a pop-up at a screen
       point (clicking its status-bar icon). */
    void PopUpNic(int id, BPoint screen_where);

    /* Mount the file or folder `path` in a drive, as dropping it on the
       drive's status-bar icon does in Qt. Returns false when the drive does
       not exist or the path is empty. */
    bool Mount(haiku_media_type type, int id, const char *path);

    /* Whether a drive exists and has a menu, by the rules of Qt's
       MachineStatus::iterate*(); the status bar shows the same drives. */
    static bool DrivePresent(haiku_media_type type, int id);

    /* Messages of the media menu ('mm..' codes) and of its file panels.
       Returns true if the message was the menu's. Runs on the window's
       thread. */
    bool HandleMessage(BMessage *message);

private:
    struct Impl;
    Impl *impl;
};

#endif

/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the icons in the status bar, like the Qt
 *          MachineStatus: cassette, cartridges, floppy, CD-ROM, removable
 *          disk, MO and tape drives, hard disk buses, network cards, the
 *          USB ports, board LEDs, sound, the dynamic recompiler and the
 *          keyboard lock LEDs. Drives are dimmed when empty and show their
 *          activity lights; clicking one opens its media menu, and dropping
 *          a file on one mounts it.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_MACHINESTATUS_HPP
#define HAIKU_MACHINESTATUS_HPP

#include <View.h>

#include <vector>

class BBitmap;
struct usb_port_info_t;

class MachineStatusView : public BView {
public:
    /* lock_leds: the view of the Num/Caps/Scroll/Kana Lock LEDs only, at
       the right end of the status bar (Qt's permanent widgets); the
       other view has every icon but those. */
    explicit MachineStatusView(BRect frame, bool lock_leds = false);
    ~MachineStatusView() override;

    void AttachedToWindow() override;
    void Draw(BRect update) override;
    void Pulse() override;
    void MouseDown(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage *drag) override;
    void MessageReceived(BMessage *message) override;

    /* Rebuild the icons from the configured devices (ui_sb_update_panes). */
    void Refresh();

    /* Width needed for the current icons. */
    float ContentWidth() const;

private:
    enum Kind {
        KIND_CASSETTE,
        KIND_CARTRIDGE,
        KIND_FLOPPY,
        KIND_CDROM,
        KIND_RDISK,
        KIND_MO,
        KIND_TAPE,
        KIND_HDD,
        KIND_NETWORK,
        KIND_USB, /* one icon for all the ports */
        KIND_BOARD_LED,
        KIND_SOUND,
        KIND_DYNAREC,
        KIND_LOCK_LED, /* id: 0 Num, 1 Caps, 2 Scroll, 3 Kana */
    };

    struct Item {
        Kind     kind;
        int      id;    /* drive number, hard-disk bus, card, LED */
        BBitmap *icon;
        BBitmap *icon_off;
        int      state; /* last drawn state, see ItemState() */
        float    x;
    };

    int   ItemState(const Item &item) const;
    int   ItemAt(BPoint where) const;
    BRect ItemRect(const Item &item) const;
    void  ToolTipFor(const Item &item, char *buf, size_t len) const;
    int   DropTarget(BPoint where, const BMessage *drag) const;
    void  SetDropItem(int i);
    bool  UsbWorking();

    /* The media menu's type (haiku_media_type) for a drive's icon; false
       for an icon that is not a drive. */
    static bool MediaTypeOf(Kind kind, int *type);

    bool                         lock_leds;
    std::vector<Item>            items;
    int                          tip_item;
    int                          drop_item; /* the drive a file is dragged over */
    bool                         usb_shown;
    std::vector<usb_port_info_t> usb_ports; /* a buffer for usb_ports_get() */
};

#endif

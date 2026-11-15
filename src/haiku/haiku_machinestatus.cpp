/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the icons in the status bar.
 *
 *          The core records drive state in machine_status (see haiku_ui.cpp);
 *          like the Qt status bar, this view polls it, here on the window's
 *          pulse, and redraws only the icons whose state changed. The order
 *          and the rules for which icons appear follow
 *          MachineStatus::refresh() in Qt; a drive has an icon exactly when
 *          the media menu has a submenu for it.
 *
 *          Dropping a file from Tracker on a drive's icon mounts it there,
 *          like Qt's ClickableLabel::dropped handlers.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Bitmap.h>
#include <Entry.h>
#include <MenuItem.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Window.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "haiku_frontend.hpp"
#include "haiku_icons.hpp"
#include "haiku_machinestatus.hpp"
#include "haiku_mainwindow.hpp"
#include "haiku_mediamenu.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/ui.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/fdd.h>
#include <86box/hdc.h>
#include <86box/scsi.h>
#include <86box/scsi_device.h>
#include <86box/cartridge.h>
#include <86box/cassette.h>
#include <86box/cdrom.h>
#include <86box/rdisk.h>
#include <86box/mo.h>
#include <86box/scsi_tape.h>
#include <86box/hdd.h>
#include <86box/keyboard.h>
#include <86box/network.h>
#include <86box/board_led.h>
#include <86box/cpu_events.h>
#include <86box/machine_status.h>
#include <86box/runctl.h>
#include <86box/sound.h>
#include <86box/mem.h>
#include <86box/usb.h>
#include "../cpu/cpu.h"
}

static const float ICON_SIZE = 16;
static const float ITEM_W    = 20;

enum {
    STATE_EMPTY        = 1,
    STATE_ACTIVE       = 2,
    STATE_WRITE_ACTIVE = 4,
    STATE_WRITE_PROT   = 8,
    STATE_OFF          = 16, /* LED off, sound muted, dynarec inactive */
};

static const char *
hdd_bus_name(int bus)
{
    switch (bus) {
        case HDD_BUS_MFM:
            return "MFM/RLL";
        case HDD_BUS_XTA:
            return "XTA";
        case HDD_BUS_ESDI:
            return "ESDI";
        case HDD_BUS_LPT:
            return "Parallel port";
        case HDD_BUS_IDE:
            return "IDE";
        case HDD_BUS_ATAPI:
            return "ATAPI";
        case HDD_BUS_SCSI:
            return "SCSI";
        default:
            return "Other";
    }
}

static int
hdd_count(int bus)
{
    int count = 0;
    for (int i = 0; i < HDD_SLOTS; i++) {
        if (hdd[i].bus_type == bus)
            count++;
    }
    return count;
}

/* Whether the machine has a controller for a hard-disk bus, as Qt's
   MachineStatus::refresh() decides it. */
static bool
hdd_bus_shown(int bus)
{
    const char *name = hdc_get_internal_name(hdc_current[0]);
    if (name == nullptr)
        name = "";

    switch (bus) {
        case HDD_BUS_MFM:
            return (machine_has_flags(machine, MACHINE_MFM) > 0) || !strncmp(name, "st506", 5);
        case HDD_BUS_ESDI:
            return (machine_has_flags(machine, MACHINE_ESDI) > 0) || !strncmp(name, "esdi", 4);
        case HDD_BUS_XTA:
            return (machine_has_flags(machine, MACHINE_XTA) > 0) || !strncmp(name, "xta", 3);
        case HDD_BUS_IDE:
        case HDD_BUS_ATAPI:
            return (machine_has_flags(machine, MACHINE_IDE_QUAD) > 0) || other_ide_present
                || !strncmp(name, "xtide", 5) || !strncmp(name, "jride", 5) || !strncmp(name, "mcide", 5)
                || !strncmp(name, "ide", 3);
        case HDD_BUS_SCSI:
            return (machine_has_flags(machine, MACHINE_SCSI) > 0) || other_scsi_present || (scsi_card_current[0] != 0)
                || (scsi_card_current[1] != 0) || (scsi_card_current[2] != 0) || (scsi_card_current[3] != 0);
        default:
            return false;
    }
}

static const char *
rdisk_icon(int i)
{
    const int t = rdisk_drives[i].type;
    if ((t == RDISK_TYPE_ZIP_100) || (t == RDISK_TYPE_ZIP_250))
        return "zip";
    if ((t == RDISK_TYPE_JAZ_1GB) || (t == RDISK_TYPE_JAZ_2GB))
        return "jaz";
    if ((t == RDISK_TYPE_SYJET_1_5GB) || (t == RDISK_TYPE_SPARQ_1GB))
        return "syquest";
    return "rdisk";
}

/* MainWindow::hasKanaLock() in Qt: only the AX keyboard has a Kana Lock
   key. A machine that ships one has the indicator whatever the bus, any
   other machine has it while an AX keyboard is selected on a keyboard
   port. */
static bool
has_kana_lock()
{
    if (machine_get_kbd_device(machine) == &keyboard_ax_device)
        return true;

    return machine_has_bus(machine, MACHINE_BUS_PS2_PORTS | MACHINE_BUS_AT_KBD) && (keyboard_type == KEYBOARD_TYPE_AX);
}

MachineStatusView::MachineStatusView(BRect frame, bool locks)
    : BView(frame, locks ? "lock_leds" : "machine_status", (locks ? B_FOLLOW_RIGHT : B_FOLLOW_LEFT) | B_FOLLOW_BOTTOM,
            B_WILL_DRAW | B_PULSE_NEEDED)
    , lock_leds(locks)
    , tip_item(-1)
    , drop_item(-1)
    , usb_shown(false)
    , usb_ports(USB_PORTS_MAX)
{
    Refresh();
}

MachineStatusView::~MachineStatusView()
{
}

void
MachineStatusView::AttachedToWindow()
{
    BView::AttachedToWindow();
    AdoptParentColors();
    /* Activity lights are polled ten times a second. */
    Window()->SetPulseRate(100000);
}

/* Whether any USB port works: Qt shows its USB icon only then. */
bool
MachineStatusView::UsbWorking()
{
    const int n = usb_ports_get(usb_ports.data(), USB_PORTS_MAX);

    for (int k = 0; (k < n) && (k < USB_PORTS_MAX); k++) {
        if (usb_ports[k].state != USB_PORT_OFF)
            return true;
    }
    return false;
}

void
MachineStatusView::Refresh()
{
    items.clear();
    tip_item  = -1;
    drop_item = -1;

    auto add = [this](Kind kind, int id, const char *icon, const char *icon_off = nullptr) {
        items.push_back({ kind, id, haiku_icon(icon), icon_off ? haiku_icon(icon_off) : nullptr, -1, 0 });
    };

    /* The drives the media menu has, by Qt's iterate*() rules. */
    auto present = [](haiku_media_type type, int i) { return MediaMenu::DrivePresent(type, i); };

    if (present(HAIKU_MEDIA_CASSETTE, 0))
        add(KIND_CASSETTE, 0, "cassette");

    for (int i = 0; i < 2; i++) {
        if (present(HAIKU_MEDIA_CARTRIDGE, i))
            add(KIND_CARTRIDGE, i, "cartridge");
    }

    for (int i = 0; i < FDD_NUM; i++) {
        if (present(HAIKU_MEDIA_FLOPPY, i))
            add(KIND_FLOPPY, i, fdd_is_525(&drives[i]) ? "floppy_525" : "floppy_35");
    }

    for (int i = 0; i < CDROM_NUM; i++) {
        if (present(HAIKU_MEDIA_CDROM, i))
            add(KIND_CDROM, i, "cdrom");
    }

    for (int i = 0; i < RDISK_NUM; i++) {
        if (present(HAIKU_MEDIA_RDISK, i))
            add(KIND_RDISK, i, rdisk_icon(i));
    }

    for (int i = 0; i < MO_NUM; i++) {
        if (present(HAIKU_MEDIA_MO, i))
            add(KIND_MO, i, "mo");
    }

    for (int i = 0; i < TAPE_NUM; i++) {
        if (present(HAIKU_MEDIA_TAPE, i))
            add(KIND_TAPE, i, "tape");
    }

    for (int i = 0; i < NET_CARD_MAX; i++) {
        if (network_dev_available(i))
            add(KIND_NETWORK, i, "network");
    }

    /* One USB icon for all the ports, while any of them works; Pulse()
       has the icons made again when that changes. */
    usb_shown = UsbWorking();
    if (usb_shown)
        add(KIND_USB, 0, "usb_port");

    /* One icon per hard-disk bus with disks on it and a controller for
       it, by the rules and in the order of Qt's MachineStatus::refresh(). */
    for (int bus : { HDD_BUS_MFM, HDD_BUS_ESDI, HDD_BUS_XTA, HDD_BUS_IDE, HDD_BUS_ATAPI, HDD_BUS_SCSI }) {
        if ((hdd_count(bus) > 0) && hdd_bus_shown(bus))
            add(KIND_HDD, bus, "hard_disk");
    }

    for (int i = 0; i < board_led_count(); i++)
        add(KIND_BOARD_LED, i, nullptr);

    add(KIND_SOUND, 0, "sound");
#ifdef USE_DYNAREC
    if (cpu_use_dynarec)
        add(KIND_DYNAREC, 0, "recompiler", "interpreter");
#endif

    /* Lock LEDs only on machines with an AT or PS/2 keyboard port; the
       Kana Lock LED only with an AX keyboard. */
    if (machine_has_bus(machine, MACHINE_BUS_PS2_PORTS | MACHINE_BUS_AT_KBD)) {
        add(KIND_LOCK_LED, 0, "num_lock_on", "num_lock_off");
        add(KIND_LOCK_LED, 1, "caps_lock_on", "caps_lock_off");
        add(KIND_LOCK_LED, 2, "scroll_lock_on", "scroll_lock_off");
    }
    if (has_kana_lock())
        add(KIND_LOCK_LED, 3, "kana_lock_on", "kana_lock_off");

    /* The lock LEDs have a view of their own, at the right end of the
       status bar like Qt's permanent widgets. */
    std::vector<Item> kept;
    for (const Item &item : items) {
        if ((item.kind == KIND_LOCK_LED) == lock_leds)
            kept.push_back(item);
    }
    items.swap(kept);

    float x = 2;
    for (Item &item : items) {
        item.x = x;
        x += ITEM_W;
    }

    Invalidate();
}

float
MachineStatusView::ContentWidth() const
{
    return items.empty() ? 0 : (items.back().x + ITEM_W + 2);
}

BRect
MachineStatusView::ItemRect(const Item &item) const
{
    const float top = floorf((Bounds().Height() - ICON_SIZE) / 2);
    return BRect(item.x, top, item.x + ICON_SIZE - 1, top + ICON_SIZE - 1);
}

int
MachineStatusView::ItemState(const Item &item) const
{
    auto drive = [](const dev_status_empty_active_t &s) {
        return (s.empty ? STATE_EMPTY : 0) | (s.active ? STATE_ACTIVE : 0) | (s.write_active ? STATE_WRITE_ACTIVE : 0)
            | (s.write_prot ? STATE_WRITE_PROT : 0);
    };

    switch (item.kind) {
        case KIND_CASSETTE:
            return drive(machine_status.cassette);
        case KIND_CARTRIDGE:
            return machine_status.cartridge[item.id].empty ? STATE_EMPTY : 0;
        case KIND_FLOPPY:
            return drive(machine_status.fdd[item.id]);
        case KIND_CDROM:
            return drive(machine_status.cdrom[item.id]) & ~STATE_WRITE_PROT;
        case KIND_RDISK:
            return drive(machine_status.rdisk[item.id]);
        case KIND_MO:
            return drive(machine_status.mo[item.id]);
        case KIND_TAPE:
            return drive(machine_status.tape[item.id]);
        case KIND_HDD:
            return (machine_status.hdd[item.id].active ? STATE_ACTIVE : 0)
                | (machine_status.hdd[item.id].write_active ? STATE_WRITE_ACTIVE : 0);
        case KIND_NETWORK:
            return drive(machine_status.net[item.id]) & ~STATE_WRITE_PROT;
        case KIND_USB:
            return 0;
        case KIND_BOARD_LED:
            return board_led_get(item.id) ? 0 : STATE_OFF;
        case KIND_SOUND:
            return (sound_muted || (runctl_mode() == RUN_MODE_FAST)) ? STATE_OFF : 0;
        case KIND_DYNAREC:
            return is_dynarec_active() ? 0 : STATE_OFF;
        case KIND_LOCK_LED:
            {
                uint8_t caps   = 0;
                uint8_t num    = 0;
                uint8_t scroll = 0;
                uint8_t kana   = 0;
                keyboard_get_states(&caps, &num, &scroll, &kana);
                uint8_t on;
                switch (item.id) {
                    case 0:
                        on = num;
                        break;
                    case 1:
                        on = caps;
                        break;
                    case 2:
                        on = scroll;
                        break;
                    default:
                        on = kana;
                        break;
                }
                return on ? 0 : STATE_OFF;
            }
    }
    return 0;
}

void
MachineStatusView::Draw(BRect)
{
    SetDrawingMode(B_OP_ALPHA);
    SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);

    for (Item &item : items) {
        const int   state = ItemState(item);
        const BRect rect  = ItemRect(item);

        item.state = state;

        /* Board LEDs are drawn, as in Qt. */
        if (item.kind == KIND_BOARD_LED) {
            const rgb_color on  = { 0x20, 0xe0, 0x20, 0xff };
            const rgb_color off = { 0x20, 0x50, 0x20, 0xff };
            SetHighColor((state & STATE_OFF) ? off : on);
            FillEllipse(rect.InsetByCopy(4, 4));
            SetHighColor(0, 0, 0, 0x80);
            StrokeEllipse(rect.InsetByCopy(4, 4));
            continue;
        }

        BBitmap *icon = ((state & STATE_OFF) && (item.icon_off != nullptr)) ? item.icon_off : item.icon;
        if (icon != nullptr)
            DrawBitmap(icon, icon->Bounds(), rect, B_FILTER_BITMAP_BILINEAR);

        /* Empty drives, muted sound and an idle recompiler are dimmed,
           like Qt's disabled icon mode. */
        if ((state & STATE_EMPTY) || ((state & STATE_OFF) && (item.icon_off == nullptr))) {
            rgb_color fade = ViewColor();
            fade.alpha     = 150;
            SetHighColor(fade);
            SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
            FillRect(rect);
            SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        }

        /* Indicator overlays, as getIconWithIndicator() draws them. */
        const char *overlays[3] = { nullptr, nullptr, nullptr };
        int         n           = 0;
        if (state & STATE_ACTIVE)
            overlays[n++] = "active";
        if (state & STATE_WRITE_ACTIVE)
            overlays[n++] = "write_active";
        else if ((state & STATE_WRITE_PROT) && !(state & STATE_EMPTY))
            overlays[n++] = "write_protected";
        for (int i = 0; i < n; i++) {
            if (BBitmap *light = haiku_icon(overlays[i]))
                DrawBitmap(light, light->Bounds(), rect, B_FILTER_BITMAP_BILINEAR);
        }
    }

    /* The drive a file would be mounted in if dropped now. */
    if ((drop_item >= 0) && (drop_item < (int) items.size())) {
        SetDrawingMode(B_OP_COPY);
        SetHighColor(ui_color(B_KEYBOARD_NAVIGATION_COLOR));
        StrokeRect(ItemRect(items[drop_item]).InsetByCopy(-1, -1));
    }

    SetDrawingMode(B_OP_COPY);
}

void
MachineStatusView::Pulse()
{
    /* USB ports switch on and off with the guest's controllers (and a
       hub's ports come and go with it): the USB icon follows, as Qt's
       refreshIcons() shows or hides it. The icons are made again through
       the window, which also lays the status bar out again. */
    if (UsbWorking() != usb_shown) {
        usb_shown = !usb_shown;
        ui_sb_update_panes();
    }

    /* With "Update status bar icons" off, only empty/full changes show. */
    for (Item &item : items) {
        int state = ItemState(item);
        if (!update_icons && (item.state >= 0))
            state = (state & ~(STATE_ACTIVE | STATE_WRITE_ACTIVE)) | (item.state & (STATE_ACTIVE | STATE_WRITE_ACTIVE));
        if (state != item.state)
            Invalidate(ItemRect(item));
    }
}

int
MachineStatusView::ItemAt(BPoint where) const
{
    for (size_t i = 0; i < items.size(); i++) {
        if ((where.x >= items[i].x) && (where.x < items[i].x + ITEM_W))
            return static_cast<int>(i);
    }
    return -1;
}

void
MachineStatusView::ToolTipFor(const Item &item, char *buf, size_t len) const
{
    auto image = [](const char *path) { return (path && path[0]) ? path : "(empty)"; };

    switch (item.kind) {
        case KIND_CASSETTE:
            snprintf(buf, len, "Cassette: %s", image(cassette_fname));
            break;
        case KIND_CARTRIDGE:
            snprintf(buf, len, "Cartridge %i: %s", item.id + 1, image(cart_fns[item.id]));
            break;
        case KIND_FLOPPY:
            snprintf(buf, len, "Floppy %i (%s): %s", item.id + 1, fdd_is_525(&drives[item.id]) ? "5.25\"" : "3.5\"",
                     image(drives[item.id].image_path));
            break;
        case KIND_CDROM:
            snprintf(buf, len, "CD-ROM %i: %s", item.id + 1, image(cdrom[item.id].image_path));
            break;
        case KIND_RDISK:
            snprintf(buf, len, "Removable disk %i: %s", item.id + 1, image(rdisk_drives[item.id].image_path));
            break;
        case KIND_MO:
            snprintf(buf, len, "MO %i: %s", item.id + 1, image(mo_drives[item.id].image_path));
            break;
        case KIND_TAPE:
            snprintf(buf, len, "Tape %i: %s", item.id + 1, image(tape_drives[item.id].image_path));
            break;
        case KIND_HDD:
            snprintf(buf, len, "Hard disk (%s)", hdd_bus_name(item.id));
            break;
        case KIND_NETWORK:
            snprintf(buf, len, "Network card #%i%s", item.id + 1,
                     machine_status.net[item.id].empty ? " (disconnected)" : "");
            break;
        case KIND_USB:
            snprintf(buf, len, "USB");
            break;
        case KIND_BOARD_LED:
            if (board_led_count() > 1)
                snprintf(buf, len, "Board LED %i", item.id + 1);
            else
                snprintf(buf, len, "Board LED");
            break;
        case KIND_SOUND:
            snprintf(buf, len, "Sound");
            break;
        case KIND_DYNAREC:
            snprintf(buf, len, is_dynarec_active() ? "Dynamic recompiler is active" : "Dynamic recompiler is inactive");
            break;
        case KIND_LOCK_LED:
            {
                static const char *const names[] = { "Num Lock", "Caps Lock", "Scroll Lock", "Kana Lock" };
                snprintf(buf, len, "%s", names[((item.id >= 0) && (item.id < 4)) ? item.id : 0]);
                break;
            }
    }
}

bool
MachineStatusView::MediaTypeOf(Kind kind, int *type)
{
    switch (kind) {
        case KIND_CASSETTE:
            *type = HAIKU_MEDIA_CASSETTE;
            return true;
        case KIND_CARTRIDGE:
            *type = HAIKU_MEDIA_CARTRIDGE;
            return true;
        case KIND_FLOPPY:
            *type = HAIKU_MEDIA_FLOPPY;
            return true;
        case KIND_CDROM:
            *type = HAIKU_MEDIA_CDROM;
            return true;
        case KIND_RDISK:
            *type = HAIKU_MEDIA_RDISK;
            return true;
        case KIND_MO:
            *type = HAIKU_MEDIA_MO;
            return true;
        case KIND_TAPE:
            *type = HAIKU_MEDIA_TAPE;
            return true;
        default:
            return false;
    }
}

/* The drive icon under `where` that would take the dragged message: one
   file or folder from Tracker, as Qt's ClickableLabel takes one URL. */
int
MachineStatusView::DropTarget(BPoint where, const BMessage *drag) const
{
    type_code type;
    int32     count = 0;
    int       media;

    if ((drag == nullptr) || (drag->what != B_SIMPLE_DATA) || (drag->GetInfo("refs", &type, &count) != B_OK)
        || (count != 1))
        return -1;

    const int i = ItemAt(where);
    if ((i < 0) || !MediaTypeOf(items[i].kind, &media))
        return -1;
    return i;
}

void
MachineStatusView::SetDropItem(int i)
{
    if (i == drop_item)
        return;
    if ((drop_item >= 0) && (drop_item < (int) items.size()))
        Invalidate(ItemRect(items[drop_item]).InsetByCopy(-2, -2));
    drop_item = i;
    if (i >= 0)
        Invalidate(ItemRect(items[i]).InsetByCopy(-2, -2));
}

void
MachineStatusView::MouseMoved(BPoint where, uint32 transit, const BMessage *drag)
{
    const bool inside = (transit != B_EXITED_VIEW) && (transit != B_OUTSIDE_VIEW);

    SetDropItem(inside ? DropTarget(where, drag) : -1);

    const int i = (!inside || (drag != nullptr)) ? -1 : ItemAt(where);

    if (i != tip_item) {
        tip_item = i;
        if (i < 0) {
            SetToolTip(static_cast<const char *>(nullptr));
        } else {
            char tip[1200];
            ToolTipFor(items[i], tip, sizeof(tip));
            SetToolTip(tip);
        }
    }

    BView::MouseMoved(where, transit, drag);
}

/* Clicking a drive or network card opens its media menu; the USB icon the
   menu of the working ports; the sound icon the sound menu; the
   recompiler icon the "Force interpretation" switch -- as in Qt. */
void
MachineStatusView::MouseDown(BPoint where)
{
    const int i = ItemAt(where);
    if (i < 0)
        return;

    const Item  &item  = items[i];
    const BPoint point = ConvertToScreen(BPoint(item.x, Bounds().top));
    MainWindow  *owner = dynamic_cast<MainWindow *>(Window());
    if (owner == nullptr)
        return;

    int type;
    if (MediaTypeOf(item.kind, &type)) {
        owner->Media()->PopUp(static_cast<haiku_media_type>(type), item.id, point);
        return;
    }

    switch (item.kind) {
        case KIND_NETWORK:
            owner->Media()->PopUpNic(item.id, point);
            return;

        case KIND_USB:
            owner->Media()->PopUpUsb(point);
            return;

        case KIND_SOUND:
        case KIND_DYNAREC:
            {
                BPopUpMenu *menu = new BPopUpMenu("menu", false, false);
                menu->SetAsyncAutoDestruct(true);
                if (item.kind == KIND_SOUND) {
                    menu->AddItem(new BMenuItem(sound_muted ? "Unmute" : "Mute", new BMessage(MSG_MUTE)));
                    menu->AddSeparatorItem();
                    menu->AddItem(new BMenuItem("Sound gain" B_UTF8_ELLIPSIS, new BMessage(MSG_SOUND_GAIN)));
                } else {
                    BMenuItem *force = new BMenuItem("Force interpretation", new BMessage(MSG_FORCE_INTERPRETER));
                    force->SetMarked(cpu_force_interpreter);
                    menu->AddItem(force);
                }
                menu->SetTargetForItems(owner);
                menu->Go(point, true, false, true);
                return;
            }

        default:
            return;
    }
}

/* A file dropped on a drive is mounted there, as the dropped() handlers
   of qt_machinestatus.cpp do (never write-protected). */
void
MachineStatusView::MessageReceived(BMessage *message)
{
    if (!message->WasDropped() || (message->what != B_SIMPLE_DATA)) {
        BView::MessageReceived(message);
        return;
    }

    SetDropItem(-1);

    const int   i     = DropTarget(ConvertFromScreen(message->DropPoint()), message);
    MainWindow *owner = dynamic_cast<MainWindow *>(Window());
    entry_ref   ref;
    int         type;

    if ((i < 0) || (owner == nullptr) || !MediaTypeOf(items[i].kind, &type) || (message->FindRef("refs", &ref) != B_OK))
        return;

    BPath path(&ref);
    if ((path.InitCheck() != B_OK) || (path.Path() == nullptr))
        return;

    owner->Media()->Mount(static_cast<haiku_media_type>(type), items[i].id, path.Path());
}

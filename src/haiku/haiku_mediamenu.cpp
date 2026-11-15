/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Media menu, like the Qt MediaMenu
 *          (qt_mediamenu.cpp), with the recent-image lists of the Qt
 *          MediaHistoryManager (qt_mediahistorymanager.cpp).
 *
 *          The menus are made again each time they open, from the core's
 *          state, so nothing has to be updated after a mount. Mounting
 *          follows Qt step by step (the plain-C helpers of sdl_cdrom.c do
 *          less: no history, no "wp://" handling, no media-change signal).
 *          The emulation keeps running, as in Qt, except around a floppy
 *          folder mount and an 86F export, where Qt pauses it too.
 *
 *          USB devices and flash drives are plugged and unplugged on the
 *          emulation thread: the menus only ask for it (usb_slot_want(),
 *          usb_slot_move(), usb_port_flash_want()), as Qt's do.
 *
 *          This file also provides plat_cdrom_ui_update() (sdl_cdrom.c
 *          leaves it out under HAIKU_NATIVE_MEDIA), so a disc the guest
 *          ejects or reloads goes into the image history as in Qt.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Alert.h>
#include <Directory.h>
#include <Entry.h>
#include <FilePanel.h>
#include <Menu.h>
#include <MenuItem.h>
#include <Messenger.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Window.h>

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "haiku_mediamenu.hpp"
#include "haiku_newflashdialog.hpp"
#include "haiku_newfloppydialog.hpp"
#include "haiku_settings_common.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/plat.h>
#include <86box/cassette.h>
#include <86box/machine.h>
#include <86box/cartridge.h>
#include <86box/fdd.h>
#include <86box/fdd_86f.h>
#include <86box/fdd_folder.h>
#include <86box/cdrom.h>
#include <86box/cdrom_interface.h>
#include <86box/hdc.h>
#include <86box/scsi.h>
#include <86box/scsi_device.h>
#include <86box/rdisk.h>
#include <86box/mo.h>
#include <86box/scsi_tape.h>
#include <86box/lpt.h>
#include <86box/sound.h>
#include <86box/ui.h>
#include <86box/runctl.h>
#include <86box/thread.h>
#include <86box/network.h>
#include <86box/mem.h>
#include <86box/usb.h>
#include <86box/path.h>
#include <86box/plat_floppy_ioctl.h>
#include <86box/version.h>

extern volatile int fdcinited;
}

enum : uint32 {
    MSG_MM_NEW           = 'mmnw', /* "type", "id" */
    MSG_MM_EXISTING      = 'mmex', /* "type", "id", "wp" */
    MSG_MM_FOLDER        = 'mmfo', /* "type", "id": floppy or CD-ROM folder */
    MSG_MM_HISTORY       = 'mmhi', /* "type", "id", "slot" */
    MSG_MM_HOST_FLOPPY   = 'mmhf', /* "id", "path" */
    MSG_MM_EXPORT        = 'mmxp', /* "id" */
    MSG_MM_EJECT         = 'mmej', /* "type", "id" */
    MSG_MM_CAS_RECORD    = 'mmcr',
    MSG_MM_CAS_PLAY      = 'mmcp',
    MSG_MM_CAS_REWIND    = 'mmcw',
    MSG_MM_CAS_FFWD      = 'mmcf',
    MSG_MM_CD_MUTE       = 'mmmu', /* "id" */
    MSG_MM_CD_BLANK      = 'mmbl', /* "id", "blank" */
    MSG_MM_NIC           = 'mmni', /* "id" */
    MSG_MM_CLEAR_HISTORY = 'mmch',
    MSG_MM_CD_UPDATE     = 'mmcu', /* "id": plat_cdrom_ui_update() */
    MSG_MM_USB_TOGGLE    = 'mmut', /* "slot" */
    MSG_MM_USB_UNPLUG    = 'mmud', /* "slot" */
    MSG_MM_USB_MOVE      = 'mmum', /* "slot", "label" */
    MSG_MM_USB_NEW       = 'mmun', /* "label" */
    MSG_MM_USB_EXISTING  = 'mmue', /* "label", "wp" */
    MSG_MM_USB_PLUG      = 'mmup', /* "label", "path" */
    MSG_MM_USB_EJECT     = 'mmux', /* "label" */
};

/* File panel requests: the panel's B_REFS_RECEIVED / B_SAVE_REQUESTED
   carries "mm_action" with one of these, and the fields of the item. */
enum {
    PANEL_OPEN = 1, /* mount the file in drive "type"/"id", "wp" */
    PANEL_FOLDER,   /* mount the folder in drive "type"/"id" */
    PANEL_CASSETTE_NEW,
    PANEL_EXPORT_86F, /* "id" */
    PANEL_CD_BLANK,   /* "id", "blank" */
    PANEL_USB_OPEN,   /* plug the file into USB port "label", "wp" */
};

static const char *const PANEL_TAG = "mm_action";

static const int MEDIA_TYPES = HAIKU_MEDIA_TAPE + 1;

/* ------------------------------------------------------------ helpers */

static bool
starts_with(const std::string &s, const char *prefix)
{
    return s.compare(0, strlen(prefix), prefix) == 0;
}

/* QFileInfo::fileName() */
static std::string
leaf_name(const std::string &path)
{
    const size_t slash = path.rfind('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

/* QFileInfo::suffix() */
static std::string
suffix_of(const std::string &path)
{
    const std::string leaf = leaf_name(path);
    const size_t      dot  = leaf.rfind('.');
    return (dot == std::string::npos) ? std::string() : leaf.substr(dot + 1);
}

/* Whether a history entry still names something that exists, as
   MediaHistoryManager::removeMissingImages() decides it: a relative path
   is taken from usr_path, "wp://" is dropped, and a host drive must have
   its device node. */
static bool
image_exists(const std::string &fn)
{
    struct stat st;

    if (fn.empty())
        return false;

    if (starts_with(fn, "ioctl://"))
        return stat(fn.c_str() + 8, &st) == 0;

    const std::string path = starts_with(fn, "wp://") ? fn.substr(5) : fn;
    std::vector<char> buf(path.begin(), path.end());
    buf.push_back('\0');

    char temp[MAX_IMAGE_PATH_LEN * 2] = { 0 };
    if (path_abs(buf.data())) {
        if (path.size() > (MAX_IMAGE_PATH_LEN - 1))
            return false;
        snprintf(temp, sizeof(temp), "%s", path.c_str());
    } else {
        if ((strlen(usr_path) + strlen(path_get_slash(usr_path)) + path.size()) > (MAX_IMAGE_PATH_LEN - 1))
            return false;
        snprintf(temp, sizeof(temp), "%s%s%s", usr_path, path_get_slash(usr_path), path.c_str());
    }
    path_normalize(temp);

    return stat(temp, &st) == 0;
}

static void
show_alert(const char *title, const char *text, alert_type type)
{
    const std::string message = std::string(title) + "\n\n" + text;
    BAlert           *alert   = new BAlert(EMU_NAME, message.c_str(), "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, type);
    alert->Go(nullptr);
}

static BMessage *
drive_message(uint32 what, int type, int id)
{
    BMessage *msg = new BMessage(what);
    msg->AddInt32("type", type);
    msg->AddInt32("id", id);
    return msg;
}

static BMenuItem *
add_item(BMenu *menu, const std::string &label, BMessage *msg, bool enabled = true, bool marked = false)
{
    BMenuItem *item = new BMenuItem(label.c_str(), msg);
    item->SetEnabled(enabled);
    item->SetMarked(marked);
    menu->AddItem(item);
    return item;
}

/* Separators collapse, as in a QMenu: none first, none twice in a row. */
static void
add_separator(BMenu *menu)
{
    const int32 n = menu->CountItems();
    if ((n > 0) && (dynamic_cast<BSeparatorItem *>(menu->ItemAt(n - 1)) == nullptr))
        menu->AddSeparatorItem();
}

static void
trim_separator(BMenu *menu)
{
    const int32 n = menu->CountItems();
    if ((n > 0) && (dynamic_cast<BSeparatorItem *>(menu->ItemAt(n - 1)) != nullptr))
        delete menu->RemoveItem(n - 1);
}

static void
set_targets(BMenu *menu, const BMessenger &target)
{
    for (int32 i = 0; i < menu->CountItems(); i++) {
        BMenuItem *item = menu->ItemAt(i);
        if (item->Submenu() != nullptr)
            set_targets(item->Submenu(), target);
        else
            item->SetTarget(target);
    }
}

static int
max_devices(int type)
{
    switch (type) {
        case HAIKU_MEDIA_CASSETTE:
            return 1;
        case HAIKU_MEDIA_CARTRIDGE:
            return 2;
        case HAIKU_MEDIA_FLOPPY:
            return FDD_NUM;
        case HAIKU_MEDIA_CDROM:
            return CDROM_NUM;
        case HAIKU_MEDIA_RDISK:
            return RDISK_NUM;
        case HAIKU_MEDIA_MO:
            return MO_NUM;
        case HAIKU_MEDIA_TAPE:
            return TAPE_NUM;
        default:
            return 0;
    }
}

/* The core's history slots for a drive (MAX_PREV_IMAGES each). */
static char **
core_history(int type, int index)
{
    switch (type) {
        case HAIKU_MEDIA_CASSETTE:
            return &cassette_image_history[0];
        case HAIKU_MEDIA_CARTRIDGE:
            return &cart_image_history[index][0];
        case HAIKU_MEDIA_FLOPPY:
            return &drives[index].image_history[0];
        case HAIKU_MEDIA_CDROM:
            return &cdrom[index].image_history[0];
        case HAIKU_MEDIA_RDISK:
            return &rdisk_drives[index].image_history[0];
        case HAIKU_MEDIA_MO:
            return &mo_drives[index].image_history[0];
        case HAIKU_MEDIA_TAPE:
            return &tape_drives[index].image_history[0];
        default:
            return nullptr;
    }
}

static const char *
current_image(int type, int index)
{
    switch (type) {
        case HAIKU_MEDIA_CASSETTE:
            return cassette_fname;
        case HAIKU_MEDIA_CARTRIDGE:
            return cart_fns[index];
        case HAIKU_MEDIA_FLOPPY:
            return drives[index].image_path;
        case HAIKU_MEDIA_CDROM:
            return cdrom[index].image_path;
        case HAIKU_MEDIA_RDISK:
            return rdisk_drives[index].image_path;
        case HAIKU_MEDIA_MO:
            return mo_drives[index].image_path;
        case HAIKU_MEDIA_TAPE:
            return tape_drives[index].image_path;
        default:
            return "";
    }
}

/* ------------------------------------------------- which drives exist */

/* MachineStatus::hasIDE() / hasSCSI() and the iterate*() checks of
   qt_machinestatus.cpp. */
static bool
has_ide()
{
    if ((machine_has_flags(machine, MACHINE_IDE_QUAD) > 0) || other_ide_present)
        return true;

    const char *hdc_name = hdc_get_internal_name(hdc_current[0]);
    if (hdc_name == nullptr)
        return false;
    return !strncmp(hdc_name, "ide", 3) || !strncmp(hdc_name, "xtide", 5) || !strncmp(hdc_name, "mcide", 5);
}

static bool
has_scsi()
{
    return (machine_has_flags(machine, MACHINE_SCSI) > 0) || other_scsi_present || (scsi_card_current[0] != 0)
        || (scsi_card_current[1] != 0) || (scsi_card_current[2] != 0) || (scsi_card_current[3] != 0);
}

static bool
drive_present(int type, int i)
{
    if ((i < 0) || (i >= max_devices(type)))
        return false;

    switch (type) {
        case HAIKU_MEDIA_CASSETTE:
            return cassette_enable > 0;

        case HAIKU_MEDIA_CARTRIDGE:
            return machine_has_cartridge(machine) != 0;

        case HAIKU_MEDIA_FLOPPY:
            return fdcinited && (fdd_get_type(&drives[i]) != 0);

        case HAIKU_MEDIA_CDROM:
            {
                const int bus = cdrom[i].bus_type;
                if ((bus == CDROM_BUS_ATAPI) && !has_ide())
                    return false;
                if ((bus == CDROM_BUS_SCSI) && !has_scsi())
                    return false;
                if (((bus == CDROM_BUS_SONY) || (bus == CDROM_BUS_CM100) || (bus == CDROM_BUS_PHILIPS)
                     || (bus == CDROM_BUS_HITACHI) || (bus == CDROM_BUS_MITSUMI) || (bus == CDROM_BUS_MKE))
                    && (cdrom_interface_current == 0)
                    && !((bus == CDROM_BUS_MITSUMI) && (machines[machine].init == machine_at_tandy_vis_init))
                    /* A Mitsumi drive behind a parallel port pod has no card either. */
                    && !((bus == CDROM_BUS_MITSUMI) && (cdrom[i].priv != nullptr)))
                    return false;
                return bus != 0;
            }

        case HAIKU_MEDIA_RDISK:
            {
                const int bus = rdisk_drives[i].bus_type;
                if (((bus == RDISK_BUS_ATAPI) || (bus == RDISK_BUS_IDE)) && !has_ide())
                    return false;
                if ((bus == RDISK_BUS_SCSI) && !has_scsi())
                    return false;
                return bus != 0;
            }

        case HAIKU_MEDIA_MO:
            {
                const int bus = mo_drives[i].bus_type;
                if ((bus == MO_BUS_ATAPI) && !has_ide())
                    return false;
                if ((bus == MO_BUS_SCSI) && !has_scsi())
                    return false;
                return bus != 0;
            }

        case HAIKU_MEDIA_TAPE:
            {
                const int bus = tape_drives[i].bus_type;
                if ((bus == TAPE_BUS_ATAPI) && !has_ide())
                    return false;
                if ((bus == TAPE_BUS_SCSI) && !has_scsi())
                    return false;
                /* A parallel-port tape needs the port it sits on to exist. */
                if ((bus == TAPE_BUS_LPT) && !lpt_ports[tape_drives[i].lpt_port].enabled)
                    return false;
                return bus != 0;
            }

        default:
            return false;
    }
}

static const char *
cdrom_bus_name(int bus)
{
    switch (bus) {
        case CDROM_BUS_ATAPI:
            return "ATAPI";
        case CDROM_BUS_SCSI:
            return "SCSI";
        case CDROM_BUS_MITSUMI:
            return "Mitsumi";
        case CDROM_BUS_PHILIPS:
            return "Philips/LMS";
        case CDROM_BUS_SONY:
            return "Sony";
        case CDROM_BUS_CM100:
            return "Philips CM-100/CM-153";
        case CDROM_BUS_HITACHI:
            return "Hitachi";
        case CDROM_BUS_MKE:
            return "Panasonic/MKE";
        case CDROM_BUS_LPT:
            return "LPT";
        default:
            return "Unknown Bus";
    }
}

static const char *
rdisk_bus_name(int bus)
{
    switch (bus) {
        case RDISK_BUS_IDE:
            return "IDE";
        case RDISK_BUS_ATAPI:
            return "ATAPI";
        case RDISK_BUS_SCSI:
            return "SCSI";
        case RDISK_BUS_LPT:
            return "LPT";
        default:
            return "Unknown Bus";
    }
}

static const char *
mo_bus_name(int bus)
{
    switch (bus) {
        case MO_BUS_ATAPI:
            return "ATAPI";
        case MO_BUS_SCSI:
            return "SCSI";
        default:
            return "Unknown Bus";
    }
}

static const char *
tape_bus_name(int bus)
{
    switch (bus) {
        case TAPE_BUS_ATAPI:
            return "ATAPI";
        case TAPE_BUS_SCSI:
            return "SCSI";
        case TAPE_BUS_FDC:
            return "FDC";
        case TAPE_BUS_LPT:
            return "LPT";
        default:
            return "Unknown Bus";
    }
}

static const char *
net_type_name(int type)
{
    switch (type) {
        case NET_TYPE_SLIRP:
            return "SLiRP";
        case NET_TYPE_PCAP:
            return "PCAP";
        case NET_TYPE_VDE:
            return "VDE";
        case NET_TYPE_TAP:
            return "TAP";
        case NET_TYPE_NLSWITCH:
            return "Local Switch";
        case NET_TYPE_NRSWITCH:
            return "Remote Switch";
        default:
            return "Null Driver";
    }
}

/* "Host CD/DVD Drive (dev)" for an "ioctl://dev" path. */
static std::string
host_drive_name(const std::string &fn)
{
    return "Host CD/DVD Drive (" + fn.substr(8) + ")";
}

/* ------------------------------------------------------------- USB */

typedef std::vector<usb_port_info_t> usb_port_list;

/* usb_ports_get() into a list sized to the ports there are. */
static usb_port_list
usb_ports()
{
    usb_port_list ports(USB_PORTS_MAX);
    int           n = usb_ports_get(ports.data(), USB_PORTS_MAX);
    if (n < 0)
        n = 0;
    ports.resize((n < USB_PORTS_MAX) ? n : USB_PORTS_MAX);
    return ports;
}

/* MediaMenu::usbPortName(): a port's number and where it is, so a card's
   ports can be told from the board's. */
static std::string
usb_port_name(const usb_port_info_t &p)
{
    const char *where;

    switch (p.where) {
        case USB_PORT_ON_CARD:
            where = "PCI card";
            break;
        case USB_PORT_ON_HUB:
            where = "hub";
            break;
        default:
            where = "board";
            break;
    }

    return std::string("USB port ") + p.label + " (" + where + ")";
}

/* MediaMenu::usbPortTitle() */
static std::string
usb_port_title(const usb_port_info_t &p)
{
    std::string what;

    switch (p.state) {
        case USB_PORT_FLASH:
            what = leaf_name(p.image);
            if (p.wp)
                what += " (write-protected)";
            break;
        case USB_PORT_DEVICE:
            what = p.name;
            break;
        case USB_PORT_OFF:
            what = "(not available)";
            break;
        default:
            what = "(empty)";
            break;
    }

    return usb_port_name(p) + ": " + what;
}

/* The name the port titles use, so the hub is "USB Hub" everywhere. */
static std::string
usb_slot_title(int slot)
{
    const char *own = usb_slot_name(slot);
    if (own != nullptr)
        return own;

    const device_t *dev = usb_slot_device(slot);
    return (dev != nullptr) ? settings_device_name(dev, dev->internal_name, 0) : std::string();
}

static BMessage *
usb_port_message(uint32 what, const usb_port_info_t &p)
{
    BMessage *msg = new BMessage(what);
    msg->AddString("label", p.label);
    return msg;
}

/* ------------------------------------------------------------- Impl */

typedef std::vector<std::string> history_list;

struct MediaMenu::Impl {
    explicit Impl(BWindow *w);
    ~Impl();

    BWindow                  *window;
    std::vector<history_list> history[MEDIA_TYPES]; /* [type][drive] */

    ImageRefFilter open_filter; /* outlive the panels that use them */
    ImageRefFilter save_filter;
    BFilePanel    *open_panel = nullptr;
    BFilePanel    *dir_panel  = nullptr;
    BFilePanel    *save_panel = nullptr;

    /* MediaHistoryManager */
    void        HistoryInit();
    void        Serialize(int type);
    std::string ImageForSlot(int type, int index, int slot) const;
    void        AddToHistory(int type, int index, const std::string &image_name, const std::string &new_image_name);
    void        ClearHistory();

    /* menus */
    std::string Title(int type, int id) const;
    void        Fill(BMenu *menu, int type, int id);
    void        AddHistory(BMenu *menu, int type, int id);
    void        AddEject(BMenu *menu, int type, int id);
    void        FillUsbPort(BMenu *menu, const usb_port_list &ports, int port);

    /* file panels */
    void PrepareOpen(BFilePanel *panel, const BMessage &msg, const char *title);
    void OpenFile(int type, int id, bool wp);
    void OpenFolder(int type, int id);
    void SaveFile(BMessage &msg, const char *title, const std::vector<std::string> &patterns);
    void PanelResult(BMessage *message);

    /* actions */
    void NewImage(int type, int id);
    void Mount(int type, int id, const std::string &fn, bool wp);
    void HistorySelect(int type, int id, int slot);
    void Eject(int type, int id);

    void CassetteMount(const std::string &fn, bool wp);
    void CassetteEject();
    void CartridgeMount(int i, const std::string &fn);
    void CartridgeEject(int i);
    void FloppyMount(int i, const std::string &fn, bool wp);
    void FloppyEject(int i);
    void FloppyExportTo86f(int i, const std::string &fn);
    void CdromMute(int i);
    void CdromMount(int i, const std::string &fn);
    void CdromInsertBlank(int i, int type, std::string fn);
    void CdromEject(int i);
    void RDiskMount(int i, const std::string &fn, bool wp);
    void RDiskEject(int i);
    void MoMount(int i, const std::string &fn, bool wp);
    void MoEject(int i);
    void TapeMount(int i, const std::string &fn, bool wp);
    void TapeEject(int i);

    void CdromUpdateUi(int i);

    void UsbPlug(const std::string &label, const std::string &fn, bool wp);
    void UsbNewImage(const std::string &label);
    void UsbOpenFile(const std::string &label, bool wp);
};

MediaMenu::Impl::Impl(BWindow *w)
    : window(w)
{
    HistoryInit();
}

MediaMenu::Impl::~Impl()
{
    delete open_panel;
    delete dir_panel;
    delete save_panel;
}

/* ---------------------------------------------------------- history */

static void
deduplicate(history_list &list, const std::vector<std::string> &names)
{
    std::vector<std::string> remove;
    for (const std::string &item : list) {
        if (item.empty())
            continue;
        for (const std::string &name : names) {
            if (!name.empty() && (item == name))
                remove.push_back(item);
        }
    }
    for (const std::string &name : remove) {
        for (size_t k = 0; k < list.size();) {
            if (list[k] == name)
                list.erase(list.begin() + k);
            else
                k++;
        }
    }
}

static void
remove_missing(history_list &list)
{
    for (std::string &fn : list) {
        if (fn.empty())
            continue;
        if (!image_exists(fn)) {
            fn.clear();
            config_save();
        }
    }
}

static void
fill_missing(history_list &list)
{
    while (list.size() < MAX_PREV_IMAGES)
        list.push_back(std::string());
}

/* The constructor of MediaHistoryManager: read the core's lists, then drop
   the mounted image and the missing files from them. */
void
MediaMenu::Impl::HistoryInit()
{
    for (int type = 0; type < MEDIA_TYPES; type++) {
        history[type].assign(max_devices(type), history_list(MAX_PREV_IMAGES));
        for (int i = 0; i < max_devices(type); i++) {
            char **slots = core_history(type, i);
            for (int slot = 0; slot < MAX_PREV_IMAGES; slot++) {
                if ((slots != nullptr) && (slots[slot] != nullptr))
                    history[type][i][slot] = slots[slot];
            }
        }
    }

    for (int type = 0; type < MEDIA_TYPES; type++) {
        for (int i = 0; i < max_devices(type); i++) {
            history_list list = history[type][i];
            deduplicate(list, { current_image(type, i) });
            remove_missing(list);
            fill_missing(list);
            history[type][i] = list;
            Serialize(type);
        }
    }
}

void
MediaMenu::Impl::Serialize(int type)
{
    for (int i = 0; i < max_devices(type); i++) {
        char **slots = core_history(type, i);
        if (slots == nullptr)
            continue;
        for (int slot = 0; slot < MAX_PREV_IMAGES; slot++) {
            if (slots[slot] != nullptr)
                snprintf(slots[slot], MAX_IMAGE_PATH_LEN, "%s", history[type][i][slot].c_str());
        }
    }
}

std::string
MediaMenu::Impl::ImageForSlot(int type, int index, int slot) const
{
    if ((type < 0) || (type >= MEDIA_TYPES) || (index < 0) || (index >= (int) history[type].size()))
        return std::string();
    const history_list &list = history[type][index];
    return ((slot >= 0) && (slot < (int) list.size())) ? list[slot] : std::string();
}

void
MediaMenu::Impl::AddToHistory(int type, int index, const std::string &image_name, const std::string &new_image_name)
{
    if ((type < 0) || (type >= MEDIA_TYPES) || (index < 0) || (index >= (int) history[type].size()))
        return;

    history_list list = history[type][index];

    deduplicate(list, { image_name, new_image_name });
    if (!image_name.empty())
        list.insert(list.begin(), image_name);
    if (list.size() > MAX_PREV_IMAGES)
        list.pop_back();
    fill_missing(list);
    remove_missing(list);

    history[type][index] = list;
    Serialize(type);

    config_save();
}

/* MediaMenu::clearImageHistory() */
void
MediaMenu::Impl::ClearHistory()
{
    for (int type = 0; type < MEDIA_TYPES; type++) {
        history[type].assign(max_devices(type), history_list(MAX_PREV_IMAGES));
        Serialize(type);
    }
    /* The USB flash drives keep their own recent list. */
    usb_flash_history_clear();
    ui_sb_update_panes();
}

/* ------------------------------------------------------------ menus */

std::string
MediaMenu::Impl::Title(int type, int id) const
{
    const std::string name  = current_image(type, id);
    const std::string shown = name.empty() ? std::string("(empty)") : name;
    char              head[256];

    switch (type) {
        case HAIKU_MEDIA_CASSETTE:
            return "Cassette: " + shown;
        case HAIKU_MEDIA_CARTRIDGE:
            snprintf(head, sizeof(head), "Cartridge %i: ", id + 1);
            break;
        case HAIKU_MEDIA_FLOPPY:
            snprintf(head, sizeof(head), "Floppy %i (%s): ", id + 1, fdd_getname(fdd_get_type(&drives[id])));
            break;
        case HAIKU_MEDIA_CDROM:
            snprintf(head, sizeof(head), "CD-ROM %i (%s): ", id + 1, cdrom_bus_name(cdrom[id].bus_type));
            if (starts_with(name, "ioctl://"))
                return head + host_drive_name(name);
            break;
        case HAIKU_MEDIA_RDISK:
            snprintf(head, sizeof(head), "Removable disk %i (%s): ", id + 1, rdisk_bus_name(rdisk_drives[id].bus_type));
            break;
        case HAIKU_MEDIA_MO:
            snprintf(head, sizeof(head), "MO %i (%s): ", id + 1, mo_bus_name(mo_drives[id].bus_type));
            break;
        case HAIKU_MEDIA_TAPE:
            snprintf(head, sizeof(head), "Tape %i (%s): ", id + 1, tape_bus_name(tape_drives[id].bus_type));
            break;
        default:
            return shown;
    }
    return head + shown;
}

/* The recent images (MediaMenu::updateImageHistory()): one item per entry
   that still exists, numbered as Qt numbers them. */
void
MediaMenu::Impl::AddHistory(BMenu *menu, int type, int id)
{
    for (int slot = 0; slot < MAX_PREV_IMAGES; slot++) {
        const std::string fn = ImageForSlot(type, id, slot);
        std::string       name;

        if (fn.empty())
            continue;
        if (starts_with(fn, "ioctl://")) {
            /* A host drive is always listed. */
            name = (type == HAIKU_MEDIA_CDROM) ? host_drive_name(fn) : fn;
        } else {
            if (!image_exists(fn))
                continue;
            if ((type != HAIKU_MEDIA_CDROM) && (type != HAIKU_MEDIA_CARTRIDGE) && starts_with(fn, "wp://"))
                name = fn.substr(5) + " (write-protected)";
            else
                name = fn;
        }

        char number[16];
        snprintf(number, sizeof(number), "%i ", (slot == 9) ? 0 : (slot + 1));

        BMessage *msg = drive_message(MSG_MM_HISTORY, type, id);
        msg->AddInt32("slot", slot);
        add_item(menu, number + name, msg);
    }
}

void
MediaMenu::Impl::AddEject(BMenu *menu, int type, int id)
{
    const std::string name = current_image(type, id);
    std::string       label("Eject");

    if (!name.empty()) {
        if ((type == HAIKU_MEDIA_CDROM) && starts_with(name, "ioctl://"))
            label += " " + host_drive_name(name);
        else
            label += " " + leaf_name(name);
    }
    add_item(menu, label, drive_message(MSG_MM_EJECT, type, id), !name.empty());
}

void
MediaMenu::Impl::Fill(BMenu *menu, int type, int id)
{
    switch (type) {
        case HAIKU_MEDIA_CASSETTE:
            {
                const bool has_image = (cassette_fname[0] != '\0');
                const bool saving    = !strcmp(cassette_mode, "save");

                add_item(menu, "New image" B_UTF8_ELLIPSIS, drive_message(MSG_MM_NEW, type, id));
                add_separator(menu);
                add_item(menu, "Existing image" B_UTF8_ELLIPSIS, drive_message(MSG_MM_EXISTING, type, id));
                BMessage *wp = drive_message(MSG_MM_EXISTING, type, id);
                wp->AddBool("wp", true);
                add_item(menu, "Existing image (Write-protected)" B_UTF8_ELLIPSIS, wp);
                add_separator(menu);
                AddHistory(menu, type, id);
                add_separator(menu);
                add_item(menu, "Record", new BMessage(MSG_MM_CAS_RECORD), has_image, saving);
                add_item(menu, "Play", new BMessage(MSG_MM_CAS_PLAY), has_image, !saving);
                add_item(menu, "Rewind to the beginning", new BMessage(MSG_MM_CAS_REWIND), has_image);
                add_item(menu, "Fast forward to the end", new BMessage(MSG_MM_CAS_FFWD), has_image);
                add_separator(menu);
                AddEject(menu, type, id);
                break;
            }

        case HAIKU_MEDIA_CARTRIDGE:
            add_item(menu, "Image" B_UTF8_ELLIPSIS, drive_message(MSG_MM_EXISTING, type, id));
            add_separator(menu);
            AddHistory(menu, type, id);
            add_separator(menu);
            AddEject(menu, type, id);
            break;

        case HAIKU_MEDIA_FLOPPY:
            {
                fdd_drive_t *drv = &drives[id];

                add_item(menu, "New image" B_UTF8_ELLIPSIS, drive_message(MSG_MM_NEW, type, id));
                add_separator(menu);
                add_item(menu, "Existing image" B_UTF8_ELLIPSIS, drive_message(MSG_MM_EXISTING, type, id));
                BMessage *wp = drive_message(MSG_MM_EXISTING, type, id);
                wp->AddBool("wp", true);
                add_item(menu, "Existing image (Write-protected)" B_UTF8_ELLIPSIS, wp);
                add_item(menu, "Mount folder" B_UTF8_ELLIPSIS, drive_message(MSG_MM_FOLDER, type, id));
                add_separator(menu);
                AddHistory(menu, type, id);
                add_separator(menu);
                const char *host = fdd_get_host_device(drv);
                if ((host != nullptr) && (host[0] != '\0')) {
                    BMessage *msg = drive_message(MSG_MM_HOST_FLOPPY, type, id);
                    msg->AddString("path", (std::string("ioctl://") + host).c_str());
                    add_item(menu, "Use Host Floppy Drive", msg);
                    add_separator(menu);
                }
                add_item(menu, "Export to 86F" B_UTF8_ELLIPSIS, drive_message(MSG_MM_EXPORT, type, id),
                         (drv->image_path[0] != '\0') && !drv->folder_image);
                add_separator(menu);
                AddEject(menu, type, id);
                break;
            }

        case HAIKU_MEDIA_CDROM:
            {
                const int t = cdrom[id].type;

                add_item(menu, (cdrom[id].sound_on == 0) ? "Unmute" : "Mute", drive_message(MSG_MM_CD_MUTE, type, id));
                if (cdrom_is_writable(t))
                    add_separator(menu);
                if (cdrom_is_cdr(t)) {
                    BMessage *msg = drive_message(MSG_MM_CD_BLANK, type, id);
                    msg->AddInt32("blank", CD_BLANK_CDR);
                    add_item(menu, "New CD-R image" B_UTF8_ELLIPSIS, msg);
                }
                if (cdrom_is_cdrw(t)) {
                    BMessage *msg = drive_message(MSG_MM_CD_BLANK, type, id);
                    msg->AddInt32("blank", CD_BLANK_CDRW);
                    add_item(menu, "New CD-RW image" B_UTF8_ELLIPSIS, msg);
                }
                add_separator(menu);
                add_item(menu, "Image" B_UTF8_ELLIPSIS, drive_message(MSG_MM_EXISTING, type, id));
                add_item(menu, "Folder" B_UTF8_ELLIPSIS, drive_message(MSG_MM_FOLDER, type, id));
                add_separator(menu);
                AddHistory(menu, type, id);
                add_separator(menu);
                AddEject(menu, type, id);
                break;
            }

        case HAIKU_MEDIA_RDISK:
        case HAIKU_MEDIA_MO:
        case HAIKU_MEDIA_TAPE:
            {
                add_item(menu, "New image" B_UTF8_ELLIPSIS, drive_message(MSG_MM_NEW, type, id));
                add_separator(menu);
                add_item(menu, "Existing image" B_UTF8_ELLIPSIS, drive_message(MSG_MM_EXISTING, type, id));
                BMessage *wp = drive_message(MSG_MM_EXISTING, type, id);
                wp->AddBool("wp", true);
                add_item(menu, "Existing image (Write-protected)" B_UTF8_ELLIPSIS, wp);
                add_separator(menu);
                AddHistory(menu, type, id);
                add_separator(menu);
                AddEject(menu, type, id);
                break;
            }

        default:
            break;
    }

    trim_separator(menu);
}

/* MediaMenu::usbPortUpdateMenu(): an empty port, or one with a flash
   drive, takes a new or existing image, or a recent one; a flash drive can
   be unplugged. A port with another device can unplug that device or move
   it; it comes back from its own submenu. */
void
MediaMenu::Impl::FillUsbPort(BMenu *menu, const usb_port_list &ports, int port)
{
    const int n = (int) ports.size();

    if ((port < 0) || (port >= n) || (ports[port].state == USB_PORT_OFF)) {
        add_item(menu, "(not available)", nullptr, false);
        return;
    }

    const usb_port_info_t &p = ports[port];

    if (p.state == USB_PORT_DEVICE) {
        const int  slot = p.slot;
        const bool hub  = (slot >= 0) && usb_slot_is_hub(slot);

        BMessage *unplug = new BMessage(MSG_MM_USB_UNPLUG);
        unplug->AddInt32("slot", slot);
        add_item(menu, std::string("Unplug ") + p.name, unplug, slot >= 0);

        /* Every other working port is listed; one in use is grayed out with
           what is in it. The hub goes on root ports only. */
        BMenu *move = new BMenu((std::string("Move ") + p.name + " to").c_str());
        for (int k = 0; (k < n) && (slot >= 0); k++) {
            const usb_port_info_t &q = ports[k];

            if ((k == port) || (q.state == USB_PORT_OFF) || (hub && (q.where == USB_PORT_ON_HUB)))
                continue;
            BMessage *msg = usb_port_message(MSG_MM_USB_MOVE, q);
            msg->AddInt32("slot", slot);
            add_item(move, (q.state == USB_PORT_EMPTY) ? usb_port_name(q) : usb_port_title(q), msg,
                     q.state == USB_PORT_EMPTY);
        }
        BMenuItem *item = new BMenuItem(move);
        item->SetEnabled(slot >= 0);
        menu->AddItem(item);
        return;
    }

    add_item(menu, "New flash drive" B_UTF8_ELLIPSIS, usb_port_message(MSG_MM_USB_NEW, p));
    add_separator(menu);
    add_item(menu, "Existing image" B_UTF8_ELLIPSIS, usb_port_message(MSG_MM_USB_EXISTING, p));
    BMessage *wp = usb_port_message(MSG_MM_USB_EXISTING, p);
    wp->AddBool("wp", true);
    add_item(menu, "Existing image (Write-protected)" B_UTF8_ELLIPSIS, wp);
    add_separator(menu);

    /* An empty port takes any of the machine's USB devices: one in another
       port moves here, one that was unplugged is plugged in here. The hub
       cannot go on a hub port. */
    if (p.state == USB_PORT_EMPTY) {
        bool any = false;

        for (int slot = 0; slot < USB_SLOTS; slot++) {
            const device_t *dev = usb_slot_device(slot);
            std::string     now = "unplugged";

            if ((dev == nullptr) || (usb_slot_is_hub(slot) && (p.where == USB_PORT_ON_HUB)))
                continue;
            for (int k = 0; k < n; k++) {
                if ((ports[k].state == USB_PORT_DEVICE) && (ports[k].slot == slot))
                    now = std::string("now in port ") + ports[k].label;
            }
            BMessage *msg = usb_port_message(MSG_MM_USB_MOVE, p);
            msg->AddInt32("slot", slot);
            add_item(menu, "Plug in " + usb_slot_title(slot) + " (" + now + ")", msg);
            any = true;
        }
        if (any)
            add_separator(menu);
    }

    for (int i = 0; i < MAX_PREV_IMAGES; i++) {
        char recent[MAX_IMAGE_PATH_LEN];

        if (!usb_flash_history_get(i, recent, sizeof(recent)) || (recent[0] == '\0'))
            continue;
        BMessage *msg = usb_port_message(MSG_MM_USB_PLUG, p);
        msg->AddString("path", recent);
        add_item(menu, leaf_name(recent), msg, true, (p.state == USB_PORT_FLASH) && !strcmp(recent, p.image));
    }
    add_separator(menu);

    if (p.state == USB_PORT_FLASH)
        add_item(menu, "Unplug " + leaf_name(p.image), usb_port_message(MSG_MM_USB_EJECT, p));
    else
        add_item(menu, "Unplug", nullptr, false);
}

/* ------------------------------------------------------- file panels */

void
MediaMenu::Impl::PrepareOpen(BFilePanel *panel, const BMessage &msg, const char *title)
{
    panel->SetMessage(const_cast<BMessage *>(&msg));
    panel->SetTarget(BMessenger(window));
    if (panel->Window()->Lock()) {
        panel->Window()->SetTitle(title);
        panel->Window()->Unlock();
    }
    panel->Refresh();
    panel->Show();
}

/* A new panel starts in usr_path when "open_dir_usr_path" is set (Qt's
   getMediaOpenDirectory()); afterwards it stays where it was left. */
static void
start_directory(BFilePanel *panel)
{
    if ((open_dir_usr_path > 0) && (usr_path[0] != '\0'))
        panel->SetPanelDirectory(usr_path);
}

void
MediaMenu::Impl::OpenFile(int type, int id, bool wp)
{
    const char *title = EMU_NAME ": Open image";

    switch (type) {
        case HAIKU_MEDIA_CASSETTE:
            open_filter.SetPatterns({ "pcm", "raw", "wav", "cas" });
            title = EMU_NAME ": Cassette image";
            break;
        case HAIKU_MEDIA_CARTRIDGE:
            open_filter.SetPatterns({ "a", "b", "jrc" });
            title = EMU_NAME ": Cartridge image";
            break;
        case HAIKU_MEDIA_FLOPPY:
            open_filter.SetPatterns({ "0??", "1??", "??0", "86f", "bin", "cq?", "d??", "flp", "hdm", "im?", "json",
                                      "td0", "*fd?", "mfm", "xdf", "zip" });
            title = EMU_NAME ": Floppy image";
            break;
        case HAIKU_MEDIA_CDROM:
            open_filter.SetPatterns({ "iso", "cue", "toc", "ccd", "mds", "mdx", "b5t", "b6t", "nrg", "aaruf",
                                      "aaruformat", "aif", "chd", "zip" });
            title = EMU_NAME ": CD-ROM image";
            break;
        case HAIKU_MEDIA_RDISK:
            open_filter.SetPatterns({ "im?", "rdi", "sdi", "zdi" });
            title = EMU_NAME ": Removable disk image";
            break;
        case HAIKU_MEDIA_MO:
            open_filter.SetPatterns({ "im?", "mdi" });
            title = EMU_NAME ": MO image";
            break;
        case HAIKU_MEDIA_TAPE:
            open_filter.SetPatterns({ "tap" });
            title = EMU_NAME ": Tape image";
            break;
        default:
            return;
    }

    if (open_panel == nullptr) {
        open_panel = new BFilePanel(B_OPEN_PANEL, nullptr, nullptr, B_FILE_NODE, false, nullptr, &open_filter, true);
        start_directory(open_panel);
    }

    BMessage msg(B_REFS_RECEIVED);
    msg.AddInt32(PANEL_TAG, PANEL_OPEN);
    msg.AddInt32("type", type);
    msg.AddInt32("id", id);
    msg.AddBool("wp", wp);
    PrepareOpen(open_panel, msg, title);
}

void
MediaMenu::Impl::OpenFolder(int type, int id)
{
    if (dir_panel == nullptr) {
        dir_panel = new BFilePanel(B_OPEN_PANEL, nullptr, nullptr, B_DIRECTORY_NODE, false, nullptr, nullptr, true);
        start_directory(dir_panel);
    }

    BMessage msg(B_REFS_RECEIVED);
    msg.AddInt32(PANEL_TAG, PANEL_FOLDER);
    msg.AddInt32("type", type);
    msg.AddInt32("id", id);
    PrepareOpen(dir_panel, msg,
                (type == HAIKU_MEDIA_FLOPPY) ? EMU_NAME ": Mount folder as a temporary floppy" : EMU_NAME ": CD-ROM folder");
}

void
MediaMenu::Impl::SaveFile(BMessage &msg, const char *title, const std::vector<std::string> &patterns)
{
    save_filter.SetPatterns(patterns);

    if (save_panel == nullptr) {
        save_panel = new BFilePanel(B_SAVE_PANEL, nullptr, nullptr, B_FILE_NODE, false, nullptr, &save_filter, true);
        start_directory(save_panel);
    }

    save_panel->SetSaveText("");
    PrepareOpen(save_panel, msg, title);
}

void
MediaMenu::Impl::PanelResult(BMessage *message)
{
    const int32 action = message->GetInt32(PANEL_TAG, 0);
    const int32 type   = message->GetInt32("type", -1);
    const int32 id     = message->GetInt32("id", 0);
    const bool  wp     = message->GetBool("wp", false);
    entry_ref   ref;
    BPath       path;

    if (message->what == B_REFS_RECEIVED) {
        if (message->FindRef("refs", &ref) != B_OK)
            return;
        path.SetTo(&ref);
    } else {
        const char *name = nullptr;
        if ((message->FindRef("directory", &ref) != B_OK) || (message->FindString("name", &name) != B_OK))
            return;
        BDirectory dir(&ref);
        path.SetTo(&dir, name);
    }
    if ((path.InitCheck() != B_OK) || (path.Path() == nullptr))
        return;

    std::string fn = path.Path();

    switch (action) {
        case PANEL_OPEN:
            if (drive_present(type, id))
                Mount(type, id, fn, wp);
            break;

        case PANEL_FOLDER:
            if (!drive_present(type, id))
                break;
            if (type == HAIKU_MEDIA_FLOPPY)
                FloppyMount(id, fn, false);
            else if (type == HAIKU_MEDIA_CDROM)
                CdromMount(id, fn);
            break;

        case PANEL_CASSETTE_NEW:
            if (suffix_of(fn).empty())
                fn += ".cas";
            CassetteMount(fn, false);
            break;

        case PANEL_EXPORT_86F:
            if (suffix_of(fn).empty())
                fn += ".86f";
            FloppyExportTo86f(id, fn);
            break;

        case PANEL_CD_BLANK:
            CdromInsertBlank(id, message->GetInt32("blank", CD_BLANK_CDR), fn);
            break;

        case PANEL_USB_OPEN:
            UsbPlug(message->GetString("label", ""), fn, wp);
            break;

        default:
            break;
    }
}

/* ---------------------------------------------------------- actions */

void
MediaMenu::Impl::NewImage(int type, int id)
{
    std::string fn;
    int         medium = 0;

    switch (type) {
        case HAIKU_MEDIA_CASSETTE:
            {
                BMessage msg(B_SAVE_REQUESTED);
                msg.AddInt32(PANEL_TAG, PANEL_CASSETTE_NEW);
                SaveFile(msg, EMU_NAME ": Create" B_UTF8_ELLIPSIS, {});
                break;
            }

        case HAIKU_MEDIA_FLOPPY:
            if (haiku_new_floppy_dialog(window, NewImageMedia::Floppy, -1, &fn, &medium))
                FloppyMount(id, fn, false);
            break;

        case HAIKU_MEDIA_RDISK:
            if (haiku_new_floppy_dialog(window, NewImageMedia::RDisk, rdisk_drives[id].type, &fn, &medium)) {
                rdisk_drives[id].medium_type = medium;
                RDiskMount(id, fn, false);
            }
            break;

        case HAIKU_MEDIA_MO:
            if (haiku_new_floppy_dialog(window, NewImageMedia::Mo, mo_drives[id].type, &fn, &medium)) {
                mo_drives[id].medium_type = medium;
                MoMount(id, fn, false);
            }
            break;

        case HAIKU_MEDIA_TAPE:
            if (haiku_new_floppy_dialog(window, NewImageMedia::Tape, tape_drives[id].type, &fn, &medium)) {
                tape_drives[id].medium_type = medium;
                TapeMount(id, fn, false);
            }
            break;

        default:
            break;
    }
}

void
MediaMenu::Impl::Mount(int type, int id, const std::string &fn, bool wp)
{
    switch (type) {
        case HAIKU_MEDIA_CASSETTE:
            CassetteMount(fn, wp);
            break;
        case HAIKU_MEDIA_CARTRIDGE:
            CartridgeMount(id, fn);
            break;
        case HAIKU_MEDIA_FLOPPY:
            FloppyMount(id, fn, wp);
            break;
        case HAIKU_MEDIA_CDROM:
            CdromMount(id, fn);
            break;
        case HAIKU_MEDIA_RDISK:
            RDiskMount(id, fn, wp);
            break;
        case HAIKU_MEDIA_MO:
            MoMount(id, fn, wp);
            break;
        case HAIKU_MEDIA_TAPE:
            TapeMount(id, fn, wp);
            break;
        default:
            break;
    }
}

/* The *MenuSelect() / *Reload(index, slot) of Qt. */
void
MediaMenu::Impl::HistorySelect(int type, int id, int slot)
{
    const std::string fn = ImageForSlot(type, id, slot);

    switch (type) {
        case HAIKU_MEDIA_RDISK:
            RDiskMount(id, fn, rdisk_drives[id].read_only);
            break;
        default:
            Mount(type, id, fn, false);
            break;
    }
}

void
MediaMenu::Impl::Eject(int type, int id)
{
    switch (type) {
        case HAIKU_MEDIA_CASSETTE:
            CassetteEject();
            break;
        case HAIKU_MEDIA_CARTRIDGE:
            CartridgeEject(id);
            break;
        case HAIKU_MEDIA_FLOPPY:
            FloppyEject(id);
            break;
        case HAIKU_MEDIA_CDROM:
            CdromEject(id);
            break;
        case HAIKU_MEDIA_RDISK:
            RDiskEject(id);
            break;
        case HAIKU_MEDIA_MO:
            MoEject(id);
            break;
        case HAIKU_MEDIA_TAPE:
            TapeEject(id);
            break;
        default:
            break;
    }
}

/* ---------------------------------------------------------- cassette */

void
MediaMenu::Impl::CassetteMount(const std::string &filename, bool wp)
{
    const std::string previous = cassette_fname;

    pc_cas_set_fname(cassette, nullptr);
    memset(cassette_fname, 0, sizeof(cassette_fname));
    cassette_ui_writeprot = wp ? 1 : 0;

    if (!filename.empty()) {
        std::string name = filename;

        if (starts_with(filename, "wp://"))
            cassette_ui_writeprot = 1;
        else if (cassette_ui_writeprot)
            name = "wp://" + filename;

        snprintf(cassette_fname, sizeof(cassette_fname), "%s", name.c_str());
        pc_cas_set_fname(cassette, cassette_fname);
    }

    ui_sb_update_icon_state(SB_CASSETTE, cassette->fname == nullptr);
    ui_sb_update_icon_wp(SB_CASSETTE, cassette_ui_writeprot);
    AddToHistory(HAIKU_MEDIA_CASSETTE, 0, previous, filename);
    ui_sb_update_tip(SB_CASSETTE);
    config_save();
}

void
MediaMenu::Impl::CassetteEject()
{
    AddToHistory(HAIKU_MEDIA_CASSETTE, 0, cassette_fname, std::string());
    pc_cas_set_fname(cassette, nullptr);
    memset(cassette_fname, 0, sizeof(cassette_fname));
    ui_sb_update_icon_state(SB_CASSETTE, 1);
    ui_sb_update_tip(SB_CASSETTE);
    config_save();
}

/* -------------------------------------------------------- cartridges */

void
MediaMenu::Impl::CartridgeMount(int i, const std::string &filename)
{
    const std::string previous = cart_fns[i];
    std::vector<char> name(filename.begin(), filename.end());
    name.push_back('\0');

    cart_close(i);
    cart_load(i, name.data());

    ui_sb_update_icon_state(SB_CARTRIDGE | i, cart_fns[i][0] == 0);
    AddToHistory(HAIKU_MEDIA_CARTRIDGE, i, previous, filename);
    ui_sb_update_tip(SB_CARTRIDGE | i);
    config_save();
}

void
MediaMenu::Impl::CartridgeEject(int i)
{
    AddToHistory(HAIKU_MEDIA_CARTRIDGE, i, cart_fns[i], std::string());
    cart_close(i);
    ui_sb_update_icon_state(SB_CARTRIDGE | i, 1);
    ui_sb_update_tip(SB_CARTRIDGE | i);
    config_save();
}

/* ---------------------------------------------------------- floppies */

void
MediaMenu::Impl::FloppyMount(int i, const std::string &filename, bool wp)
{
    fdd_drive_t      *drv         = &drives[i];
    const std::string previous    = drv->image_path;
    const bool        prefixed_wp = starts_with(filename, "wp://");
    const std::string path        = prefixed_wp ? filename.substr(5) : filename;
    std::vector<char> source(path.begin(), path.end());
    source.push_back('\0');

    if (!path.empty() && fdd_folder_source(source.data())) {
        /* A folder or a ZIP archive becomes a temporary floppy image. */
        runctl_pause_wait(RUN_PAUSE_SYNC);
        const fdd_folder_result_t result = fdd_mount_folder(drv, source.data(), wp || prefixed_wp);
        runctl_resume(RUN_PAUSE_SYNC);
        if (result != FDD_FOLDER_OK) {
            const char *message;
            switch (result) {
                case FDD_FOLDER_UNSUPPORTED:
                    message = "The selected drive does not support folder mounting.";
                    break;
                case FDD_FOLDER_FULL:
                    message = "The contents exceed the capacity of the configured floppy drive.";
                    break;
                case FDD_FOLDER_CANNOT_CREATE:
                    message = "The floppy is out of directory entries or space, or a filename cannot be "
                              "represented on FAT12.";
                    break;
                case FDD_FOLDER_INVALID_NAME:
                    message = "The source contains a filename that cannot be represented on FAT12.";
                    break;
                case FDD_FOLDER_NO_MEMORY:
                    message = "There is not enough memory to create the floppy image.";
                    break;
                default:
                    message = "The source path is too long or its contents could not be read.";
                    break;
            }
            show_alert("Unable to mount folder or ZIP archive", message, B_STOP_ALERT);
            return;
        }
        AddToHistory(HAIKU_MEDIA_FLOPPY, i, previous, drv->image_path);
    } else {
        const bool was_folder = drv->folder_image;
        if (was_folder)
            runctl_pause_wait(RUN_PAUSE_SYNC);
        fdd_close(drv);
        drv->read_only   = wp ? 1 : 0;
        std::string name = filename;

        if (!filename.empty()) {
            if (prefixed_wp)
                drv->read_only = 1;
            else if (drv->read_only)
                name = "wp://" + name;
            std::vector<char> buf(name.begin(), name.end());
            buf.push_back('\0');
            fdd_load(drv, buf.data());
        }
        if (was_folder)
            runctl_resume(RUN_PAUSE_SYNC);
        AddToHistory(HAIKU_MEDIA_FLOPPY, i, previous, name);
    }

    ui_sb_update_icon_state(SB_FLOPPY | i, drv->empty);
    ui_sb_update_icon_wp(SB_FLOPPY | i, drv->writeprot);
    ui_sb_update_tip(SB_FLOPPY | i);
    config_save();
}

void
MediaMenu::Impl::FloppyEject(int i)
{
    fdd_drive_t *drv = &drives[i];

    AddToHistory(HAIKU_MEDIA_FLOPPY, i, drv->image_path, std::string());
    const bool was_folder = drv->folder_image;
    if (was_folder)
        runctl_pause_wait(RUN_PAUSE_SYNC);
    fdd_close(drv);
    if (was_folder)
        runctl_resume(RUN_PAUSE_SYNC);
    ui_sb_update_icon_state(SB_FLOPPY | i, 1);
    ui_sb_update_tip(SB_FLOPPY | i);
    config_save();
}

void
MediaMenu::Impl::FloppyExportTo86f(int i, const std::string &filename)
{
    std::vector<char> name(filename.begin(), filename.end());
    name.push_back('\0');

    runctl_pause_wait(RUN_PAUSE_SYNC);
    const int ok = d86f_export(&drives[i], name.data());
    runctl_resume(RUN_PAUSE_SYNC);

    if (ok == 0)
        show_alert("Unable to write file", "Make sure the file is being saved to a writable directory", B_STOP_ALERT);
}

/* ----------------------------------------------------------- CD-ROMs */

void
MediaMenu::Impl::CdromMute(int i)
{
    cdrom[i].sound_on ^= 1;
    config_save();
    sound_cd_thread_reset();
}

void
MediaMenu::Impl::CdromMount(int i, const std::string &filename)
{
    std::vector<char> fn(filename.begin(), filename.end());
    fn.push_back('\0');
    const int was_empty = cdrom_is_empty(i);

    cdrom_exit(i);

    memset(cdrom[i].image_path, 0, sizeof(cdrom[i].image_path));
    const size_t len = strlen(fn.data());
    if ((len >= 1) && (fn[len - 1] == '\\'))
        fn[len - 1] = '/';
    cdrom_load(&cdrom[i], fn.data(), 1);

    /* Signal media change to the emulated machine. */
    if (cdrom[i].insert) {
        cdrom[i].insert(cdrom[i].priv);

        /* The drive was previously empty, transition directly to UNIT ATTENTION. */
        if (was_empty)
            cdrom[i].insert(cdrom[i].priv);
    }

    ui_sb_update_icon_state(SB_CDROM | i, (cdrom[i].image_path[0] != '\0') ? 0 : 1);
    AddToHistory(HAIKU_MEDIA_CDROM, i, cdrom[i].prev_image_path, cdrom[i].image_path);
    ui_sb_update_tip(SB_CDROM | i);
    config_save();
}

/* The burned disc is written to `filename` when the guest closes a
   session. */
void
MediaMenu::Impl::CdromInsertBlank(int i, int type, std::string filename)
{
    if (!cdrom_is_empty(i)) {
        show_alert("Drive not empty", "The drive must be empty to add a blank disc.", B_INFO_ALERT);
        return;
    }

    if ((filename.size() < 6) || strcasecmp(filename.c_str() + filename.size() - 6, ".aaruf"))
        filename += ".aaruf";

    if (cdrom_insert_blank(&cdrom[i], filename.c_str(), type) != 0) {
        ui_sb_update_icon_state(SB_CDROM | i, 1);
        return;
    }

    ui_sb_update_icon_state(SB_CDROM | i, 0);
    AddToHistory(HAIKU_MEDIA_CDROM, i, cdrom[i].prev_image_path, cdrom[i].image_path);
    ui_sb_update_tip(SB_CDROM | i);
    config_save();
}

void
MediaMenu::Impl::CdromEject(int i)
{
    AddToHistory(HAIKU_MEDIA_CDROM, i, cdrom[i].image_path, std::string());
    cdrom_eject(i);
    ui_sb_update_tip(SB_CDROM | i);
}

/* ---------------------------------------------------- removable disks */

void
MediaMenu::Impl::RDiskMount(int i, const std::string &filename, bool wp)
{
    rdisk_t *dev = static_cast<rdisk_t *>(rdisk_drives[i].priv);
    if (dev == nullptr)
        return;
    const int was_empty = rdisk_is_empty(i);

    rdisk_disk_close(dev);
    rdisk_drives[i].read_only = wp;
    if (!filename.empty()) {
        std::string name = filename;

        if (starts_with(filename, "wp://"))
            rdisk_drives[i].read_only = 1;
        else if (rdisk_drives[i].read_only)
            name = "wp://" + filename;

        rdisk_load(dev, name.c_str(), 1);

        /* Signal media change to the emulated machine. */
        rdisk_insert(dev);

        /* The drive was previously empty, transition directly to UNIT ATTENTION. */
        if (was_empty)
            rdisk_insert(dev);
    }
    AddToHistory(HAIKU_MEDIA_RDISK, i, rdisk_drives[i].prev_image_path, rdisk_drives[i].image_path);

    ui_sb_update_icon_state(SB_RDISK | i, rdisk_drives[i].fp == nullptr);
    /* rdisk_load() may have made the image read-only on its own (e.g. a ZIP
       100 disk in a ZIP 750), so show the drive's state. */
    ui_sb_update_icon_wp(SB_RDISK | i, rdisk_drives[i].read_only);
    ui_sb_update_tip(SB_RDISK | i);

    config_save();
}

void
MediaMenu::Impl::RDiskEject(int i)
{
    rdisk_t *dev = static_cast<rdisk_t *>(rdisk_drives[i].priv);
    if (dev == nullptr)
        return;

    AddToHistory(HAIKU_MEDIA_RDISK, i, rdisk_drives[i].image_path, std::string());
    rdisk_disk_close(dev);
    rdisk_drives[i].image_path[0] = 0;
    if (rdisk_drives[i].bus_type) {
        /* Signal disk change to the emulated machine. */
        rdisk_insert(dev);
    }

    ui_sb_update_icon_state(SB_RDISK | i, 1);
    ui_sb_update_tip(SB_RDISK | i);
    config_save();
}

/* -------------------------------------------------------------- MO */

void
MediaMenu::Impl::MoMount(int i, const std::string &filename, bool wp)
{
    mo_t *dev = static_cast<mo_t *>(mo_drives[i].priv);
    if (dev == nullptr)
        return;
    const int was_empty = mo_is_empty(i);

    mo_disk_close(dev);
    mo_drives[i].read_only = wp;
    if (!filename.empty()) {
        std::string name = filename;

        if (starts_with(filename, "wp://"))
            mo_drives[i].read_only = 1;
        else if (mo_drives[i].read_only)
            name = "wp://" + filename;

        mo_load(dev, name.c_str(), 1);

        /* Signal media change to the emulated machine. */
        mo_insert(dev);

        /* The drive was previously empty, transition directly to UNIT ATTENTION. */
        if (was_empty)
            mo_insert(dev);
    }
    AddToHistory(HAIKU_MEDIA_MO, i, mo_drives[i].prev_image_path, mo_drives[i].image_path);

    ui_sb_update_icon_state(SB_MO | i, mo_drives[i].fp == nullptr);
    ui_sb_update_tip(SB_MO | i);

    config_save();
}

void
MediaMenu::Impl::MoEject(int i)
{
    mo_t *dev = static_cast<mo_t *>(mo_drives[i].priv);
    if (dev == nullptr)
        return;

    AddToHistory(HAIKU_MEDIA_MO, i, mo_drives[i].image_path, std::string());
    mo_disk_close(dev);
    mo_drives[i].image_path[0] = 0;
    if (mo_drives[i].bus_type) {
        /* Signal disk change to the emulated machine. */
        mo_insert(dev);
    }

    ui_sb_update_icon_state(SB_MO | i, 1);
    ui_sb_update_tip(SB_MO | i);
    config_save();
}

/* ------------------------------------------------------------ tapes */

void
MediaMenu::Impl::TapeMount(int i, const std::string &filename, bool wp)
{
    tape_t   *dev       = static_cast<tape_t *>(tape_drives[i].priv);
    const int was_empty = (tape_drives[i].fp == nullptr);

    if ((tape_drives[i].bus_type == TAPE_BUS_FDC) || (tape_drives[i].bus_type == TAPE_BUS_LPT)) {
        /* The floppy-tape and Ditto cores take care of their own image
           handling through the bus-agnostic dispatch. */
        if (!filename.empty()) {
            std::string name = filename;
            if (wp && !starts_with(filename, "wp://"))
                name = "wp://" + filename;
            tape_drive_mount(i, name.c_str(), wp);
        } else
            tape_drive_eject(i);

        AddToHistory(HAIKU_MEDIA_TAPE, i, tape_drives[i].prev_image_path, tape_drives[i].image_path);

        ui_sb_update_icon_state(SB_TAPE | i, tape_drives[i].image_path[0] == '\0');
        ui_sb_update_icon_wp(SB_TAPE | i, tape_drives[i].read_only);
        ui_sb_update_tip(SB_TAPE | i);

        config_save();
        return;
    }

    if (dev == nullptr)
        return;

    tape_disk_close(dev);
    tape_drives[i].read_only = wp;
    if (!filename.empty()) {
        std::string name = filename;

        if (starts_with(filename, "wp://"))
            tape_drives[i].read_only = 1;
        else if (tape_drives[i].read_only)
            name = "wp://" + filename;

        tape_load(dev, name.c_str(), 1);

        /* Signal media change to the emulated machine. */
        tape_insert(dev);

        /* The drive was previously empty, transition directly to UNIT ATTENTION. */
        if (was_empty)
            tape_insert(dev);
    }
    AddToHistory(HAIKU_MEDIA_TAPE, i, tape_drives[i].prev_image_path, tape_drives[i].image_path);

    ui_sb_update_icon_state(SB_TAPE | i, tape_drives[i].fp == nullptr);
    ui_sb_update_icon_wp(SB_TAPE | i, wp);
    ui_sb_update_tip(SB_TAPE | i);

    config_save();
}

void
MediaMenu::Impl::TapeEject(int i)
{
    tape_t *dev = static_cast<tape_t *>(tape_drives[i].priv);

    if ((tape_drives[i].bus_type == TAPE_BUS_FDC) || (tape_drives[i].bus_type == TAPE_BUS_LPT)) {
        AddToHistory(HAIKU_MEDIA_TAPE, i, tape_drives[i].image_path, std::string());
        tape_drive_eject(i);

        ui_sb_update_icon_state(SB_TAPE | i, 1);
        ui_sb_update_icon_wp(SB_TAPE | i, 0);
        ui_sb_update_tip(SB_TAPE | i);
        config_save();
        return;
    }

    if (dev == nullptr)
        return;

    AddToHistory(HAIKU_MEDIA_TAPE, i, tape_drives[i].image_path, std::string());
    tape_disk_close(dev);
    tape_drives[i].image_path[0] = 0;
    if (tape_drives[i].bus_type) {
        /* Signal media change to the emulated machine. */
        tape_insert(dev);
    }

    ui_sb_update_icon_state(SB_TAPE | i, 1);
    ui_sb_update_tip(SB_TAPE | i);
    config_save();
}

/* MediaMenu::cdromUpdateUi(): the core loaded, reloaded or ejected a disc
   on its own (the guest asked, or an image failed to open). */
void
MediaMenu::Impl::CdromUpdateUi(int i)
{
    cdrom_t *drv = &cdrom[i];

    if (drv->image_path[0] == '\0') {
        AddToHistory(HAIKU_MEDIA_CDROM, i, drv->prev_image_path, std::string());
        ui_sb_update_icon_state(SB_CDROM | i, 1);
    } else {
        AddToHistory(HAIKU_MEDIA_CDROM, i, drv->prev_image_path, drv->image_path);
        ui_sb_update_icon_state(SB_CDROM | i, 0);
    }

    ui_sb_update_tip(SB_CDROM | i);
}

/* ------------------------------------------------------------- USB */

/* MediaMenu::usbPortPlug(): the image must be a disk image; it may already
   be in another port, and then it moves here. */
void
MediaMenu::Impl::UsbPlug(const std::string &label, const std::string &fn, bool wp)
{
    struct stat st;

    if (label.empty())
        return;
    if ((stat(fn.c_str(), &st) != 0) || !S_ISREG(st.st_mode) || (st.st_size < 512)) {
        show_alert("USB flash drive", (fn + " is not a disk image.").c_str(), B_WARNING_ALERT);
        return;
    }

    usb_port_flash_want(label.c_str(), fn.c_str(), wp ? 1 : 0);
    config_save();
}

void
MediaMenu::Impl::UsbNewImage(const std::string &label)
{
    std::string fn;

    if (haiku_new_flash_dialog(window, &fn))
        UsbPlug(label, fn, false);
}

void
MediaMenu::Impl::UsbOpenFile(const std::string &label, bool wp)
{
    open_filter.SetPatterns({ "img", "ima", "bin", "raw" });

    if (open_panel == nullptr) {
        open_panel = new BFilePanel(B_OPEN_PANEL, nullptr, nullptr, B_FILE_NODE, false, nullptr, &open_filter, true);
        start_directory(open_panel);
    }

    BMessage msg(B_REFS_RECEIVED);
    msg.AddInt32(PANEL_TAG, PANEL_USB_OPEN);
    msg.AddString("label", label.c_str());
    msg.AddBool("wp", wp);
    PrepareOpen(open_panel, msg, EMU_NAME ": Flash drive image");
}

/* ----------------------------------------------- guest-initiated changes */

/* The window the media menu posts to, for plat_cdrom_ui_update(), which
   the core calls from the emulation thread. */
static BMessenger        media_target;
static std::atomic<bool> media_target_ok(false);

/* The Qt version emits a queued signal to MediaMenu::cdromUpdateUi(); here
   the main window gets a message and passes it to HandleMessage(). Before
   the window exists, only the status bar is updated, as sdl_cdrom.c does. */
void
plat_cdrom_ui_update(uint8_t id, uint8_t)
{
    if (media_target_ok.load()) {
        BMessage msg(MSG_MM_CD_UPDATE);
        msg.AddInt32("id", id);
        if (media_target.SendMessage(&msg, (BHandler *) nullptr, 0) == B_OK)
            return;
    }

    ui_sb_update_icon_state(SB_CDROM | id, (cdrom[id].image_path[0] == '\0') ? 1 : 0);
    ui_sb_update_tip(SB_CDROM | id);
}

/* --------------------------------------------------------- MediaMenu */

MediaMenu::MediaMenu(BWindow *window)
    : impl(new Impl(window))
{
    media_target = BMessenger(window);
    media_target_ok.store(true);
}

MediaMenu::~MediaMenu()
{
    media_target_ok.store(false);
    delete impl;
}

/* MediaMenu::refresh() */
void
MediaMenu::Rebuild(BMenu *media_menu)
{
    while (BMenuItem *item = media_menu->RemoveItem((int32) 0))
        delete item;

    static const haiku_media_type order[] = {
        HAIKU_MEDIA_CASSETTE, HAIKU_MEDIA_CARTRIDGE, HAIKU_MEDIA_FLOPPY, HAIKU_MEDIA_CDROM,
        HAIKU_MEDIA_RDISK,    HAIKU_MEDIA_MO,        HAIKU_MEDIA_TAPE,
    };

    for (haiku_media_type type : order) {
        for (int i = 0; i < max_devices(type); i++) {
            if (!drive_present(type, i))
                continue;
            BMenu *sub = new BMenu(impl->Title(type, i).c_str());
            impl->Fill(sub, type, i);
            media_menu->AddItem(sub);
        }
    }

    /* One submenu per network card, with its "Connected" switch. */
    for (int i = 0; i < NET_CARD_MAX; i++) {
        if (!network_dev_available(i))
            continue;

        const int         dev_num = net_cards_conf[i].device_num;
        const std::string dev_name
            = settings_device_name(network_card_getdevice(dev_num), network_card_get_internal_name(dev_num), 1);
        char title[512];
        snprintf(title, sizeof(title), "NIC %i (%s) %s", i + 1, net_type_name(net_cards_conf[i].net_type),
                 dev_name.c_str());

        BMenu    *sub = new BMenu(title);
        BMessage *msg = new BMessage(MSG_MM_NIC);
        msg->AddInt32("id", i);
        add_item(sub, "Connected", msg, true, network_is_connected(i) != 0);
        media_menu->AddItem(sub);
    }

    /* One submenu per USB device the machine has. The plug or unplug is
       made on the emulation thread, so the check mark shows the request
       while it is pending, else the device's state. */
    for (int slot = 0; slot < USB_SLOTS; slot++) {
        if (usb_slot_device(slot) == nullptr)
            continue;

        BMenu    *sub = new BMenu(usb_slot_title(slot).c_str());
        BMessage *msg = new BMessage(MSG_MM_USB_TOGGLE);
        msg->AddInt32("slot", slot);
        add_item(sub, "Connected", msg, true, usb_slot_requested(slot) != 0);
        media_menu->AddItem(sub);
    }

    /* One submenu per USB port, as for each CD-ROM drive: a flash drive
       image goes into a port from here. A port the chipset has switched
       off is left out. */
    const usb_port_list ports = usb_ports();
    for (int k = 0; k < (int) ports.size(); k++) {
        if (ports[k].state == USB_PORT_OFF)
            continue;
        BMenu *sub = new BMenu(usb_port_title(ports[k]).c_str());
        impl->FillUsbPort(sub, ports, k);
        media_menu->AddItem(sub);
    }

    add_item(media_menu, "Clear image history", new BMessage(MSG_MM_CLEAR_HISTORY));

    set_targets(media_menu, BMessenger(impl->window));
}

void
MediaMenu::PopUp(haiku_media_type type, int id, BPoint screen_where)
{
    if (!drive_present(type, id))
        return;

    BPopUpMenu *menu = new BPopUpMenu("media", false, false);
    impl->Fill(menu, type, id);
    set_targets(menu, BMessenger(impl->window));
    menu->SetAsyncAutoDestruct(true);
    menu->Go(screen_where, true, true, true);
}

/* The status bar's USB menu (popupUsbMenu() in qt_machinestatus.cpp): every
   working port by its submenu, in port order, with a separator where the
   ports go from the board to a PCI card or a hub. */
void
MediaMenu::PopUpUsb(BPoint screen_where)
{
    const usb_port_list ports = usb_ports();
    BPopUpMenu         *menu  = new BPopUpMenu("usb", false, false);
    int                 where = -1;

    for (int k = 0; k < (int) ports.size(); k++) {
        if (ports[k].state == USB_PORT_OFF)
            continue;
        if ((where >= 0) && (ports[k].where != where))
            menu->AddSeparatorItem();
        where = ports[k].where;

        BMenu *sub = new BMenu(usb_port_title(ports[k]).c_str());
        impl->FillUsbPort(sub, ports, k);
        menu->AddItem(sub);
    }

    if (menu->CountItems() == 0) {
        delete menu;
        return;
    }

    set_targets(menu, BMessenger(impl->window));
    menu->SetAsyncAutoDestruct(true);
    menu->Go(screen_where, true, true, true);
}

void
MediaMenu::PopUpNic(int id, BPoint screen_where)
{
    if ((id < 0) || (id >= NET_CARD_MAX) || !network_dev_available(id))
        return;

    BPopUpMenu *menu = new BPopUpMenu("nic", false, false);
    BMessage   *msg  = new BMessage(MSG_MM_NIC);
    msg->AddInt32("id", id);
    add_item(menu, "Connected", msg, true, network_is_connected(id) != 0);
    set_targets(menu, BMessenger(impl->window));
    menu->SetAsyncAutoDestruct(true);
    menu->Go(screen_where, true, true, true);
}

bool
MediaMenu::Mount(haiku_media_type type, int id, const char *path)
{
    if ((path == nullptr) || (path[0] == '\0') || !drive_present(type, id))
        return false;

    impl->Mount(type, id, path, false);
    return true;
}

bool
MediaMenu::DrivePresent(haiku_media_type type, int id)
{
    return drive_present(type, id);
}

bool
MediaMenu::HandleMessage(BMessage *message)
{
    const int32 type = message->GetInt32("type", -1);
    const int32 id   = message->GetInt32("id", 0);

    switch (message->what) {
        case B_REFS_RECEIVED:
        case B_SAVE_REQUESTED:
            if (!message->HasInt32(PANEL_TAG))
                return false;
            impl->PanelResult(message);
            return true;

        case MSG_MM_NEW:
            if (drive_present(type, id))
                impl->NewImage(type, id);
            return true;

        case MSG_MM_EXISTING:
            if (drive_present(type, id))
                impl->OpenFile(type, id, message->GetBool("wp", false));
            return true;

        case MSG_MM_FOLDER:
            if (drive_present(type, id))
                impl->OpenFolder(type, id);
            return true;

        case MSG_MM_HISTORY:
            if (drive_present(type, id))
                impl->HistorySelect(type, id, message->GetInt32("slot", -1));
            return true;

        case MSG_MM_HOST_FLOPPY:
            if (drive_present(HAIKU_MEDIA_FLOPPY, id))
                impl->FloppyMount(id, message->GetString("path", ""), false);
            return true;

        case MSG_MM_EXPORT:
            if (drive_present(HAIKU_MEDIA_FLOPPY, id)) {
                BMessage msg(B_SAVE_REQUESTED);
                msg.AddInt32(PANEL_TAG, PANEL_EXPORT_86F);
                msg.AddInt32("id", id);
                impl->SaveFile(msg, EMU_NAME ": Export to 86F", { "86f" });
            }
            return true;

        case MSG_MM_EJECT:
            if (drive_present(type, id))
                impl->Eject(type, id);
            return true;

        case MSG_MM_CAS_RECORD:
            if (cassette != nullptr)
                pc_cas_set_mode(cassette, 1);
            return true;
        case MSG_MM_CAS_PLAY:
            if (cassette != nullptr)
                pc_cas_set_mode(cassette, 0);
            return true;
        case MSG_MM_CAS_REWIND:
            if (cassette != nullptr)
                pc_cas_rewind(cassette);
            return true;
        case MSG_MM_CAS_FFWD:
            if (cassette != nullptr)
                pc_cas_append(cassette);
            return true;

        case MSG_MM_CD_MUTE:
            if (drive_present(HAIKU_MEDIA_CDROM, id))
                impl->CdromMute(id);
            return true;

        case MSG_MM_CD_BLANK:
            if (drive_present(HAIKU_MEDIA_CDROM, id)) {
                const int32 blank = message->GetInt32("blank", CD_BLANK_CDR);
                if (!cdrom_is_empty(id)) {
                    show_alert("Drive not empty", "The drive must be empty to add a blank disc.", B_INFO_ALERT);
                    return true;
                }
                BMessage msg(B_SAVE_REQUESTED);
                msg.AddInt32(PANEL_TAG, PANEL_CD_BLANK);
                msg.AddInt32("id", id);
                msg.AddInt32("blank", blank);
                impl->SaveFile(msg, (blank == CD_BLANK_CDRW) ? EMU_NAME ": CD-RW image" : EMU_NAME ": CD-R image",
                               { "aaruf" });
            }
            return true;

        case MSG_MM_NIC:
            if ((id >= 0) && (id < NET_CARD_MAX)) {
                const bool connect = !network_is_connected(id);
                network_connect(id, connect ? 1 : 0);
                ui_sb_update_icon_state(SB_NETWORK | id, connect ? 0 : 1);
                config_save();
            }
            return true;

        case MSG_MM_CLEAR_HISTORY:
            impl->ClearHistory();
            return true;

        case MSG_MM_CD_UPDATE:
            if ((id >= 0) && (id < CDROM_NUM))
                impl->CdromUpdateUi(id);
            return true;

        case MSG_MM_USB_TOGGLE:
            {
                const int32 slot = message->GetInt32("slot", -1);
                if (usb_slot_device(slot) != nullptr) {
                    usb_slot_want(slot, !usb_slot_requested(slot));
                    config_save();
                }
                return true;
            }

        case MSG_MM_USB_UNPLUG:
            {
                const int32 slot = message->GetInt32("slot", -1);
                if ((slot >= 0) && (slot < USB_SLOTS)) {
                    usb_slot_want(slot, 0);
                    config_save();
                }
                return true;
            }

        case MSG_MM_USB_MOVE:
            {
                const int32 slot  = message->GetInt32("slot", -1);
                const char *label = message->GetString("label", "");
                if ((slot >= 0) && (slot < USB_SLOTS) && (label[0] != '\0')) {
                    usb_slot_move(slot, label);
                    config_save();
                }
                return true;
            }

        case MSG_MM_USB_NEW:
            impl->UsbNewImage(message->GetString("label", ""));
            return true;

        case MSG_MM_USB_EXISTING:
            impl->UsbOpenFile(message->GetString("label", ""), message->GetBool("wp", false));
            return true;

        case MSG_MM_USB_PLUG:
            impl->UsbPlug(message->GetString("label", ""), message->GetString("path", ""), false);
            return true;

        case MSG_MM_USB_EJECT:
            {
                const char *label = message->GetString("label", "");
                if (label[0] != '\0') {
                    usb_port_flash_want(label, nullptr, 0);
                    config_save();
                }
                return true;
            }

        default:
            return false;
    }
}

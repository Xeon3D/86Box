/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: what the hard disk page and dialog share
 *          (qt_harddrive_common.cpp and qt_settings_bus_tracking.cpp).
 *
 *          Who owns each IDE board and SCSI bus is worked out by the core
 *          (ide_plan(), scsi_plan()) for the machine and the controllers
 *          selected now on the Machine and Storage controllers pages; the
 *          sound and network cards and the USB mass storage device are
 *          taken as saved.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Menu.h>
#include <MenuItem.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "haiku_harddrive_common.hpp"
#include "haiku_settings_common.hpp"
#include "haiku_settings_storage.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/hdd.h>
#include <86box/device.h>
#include <86box/scsi.h>
#include <86box/cdrom.h>
#include <86box/scsi_device.h>
#include <86box/scsi_tape.h>
#include <86box/rdisk.h>
#include <86box/mo.h>
#include <86box/lpt.h>
#include <86box/hdc.h>
#include <86box/hdc_ide.h>
#include <86box/sound.h>
#include <86box/machine.h>
#include <86box/mem.h>
#include <86box/usb.h>
#include <86box/thread.h>
#include <86box/network.h>
}

/* ---------------------------------------------------------- BusTracking */

BusTracking::BusTracking()
{
    Clear();
}

void
BusTracking::Clear()
{
    memset(use, 0, sizeof(use));
}

int
BusTracking::Slot(int bus) const
{
    switch (bus) {
        case HDD_BUS_MFM:
            return 0;
        case HDD_BUS_XTA:
            return 1;
        case HDD_BUS_ESDI:
            return 2;
        case HDD_BUS_IDE:
        case HDD_BUS_ATAPI:
            return 3;
        case HDD_BUS_SCSI:
            return 4;
        case HDD_BUS_LPT:
            return 5;
        default:
            return -1;
    }
}

void
BusTracking::Track(bool set, int bus, int channel)
{
    const int slot = Slot(bus);
    if (channel == CHANNEL_NONE)
        return;
    if (bus == HDD_BUS_LPT)
        channel &= 3;
    if ((slot < 0) || (channel < 0) || (channel >= CHANNELS))
        return;
    if (set)
        use[slot][channel]++;
    else if (use[slot][channel] > 0)
        use[slot][channel]--;
}

bool
BusTracking::InUse(int bus, int channel) const
{
    const int slot = Slot(bus);
    if (channel == CHANNEL_NONE)
        return false;
    if (bus == HDD_BUS_LPT)
        channel &= 3;
    return (slot >= 0) && (channel >= 0) && (channel < CHANNELS) && (use[slot][channel] != 0);
}

std::vector<int>
BusTracking::InUse(int bus) const
{
    std::vector<int> list;
    const int        slot = Slot(bus);
    for (int i = 0; (slot >= 0) && (i < CHANNELS); i++) {
        if (use[slot][i])
            list.push_back(i);
    }
    return list;
}

void
BusTracking::AddSavedDrives()
{
    for (int i = 0; i < CDROM_NUM; i++) {
        if ((cdrom[i].bus_type == CDROM_BUS_ATAPI) || (cdrom[i].bus_type == CDROM_BUS_SCSI) || (cdrom[i].bus_type == CDROM_BUS_LPT))
            Track(true, cdrom[i].bus_type, cdrom[i].ide_channel);
    }
    for (int i = 0; i < RDISK_NUM; i++) {
        const int bus = rdisk_drives[i].bus_type;
        if ((bus == RDISK_BUS_IDE) || (bus == RDISK_BUS_ATAPI) || (bus == RDISK_BUS_SCSI) || (bus == RDISK_BUS_LPT))
            Track(true, bus, rdisk_drives[i].ide_channel);
    }
    for (int i = 0; i < MO_NUM; i++) {
        const int bus = mo_drives[i].bus_type;
        if ((bus == MO_BUS_ATAPI) || (bus == MO_BUS_SCSI) || (bus == MO_BUS_LPT))
            Track(true, bus, mo_drives[i].ide_channel);
    }
    for (int i = 0; i < TAPE_NUM; i++) {
        const int bus = tape_drives[i].bus_type;
        if ((bus == TAPE_BUS_ATAPI) || (bus == TAPE_BUS_SCSI) || (bus == TAPE_BUS_LPT))
            Track(true, bus, tape_drives[i].ide_channel);
    }
}

/* The owner of each IDE board for what the settings hold now. Returns the
   number of boards to show. */
static int
ide_plan_now(bus_owner_t *owners, ide_conflict_t *conflicts = nullptr, int *conflict_count = nullptr)
{
    int hdc[HDC_MAX];
    int snd[SOUND_CARD_MAX];

    for (int i = 0; i < HDC_MAX; i++)
        hdc[i] = settings_current_hdc(i);
    for (int i = 0; i < SOUND_CARD_MAX; i++)
        snd[i] = sound_card_current[i];

    ide_conflict_t dummy[IDE_CONFLICTS_MAX];
    int            dummy_count = 0;
    return ide_plan(owners, SettingsPage::CurrentMachine(), hdc, snd, conflicts ? conflicts : dummy,
                    conflict_count ? conflict_count : &dummy_count);
}

/* The owner of each SCSI bus, as for IDE. Returns the number of buses taken. */
static int
scsi_plan_now(bus_owner_t *owners)
{
    int snd[SOUND_CARD_MAX];
    int scsi[SCSI_CARD_MAX];
    int net[NET_CARD_MAX];

    for (int i = 0; i < SOUND_CARD_MAX; i++)
        snd[i] = sound_card_current[i];
    for (int i = 0; i < SCSI_CARD_MAX; i++)
        scsi[i] = settings_current_scsi_card(i);
    for (int i = 0; i < NET_CARD_MAX; i++)
        net[i] = net_cards_conf[i].device_num;

    return scsi_plan(owners, SettingsPage::CurrentMachine(), snd, net, scsi, usb_msd_type);
}

uint8
BusTracking::NextFree(int bus) const
{
    switch (bus) {
        case HDD_BUS_MFM:
        case HDD_BUS_XTA:
        case HDD_BUS_ESDI:
            for (int i = 0; i < 2; i++) {
                if (!InUse(bus, i))
                    return i;
            }
            break;

        case HDD_BUS_IDE:
        case HDD_BUS_ATAPI:
            {
                /* A free channel on a board with a controller, or failing
                   that the first free one listed. */
                bus_owner_t owners[IDE_BUS_MAX];
                const int   channels = ide_plan_now(owners) * 2;
                for (int pass = 0; pass < 2; pass++) {
                    for (int i = 0; i < channels; i++) {
                        const bool owned = owners[i >> 1].onboard || (owners[i >> 1].device != nullptr);
                        if (((pass == 1) || owned) && !InUse(bus, i))
                            return i;
                    }
                }
                break;
            }

        case HDD_BUS_SCSI:
            {
                bus_owner_t owners[SCSI_BUS_MAX];
                const int   buses = scsi_plan_now(owners);
                for (int pass = 0; pass < 2; pass++) {
                    for (int i = 0; i < (((pass == 0) ? buses : 1) * SCSI_ID_MAX); i++) {
                        if (!InUse(bus, i))
                            return i;
                    }
                }
                break;
            }

        case HDD_BUS_LPT:
            /* The port's first entry: the OnSpec 90c26. */
            for (int i = 0; i < PARALLEL_MAX; i++) {
                if (!InUse(bus, i))
                    return i;
            }
            break;

        default:
            break;
    }

    return CHANNEL_NONE;
}

/* ----------------------------------------------------------- Harddrives */

/* A device's label: its short name, with "(Onboard)" for a chip on the
   machine's board; where there are two or more of a card, each is
   numbered by its slot order, #1 first. */
static std::string
device_label(const device_t *dev, int instance, int onboard, const bus_owner_t *owners, int count,
             const ide_conflict_t *conflicts, int conflict_count)
{
    const std::string name = (dev->short_name != nullptr) ? dev->short_name : dev->name;

    if (onboard)
        return name + " (Onboard)";

    std::vector<int> instances;
    for (int i = 0; i < count; i++) {
        if ((owners[i].device == dev) && (std::find(instances.begin(), instances.end(), owners[i].instance) == instances.end()))
            instances.push_back(owners[i].instance);
    }
    for (int i = 0; i < conflict_count; i++) {
        if ((conflicts[i].device == dev) && (std::find(instances.begin(), instances.end(), conflicts[i].instance) == instances.end()))
            instances.push_back(conflicts[i].instance);
    }
    if (instances.size() > 1) {
        std::sort(instances.begin(), instances.end());
        const int number = (int) (std::find(instances.begin(), instances.end(), instance) - instances.begin()) + 1;
        return name + " #" + std::to_string(number);
    }

    return name;
}

static std::string
owner_name(int bus, const bus_owner_t *owners, int count, const ide_conflict_t *conflicts = nullptr, int conflict_count = 0)
{
    if ((bus < 0) || (bus >= count))
        return "(none)";

    const bus_owner_t &owner = owners[bus];
    if (owner.device == nullptr)
        return owner.onboard ? "Onboard" : "(none)";

    return device_label(owner.device, owner.instance, owner.onboard, owners, count, conflicts, conflict_count);
}

static std::string
conflict_text(int index, const bus_owner_t *owners, int count, const ide_conflict_t *conflicts, int conflict_count)
{
    const ide_conflict_t &c     = conflicts[index];
    const std::string     label = device_label(c.device, c.instance, c.onboard, owners, count, conflicts, conflict_count);

    if (c.lost == 0)
        return label + ": no free channels";

    std::string boards;
    for (int board = 0; board < IDE_BUS_MAX; board++) {
        if (c.lost & (1 << board)) {
            if (!boards.empty())
                boards += ", ";
            boards += std::to_string(board);
        }
    }
    return label + ": channels " + boards + " in use";
}

void
Harddrives::PopulateBuses(Choice *choice)
{
    choice->Clear();
    choice->Add("ST-506/ST-412 (MFM/RLL)", HDD_BUS_MFM);
    choice->Add("XTA", HDD_BUS_XTA);
    choice->Add("ESDI", HDD_BUS_ESDI);
    choice->Add("IDE", HDD_BUS_IDE);
    choice->Add("ATAPI", HDD_BUS_ATAPI);
    choice->Add("SCSI", HDD_BUS_SCSI);
    choice->Add("LPT", HDD_BUS_LPT);
}

void
Harddrives::PopulateSpeeds(Choice *choice, int bus)
{
    int num_preset;

    switch (bus) {
        case HDD_BUS_MFM:
        case HDD_BUS_XTA:
        case HDD_BUS_ESDI:
        case HDD_BUS_IDE:
        case HDD_BUS_ATAPI:
        case HDD_BUS_SCSI:
        case HDD_BUS_LPT:
            num_preset = hdd_preset_get_num();
            break;
        default:
            num_preset = 1;
    }

    choice->Clear();
    for (int i = 0; i < num_preset; i++)
        choice->Add(hdd_preset_getname(i), i);
}

std::string
Harddrives::LptBridgeName(int bridge)
{
    const char *name = hdd_lpt_bridge_get_name(bridge);
    return (name != nullptr) ? name : "";
}

bool
Harddrives::LptPortEnabled(int port)
{
    return (port >= 0) && (port < PARALLEL_MAX) && lpt_ports[port].enabled;
}

/* The last item added to a choice, to disable it. */
static void
disable_last(Choice *choice)
{
    BMenu *menu = choice->field->Menu();
    if (BMenuItem *item = menu->ItemAt(menu->CountItems() - 1))
        item->SetEnabled(false);
}

void
Harddrives::PopulateBusChannels(Choice *choice, int bus, const BusTracking &tracking)
{
    char           text[512];
    bus_owner_t    owners[(IDE_BUS_MAX > SCSI_BUS_MAX) ? IDE_BUS_MAX : SCSI_BUS_MAX];
    ide_conflict_t conflicts[IDE_CONFLICTS_MAX];
    int            conflict_count = 0;
    int            owned          = 0;
    int            rows           = 0;
    const bool     is_ide         = (bus == HDD_BUS_IDE) || (bus == HDD_BUS_ATAPI);
    const bool     is_scsi        = (bus == HDD_BUS_SCSI);

    choice->Clear();

    if (bus == HDD_BUS_LPT) {
        /* Each port once per bridge; the port is in use either way. */
        for (int i = 0; i < (PARALLEL_MAX * HDD_LPT_BRIDGES); i++) {
            const int port   = i / HDD_LPT_BRIDGES;
            const int bridge = i % HDD_LPT_BRIDGES;
            snprintf(text, sizeof(text), "LPT%i %s", port + 1, LptBridgeName(bridge).c_str());
            choice->Add(text, port | (bridge << 2));
            if (tracking.InUse(bus, port) || !LptPortEnabled(port))
                disable_last(choice);
        }
        return;
    }

    switch (bus) {
        case HDD_BUS_MFM:
        case HDD_BUS_XTA:
        case HDD_BUS_ESDI:
            rows = 2;
            break;
        case HDD_BUS_IDE:
        case HDD_BUS_ATAPI:
            owned = ide_plan_now(owners, conflicts, &conflict_count);
            rows  = owned * 2;
            break;
        case HDD_BUS_SCSI:
            /* The buses something has, at least the first. */
            owned = scsi_plan_now(owners);
            rows  = ((owned > 0) ? owned : 1) * SCSI_ID_MAX;
            break;
        default:
            break;
    }

    /* A drive left on a board with no controller any more still has its
       channel listed, so that it shows where it is. */
    for (const int channel : tracking.InUse(bus)) {
        if (is_ide && (channel < IDE_DRIVES_MAX) && (channel >= rows))
            rows = (channel | 1) + 1;
        else if (is_scsi && (channel < (SCSI_BUS_MAX * SCSI_ID_MAX)) && (channel >= rows))
            rows = (channel | 15) + 1;
    }

    for (int i = 0; i < rows; i++) {
        if (is_ide)
            snprintf(text, sizeof(text), "%i:%i %s", i >> 1, i & 1, owner_name(i >> 1, owners, owned, conflicts, conflict_count).c_str());
        else if (is_scsi)
            snprintf(text, sizeof(text), "%i:%02i %s", i >> 4, i & 15, owner_name(i >> 4, owners, owned).c_str());
        else
            snprintf(text, sizeof(text), "%i:%i", i >> 1, i & 1);
        choice->Add(text, i);

        bool enabled = !tracking.InUse(bus, i);
        if (is_ide && !owners[i >> 1].onboard && (owners[i >> 1].device == nullptr))
            enabled = false;
        if (is_scsi && (((i >> 4) >= owned) || (!owners[i >> 4].onboard && (owners[i >> 4].device == nullptr))))
            enabled = false;
        if (!enabled)
            disable_last(choice);
    }

    /* Then, not to be picked, each controller left without its channels. */
    for (int i = 0; is_ide && (i < conflict_count); i++) {
        choice->Add(conflict_text(i, owners, owned, conflicts, conflict_count).c_str(), CHANNEL_NONE);
        disable_last(choice);
    }
}

std::string
Harddrives::BusChannelName(uint8 bus, uint8 channel)
{
    char text[512];

    switch (bus) {
        case HDD_BUS_DISABLED:
            return "Disabled";
        case HDD_BUS_MFM:
            snprintf(text, sizeof(text), "MFM/RLL (%i:%i)", channel >> 1, channel & 1);
            break;
        case HDD_BUS_XTA:
            snprintf(text, sizeof(text), "XTA (%i:%i)", channel >> 1, channel & 1);
            break;
        case HDD_BUS_ESDI:
            snprintf(text, sizeof(text), "ESDI (%i:%i)", channel >> 1, channel & 1);
            break;
        case HDD_BUS_IDE:
        case HDD_BUS_ATAPI:
            {
                bus_owner_t owners[IDE_BUS_MAX];
                const int   owned = ide_plan_now(owners);
                snprintf(text, sizeof(text), "%s %i:%i %s", (bus == HDD_BUS_IDE) ? "IDE" : "ATAPI", channel >> 1, channel & 1,
                         owner_name(channel >> 1, owners, owned).c_str());
                break;
            }
        case HDD_BUS_SCSI:
            {
                bus_owner_t owners[SCSI_BUS_MAX];
                const int   owned = scsi_plan_now(owners);
                snprintf(text, sizeof(text), "SCSI %i:%02i %s", channel >> 4, channel & 15, owner_name(channel >> 4, owners, owned).c_str());
                break;
            }
        case HDD_BUS_LPT:
            if (channel == CHANNEL_NONE)
                return "LPT";
            snprintf(text, sizeof(text), "LPT%i (%s)", (channel & 3) + 1, LptBridgeName(channel >> 2).c_str());
            break;
        default:
            return "";
    }

    return text;
}

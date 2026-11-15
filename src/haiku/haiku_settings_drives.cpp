/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: what the drive pages of Settings share
 *          (qt_settings_bus_tracking.cpp and qt_harddrive_common.cpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Menu.h>
#include <MenuField.h>
#include <MenuItem.h>

#include <algorithm>
#include <cstdio>

#include "haiku_settings_drives.hpp"

/* The controllers chosen on the Storage controllers page (not yet saved). */
#if __has_include("haiku_settings_storage.hpp")
#    include "haiku_settings_storage.hpp"
#else
/* Until haiku_settings_storage.hpp exists: defined by the Storage
   controllers page (Settings::currentHdc() and currentScsiCard() in Qt). */
int settings_current_hdc(int i);
int settings_current_scsi_card(int i);
#endif

extern "C" {
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/fdd.h>
#include <86box/hdd.h>
#include <86box/hdc.h>
#include <86box/hdc_ide.h>
#include <86box/scsi.h>
#include <86box/cdrom.h>
#include <86box/scsi_device.h>
#include <86box/rdisk.h>
#include <86box/mo.h>
#include <86box/scsi_tape.h>
#include <86box/lpt.h>
#include <86box/sound.h>
#include <86box/machine.h>
#include <86box/mem.h>
#include <86box/usb.h>
#include <86box/thread.h>
#include <86box/network.h>
}

/* ------------------------------------------------------------ tracking */

static uint8
next_free(uint64 tracking, int count)
{
    for (int i = 0; i < count; i++) {
        if (!(tracking & (0xffULL << ((i << 3) & 0x3f))))
            return (uint8) i;
    }
    return CHANNEL_NONE;
}

uint8
BusTracking::NextFreeHitachiChannel() const
{
    return next_free(hitachi_tracking, 4);
}

uint8
BusTracking::NextFreeMkeChannel() const
{
    return next_free(mke_tracking, 4);
}

uint8
BusTracking::NextFreeMfmChannel() const
{
    return next_free(mfm_tracking, 2);
}

uint8
BusTracking::NextFreeEsdiChannel() const
{
    return next_free(esdi_tracking, 2);
}

uint8
BusTracking::NextFreeXtaChannel() const
{
    return next_free(xta_tracking, 2);
}

uint8
BusTracking::NextFreeFdcUnit() const
{
    return next_free(fdc_tracking, 4);
}

uint8
BusTracking::NextFreeLptPort() const
{
    return next_free(lpt_tracking, 4);
}

/* The first free channel on a board with a controller, or failing that the
   first free one listed. */
uint8
BusTracking::NextFreeIdeChannel() const
{
    bus_owner_t owners[IDE_BUS_MAX];
    const int   channels = drives_ide_plan(owners) * 2;

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < channels; i++) {
            const int    element = ((i << 3) >> 6);
            const uint64 mask    = 0xffULL << ((i << 3) & 0x3f);
            const bool   owned   = owners[i >> 1].onboard || (owners[i >> 1].device != nullptr);

            if (((pass == 1) || owned) && !(ide_tracking[element] & mask))
                return (uint8) i;
        }
    }

    return CHANNEL_NONE;
}

/* The first free ID on a bus something has, or failing that the first
   free one on the first bus. */
uint8
BusTracking::NextFreeScsiId() const
{
    bus_owner_t owners[SCSI_BUS_MAX];
    const int   buses = drives_scsi_plan(owners);

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < (((pass == 0) ? buses : 1) * SCSI_ID_MAX); i++) {
            const int    element = ((i << 3) >> 6);
            const uint64 mask    = 0xffULL << ((i << 3) & 0x3f);

            if (!(scsi_tracking[element] & mask))
                return (uint8) i;
        }
    }

    return CHANNEL_NONE;
}

std::vector<int>
BusTracking::BusChannelsInUse(int bus) const
{
    std::vector<int> in_use;

    auto scan = [&in_use](uint64 tracking, int count) {
        for (int i = 0; i < count; i++) {
            if (tracking & (0xffULL << ((i << 3) & 0x3f)))
                in_use.push_back(i);
        }
    };
    auto scan_array = [&in_use](const uint64 *tracking, int count) {
        for (int i = 0; i < count; i++) {
            if (tracking[(i << 3) >> 6] & (0xffULL << ((i << 3) & 0x3f)))
                in_use.push_back(i);
        }
    };

    switch (bus) {
        case CDROM_BUS_HITACHI:
            scan(hitachi_tracking, 4);
            break;
        case CDROM_BUS_MKE:
            scan(mke_tracking, 4);
            break;
        case CDROM_BUS_PHILIPS:
            if (philips_tracking)
                in_use.push_back(0);
            break;
        case CDROM_BUS_SONY:
            if (sony_tracking)
                in_use.push_back(0);
            break;
        case CDROM_BUS_CM100:
            if (cm100_tracking)
                in_use.push_back(0);
            break;
        case CDROM_BUS_MITSUMI:
            if (mitsumi_tracking)
                in_use.push_back(0);
            break;
        case HDD_BUS_MFM:
            scan(mfm_tracking, 2);
            break;
        case HDD_BUS_ESDI:
            scan(esdi_tracking, 2);
            break;
        case HDD_BUS_XTA:
            scan(xta_tracking, 2);
            break;
        case HDD_BUS_IDE:
        case HDD_BUS_ATAPI:
            scan_array(ide_tracking, 32);
            break;
        case HDD_BUS_SCSI:
            scan_array(scsi_tracking, SCSI_BUS_MAX * SCSI_ID_MAX);
            break;
        case TAPE_BUS_FDC:
            scan(fdc_tracking, 4);
            break;
        case TAPE_BUS_LPT:
            scan(lpt_tracking, 4);
            break;
        default:
            break;
    }

    return in_use;
}

void
BusTracking::DeviceTrack(int set, uint8 dev_type, int bus, int channel)
{
    auto apply = [set](uint64 &tracking, uint64 mask) {
        if (set)
            tracking |= mask;
        else
            tracking &= ~mask;
    };
    const uint64 mask = ((uint64) dev_type) << ((channel << 3) & 0x3f);

    switch (bus) {
        case CDROM_BUS_HITACHI:
            apply(hitachi_tracking, mask);
            break;
        case CDROM_BUS_MKE:
            apply(mke_tracking, mask);
            break;
        case CDROM_BUS_PHILIPS:
            philips_tracking = set;
            break;
        case CDROM_BUS_SONY:
            sony_tracking = set;
            break;
        case CDROM_BUS_CM100:
            cm100_tracking = set;
            break;
        case CDROM_BUS_MITSUMI:
            mitsumi_tracking = set;
            break;
        case HDD_BUS_MFM:
            apply(mfm_tracking, mask);
            break;
        case HDD_BUS_ESDI:
            apply(esdi_tracking, mask);
            break;
        case HDD_BUS_XTA:
            apply(xta_tracking, mask);
            break;
        case HDD_BUS_IDE:
        case HDD_BUS_ATAPI:
            apply(ide_tracking[(channel << 3) >> 6], mask);
            break;
        case HDD_BUS_SCSI:
            apply(scsi_tracking[(channel << 3) >> 6], mask);
            break;
        case TAPE_BUS_FDC:
            apply(fdc_tracking, mask);
            break;
        case TAPE_BUS_LPT:
            /* A drive's channel may also hold its bridge: port | bridge << 2. */
            apply(lpt_tracking, ((uint64) dev_type) << (((channel & 3) << 3) & 0x3f));
            break;
        default:
            break;
    }
}

static BusTracking *tracking      = nullptr;
static int          tracking_refs = 0;

/* As the pages' constructors fill it in Qt, from the saved drives. */
static void
bus_tracking_fill(BusTracking *sbt)
{
    for (int i = 0; i < HDD_NUM; i++) {
        if (hdd[i].bus_type != HDD_BUS_DISABLED)
            sbt->DeviceTrack(1, DEV_HDD, hdd[i].bus_type, hdd[i].channel);
    }

    for (int i = 0; i < FDD_NUM; i++)
        sbt->DeviceTrack(fdd_get_type(&drives[i]) ? 1 : 0, DEV_FDD, TAPE_BUS_FDC, i);

    for (int i = 0; i < CDROM_NUM; i++) {
        switch (cdrom[i].bus_type) {
            case CDROM_BUS_MITSUMI:
            case CDROM_BUS_PHILIPS:
            case CDROM_BUS_CM100:
            case CDROM_BUS_SONY:
                sbt->DeviceTrack(1, DEV_CDROM, cdrom[i].bus_type, 0);
                break;
            case CDROM_BUS_HITACHI:
            case CDROM_BUS_MKE:
            case CDROM_BUS_ATAPI:
            case CDROM_BUS_SCSI:
            case CDROM_BUS_LPT:
                sbt->DeviceTrack(1, DEV_CDROM, cdrom[i].bus_type, cdrom[i].res);
                break;
            default:
                break;
        }
    }

    for (int i = 0; i < RDISK_NUM; i++)
        sbt->DeviceTrack(1, DEV_RDISK, rdisk_drives[i].bus_type, rdisk_drives[i].res);
    for (int i = 0; i < MO_NUM; i++)
        sbt->DeviceTrack(1, DEV_MO, mo_drives[i].bus_type, mo_drives[i].res);
    for (int i = 0; i < TAPE_NUM; i++)
        sbt->DeviceTrack(1, DEV_TAPE, tape_drives[i].bus_type, tape_drives[i].res);
}

BusTracking *
settings_bus_tracking_acquire()
{
    if (tracking_refs++ == 0) {
        tracking = new BusTracking();
        bus_tracking_fill(tracking);
    }
    return tracking;
}

void
settings_bus_tracking_release()
{
    if ((tracking_refs > 0) && (--tracking_refs == 0)) {
        delete tracking;
        tracking = nullptr;
    }
}

/* ------------------------------------------------------- bus owners */

/* The owner of each IDE board for what the settings hold now: the machine
   and disk controllers selected; the sound cards as saved (the Sound page
   does not publish its selection). Returns the number of boards to show. */
int
drives_ide_plan(bus_owner_t *owners, ide_conflict_t *conflicts, int *conflict_count)
{
    int hdc[HDC_MAX];
    int snd[SOUND_CARD_MAX];

    for (int i = 0; i < HDC_MAX; i++)
        hdc[i] = settings_current_hdc(i);
    for (int i = 0; i < SOUND_CARD_MAX; i++)
        snd[i] = sound_card_current[i];

    return ide_plan(owners, SettingsPage::CurrentMachine(), hdc, snd, conflicts, conflict_count);
}

/* The owner of each SCSI bus, as for IDE: the machine and SCSI cards
   selected; sound and network cards and the USB mass storage as saved. */
int
drives_scsi_plan(bus_owner_t *owners)
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

/* A device's label: its short name, with "(Onboard)" for a chip on the
   machine's board; where there are two or more of a card, each is numbered
   by its slot order, #1 first. */
static std::string
device_label(const device_t *dev, int instance, int onboard, const bus_owner_t *owners, int count,
             const ide_conflict_t *conflicts, int conflict_count)
{
    const std::string name = (dev->short_name != nullptr) ? dev->short_name : dev->name;

    if (onboard)
        return name + " (Onboard)";

    std::vector<int> instances;
    auto             note = [&instances](int inst) {
        if (std::find(instances.begin(), instances.end(), inst) == instances.end())
            instances.push_back(inst);
    };
    for (int i = 0; i < count; i++) {
        if (owners[i].device == dev)
            note(owners[i].instance);
    }
    for (int i = 0; i < conflict_count; i++) {
        if (conflicts[i].device == dev)
            note(conflicts[i].instance);
    }
    if (instances.size() > 1) {
        std::sort(instances.begin(), instances.end());
        const int at = (int) (std::find(instances.begin(), instances.end(), instance) - instances.begin());
        return name + " #" + std::to_string(at + 1);
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

/* ----------------------------------------------------- channel names */

std::string
drives_lpt_bridge_name(int bridge)
{
    const char *name = hdd_lpt_bridge_get_name(bridge);
    return (name != nullptr) ? name : "";
}

bool
drives_lpt_port_enabled(int port)
{
    return (port >= 0) && (port < PARALLEL_MAX) && lpt_ports[port].enabled;
}

std::string
drives_bus_channel_name(uint8 bus, uint8 channel)
{
    char temp[512];

    switch (bus) {
        case HDD_BUS_DISABLED:
            return "Disabled";
        case HDD_BUS_MFM:
            snprintf(temp, sizeof(temp), "MFM/RLL (%i:%i)", channel >> 1, channel & 1);
            return temp;
        case HDD_BUS_XTA:
            snprintf(temp, sizeof(temp), "XTA (%i:%i)", channel >> 1, channel & 1);
            return temp;
        case HDD_BUS_ESDI:
            snprintf(temp, sizeof(temp), "ESDI (%i:%i)", channel >> 1, channel & 1);
            return temp;
        case HDD_BUS_IDE:
        case HDD_BUS_ATAPI:
            {
                bus_owner_t owners[IDE_BUS_MAX];
                const int   owned = drives_ide_plan(owners);

                snprintf(temp, sizeof(temp), "%s %i:%i %s", (bus == HDD_BUS_IDE) ? "IDE" : "ATAPI",
                         channel >> 1, channel & 1, owner_name(channel >> 1, owners, owned).c_str());
                return temp;
            }
        case HDD_BUS_SCSI:
            {
                bus_owner_t owners[SCSI_BUS_MAX];
                const int   owned = drives_scsi_plan(owners);

                snprintf(temp, sizeof(temp), "SCSI %i:%02i %s", channel >> 4, channel & 15,
                         owner_name(channel >> 4, owners, owned).c_str());
                return temp;
            }
        case CDROM_BUS_PHILIPS:
            return "Philips/LMS";
        case CDROM_BUS_SONY:
            return "Sony";
        case CDROM_BUS_CM100:
            return "Philips CM-100/CM-153";
        case CDROM_BUS_MITSUMI:
            return "Mitsumi";
        case CDROM_BUS_HITACHI:
            snprintf(temp, sizeof(temp), "Hitachi (%i:%i)", channel >> 2, channel & 3);
            return temp;
        case CDROM_BUS_MKE:
            snprintf(temp, sizeof(temp), "Panasonic/MKE (%i:%i)", channel >> 2, channel & 3);
            return temp;
        case TAPE_BUS_FDC:
            snprintf(temp, sizeof(temp), "FDC (%i:%i)", channel >> 2, channel & 3);
            return temp;
        case TAPE_BUS_LPT:
            snprintf(temp, sizeof(temp), "LPT%i", channel + 1);
            return temp;
        default:
            return "";
    }
}

/* Harddrives::populateBusChannels(): the channels of a bus, those in use
   (and on IDE and SCSI, those of a board nobody has) not to be picked. */
void
drives_populate_bus_channels(Choice *channel, int bus, const BusTracking *sbt, int lpt_bridges)
{
    channel->Clear();

    int              bus_rows          = 0;
    int              shifter           = 1;
    int              orer              = 1;
    int              sub_channel_width = 1;
    std::vector<int> buses_to_check;
    std::vector<int> in_use;
    bus_owner_t      owners[(IDE_BUS_MAX > SCSI_BUS_MAX) ? IDE_BUS_MAX : SCSI_BUS_MAX] = {};
    ide_conflict_t   conflicts[IDE_CONFLICTS_MAX];
    int              conflict_count = 0;
    int              owned          = 0;
    const bool       is_ide         = (bus == HDD_BUS_IDE) || (bus == HDD_BUS_ATAPI);
    const bool       is_scsi        = (bus == HDD_BUS_SCSI);

    switch (bus) {
        case HDD_BUS_MFM:
        case HDD_BUS_XTA:
        case HDD_BUS_ESDI:
            bus_rows = 2;
            buses_to_check.push_back(bus);
            break;
        case HDD_BUS_IDE:
        case HDD_BUS_ATAPI:
            owned    = drives_ide_plan(owners, conflicts, &conflict_count);
            bus_rows = owned * 2;
            buses_to_check.push_back(HDD_BUS_ATAPI);
            buses_to_check.push_back(HDD_BUS_IDE);
            break;
        case HDD_BUS_SCSI:
            shifter           = 4;
            orer              = 15;
            /* The buses something has, at least the first. */
            owned             = drives_scsi_plan(owners);
            bus_rows          = ((owned > 0) ? owned : 1) * SCSI_ID_MAX;
            sub_channel_width = 2;
            buses_to_check.push_back(HDD_BUS_SCSI);
            break;
        case CDROM_BUS_HITACHI:
        case CDROM_BUS_MKE:
            shifter  = 2;
            orer     = 3;
            bus_rows = 4;
            buses_to_check.push_back(bus);
            break;
        case TAPE_BUS_FDC:
            shifter  = 2;
            orer     = 3;
            bus_rows = 8;
            buses_to_check.push_back(TAPE_BUS_FDC);
            break;
        case TAPE_BUS_LPT:
            shifter  = 0;
            orer     = 0;
            bus_rows = 4;
            buses_to_check.push_back(TAPE_BUS_LPT);
            break;
        default:
            break;
    }
    if (sbt != nullptr) {
        for (const int check : buses_to_check) {
            const std::vector<int> used = sbt->BusChannelsInUse(check);
            in_use.insert(in_use.end(), used.begin(), used.end());
        }
    }
    auto used = [&in_use](int c) { return std::find(in_use.begin(), in_use.end(), c) != in_use.end(); };

    /* A drive left on a board with no controller any more still has its
       channel listed, so that it shows where it is. */
    for (const int c : in_use) {
        if (is_ide && (c < IDE_DRIVES_MAX) && (c >= bus_rows))
            bus_rows = (c | 1) + 1;
        else if (is_scsi && (c < (SCSI_BUS_MAX * SCSI_ID_MAX)) && (c >= bus_rows))
            bus_rows = (c | 15) + 1;
    }

    char temp[512];

    if ((bus == TAPE_BUS_LPT) && lpt_bridges) {
        /* Each port once per bridge; the port is in use either way. */
        for (int i = 0; i < (PARALLEL_MAX * HDD_LPT_BRIDGES); ++i) {
            const int port   = i / HDD_LPT_BRIDGES;
            const int bridge = i % HDD_LPT_BRIDGES;

            snprintf(temp, sizeof(temp), "LPT%i %s", port + 1, drives_lpt_bridge_name(bridge).c_str());
            channel->Add(temp, port | (bridge << 2));
            choice_set_item_enabled(channel, i, !used(port) && lpt_ports[port].enabled &&
                                                    ((lpt_bridges != LptBridgesCdrom) || cdrom_lpt_bridge_valid(bridge)));
        }
        return;
    }

    for (int i = 0; i < bus_rows; ++i) {
        if (bus == TAPE_BUS_LPT)
            snprintf(temp, sizeof(temp), "LPT%i", i + 1);
        else if (is_ide)
            snprintf(temp, sizeof(temp), "%i:%i %s", i >> 1, i & 1,
                     owner_name(i >> 1, owners, owned, conflicts, conflict_count).c_str());
        else if (is_scsi)
            snprintf(temp, sizeof(temp), "%i:%02i %s", i >> 4, i & 15, owner_name(i >> 4, owners, owned).c_str());
        else
            snprintf(temp, sizeof(temp), "%i:%0*i", i >> shifter, sub_channel_width, i & orer);
        channel->Add(temp, ((i >> shifter) << shifter) | (i & orer));

        bool enabled = !used(i);
        if ((bus == TAPE_BUS_LPT) && (i < PARALLEL_MAX) && !lpt_ports[i].enabled)
            enabled = false;
        if (is_ide && !owners[i >> 1].onboard && (owners[i >> 1].device == nullptr))
            enabled = false;
        if (is_scsi && (((i >> 4) >= owned) || (!owners[i >> 4].onboard && (owners[i >> 4].device == nullptr))))
            enabled = false;
        choice_set_item_enabled(channel, i, enabled);
    }

    /* Then, not to be picked, each controller left without its channels. */
    for (int i = 0; is_ide && (i < conflict_count); i++) {
        channel->Add(conflict_text(i, owners, owned, conflicts, conflict_count).c_str(), CHANNEL_NONE);
        choice_set_item_enabled(channel, bus_rows + i, false);
    }
}

/* ---------------------------------------------------- Choice helpers */

int32
choice_index(const Choice *choice)
{
    BMenu *menu = choice->field->Menu();
    return menu->IndexOf(menu->FindMarked());
}

int32
choice_value_at(const Choice *choice, int32 row)
{
    BMenuItem *item = choice->field->Menu()->ItemAt(row);
    return ((item != nullptr) && (item->Message() != nullptr)) ? item->Message()->GetInt32("value", -1) : -1;
}

void
choice_pick(Choice *choice, int32 value)
{
    const bool enabled = choice->field->IsEnabled();
    choice->Select(value);
    choice->SetEnabled(enabled);
}

void
choice_pick_index(Choice *choice, int32 row)
{
    BMenu     *menu = choice->field->Menu();
    BMenuItem *item = menu->ItemAt(row);

    if ((row >= 0) && (item != nullptr)) {
        item->SetMarked(true);
        return;
    }
    if (BMenuItem *marked = menu->FindMarked())
        marked->SetMarked(false);
}

void
choice_set_item_enabled(Choice *choice, int32 row, bool enabled)
{
    if (BMenuItem *item = choice->field->Menu()->ItemAt(row))
        item->SetEnabled(enabled);
}

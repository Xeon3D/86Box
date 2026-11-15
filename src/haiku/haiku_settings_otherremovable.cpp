/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Other removable devices Settings page
 *          (qt_settingsotherremovable.cpp): MO, removable disk (Zip, Jaz,
 *          SyQuest...) and tape drives.
 *
 *          As in Qt, each list keeps the drives' values until Save() writes
 *          them to the core; the controls below a list edit its selected
 *          drive.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Menu.h>
#include <ScrollView.h>
#include <TabView.h>

#include <cstdio>
#include <string>

#include "haiku_icons.hpp"
#include "haiku_settings_common.hpp"
#include "haiku_settings_drives.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/scsi_device.h>
#include <86box/mo.h>
#include <86box/hdd.h>
#include <86box/rdisk.h>
#include <86box/scsi_tape.h>
}

enum : uint32 {
    MSG_MO_ROW        = 'srmr',
    MSG_MO_BUS        = 'srmb',
    MSG_MO_CHANNEL    = 'srmc',
    MSG_MO_TYPE       = 'srmt',
    MSG_RDISK_ROW     = 'srrr',
    MSG_RDISK_BUS     = 'srrb',
    MSG_RDISK_CHANNEL = 'srrc',
    MSG_RDISK_TYPE    = 'srrt',
    MSG_TAPE_ROW      = 'srtr',
    MSG_TAPE_BUS      = 'srtb',
    MSG_TAPE_CHANNEL  = 'srtc',
    MSG_TAPE_TYPE     = 'srtt',
};

static std::string
drive_type_name(const char *vendor, const char *model, const char *revision)
{
    return std::string(vendor) + " " + model + " " + revision;
}

static std::string
mo_type_name(uint32 i)
{
    return drive_type_name(mo_drive_types[i].vendor, mo_drive_types[i].model, mo_drive_types[i].revision);
}

static std::string
rdisk_type_name(uint32 i)
{
    return drive_type_name(rdisk_drive_types[i].vendor, rdisk_drive_types[i].model, rdisk_drive_types[i].revision);
}

static std::string
tape_type_name(uint32 i)
{
    return drive_type_name(tape_drive_types[i].vendor, tape_drive_types[i].model, tape_drive_types[i].revision);
}

static bool
tape_type_bus_compatible(int bus, uint32 type)
{
    if (type >= KNOWN_TAPE_DRIVE_TYPES)
        return false;

    switch (tape_drive_types[type].drive_type) {
        case TAPE_DRIVE_TYPE_SCSI_ATAPI:
            return (bus == TAPE_BUS_ATAPI) || (bus == TAPE_BUS_SCSI);
        case TAPE_DRIVE_TYPE_FDC:
            return (bus == TAPE_BUS_FDC);
        case TAPE_DRIVE_TYPE_LPT:
            return (bus == TAPE_BUS_LPT);
        default:
            return false;
    }
}

static uint32
first_compatible_tape_type(int bus)
{
    for (uint32 i = 0; i < KNOWN_TAPE_DRIVE_TYPES; i++) {
        if (tape_type_bus_compatible(bus, i))
            return i;
    }
    return 0;
}

/* Whether a removable disk drive type belongs on a bus, from its drive_type
   in rdisk.h. */
static bool
rdisk_type_bus_compatible(int bus, uint32 type)
{
    if (type >= KNOWN_RDISK_DRIVE_TYPES)
        return false;

    const int drive_type = rdisk_drive_types[type].drive_type;

    /* On bus LPT an ATAPI drive sits behind one of the IDE bridges. */
    if ((bus == RDISK_BUS_LPT) && rdisk_drive_type_supports_bus(drive_type, RDISK_BUS_ATAPI))
        return true;

    return rdisk_drive_type_supports_bus(drive_type, (uint8_t) bus) != 0;
}

static uint32
first_compatible_rdisk_type(int bus)
{
    for (uint32 i = 0; i < KNOWN_RDISK_DRIVE_TYPES; i++) {
        if (rdisk_type_bus_compatible(bus, i))
            return i;
    }
    return 0;
}

/* A Zip drive on the parallel port has its own bridge chip. */
static bool
rdisk_own_chip(uint32 type)
{
    return (type == RDISK_TYPE_ZIP_100) || (type == RDISK_TYPE_ZIP_100_PLUS) || (type == RDISK_TYPE_ZIP_250);
}

/* On bus LPT the list's channel is the port and, above it, the bridge. */
static std::string
rdisk_channel_name(uint8 bus, uint32 type, uint8 channel)
{
    if ((bus == RDISK_BUS_LPT) && (channel != CHANNEL_NONE)) {
        char temp[256];
        if (rdisk_own_chip(type))
            snprintf(temp, sizeof(temp), "LPT%i", (channel & 3) + 1);
        else
            snprintf(temp, sizeof(temp), "LPT%i (%s)", (channel & 3) + 1, drives_lpt_bridge_name(channel >> 2).c_str());
        return temp;
    }

    return drives_bus_channel_name(bus, channel);
}

/* A Zip drive on bus LPT: of each port only the Shuttle EPAT entry can be
   chosen, and a selected other bridge of that port becomes it. */
static void
restrict_rdisk_bridges(Choice *channel, int bus, uint32 type)
{
    if ((bus != RDISK_BUS_LPT) || !rdisk_own_chip(type))
        return;

    const int32 count = channel->field->Menu()->CountItems();
    for (int32 row = 0; row < count; row++) {
        if ((((uint32) choice_value_at(channel, row)) >> 2) != HDD_LPT_EPAT)
            choice_set_item_enabled(channel, row, false);
    }

    if (choice_index(channel) < 0)
        return;
    const uint32 current = channel->Value(0);
    if ((current >> 2) == HDD_LPT_EPAT)
        return;
    const int32 wanted = (current & 3) | (HDD_LPT_EPAT << 2);
    if (channel->Has(wanted))
        choice_pick(channel, wanted);
}

/* What the bus tracking counts: on bus LPT only the port. */
static int
rdisk_track_channel(int bus, int channel)
{
    return (bus == RDISK_BUS_LPT) ? (channel & 3) : channel;
}

static uint8
rdisk_list_channel(const rdisk_drive_t *drv)
{
    return (drv->bus_type == RDISK_BUS_LPT) ? (uint8) (drv->res | (drv->lpt_bridge << 2)) : drv->res;
}

static uint8
next_free_rdisk_channel(const BusTracking *sbt, int bus)
{
    switch (bus) {
        case RDISK_BUS_IDE:
        case RDISK_BUS_ATAPI:
            return sbt->NextFreeIdeChannel();
        case RDISK_BUS_LPT:
            {
                /* The port's EPAT entry. */
                const uint8 port = sbt->NextFreeLptPort();
                return (port == CHANNEL_NONE) ? port : (uint8) ((port * HDD_LPT_BRIDGES) + HDD_LPT_EPAT);
            }
        default:
            return sbt->NextFreeScsiId();
    }
}

static uint8
next_free_tape_channel(const BusTracking *sbt, int bus)
{
    switch (bus) {
        case TAPE_BUS_ATAPI:
            return sbt->NextFreeIdeChannel();
        case TAPE_BUS_SCSI:
            return sbt->NextFreeScsiId();
        case TAPE_BUS_FDC:
            return sbt->NextFreeFdcUnit();
        case TAPE_BUS_LPT:
            return sbt->NextFreeLptPort();
        default:
            return 0;
    }
}

/* Qt's rdiskIconForType(). */
static BBitmap *
rdisk_icon(int type)
{
    switch (type) {
        case RDISK_TYPE_ZIP_100:
        case RDISK_TYPE_ZIP_100_PLUS:
        case RDISK_TYPE_ZIP_250:
        case RDISK_TYPE_ZIP_750:
            return haiku_icon("zip");
        case RDISK_TYPE_JAZ_1GB:
        case RDISK_TYPE_JAZ_2GB:
            return haiku_icon("jaz");
        case RDISK_TYPE_SYJET_1_5GB:
        case RDISK_TYPE_SYJET_S:
        case RDISK_TYPE_SPARQ_1GB:
        case RDISK_TYPE_SQ5200C:
        case RDISK_TYPE_SQ3105S:
        case RDISK_TYPE_EZ230S:
        case RDISK_TYPE_EZ135A:
        case RDISK_TYPE_EZ230A:
        case RDISK_TYPE_SQ3105A:
        case RDISK_TYPE_SQ3270A:
        case RDISK_TYPE_EZ135S:
        case RDISK_TYPE_SQ3270S:
        case RDISK_TYPE_SQ5110C:
        case RDISK_TYPE_SQ555:
            return haiku_icon("syquest");
        default:
            return haiku_icon("rdisk");
    }
}

static const std::vector<float> removable_columns = { 0.40f, 0.60f };

class OtherRemovablePage : public SettingsPage {
public:
    OtherRemovablePage()
        : SettingsPage("Other removable devices")
        , sbt(settings_bus_tracking_acquire())
        , mo_bus(new Choice("mo_bus", "Bus:", MSG_MO_BUS))
        , mo_channel(new Choice("mo_channel", "Channel:", MSG_MO_CHANNEL))
        , mo_type(new Choice("mo_type", "Type:", MSG_MO_TYPE))
        , rd_bus(new Choice("rdisk_bus", "Bus:", MSG_RDISK_BUS))
        , rd_channel(new Choice("rdisk_channel", "Channel:", MSG_RDISK_CHANNEL))
        , rd_type(new Choice("rdisk_type", "Type:", MSG_RDISK_TYPE))
        , tp_bus(new Choice("tape_bus", "Bus:", MSG_TAPE_BUS))
        , tp_channel(new Choice("tape_channel", "Channel:", MSG_TAPE_CHANNEL))
        , tp_type(new Choice("tape_type", "Type:", MSG_TAPE_TYPE))
    {
        mo_list = new BListView("mo_list");
        mo_list->SetSelectionMessage(new BMessage(MSG_MO_ROW));
        rd_list = new BListView("rdisk_list");
        rd_list->SetSelectionMessage(new BMessage(MSG_RDISK_ROW));
        tp_list = new BListView("tape_list");
        tp_list->SetSelectionMessage(new BMessage(MSG_TAPE_ROW));

        BTabView *tabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);
        AddIconTab(tabs, MakeTab("MO drives", "mo_scroll", mo_list, mo_bus, mo_channel, mo_type), "mo");
        AddIconTab(tabs, MakeTab("Removable disk drives", "rdisk_scroll", rd_list, rd_bus, rd_channel, rd_type), "rdisk_tab");
        AddIconTab(tabs, MakeTab("Tape drives", "tape_scroll", tp_list, tp_bus, tp_channel, tp_type), "tape");
        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .Add(tabs);

        /* MO drives. */
        mo_bus->Add("Disabled", MO_BUS_DISABLED);
        mo_bus->Add("ATAPI", MO_BUS_ATAPI);
        mo_bus->Add("SCSI", MO_BUS_SCSI);
        for (uint32 i = 0; i < KNOWN_MO_DRIVE_TYPES; i++)
            mo_type->Add(mo_type_name(i).c_str(), i);
        for (int i = 0; i < MO_NUM; i++) {
            mo[i] = { mo_drives[i].bus_type, mo_drives[i].res, mo_drives[i].type };
            mo_list->AddItem(new SettingsColumnItem(removable_columns));
        }
        mo_list->Select(0);
        MoRowChanged();

        /* Removable disk drives; IDE is for the native ATA cartridge drives
           (the SyQuest EZ135A and EZ230A). */
        rd_bus->Add("Disabled", RDISK_BUS_DISABLED);
        rd_bus->Add("ATAPI", RDISK_BUS_ATAPI);
        rd_bus->Add("SCSI", RDISK_BUS_SCSI);
        rd_bus->Add("LPT", RDISK_BUS_LPT);
        rd_bus->Add("IDE", RDISK_BUS_IDE);
        for (uint32 i = 0; i < KNOWN_RDISK_DRIVE_TYPES; i++)
            rd_type->Add(rdisk_type_name(i).c_str(), i);
        for (int i = 0; i < RDISK_NUM; i++) {
            rd[i] = { rdisk_drives[i].bus_type, rdisk_list_channel(&rdisk_drives[i]), rdisk_drives[i].type };
            rd_list->AddItem(new SettingsColumnItem(removable_columns));
        }
        rd_list->Select(0);
        RDiskRowChanged();

        /* Tape drives. */
        tp_bus->Add("Disabled", TAPE_BUS_DISABLED);
        tp_bus->Add("ATAPI", TAPE_BUS_ATAPI);
        tp_bus->Add("SCSI", TAPE_BUS_SCSI);
        tp_bus->Add("LPT", TAPE_BUS_LPT);
        tp_bus->Add("FDC", TAPE_BUS_FDC);
        for (uint32 i = 0; i < KNOWN_TAPE_DRIVE_TYPES; i++)
            tp_type->Add(tape_type_name(i).c_str(), i);
        for (int i = 0; i < TAPE_NUM; i++) {
            tp[i] = { tape_drives[i].bus_type, tape_drives[i].res, tape_drives[i].type };
            tp_list->AddItem(new SettingsColumnItem(removable_columns));
        }
        tp_list->Select(0);
        TapeRowChanged();
        UpdateRows();
    }

    ~OtherRemovablePage() override
    {
        settings_bus_tracking_release();
        for (Choice *c : { mo_bus, mo_channel, mo_type, rd_bus, rd_channel, rd_type, tp_bus, tp_channel, tp_type })
            delete c;
    }

    /* Like showEvent(): the machine, disk controllers and sound cards
       chosen on other pages decide who has each IDE channel. */
    void Show() override
    {
        UpdateRows();
        ReloadChannels(mo_bus, mo_channel, LptBridgesNone);
        ReloadRDiskChannels();
        ReloadChannels(tp_bus, tp_channel, LptBridgesNone);
        SettingsPage::Show();
    }

    bool Changed() override
    {
        bool changed = false;

        for (int i = 0; i < MO_NUM; i++) {
            changed |= (mo_drives[i].bus_type != mo[i].bus);
            changed |= (mo_drives[i].res != mo[i].channel);
            changed |= (mo_drives[i].type != mo[i].type);
        }
        for (int i = 0; i < RDISK_NUM; i++) {
            changed |= (rdisk_drives[i].bus_type != rd[i].bus);
            changed |= (rdisk_list_channel(&rdisk_drives[i]) != rd[i].channel);
            changed |= (rdisk_drives[i].type != rd[i].type);
        }
        for (int i = 0; i < TAPE_NUM; i++) {
            changed |= (tape_drives[i].bus_type != tp[i].bus);
            changed |= (tape_drives[i].res != tp[i].channel);
            changed |= (tape_drives[i].type != tp[i].type);
        }

        return changed;
    }

    void Save() override
    {
        for (int i = 0; i < MO_NUM; i++) {
            mo_drives[i].fp       = NULL;
            mo_drives[i].priv     = NULL;
            mo_drives[i].bus_type = mo[i].bus;
            mo_drives[i].res      = mo[i].channel;
            mo_drives[i].type     = mo[i].type;
        }

        for (int i = 0; i < RDISK_NUM; i++) {
            rdisk_drives[i].fp         = NULL;
            rdisk_drives[i].priv       = NULL;
            rdisk_drives[i].bus_type   = rd[i].bus;
            rdisk_drives[i].res        = rd[i].channel;
            rdisk_drives[i].type       = rd[i].type;
            rdisk_drives[i].lpt_bridge = HDD_LPT_EPAT;
            if (rdisk_drives[i].bus_type == RDISK_BUS_LPT) {
                if (!rdisk_own_chip(rdisk_drives[i].type))
                    rdisk_drives[i].lpt_bridge = rdisk_drives[i].res >> 2;
                rdisk_drives[i].res &= 3;
            }
        }

        for (int i = 0; i < TAPE_NUM; i++) {
            tape_drives[i].fp       = NULL;
            tape_drives[i].priv     = NULL;
            tape_drives[i].bus_type = tp[i].bus;
            tape_drives[i].res      = tp[i].channel;
            tape_drives[i].type     = tp[i].type;
        }
    }

    bool Handle(BMessage *msg) override
    {
        switch (msg->what) {
            case MSG_MO_ROW:
                MoRowChanged();
                return true;
            case MSG_MO_BUS:
                BusIndexChanged(mo_bus, mo_channel, mo_type, MO_BUS_DISABLED, LptBridgesNone);
                MoBusActivated(Row(mo_list));
                return true;
            case MSG_MO_CHANNEL:
                {
                    const int r = Row(mo_list);
                    sbt->DeviceTrack(0, DEV_MO, mo[r].bus, mo[r].channel);
                    mo[r].channel = mo_channel->Value(0);
                    sbt->DeviceTrack(1, DEV_MO, mo[r].bus, mo[r].channel);
                    UpdateRows();
                    return true;
                }
            case MSG_MO_TYPE:
                mo[Row(mo_list)].type = mo_type->Value(0);
                UpdateRows();
                return true;

            case MSG_RDISK_ROW:
                RDiskRowChanged();
                return true;
            case MSG_RDISK_BUS:
                RDiskBusIndexChanged();
                RDiskBusActivated(Row(rd_list));
                return true;
            case MSG_RDISK_CHANNEL:
                RDiskChannelActivated(Row(rd_list));
                return true;
            case MSG_RDISK_TYPE:
                {
                    const int r = Row(rd_list);
                    rd[r].type  = rd_type->Value(0);
                    UpdateRows();
                    /* On bus LPT the type decides which bridges can be
                       chosen, and how the list names the channel. */
                    if (rd_bus->Value(RDISK_BUS_DISABLED) == RDISK_BUS_LPT) {
                        ReloadRDiskChannels();
                        RDiskChannelActivated(r);
                    }
                    return true;
                }

            case MSG_TAPE_ROW:
                TapeRowChanged();
                return true;
            case MSG_TAPE_BUS:
                BusIndexChanged(tp_bus, tp_channel, tp_type, TAPE_BUS_DISABLED, LptBridgesNone);
                UpdateTypeItems(tp_type, tp_bus->Value(TAPE_BUS_DISABLED), tape_type_bus_compatible);
                TapeBusActivated(Row(tp_list));
                return true;
            case MSG_TAPE_CHANNEL:
                {
                    const int r = Row(tp_list);
                    sbt->DeviceTrack(0, DEV_TAPE, tp[r].bus, tp[r].channel);
                    tp[r].channel = tp_channel->Value(0);
                    sbt->DeviceTrack(1, DEV_TAPE, tp[r].bus, tp[r].channel);
                    UpdateRows();
                    return true;
                }
            case MSG_TAPE_TYPE:
                tp[Row(tp_list)].type = tp_type->Value(0);
                UpdateRows();
                return true;

            default:
                return false;
        }
    }

private:
    struct RowData {
        uint8  bus;
        uint8  channel;
        uint32 type;
    };

    /* qt_settingsotherremovable.ui: the list (Bus | Type), then a grid:
         Bus: [bus]  Channel: [channel]
         Type: [type..................]   */
    static void AddIconTab(BTabView *tabs, BView *view, const char *icon)
    {
        tabs->AddTab(view, settings_icon_tab(view, haiku_icon(icon)));
    }

    static BView *MakeTab(const char *name, const char *scroll_name, BListView *list, Choice *bus, Choice *channel, Choice *type)
    {
        BView *tab = new BView(name, 0);
        BLayoutBuilder::Group<>(tab, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .Add(settings_column_list(scroll_name, list, { "Bus", "Type" }, removable_columns), 1)
            .AddGrid()
                .AddMenuField(bus->field, 0, 0)
                .AddMenuField(channel->field, 2, 0)
                .AddMenuField(type->field, 0, 1, B_ALIGN_HORIZONTAL_UNSET, 1, 3)
                .SetColumnWeight(1, 1.0f)
                .SetColumnWeight(3, 1.0f)
            .End();
        return tab;
    }

    static int Row(const BListView *list)
    {
        const int32 r = list->CurrentSelection();
        return (r < 0) ? 0 : r;
    }

    /* A row as Qt fills it: the bus and channel with the drive's icon
       (Qt's setMOBus / setRDiskBus / setTapeBus), then the type. */
    static void SetRow(BListView *list, int i, BBitmap *icon, const std::string &bus, const std::string &type)
    {
        if (SettingsColumnItem *item = dynamic_cast<SettingsColumnItem *>(list->ItemAt(i))) {
            item->SetIcon(icon);
            item->SetText(0, bus);
            item->SetText(1, type);
            list->InvalidateItem(i);
        }
    }

    void UpdateRows()
    {
        for (int i = 0; i < MO_NUM; i++)
            SetRow(mo_list, i, haiku_icon((mo[i].bus == MO_BUS_DISABLED) ? "mo_disabled" : "mo"),
                   drives_bus_channel_name(mo[i].bus, mo[i].channel),
                   (mo[i].bus == MO_BUS_DISABLED) ? std::string("None") : mo_type_name(mo[i].type));
        for (int i = 0; i < RDISK_NUM; i++)
            SetRow(rd_list, i, (rd[i].bus == RDISK_BUS_DISABLED) ? haiku_icon("rdisk_disabled") : rdisk_icon(rd[i].type),
                   rdisk_channel_name(rd[i].bus, rd[i].type, rd[i].channel),
                   (rd[i].bus == RDISK_BUS_DISABLED) ? std::string("None") : rdisk_type_name(rd[i].type));
        for (int i = 0; i < TAPE_NUM; i++)
            SetRow(tp_list, i, haiku_icon((tp[i].bus == TAPE_BUS_DISABLED) ? "tape_disabled" : "tape"),
                   drives_bus_channel_name(tp[i].bus, tp[i].channel),
                   (tp[i].bus == TAPE_BUS_DISABLED) ? std::string("None") : tape_type_name(tp[i].type));
    }

    /* updateRDiskTypeCombo() / updateTapeTypeCombo(): the types that do not
       go on the bus can not be picked. */
    static void UpdateTypeItems(Choice *type, int bus, bool (*compatible)(int, uint32))
    {
        const int32 count = type->field->Menu()->CountItems();
        for (int32 row = 0; row < count; row++)
            choice_set_item_enabled(type, row, compatible(bus, (uint32) choice_value_at(type, row)));
    }

    /* on_comboBox*Bus_currentIndexChanged(). */
    void BusIndexChanged(Choice *bus, Choice *channel, Choice *type, int disabled, int lpt_bridges)
    {
        const int  b       = bus->Value(disabled);
        const bool enabled = (b != disabled);
        drives_populate_bus_channels(channel, b, sbt, lpt_bridges);
        channel->SetEnabled(enabled);
        type->SetEnabled(enabled);
    }

    void RDiskBusIndexChanged()
    {
        const int bus = rd_bus->Value(RDISK_BUS_DISABLED);
        BusIndexChanged(rd_bus, rd_channel, rd_type, RDISK_BUS_DISABLED, LptBridgesHdd);
        restrict_rdisk_bridges(rd_channel, bus, rd_type->Value(0));
        UpdateTypeItems(rd_type, bus, rdisk_type_bus_compatible);
    }

    /* The drive's own channel can be picked again, and on bus LPT the other
       bridges of its port. */
    static void EnableSelectedChannel(Choice *channel, bool lpt)
    {
        const int32 index = choice_index(channel);
        if (index < 0)
            return;
        choice_set_item_enabled(channel, index, true);

        if (lpt) {
            const int first = index - (index % HDD_LPT_BRIDGES);
            for (int i = first; i < (first + HDD_LPT_BRIDGES); i++) {
                if (drives_lpt_port_enabled(first / HDD_LPT_BRIDGES))
                    choice_set_item_enabled(channel, i, true);
            }
        }
    }

    void ReloadChannels(Choice *bus, Choice *channel, int lpt_bridges)
    {
        const int32 selected = choice_index(channel);
        drives_populate_bus_channels(channel, bus->Value(0), sbt, lpt_bridges);
        choice_pick_index(channel, selected);
        EnableSelectedChannel(channel, false);
    }

    void ReloadRDiskChannels()
    {
        const int32 selected = choice_index(rd_channel);
        const int   bus      = rd_bus->Value(RDISK_BUS_DISABLED);
        drives_populate_bus_channels(rd_channel, bus, sbt, LptBridgesHdd);
        choice_pick_index(rd_channel, selected);
        EnableSelectedChannel(rd_channel, bus == RDISK_BUS_LPT);
        restrict_rdisk_bridges(rd_channel, bus, rd_type->Value(0));
    }

    /* ---- MO ---- */

    void MoRowChanged()
    {
        const int r = Row(mo_list);
        choice_pick(mo_bus, mo[r].bus);
        BusIndexChanged(mo_bus, mo_channel, mo_type, MO_BUS_DISABLED, LptBridgesNone);
        choice_pick(mo_channel, mo[r].channel);
        choice_pick(mo_type, mo[r].type);
        EnableSelectedChannel(mo_channel, false);
    }

    void MoBusActivated(int r)
    {
        sbt->DeviceTrack(0, DEV_MO, mo[r].bus, mo[r].channel);
        const uint8 bus = mo_bus->Value(MO_BUS_DISABLED);
        const uint8 next_free = (bus == MO_BUS_ATAPI) ? sbt->NextFreeIdeChannel() : sbt->NextFreeScsiId();
        choice_pick_index(mo_channel, (next_free == CHANNEL_NONE) ? -1 : next_free);
        mo[r].bus     = bus;
        mo[r].channel = mo_channel->Value(0);
        mo[r].type    = mo_type->Value(0);
        sbt->DeviceTrack(1, DEV_MO, mo[r].bus, mo[r].channel);
        UpdateRows();
    }

    /* ---- removable disks ---- */

    void RDiskRowChanged()
    {
        const int r = Row(rd_list);
        choice_pick(rd_bus, rd[r].bus);
        RDiskBusIndexChanged();
        choice_pick(rd_channel, rd[r].channel);
        choice_pick(rd_type, rd[r].type);
        UpdateTypeItems(rd_type, rd[r].bus, rdisk_type_bus_compatible);
        EnableSelectedChannel(rd_channel, rd[r].bus == RDISK_BUS_LPT);
        restrict_rdisk_bridges(rd_channel, rd[r].bus, rd[r].type);
    }

    void RDiskBusActivated(int r)
    {
        sbt->DeviceTrack(0, DEV_RDISK, rd[r].bus, rdisk_track_channel(rd[r].bus, rd[r].channel));
        const int   bus       = rd_bus->Value(RDISK_BUS_DISABLED);
        const uint8 next_free = next_free_rdisk_channel(sbt, bus);
        choice_pick_index(rd_channel, (next_free == CHANNEL_NONE) ? -1 : next_free);

        /* A drive type that does not belong on the new bus is replaced by
           the first one that does; it stays as it is on Disabled. */
        uint32 type = rd_type->Value(0);
        if ((bus != RDISK_BUS_DISABLED) && !rdisk_type_bus_compatible(bus, type))
            type = first_compatible_rdisk_type(bus);
        choice_pick(rd_type, type);
        restrict_rdisk_bridges(rd_channel, bus, type);

        rd[r].bus     = bus;
        rd[r].channel = rd_channel->Value(0);
        rd[r].type    = rd_type->Value(0);
        sbt->DeviceTrack(1, DEV_RDISK, rd[r].bus, rdisk_track_channel(rd[r].bus, rd[r].channel));
        UpdateRows();
    }

    void RDiskChannelActivated(int r)
    {
        sbt->DeviceTrack(0, DEV_RDISK, rd[r].bus, rdisk_track_channel(rd[r].bus, rd[r].channel));
        rd[r].channel = rd_channel->Value(0);
        sbt->DeviceTrack(1, DEV_RDISK, rd[r].bus, rdisk_track_channel(rd[r].bus, rd[r].channel));
        UpdateRows();
    }

    /* ---- tapes ---- */

    void TapeRowChanged()
    {
        const int r = Row(tp_list);
        choice_pick(tp_bus, tp[r].bus);
        BusIndexChanged(tp_bus, tp_channel, tp_type, TAPE_BUS_DISABLED, LptBridgesNone);
        choice_pick(tp_channel, tp[r].channel);
        choice_pick(tp_type, tp[r].type);
        UpdateTypeItems(tp_type, tp[r].bus, tape_type_bus_compatible);
        EnableSelectedChannel(tp_channel, false);
    }

    void TapeBusActivated(int r)
    {
        sbt->DeviceTrack(0, DEV_TAPE, tp[r].bus, tp[r].channel);
        const int   bus       = tp_bus->Value(TAPE_BUS_DISABLED);
        const uint8 next_free = next_free_tape_channel(sbt, bus);
        choice_pick_index(tp_channel, (next_free == CHANNEL_NONE) ? -1 : next_free);

        uint32 type = tp_type->Value(0);
        if (!tape_type_bus_compatible(bus, type))
            type = first_compatible_tape_type(bus);
        choice_pick(tp_type, type);

        tp[r].bus     = bus;
        tp[r].channel = tp_channel->Value(0);
        tp[r].type    = tp_type->Value(0);
        sbt->DeviceTrack(1, DEV_TAPE, tp[r].bus, tp[r].channel);
        UpdateRows();
    }

    BusTracking *sbt;

    RowData mo[MO_NUM];
    RowData rd[RDISK_NUM];
    RowData tp[TAPE_NUM];

    BListView *mo_list;
    Choice    *mo_bus;
    Choice    *mo_channel;
    Choice    *mo_type;

    BListView *rd_list;
    Choice    *rd_bus;
    Choice    *rd_channel;
    Choice    *rd_type;

    BListView *tp_list;
    Choice    *tp_bus;
    Choice    *tp_channel;
    Choice    *tp_type;
};

SettingsPage *
create_other_removable_page()
{
    return new OtherRemovablePage();
}

/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Floppy & CD-ROM drives Settings page
 *          (qt_settingsfloppycdrom.cpp).
 *
 *          As in Qt, each list keeps the drives' values (the list's model
 *          there) until Save() writes them to the core; the controls below
 *          a list edit its selected drive.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <ListView.h>
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
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/timer.h>
#include <86box/fdd.h>
#include <86box/cdrom.h>
#include <86box/hdd.h>
#include <86box/scsi_device.h>
#include <86box/scsi_tape.h>
#include <86box/fdd_audio.h>
}

enum : uint32 {
    MSG_FD_ROW       = 'sffr',
    MSG_FD_TYPE      = 'sfft',
    MSG_FD_TURBO     = 'sftb',
    MSG_FD_BPB       = 'sfbp',
    MSG_FD_AUDIO     = 'sfau',
    MSG_CD_ROW       = 'sfcr',
    MSG_CD_BUS       = 'sfcb',
    MSG_CD_CHANNEL   = 'sfch',
    MSG_CD_TYPE      = 'sfct',
    MSG_CD_SPEED     = 'sfcs',
    MSG_CD_DVD_SPEED = 'sfds',
    MSG_CD_HD_SPEED  = 'sfhs',
    MSG_CD_UNLIMITED = 'sful',
    MSG_CD_ECC       = 'sfec',
};

/* On bus LPT the list's channel is the port and, above it, the bridge. */
static std::string
cdrom_channel_name(uint8 bus, uint8 channel)
{
    if (bus == CDROM_BUS_LPT) {
        char temp[256];
        snprintf(temp, sizeof(temp), "LPT%i (%s)", (channel & 3) + 1, drives_lpt_bridge_name(channel >> 2).c_str());
        return temp;
    }

    return drives_bus_channel_name(bus, channel);
}

/* What the bus tracking counts: on bus LPT only the port. */
static int
cdrom_track_channel(int bus, int channel)
{
    return (bus == CDROM_BUS_LPT) ? (channel & 3) : channel;
}

static uint8
cdrom_list_channel(const cdrom_t *drv)
{
    return (drv->bus_type == CDROM_BUS_LPT) ? (uint8) (drv->res | (drv->lpt_bridge << 2)) : drv->res;
}

static std::string
cdrom_name(int type)
{
    char temp[512];
    cdrom_get_name(type, temp);
    std::string name = temp;

    const int   cd   = cdrom_get_speed(type);
    const int   dvd  = cdrom_get_dvd_speed(type);
    const int   hd   = cdrom_get_hd_speed(type);
    std::string parts;
    auto        part = [&parts](const std::string &p) {
        if (!parts.empty())
            parts += " / ";
        parts += p;
    };
    if (cd > 0)
        part(std::to_string(cd) + "x CD-ROM");
    if (cdrom_is_cdr(type))
        part("CD-R");
    if (cdrom_is_cdrw(type))
        part("CD-RW");
    if (dvd > 0)
        part(std::to_string(dvd) + "x DVD-ROM");
    if (hd > 0)
        part(std::to_string(hd) + ((cdrom_drive_types[type].hd_type == 1) ? "x HD DVD-ROM" : "x Blu-ray"));
    if (!parts.empty())
        name += " (" + parts + ")";
    return name;
}

/* Whether a CD-ROM drive model goes on a bus (the filter of Qt's type box). */
static bool
cdrom_type_on_bus(int j, int bus)
{
    const int drive_bus = cdrom_drive_types[j].bus_type;

    return ((bus == CDROM_BUS_CM100) || (bus == CDROM_BUS_SONY) || (bus == CDROM_BUS_PHILIPS) || (bus == CDROM_BUS_HITACHI) ||
            (bus == CDROM_BUS_MKE) || (bus == CDROM_BUS_ATAPI) || (bus == CDROM_BUS_SCSI) || (bus == CDROM_BUS_LPT)) &&
        ((drive_bus == bus) || ((drive_bus == BUS_TYPE_IDE) && (bus == CDROM_BUS_LPT)) ||
         ((drive_bus == BUS_TYPE_BOTH) && (bus != BUS_TYPE_MKE) && (bus != BUS_TYPE_HITACHI) && (bus != CDROM_BUS_PHILIPS) &&
          (bus != CDROM_BUS_CM100) && (bus != CDROM_BUS_SONY)));
}

/* The row icons of Qt's setFloppyType() and setCDROMBus(). */
static BBitmap *
floppy_icon(int type)
{
    if (type == 0)
        return haiku_icon("floppy_disabled");
    return haiku_icon(((type >= 1) && (type <= 6)) ? "floppy_525" : "floppy_35");
}

static BBitmap *
cdrom_icon(int bus, int type)
{
    if (bus == CDROM_BUS_DISABLED)
        return haiku_icon("cdrom_disabled");
    if (cdrom_is_bd(type))
        return haiku_icon("bdrom");
    if (cdrom_is_hd_dvd(type))
        return haiku_icon("hd_dvdrom");
    if (cdrom_is_dvd(type))
        return haiku_icon("dvdrom");
    if (cdrom_is_cdrw(type))
        return haiku_icon("cdrw");
    if (cdrom_is_cdr(type))
        return haiku_icon("cdr");
    return haiku_icon("cdrom");
}

/* Qt's tree views size their columns to the contents; these shares come
   close for the usual names. */
static const std::vector<float> floppy_columns = { 0.60f, 0.20f, 0.20f };
static const std::vector<float> cdrom_columns  = { 0.30f, 0.14f, 0.56f };

class FloppyCdromPage : public SettingsPage {
public:
    FloppyCdromPage()
        : SettingsPage("Floppy & CD-ROM drives")
        , sbt(settings_bus_tracking_acquire())
        , fd_type(new Choice("fd_type", "Type:", MSG_FD_TYPE))
        , fd_audio(new Choice("fd_audio", "Audio:", MSG_FD_AUDIO))
        , cd_bus(new Choice("cd_bus", "Bus:", MSG_CD_BUS))
        , cd_channel(new Choice("cd_channel", "Channel:", MSG_CD_CHANNEL))
        , cd_type(new Choice("cd_type", "Type:", MSG_CD_TYPE))
        , cd_speed(new Choice("cd_speed", "CD speed:", MSG_CD_SPEED))
        , cd_dvd_speed(new Choice("cd_dvd_speed", "DVD speed:", MSG_CD_DVD_SPEED))
        , cd_hd_speed(new Choice("cd_hd_speed", "HD/BD speed:", MSG_CD_HD_SPEED))
    {
        fd_list = new BListView("floppy_list");
        fd_list->SetSelectionMessage(new BMessage(MSG_FD_ROW));
        fd_turbo = new BCheckBox("fd_turbo", "Turbo timings", new BMessage(MSG_FD_TURBO));
        fd_bpb   = new BCheckBox("fd_bpb", "Check BPB", new BMessage(MSG_FD_BPB));

        cd_list = new BListView("cdrom_list");
        cd_list->SetSelectionMessage(new BMessage(MSG_CD_ROW));
        cd_unlimited = new BCheckBox("cd_unlimited", "Unlimited capacity", new BMessage(MSG_CD_UNLIMITED));
        cd_ecc       = new BCheckBox("cd_ecc", "Use EDC/ECC emulation", new BMessage(MSG_CD_ECC));

        /* qt_settingsfloppycdrom.ui: the list (Type | Turbo | Check BPB),
           then a grid: Type: [type] [Turbo timings] [Check BPB]
                        Audio: [audio............................] */
        BView *fd_tab = new BView("Floppy drives", 0);
        BLayoutBuilder::Group<>(fd_tab, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .Add(settings_column_list("floppy_list", fd_list, { "Type", "Turbo", "Check BPB" }, floppy_columns), 1)
            .AddGrid()
                .AddMenuField(fd_type->field, 0, 0)
                .Add(fd_turbo, 2, 0)
                .Add(fd_bpb, 3, 0)
                .AddMenuField(fd_audio->field, 0, 1, B_ALIGN_HORIZONTAL_UNSET, 1, 3)
                .SetColumnWeight(1, 1.0f)
            .End();

        /* The list (Bus | Speed | Type), then a grid:
             Bus: [bus]  Channel: [channel]  [Unlimited capacity]
             Type: [type.................]  [Use EDC/ECC emulation]
             CD speed: [ ]  DVD speed: [ ]  HD/BD speed: [ ]           */
        BView *cd_tab = new BView("CD-ROM drives", 0);
        BLayoutBuilder::Group<>(cd_tab, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .Add(settings_column_list("cdrom_list", cd_list, { "Bus", "Speed", "Type" }, cdrom_columns), 1)
            .AddGrid()
                .AddMenuField(cd_bus->field, 0, 0)
                .AddMenuField(cd_channel->field, 2, 0)
                .Add(cd_unlimited, 4, 0, 2)
                .AddMenuField(cd_type->field, 0, 1, B_ALIGN_HORIZONTAL_UNSET, 1, 3)
                .Add(cd_ecc, 4, 1, 2)
                .SetColumnWeight(1, 1.0f)
                .SetColumnWeight(3, 1.0f)
            .End()
            .AddGroup(B_HORIZONTAL)
                .Add(cd_speed->field)
                .Add(cd_dvd_speed->field)
                .Add(cd_hd_speed->field)
                .AddGlue()
            .End();

        BTabView *tabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);
        tabs->AddTab(fd_tab, settings_icon_tab(fd_tab, haiku_icon("floppy_tab")));
        tabs->AddTab(cd_tab, settings_icon_tab(cd_tab, haiku_icon("cdrom")));
        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .Add(tabs);

        /* Floppy drives. */
        for (int i = 0;; i++) {
            const char *name = fdd_getname(i);
            if ((name == nullptr) || (name[0] == '\0'))
                break;
            fd_type->Add(name, i);
        }

        for (int i = 0; i < FDD_NUM; i++) {
            fd[i].type  = fdd_get_type(&drives[i]);
            fd[i].turbo = fdd_get_turbo(&drives[i]) > 0;
            fd[i].bpb   = fdd_get_check_bpb(&drives[i]) > 0;
#ifndef DISABLE_FDD_AUDIO
            fd[i].audio = fdd_get_audio_profile(&drives[i]);
#else
            fd[i].audio = 0;
#endif
        }

        /* CD-ROM drives. */
        cd_bus->Add("Disabled", CDROM_BUS_DISABLED);
        cd_bus->Add("ATAPI", CDROM_BUS_ATAPI);
        cd_bus->Add("LPT", CDROM_BUS_LPT);
        cd_bus->Add("SCSI", CDROM_BUS_SCSI);
        cd_bus->Add("Hitachi", CDROM_BUS_HITACHI);
        cd_bus->Add("Mitsumi", CDROM_BUS_MITSUMI);
        cd_bus->Add("Panasonic/MKE", CDROM_BUS_MKE);
        cd_bus->Add("Philips CM-100/CM-153", CDROM_BUS_CM100);
        cd_bus->Add("Philips/LMS", CDROM_BUS_PHILIPS);
        cd_bus->Add("Sony", CDROM_BUS_SONY);

        auto populate = [](Choice *box, int max, int turbo) {
            char temp[16];
            for (int i = 1; i <= max; i++) {
                snprintf(temp, sizeof(temp), "%ix", i);
                box->Add(temp, i);
            }
            box->Add("Turbo", turbo);
        };
        populate(cd_speed, 72, 73);
        populate(cd_dvd_speed, 16, 17);
        populate(cd_hd_speed, 2, 3);

        for (int i = 0; i < CDROM_NUM; i++) {
            const int type = cdrom_get_type(i);
            const int fixed = cdrom_get_speed(type);
            const int speed = (fixed == -1) ? cdrom[i].speed : fixed;

            cd[i].bus        = cdrom[i].bus_type;
            cd[i].channel    = cdrom_list_channel(&cdrom[i]);
            cd[i].type       = type;
            cd[i].speed      = speed ? speed : 8;
            cd[i].dvd_speed  = cdrom[i].dvd_speed;
            cd[i].hd_speed   = cdrom[i].hd_speed;
            cd[i].restricted = cdrom[i].restricted_capacity;
            cd[i].no_check   = cdrom[i].no_check;
            cd_list->AddItem(new SettingsColumnItem(cdrom_columns));
            UpdateCdromRow(i);
        }

        MachineChanged(CurrentMachine());

        cd_list->Select(0);
        CdromRowChanged();
    }

    ~FloppyCdromPage() override
    {
        settings_bus_tracking_release();
        for (Choice *c : { fd_type, fd_audio, cd_bus, cd_channel, cd_type, cd_speed, cd_dvd_speed, cd_hd_speed })
            delete c;
    }

    void MachineChanged(int m) override
    {
        const bool fixed = (machines[m].init == machine_ibm5140_init);
        fd_type->SetEnabled(!fixed);
        fd_turbo->SetEnabled(!fixed);
        if (fixed) {
            for (int i = 0; i < FDD_NUM; i++) {
                fd[i].type  = (i < 2) ? fdd_get_from_internal_name((char *) "35_2dd") : 0;
                fd[i].turbo = false;
            }
        }

        /* The IBM 5140 has its two drives only: the list hides the others. */
        const int32 rows     = fixed ? 2 : FDD_NUM;
        int32       selected = fixed ? 0 : fd_list->CurrentSelection();
        while (BListItem *item = fd_list->RemoveItem((int32) 0))
            delete item;
        for (int i = 0; i < rows; i++) {
            fd_list->AddItem(new SettingsColumnItem(floppy_columns));
            UpdateFloppyRow(i);
        }
        if ((selected < 0) || (selected >= rows))
            selected = 0;
        fd_list->Select(selected);
        FloppyRowChanged();
    }

    /* Like showEvent(): the machine, disk controllers and sound cards
       chosen on other pages decide who has each IDE channel. */
    void Show() override
    {
        for (int i = 0; i < CDROM_NUM; i++)
            UpdateCdromRow(i);
        ReloadBusChannels();
        SettingsPage::Show();
    }

    bool Changed() override
    {
        bool changed = false;

        for (int i = 0; i < FDD_NUM; i++) {
            changed |= (fdd_get_type(&drives[i]) != fd[i].type);
            changed |= ((fdd_get_turbo(&drives[i]) > 0) != fd[i].turbo);
            changed |= ((fdd_get_check_bpb(&drives[i]) > 0) != fd[i].bpb);
#ifndef DISABLE_FDD_AUDIO
            changed |= (fdd_get_audio_profile(&drives[i]) != fd[i].audio);
#endif
        }

        for (int i = 0; i < CDROM_NUM; i++) {
            changed |= (cdrom[i].bus_type != cd[i].bus);
            changed |= (cdrom_list_channel(&cdrom[i]) != cd[i].channel);
            changed |= cdrom[i].speed && (cdrom[i].speed != cd[i].speed);
            changed |= (cdrom_get_type(i) != cd[i].type);
            changed |= (cdrom[i].restricted_capacity != cd[i].restricted);
            changed |= (cdrom[i].no_check != cd[i].no_check);
        }

        return changed;
    }

    void Save() override
    {
        for (int i = 0; i < FDD_NUM; i++) {
            fdd_set_type(&drives[i], fd[i].type);
            fdd_set_turbo(&drives[i], fd[i].turbo ? 1 : 0);
            fdd_set_check_bpb(&drives[i], fd[i].bpb ? 1 : 0);
#ifndef DISABLE_FDD_AUDIO
            fdd_set_audio_profile(&drives[i], fd[i].audio);
#endif
        }

        for (int i = 0; i < CDROM_NUM; i++) {
            cdrom[i].priv        = NULL;
            cdrom[i].ops         = NULL;
            cdrom[i].local       = NULL;
            cdrom[i].insert      = NULL;
            cdrom[i].close       = NULL;
            cdrom[i].get_volume  = NULL;
            cdrom[i].get_channel = NULL;
            cdrom[i].bus_type    = cd[i].bus;
            cdrom[i].res         = cd[i].channel;
            if (cdrom[i].bus_type == CDROM_BUS_LPT) {
                cdrom[i].lpt_bridge = cdrom[i].res >> 2;
                cdrom[i].res &= 3;
            } else
                cdrom[i].lpt_bridge = HDD_LPT_BPCK;
            cdrom[i].speed               = cd[i].speed;
            cdrom[i].dvd_speed           = cd[i].dvd_speed;
            cdrom[i].hd_speed            = cd[i].hd_speed;
            cdrom[i].no_check            = cd[i].no_check;
            cdrom[i].restricted_capacity = cd[i].restricted;
            cdrom_set_type(i, cd[i].type);
        }

#ifdef DISABLE_FDD_AUDIO
        fdd_sounds_enabled = 0;
#else
        fdd_sounds_enabled = 1;
#endif
    }

    bool Handle(BMessage *msg) override
    {
        const int fr = FloppyRow();
        const int cr = CdromRow();

        switch (msg->what) {
            case MSG_FD_ROW:
                FloppyRowChanged();
                return true;
            case MSG_FD_TYPE:
                sbt->DeviceTrack(0, DEV_FDD, TAPE_BUS_FDC, fr);
                fd[fr].type = fd_type->Value(0);
                sbt->DeviceTrack(fd[fr].type ? 1 : 0, DEV_FDD, TAPE_BUS_FDC, fr);
                UpdateFloppyRow(fr);
                /* Rebuilds the audio profiles for the drive type. */
                FloppyRowChanged();
                return true;
            case MSG_FD_TURBO:
                fd[fr].turbo = (fd_turbo->Value() == B_CONTROL_ON);
                UpdateFloppyRow(fr);
                return true;
            case MSG_FD_BPB:
                fd[fr].bpb = (fd_bpb->Value() == B_CONTROL_ON);
                UpdateFloppyRow(fr);
                return true;
            case MSG_FD_AUDIO:
#ifndef DISABLE_FDD_AUDIO
                fd[fr].audio = fd_audio->Value(0);
#endif
                return true;

            case MSG_CD_ROW:
                CdromRowChanged();
                return true;
            case MSG_CD_BUS:
                BusIndexChanged();
                BusActivated(cr);
                return true;
            case MSG_CD_CHANNEL:
                sbt->DeviceTrack(0, DEV_CDROM, cd[cr].bus, cdrom_track_channel(cd[cr].bus, cd[cr].channel));
                cd[cr].channel = cd_channel->Value(0);
                sbt->DeviceTrack(1, DEV_CDROM, cd[cr].bus, cdrom_track_channel(cd[cr].bus, cd[cr].channel));
                UpdateCdromRow(cr);
                return true;
            case MSG_CD_TYPE:
                TypeActivated(cr);
                return true;
            case MSG_CD_SPEED:
                cd[cr].speed = cd_speed->Value(8);
                UpdateCdromRow(cr);
                return true;
            case MSG_CD_DVD_SPEED:
                cd[cr].dvd_speed = cd_dvd_speed->Value(0);
                return true;
            case MSG_CD_HD_SPEED:
                cd[cr].hd_speed = cd_hd_speed->Value(0);
                return true;
            case MSG_CD_UNLIMITED:
                /* 1 = restricted, 0 = unlimited. */
                cd[cr].restricted = (cd_unlimited->Value() == B_CONTROL_ON) ? 0 : 1;
                return true;
            case MSG_CD_ECC:
                cd[cr].no_check = (cd_ecc->Value() != B_CONTROL_ON);
                return true;
            default:
                return false;
        }
    }

private:
    struct FloppyRowData {
        int  type;
        bool turbo;
        bool bpb;
        int  audio; /* the audio profile (ifa in Qt) */
    };
    struct CdromRowData {
        uint8 bus;
        uint8 channel;
        uint8 speed;
        uint8 dvd_speed;
        uint8 hd_speed;
        uint8 restricted;
        int   type;
        int   no_check; /* inc in Qt */
    };

    int FloppyRow() const
    {
        const int32 r = fd_list->CurrentSelection();
        return (r < 0) ? 0 : r;
    }

    int CdromRow() const
    {
        const int32 r = cd_list->CurrentSelection();
        return (r < 0) ? 0 : r;
    }

    /* A floppy row as Qt fills it: the type with its icon, Turbo, Check BPB. */
    void UpdateFloppyRow(int i)
    {
        if (SettingsColumnItem *item = dynamic_cast<SettingsColumnItem *>(fd_list->ItemAt(i))) {
            item->SetIcon(floppy_icon(fd[i].type));
            item->SetText(0, fdd_getname(fd[i].type));
            item->SetText(1, fd[i].turbo ? "On" : "Off");
            item->SetText(2, fd[i].bpb ? "On" : "Off");
            fd_list->InvalidateItem(i);
        }
    }

    /* A CD-ROM row: the bus and channel with the drive's icon, the speed,
       the type (none shown for Mitsumi, as in Qt). */
    void UpdateCdromRow(int i)
    {
        if (SettingsColumnItem *item = dynamic_cast<SettingsColumnItem *>(cd_list->ItemAt(i))) {
            item->SetIcon(cdrom_icon(cd[i].bus, cd[i].type));
            item->SetText(0, cdrom_channel_name(cd[i].bus, cd[i].channel));
            item->SetText(1, (cd[i].speed > 72) ? std::string("Turbo") : (std::to_string(cd[i].speed) + "x"));
            if (cd[i].bus == CDROM_BUS_DISABLED)
                item->SetText(2, "None");
            else if (cd[i].bus != CDROM_BUS_MITSUMI)
                item->SetText(2, cdrom_name(cd[i].type));
            else
                item->SetText(2, "");
            cd_list->InvalidateItem(i);
        }
    }

    /* onFloppyRowChanged(). */
    void FloppyRowChanged()
    {
        const int r    = FloppyRow();
        const int type = fd[r].type;

        choice_pick(fd_type, type);
        fd_turbo->SetValue(fd[r].turbo ? B_CONTROL_ON : B_CONTROL_OFF);
        fd_bpb->SetValue(fd[r].bpb ? B_CONTROL_ON : B_CONTROL_OFF);

        fd_audio->Clear();
#ifndef DISABLE_FDD_AUDIO
        const int profile_count = fdd_audio_get_profile_count();
        if (!profile_count || (type == 0)) {
            fd_audio->Add("None", 0);
            fd_audio->Select(0);
            fd_audio->SetEnabled(false);
            fd[r].audio = 0;
            return;
        }

        /* Only the profiles for the drive's track count (40 or 80). */
        const bool is_40_track = (fdd_get_type_max_track(type) <= 43);
        bool       found       = false;
        for (int i = 0; i < profile_count; i++) {
            const char                       *name    = fdd_audio_get_profile_name(i);
            const fdd_audio_profile_config_t *profile = fdd_audio_get_profile(i);
            if ((name == nullptr) || (profile == nullptr))
                continue;
            if ((profile->total_tracks == 0) || (is_40_track && (profile->total_tracks == 40)) ||
                (!is_40_track && (profile->total_tracks == 80))) {
                fd_audio->Add(name, i);
                found |= (i == fd[r].audio);
            }
        }

        /* A profile that does not suit the drive becomes "None" (0). */
        if (!found)
            fd[r].audio = 0;
        fd_audio->Select(fd[r].audio);
        fd_audio->SetEnabled(true);
#else
        fd_audio->Add("None", 0);
        fd_audio->Select(0);
        fd_audio->SetEnabled(false);
#endif
    }

    /* The CD/DVD/HD speed boxes: a model with a fixed speed shows it, and
       only the generic models let it be chosen. */
    static void SetSpeed(Choice *box, int fixed_speed, int user, int turbo)
    {
        const bool selectable = (fixed_speed == -1);
        int        val        = selectable ? user : fixed_speed;
        if (selectable && (val >= turbo))
            val = turbo;
        if (box->Has(val))
            box->Select(val);
        box->SetEnabled(selectable);
    }

    void SetSpeeds(int r, int type)
    {
        SetSpeed(cd_speed, cdrom_get_speed(type), cd[r].speed, 73);
        SetSpeed(cd_dvd_speed, cdrom_get_dvd_speed(type), cd[r].dvd_speed, 17);
        SetSpeed(cd_hd_speed, cdrom_get_hd_speed(type), cd[r].hd_speed, 3);
    }

    /* Fills the model box for a bus and selects the type given, or the
       first. */
    void PopulateTypes(uint8 bus, int selected_type)
    {
        int eligible = 0;
        cd_type->Clear();
        for (int j = 0; cdrom_drive_types[j].bus_type != BUS_TYPE_NONE; j++) {
            if (cdrom_type_on_bus(j, bus)) {
                cd_type->Add(cdrom_name(j).c_str(), j);
                eligible++;
            }
        }
        cd_type->Select(selected_type);
        cd_type->SetEnabled(eligible > 1);
    }

    /* on_comboBoxBus_currentIndexChanged(). */
    void BusIndexChanged()
    {
        const int  bus     = cd_bus->Value(CDROM_BUS_DISABLED);
        const bool enabled = (bus != CDROM_BUS_DISABLED);
        const bool single  = (bus == CDROM_BUS_MITSUMI) || (bus == CDROM_BUS_PHILIPS) || (bus == CDROM_BUS_CM100) || (bus == CDROM_BUS_SONY);

        drives_populate_bus_channels(cd_channel, bus, sbt, LptBridgesCdrom);
        cd_channel->SetEnabled(single ? false : enabled);
        cd_type->SetEnabled((bus == CDROM_BUS_MITSUMI) ? false : enabled);
        cd_ecc->SetEnabled(single ? false : enabled);
    }

    /* onCDROMRowChanged(). */
    void CdromRowChanged()
    {
        const int r = CdromRow();

        cd_ecc->SetValue(cd[r].no_check ? B_CONTROL_OFF : B_CONTROL_ON);

        choice_pick(cd_bus, cd[r].bus);
        BusIndexChanged();
        choice_pick(cd_channel, cd[r].channel);

        cd_unlimited->SetValue(cd[r].restricted ? B_CONTROL_OFF : B_CONTROL_ON);
        SetSpeeds(r, cd[r].type);
        PopulateTypes(cd[r].bus, cd[r].type);
        cd_ecc->SetEnabled(cd[r].bus != CDROM_BUS_DISABLED);

        EnableSelectedChannel();
    }

    /* on_comboBoxBus_activated(): the drive goes to the first free channel
       of the new bus. */
    void BusActivated(int r)
    {
        const uint8 bus = cd_bus->Value(CDROM_BUS_DISABLED);

        sbt->DeviceTrack(0, DEV_CDROM, cd[r].bus, cdrom_track_channel(cd[r].bus, cd[r].channel));
        if (bus == CDROM_BUS_HITACHI)
            choice_pick_index(cd_channel, sbt->NextFreeHitachiChannel());
        else if (bus == CDROM_BUS_MKE)
            choice_pick_index(cd_channel, sbt->NextFreeMkeChannel());
        else if (bus == CDROM_BUS_ATAPI)
            choice_pick_index(cd_channel, sbt->NextFreeIdeChannel());
        else if (bus == CDROM_BUS_SCSI)
            choice_pick_index(cd_channel, sbt->NextFreeScsiId());
        else if (bus == CDROM_BUS_LPT) {
            /* The port's BackPack entry. */
            const int port = sbt->NextFreeLptPort();
            choice_pick_index(cd_channel, (port == CHANNEL_NONE) ? -1 : ((port * HDD_LPT_BRIDGES) + HDD_LPT_BPCK));
        } else
            choice_pick_index(cd_channel, 0);

        cd[r].bus     = bus;
        cd[r].channel = cd_channel->Value(0);
        sbt->DeviceTrack(1, DEV_CDROM, cd[r].bus, cdrom_track_channel(cd[r].bus, cd[r].channel));

        PopulateTypes(bus, cd[r].type);
        cd[r].type = cd_type->Value(0);

        int speed = cdrom_get_speed(cd[r].type);
        if ((speed == -1) && (bus != CDROM_BUS_MITSUMI))
            speed = cd[r].speed;
        else if (bus == CDROM_BUS_MITSUMI)
            speed = 2;
        cd_unlimited->SetValue(cd[r].restricted ? B_CONTROL_OFF : B_CONTROL_ON);

        SetSpeeds(r, cd[r].type);
        cd[r].speed     = speed ? speed : 8;
        cd[r].dvd_speed = cd_dvd_speed->Value(0);
        cd[r].hd_speed  = cd_hd_speed->Value(0);
        cd_ecc->SetEnabled(bus != CDROM_BUS_DISABLED);
        UpdateCdromRow(r);
    }

    /* on_comboBoxCDROMType_activated(). */
    void TypeActivated(int r)
    {
        cd[r].type = cd_type->Value(0);

        int speed = cdrom_get_speed(cd[r].type);
        if (speed == -1)
            speed = cd[r].speed;
        cd_unlimited->SetValue(cd[r].restricted ? B_CONTROL_OFF : B_CONTROL_ON);

        SetSpeeds(r, cd[r].type);
        cd[r].speed     = speed ? speed : 8;
        cd[r].dvd_speed = cd_dvd_speed->Value(0);
        cd[r].hd_speed  = cd_hd_speed->Value(0);
        UpdateCdromRow(r);
    }

    /* The drive's own channel can be picked again, and on bus LPT the other
       bridges of its port. */
    void EnableSelectedChannel()
    {
        const int32 index = choice_index(cd_channel);
        if (index < 0)
            return;
        choice_set_item_enabled(cd_channel, index, true);

        if (cd_bus->Value(CDROM_BUS_DISABLED) == CDROM_BUS_LPT) {
            const int first = index - (index % HDD_LPT_BRIDGES);
            for (int i = first; i < (first + HDD_LPT_BRIDGES); i++) {
                if (drives_lpt_port_enabled(first / HDD_LPT_BRIDGES) && cdrom_lpt_bridge_valid(i % HDD_LPT_BRIDGES))
                    choice_set_item_enabled(cd_channel, i, true);
            }
        }
    }

    void ReloadBusChannels()
    {
        const int32 selected = choice_index(cd_channel);
        const bool  enabled  = cd_channel->field->IsEnabled();
        drives_populate_bus_channels(cd_channel, cd_bus->Value(CDROM_BUS_DISABLED), sbt, LptBridgesCdrom);
        choice_pick_index(cd_channel, selected);
        cd_channel->SetEnabled(enabled);
        EnableSelectedChannel();
    }

    BusTracking *sbt;

    FloppyRowData fd[FDD_NUM];
    CdromRowData  cd[CDROM_NUM];

    BListView *fd_list;
    Choice    *fd_type;
    BCheckBox *fd_turbo;
    BCheckBox *fd_bpb;
    Choice    *fd_audio;

    BListView *cd_list;
    Choice    *cd_bus;
    Choice    *cd_channel;
    Choice    *cd_type;
    Choice    *cd_speed;
    Choice    *cd_dvd_speed;
    Choice    *cd_hd_speed;
    BCheckBox *cd_unlimited;
    BCheckBox *cd_ecc;
};

SettingsPage *
create_floppy_cdrom_page()
{
    return new FloppyCdromPage();
}

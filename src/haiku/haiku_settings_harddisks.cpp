/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Hard disks Settings page
 *          (qt_settingsharddisks.cpp).
 *
 *          Like Qt, the page keeps its own list of the disks, written to
 *          the core's hdd[] by Save(). The channels the other disks and
 *          the saved CD-ROM, removable, MO and tape drives take are worked
 *          out again whenever a channel list is filled.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Button.h>
#include <LayoutBuilder.h>
#include <ListItem.h>
#include <ListView.h>
#include <ScrollView.h>
#include <String.h>
#include <Window.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>

#include "haiku_harddiskdialog.hpp"
#include "haiku_harddrive_common.hpp"
#include "haiku_settings_common.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/hdd.h>
#include <86box/hdd_audio.h>
}

enum : uint32 {
    MSG_HD_SELECTED  = 'shsl',
    MSG_HD_BUS       = 'shbs',
    MSG_HD_CHANNEL   = 'shch',
    MSG_HD_SPEED     = 'shsp',
    MSG_HD_AUDIO     = 'shau',
    MSG_HD_CONFIGURE = 'shcf',
    MSG_HD_NEW       = 'shnw',
    MSG_HD_EXISTING  = 'shex',
    MSG_HD_REMOVE    = 'shrm',
};

static bool
generic_hdd(uint32 bus, uint32 speed)
{
    return ((bus == HDD_BUS_IDE) || (bus == HDD_BUS_ATAPI) || (bus == HDD_BUS_SCSI) || (bus == HDD_BUS_LPT)) &&
           hdd_preset_is_generic(speed);
}

/* The channel of the list: on bus LPT the port and, above it, the bridge. */
static uint8
list_channel(const hard_disk_t *hd)
{
    if (hd->bus_type == HDD_BUS_LPT)
        return hd->channel | (hd->lpt_bridge << 2);
    return hd->channel;
}

/* A row of the list, what Qt keeps in its model. */
struct Disk {
    uint8       bus;
    uint8       channel; /* list_channel() */
    uint32      tracks;
    uint32      hpc;
    uint32      spt;
    uint32      speed;
    uint32      audio;
    uint32      blocksize;
    std::string fn;
    char        vendor[9];
    char        model[41];
    char        version[5];
};

/* ---------------------------------------------------- the list's columns */

static const char *const column_titles[] = { "Bus", "File", "Geometry", "Model" };
static const float       column_share[]  = { 0.29f, 0.31f, 0.20f, 0.20f };
static const int         COLUMNS         = 4;

static void
column_edges(float left, float width, float edges[COLUMNS + 1])
{
    edges[0] = left;
    for (int i = 0; i < COLUMNS; i++)
        edges[i + 1] = edges[i] + (width * column_share[i]);
}

static void
draw_columns(BView *view, const BRect &frame, const std::string *texts, float left, float width)
{
    float edges[COLUMNS + 1];
    column_edges(left, width, edges);

    font_height fh;
    view->GetFontHeight(&fh);
    const float baseline = frame.top + ((frame.Height() + fh.ascent - fh.descent) / 2.0f);

    for (int i = 0; i < COLUMNS; i++) {
        BString text(texts[i].c_str());
        view->TruncateString(&text, B_TRUNCATE_END, edges[i + 1] - edges[i] - 8);
        view->DrawString(text.String(), BPoint(edges[i] + 4, baseline));
    }
}

class DiskItem : public BListItem {
public:
    std::string texts[COLUMNS];

    void DrawItem(BView *owner, BRect frame, bool) override
    {
        const rgb_color background = ui_color(IsSelected() ? B_LIST_SELECTED_BACKGROUND_COLOR : B_LIST_BACKGROUND_COLOR);
        owner->SetLowColor(background);
        owner->FillRect(frame, B_SOLID_LOW);
        owner->SetHighColor(ui_color(IsSelected() ? B_LIST_SELECTED_ITEM_TEXT_COLOR : B_LIST_ITEM_TEXT_COLOR));
        draw_columns(owner, frame, texts, frame.left, frame.Width());
    }

    void Update(BView *owner, const BFont *font) override
    {
        BListItem::Update(owner, font);
        font_height fh;
        font->GetHeight(&fh);
        SetHeight(ceilf(fh.ascent + fh.descent + fh.leading) + 6);
    }
};

/* The titles above the list, lined up with its columns. */
class ColumnHeader : public BView {
public:
    ColumnHeader()
        : BView("header", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE)
    {
        SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
        font_height fh;
        be_plain_font->GetHeight(&fh);
        const float height = ceilf(fh.ascent + fh.descent + fh.leading) + 6;
        SetExplicitMinSize(BSize(B_SIZE_UNSET, height));
        SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, height));
    }

    void Draw(BRect) override
    {
        const BRect bounds = Bounds();
        std::string texts[COLUMNS];
        for (int i = 0; i < COLUMNS; i++)
            texts[i] = column_titles[i];

        SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
        SetHighUIColor(B_PANEL_TEXT_COLOR);
        /* The list sits in a scroll view: a border, and the scroll bar. */
        draw_columns(this, bounds, texts, 1, bounds.Width() - B_V_SCROLL_BAR_WIDTH - 2);
        SetHighUIColor(B_PANEL_BACKGROUND_COLOR, B_DARKEN_2_TINT);
        StrokeLine(BPoint(bounds.left, bounds.bottom), BPoint(bounds.right, bounds.bottom));
    }
};

/* --- Hard disks (qt_settingsharddisks.cpp) ----------------------------- */

class HardDisksPage : public SettingsPage {
public:
    HardDisksPage()
        : SettingsPage("Hard disks")
        , bus(new Choice("bus", "Bus:", MSG_HD_BUS))
        , channel(new Choice("channel", "Channel:", MSG_HD_CHANNEL))
        , speed(new Choice("speed", "Model:", MSG_HD_SPEED))
        , audio(new Choice("audio", "Audio:", MSG_HD_AUDIO))
        , org_rows(0)
    {
        list = new BListView("disks", B_SINGLE_SELECTION_LIST, B_WILL_DRAW | B_FRAME_EVENTS | B_NAVIGABLE | B_FULL_UPDATE_ON_RESIZE);
        list->SetSelectionMessage(new BMessage(MSG_HD_SELECTED));
        BScrollView *scroll = new BScrollView("disks_scroll", list, 0, false, true);

        configure    = new BButton("configure", "Configure", new BMessage(MSG_HD_CONFIGURE));
        new_disk     = new BButton("new", "New…", new BMessage(MSG_HD_NEW));
        existing     = new BButton("existing", "Existing…", new BMessage(MSG_HD_EXISTING));
        remove       = new BButton("remove", "Remove", new BMessage(MSG_HD_REMOVE));

        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .AddGroup(B_VERTICAL, 0)
                .Add(new ColumnHeader())
                .Add(scroll, 1)
            .End()
            .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
                .AddMenuField(bus->field, 0, 0)
                .AddMenuField(channel->field, 2, 0)
                .AddMenuField(speed->field, 0, 1)
                .Add(configure, 2, 1, 2)
                .AddMenuField(audio->field, 0, 2)
            .End()
            .AddGroup(B_HORIZONTAL)
                .Add(new_disk)
                .Add(existing)
                .Add(remove)
                .AddGlue()
            .End();

        Harddrives::PopulateBuses(bus);

        for (int i = 0; i < HDD_NUM; i++) {
            if (hdd[i].bus_type > 0) {
                Disk d;
                d.bus       = hdd[i].bus_type;
                d.channel   = list_channel(&hdd[i]);
                d.tracks    = hdd[i].tracks;
                d.hpc       = hdd[i].hpc;
                d.spt       = hdd[i].spt;
                d.speed     = hdd[i].speed_preset;
                d.audio     = hdd[i].audio_profile;
                d.blocksize = hdd[i].vhd_blocksize;
                d.fn        = hdd[i].fn;
                snprintf(d.vendor, sizeof(d.vendor), "%s", hdd[i].custom_vendor);
                snprintf(d.model, sizeof(d.model), "%s", hdd[i].custom_model);
                snprintf(d.version, sizeof(d.version), "%s", hdd[i].custom_version);
                AddRow(d);
                org_rows++;
            }
        }
        UpdateButtons();

        if (!disks.empty())
            list->Select(0);
        RowChanged();
    }

    /* The machine, disk controllers and sound cards chosen on other pages
       decide who has each IDE channel: bring the names up to date. */
    void Show() override
    {
        RefreshRows();
        RowChanged();
        SettingsPage::Show();
    }

    void MachineChanged(int) override
    {
        RefreshRows();
        RowChanged();
    }

    bool Changed() override
    {
        bool changed = (disks.size() != (size_t) org_rows);

        for (size_t i = 0; !changed && (i < disks.size()); i++) {
            const Disk        &d  = disks[i];
            const hard_disk_t *hd = &hdd[i];
            changed |= (hd->bus_type != d.bus);
            changed |= (list_channel(hd) != d.channel);
            changed |= (hd->tracks != d.tracks);
            changed |= (hd->hpc != d.hpc);
            changed |= (hd->spt != d.spt);
            changed |= (hd->speed_preset != d.speed);
            changed |= (hd->audio_profile != d.audio);
            changed |= (hd->vhd_blocksize != d.blocksize);
            changed |= (strcmp(hd->custom_vendor, d.vendor) != 0);
            changed |= (strcmp(hd->custom_model, d.model) != 0);
            changed |= (strcmp(hd->custom_version, d.version) != 0);
            changed |= (strcmp(hd->fn, d.fn.c_str()) != 0);
        }

        return changed;
    }

    void Save() override
    {
        memset(hdd, 0, sizeof(hdd));

        for (size_t i = 0; i < disks.size(); i++) {
            const Disk  &d  = disks[i];
            hard_disk_t *hd = &hdd[i];
            hd->bus_type = d.bus;
            hd->channel  = d.channel;
            if (hd->bus_type == HDD_BUS_LPT) {
                hd->lpt_bridge = hd->channel >> 2;
                hd->channel &= 3;
            }
            hd->tracks        = d.tracks;
            hd->hpc           = d.hpc;
            hd->spt           = d.spt;
            hd->speed_preset  = d.speed;
            hd->audio_profile = d.audio;
            hd->vhd_blocksize = d.blocksize;
            snprintf(hd->custom_vendor, sizeof(hd->custom_vendor), "%s", d.vendor);
            snprintf(hd->custom_model, sizeof(hd->custom_model), "%s", d.model);
            snprintf(hd->custom_version, sizeof(hd->custom_version), "%s", d.version);
            strncpy(hd->fn, d.fn.c_str(), sizeof(hd->fn) - 1);
            hd->priv = nullptr;
        }
    }

    bool Handle(BMessage *message) override
    {
        const int32 row = list->CurrentSelection();
        Disk       *d   = ((row >= 0) && ((size_t) row < disks.size())) ? &disks[row] : nullptr;

        switch (message->what) {
            case MSG_HD_SELECTED:
                RowChanged();
                return true;

            case MSG_HD_BUS:
                if (d != nullptr)
                    BusChosen(row, message->GetInt32("value", d->bus));
                return true;

            case MSG_HD_CHANNEL:
                {
                    const int32 value = message->GetInt32("value", CHANNEL_NONE);
                    if ((d != nullptr) && (value != CHANNEL_NONE)) {
                        d->channel = value;
                        RefreshRow(row);
                    }
                    return true;
                }

            case MSG_HD_SPEED:
                if (d != nullptr) {
                    d->speed = message->GetInt32("value", d->speed);
                    /* The audio profile goes back to None for another model. */
                    d->audio = 0;
                    RefreshRow(row);
                    PopulateAudio(d->speed);
                    audio->Select(d->audio);
                    configure->SetEnabled(generic_hdd(d->bus, d->speed));
                }
                return true;

            case MSG_HD_AUDIO:
                if (d != nullptr)
                    d->audio = message->GetInt32("value", 0);
                return true;

            case MSG_HD_CONFIGURE:
                if ((d != nullptr) && generic_hdd(d->bus, d->speed))
                    harddisk_identity_dialog(Window(), d->vendor, d->model, d->version);
                return true;

            case MSG_HD_NEW:
            case MSG_HD_EXISTING:
                AddFromDialog(message->what == MSG_HD_EXISTING);
                return true;

            case MSG_HD_REMOVE:
                if (d != nullptr) {
                    disks.erase(disks.begin() + row);
                    delete list->RemoveItem(row);
                    UpdateButtons();
                    RowChanged();
                }
                return true;

            default:
                return false;
        }
    }

private:
    /* The channels taken, by all disks but the one given. */
    BusTracking Tracking(int skip) const
    {
        BusTracking tracking;
        tracking.AddSavedDrives();
        for (size_t i = 0; i < disks.size(); i++) {
            if ((int) i != skip)
                tracking.Track(true, disks[i].bus, disks[i].channel);
        }
        return tracking;
    }

    void AddRow(const Disk &d)
    {
        disks.push_back(d);
        list->AddItem(new DiskItem());
        RefreshRow(disks.size() - 1);
    }

    void RefreshRow(int row)
    {
        DiskItem *item = static_cast<DiskItem *>(list->ItemAt(row));
        if (item == nullptr)
            return;

        const Disk &d = disks[row];
        char        geometry[128];
        snprintf(geometry, sizeof(geometry), "%u, %u, %u (%u MiB)", (unsigned) d.tracks, (unsigned) d.hpc, (unsigned) d.spt,
                 (unsigned) (((uint64) d.tracks * d.hpc * d.spt) >> 11));

        /* A file under the user's directory is shown relative to it. */
        std::string     file   = d.fn;
        const size_t    prefix = strlen(usr_path);
        if ((prefix > 0) && (file.size() > prefix) && !strncasecmp(file.c_str(), usr_path, prefix))
            file = file.substr(prefix);

        const char *speed_name = hdd_preset_getname(d.speed);

        item->texts[0] = Harddrives::BusChannelName(d.bus, d.channel);
        item->texts[1] = file;
        item->texts[2] = geometry;
        item->texts[3] = (speed_name != nullptr) ? speed_name : "";
        list->InvalidateItem(row);
    }

    void RefreshRows()
    {
        for (size_t i = 0; i < disks.size(); i++)
            RefreshRow(i);
    }

    void UpdateButtons()
    {
        const bool room = disks.size() < HDD_NUM;
        new_disk->SetEnabled(room);
        existing->SetEnabled(room);
        remove->SetEnabled(list->CurrentSelection() >= 0);
    }

    static void SetShown(BView *view, bool shown)
    {
        if (shown && view->IsHidden(view))
            view->Show();
        else if (!shown && !view->IsHidden(view))
            view->Hide();
    }

    void PopulateAudio(uint32 preset)
    {
        audio->Clear();

        /* The profiles with no RPM, or the preset's. */
        const uint32 rpm   = hdd_preset_get_rpm(preset);
        const int    count = hdd_audio_get_profile_count();
        if (count == 0)
            audio->Add("None", 0);
        for (int i = 0; i < count; i++) {
            const char  *name        = hdd_audio_get_profile_name(i);
            const uint32 profile_rpm = hdd_audio_get_profile_rpm(i);
            if ((name != nullptr) && ((profile_rpm == 0) || (profile_rpm == rpm)))
                audio->Add(name, i);
        }
    }

    /* Qt's onTableRowChanged(): the selected disk's settings, or nothing. */
    void RowChanged()
    {
        const int32 row   = list->CurrentSelection();
        const bool  shown = (row >= 0) && ((size_t) row < disks.size());

        UpdateButtons();
        SetShown(bus->field, shown);
        SetShown(channel->field, shown);
        SetShown(speed->field, shown);
        SetShown(audio->field, shown);
        if (!shown) {
            SetShown(configure, false);
            return;
        }

        const Disk &d = disks[row];
        bus->Select(d.bus);
        Harddrives::PopulateBusChannels(channel, d.bus, Tracking(row));
        channel->Select(d.channel);
        Harddrives::PopulateSpeeds(speed, d.bus);
        speed->Select(d.speed);
        PopulateAudio(d.speed);
        audio->Select(d.audio);

        SetShown(configure, (d.bus == HDD_BUS_IDE) || (d.bus == HDD_BUS_ATAPI) || (d.bus == HDD_BUS_SCSI) || (d.bus == HDD_BUS_LPT));
        configure->SetEnabled(generic_hdd(d.bus, d.speed));
    }

    /* Qt's on_comboBoxBus_currentIndexChanged(): the disk moves to the next
       free channel of the new bus, with the first model. */
    void BusChosen(int row, int new_bus)
    {
        Disk &d = disks[row];
        if (new_bus == d.bus)
            return;

        const BusTracking tracking = Tracking(row);
        const uint8       next     = tracking.NextFree(new_bus);

        d.bus     = new_bus;
        d.channel = (next == CHANNEL_NONE) ? 0 : next;
        d.speed   = 0;
        d.audio   = 0;
        RefreshRow(row);
        RowChanged();
    }

    void AddFromDialog(bool from_existing)
    {
        if (disks.size() >= HDD_NUM)
            return;

        /* The dialog keeps a reference to it. */
        const BusTracking tracking = Tracking(-1);
        HarddiskDialog   *dialog   = new HarddiskDialog(Window(), from_existing, tracking);
        if (dialog->Go() && dialog->Lock()) {
            Disk d;
            memset(d.vendor, 0, sizeof(d.vendor));
            memset(d.model, 0, sizeof(d.model));
            memset(d.version, 0, sizeof(d.version));
            d.bus       = dialog->Bus();
            d.channel   = dialog->Channel();
            d.tracks    = dialog->Cylinders();
            d.hpc       = dialog->Heads();
            d.spt       = dialog->Sectors();
            d.speed     = dialog->Speed();
            d.audio     = 0;
            d.blocksize = 0;
            d.fn        = dialog->FileName();
            dialog->Unlock();

            AddRow(d);
            UpdateButtons();
            RowChanged();
        }
        dialog->Dismiss();
    }

    std::vector<Disk> disks;
    BListView        *list;
    Choice           *bus;
    Choice           *channel;
    Choice           *speed;
    Choice           *audio;
    BButton          *configure;
    BButton          *new_disk;
    BButton          *existing;
    BButton          *remove;
    int               org_rows;
};

SettingsPage *
create_hard_disks_page()
{
    return new HardDisksPage();
}

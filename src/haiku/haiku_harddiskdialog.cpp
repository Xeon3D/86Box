/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the New / Existing hard disk dialog
 *          (qt_harddiskdialog.cpp) and the generic hard disk's identity
 *          dialog.
 *
 *          A new image is written by a thread of its own, which reports
 *          its progress to the dialog's status bar.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Alert.h>
#include <Button.h>
#include <ByteOrder.h>
#include <Entry.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <Messenger.h>
#include <Path.h>
#include <StatusBar.h>
#include <TextControl.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
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
#include <86box/plat.h>
#include "../disk/minivhd/minivhd.h"
}

enum : uint32 {
    MSG_HD_FILE_TEXT   = 'hdft', /* the file name was edited */
    MSG_HD_FILE_DONE   = 'hdfi', /* ... and left (Enter, focus) */
    MSG_HD_BROWSE      = 'hdbr',
    MSG_HD_FILE_OPEN   = 'hdfo', /* open panel: an existing image */
    MSG_HD_FILE_SAVE   = 'hdfs', /* save panel: a new image */
    MSG_HD_PARENT_OPEN = 'hdpa', /* open panel: a differencing VHD's parent */
    MSG_HD_CYLINDERS   = 'hdcy',
    MSG_HD_HEADS       = 'hdhd',
    MSG_HD_SECTORS     = 'hdsc',
    MSG_HD_SIZE        = 'hdsz',
    MSG_HD_TYPE        = 'hdty',
    MSG_HD_BUS         = 'hdbs',
    MSG_HD_FORMAT      = 'hdfm',
    MSG_HD_OK          = 'hdok',
    MSG_HD_CANCEL      = 'hdcn',
    MSG_HD_PROGRESS    = 'hdpg',
    MSG_HD_CREATED     = 'hddn',

    MSG_ID_OK          = 'hiok',
    MSG_ID_CANCEL      = 'hicn',
};

/* The format pop-up, in Qt's order (IMG_FMT_*). */
static const char *const format_names[] = {
    "Raw image (.img)",
    "HDI image (.hdi)",
    "HDX image (.hdx)",
    "Fixed-size VHD (.vhd)",
    "Dynamic-size VHD (.vhd)",
    "Differencing VHD (.vhd)",
};
static const int DEFAULT_DISK_FORMAT = IMG_FMT_VHD_DYNAMIC;

/* Table entries 0 .. 126 are the presets; then the two custom entries. */
static const int TYPE_CUSTOM       = 127;
static const int TYPE_CUSTOM_LARGE = 128;

/* --------------------------------------------------------- ModalDialog */

ModalDialog::ModalDialog(const char *title, BWindow *parent)
    : BWindow(BRect(0, 0, 100, 100), title, B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
              B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS | B_CLOSE_ON_ESCAPE)
    , parent(parent)
    , done_sem(-1)
    , accepted(false)
{
    /* Modal to the parent only; a window that cannot be in a subset (one
       that is modal itself) gets an application-modal dialog. */
    if ((parent == nullptr) || (AddToSubset(parent) != B_OK))
        SetFeel(B_MODAL_APP_WINDOW_FEEL);
}

/* Like BAlert::Go(): wait for Done(), keeping the parent window drawn. */
bool
ModalDialog::Go()
{
    done_sem = create_sem(0, "modal dialog");
    accepted = false;

    if (Lock()) {
        ResizeToPreferred();
        if (parent != nullptr)
            CenterIn(parent->Frame());
        Unlock();
    }
    Show();

    status_t err;
    do {
        err = acquire_sem_etc(done_sem, 1, B_RELATIVE_TIMEOUT, 50000);
        if ((err == B_TIMED_OUT) && (parent != nullptr))
            parent->UpdateIfNeeded();
    } while ((err == B_TIMED_OUT) || (err == B_INTERRUPTED));

    delete_sem(done_sem);
    done_sem = -1;
    return accepted;
}

void
ModalDialog::Dismiss()
{
    if (Lock())
        Quit();
}

void
ModalDialog::Done(bool accept)
{
    if (done_sem < 0)
        return;
    accepted = accept;
    Hide();
    release_sem(done_sem);
}

/* The close box is Cancel; Dismiss() closes the window. */
bool
ModalDialog::QuitRequested()
{
    Done(false);
    return false;
}

/* ------------------------------------------------------- image creation */

typedef struct Geom {
    uint32 cyl;
    uint32 heads;
    uint32 spt;
} Geom;

/* If the geometry asked for does not fit the VHD's, take the next larger
   size that does (as Qt does); only for VHDs over 31.5 GB. */
static void
adjust_86box_geometry_for_vhd(Geom *geometry, MVHDGeom *vhd_geometry)
{
    if (geometry->cyl <= 65535) {
        vhd_geometry->cyl   = geometry->cyl;
        vhd_geometry->heads = geometry->heads;
        vhd_geometry->spt   = geometry->spt;
        return;
    }

    int desired_sectors = geometry->cyl * geometry->heads * geometry->spt;
    if (desired_sectors > 267321600)
        desired_sectors = 267321600;

    const int remainder = desired_sectors % 85680; /* the LCM of 1008 (63 * 16) and 4080 (255 * 16) */
    if (remainder > 0)
        desired_sectors += (85680 - remainder);

    geometry->cyl   = desired_sectors / (16 * 63);
    geometry->heads = 16;
    geometry->spt   = 63;

    vhd_geometry->cyl   = desired_sectors / (16 * 255);
    vhd_geometry->heads = 16;
    vhd_geometry->spt   = 255;
}

static void
adjust_vhd_geometry_for_86box(const MVHDGeom *vhd_geometry, Geom *geometry)
{
    if (vhd_geometry->spt <= 63) {
        geometry->cyl   = vhd_geometry->cyl;
        geometry->heads = vhd_geometry->heads;
        geometry->spt   = vhd_geometry->spt;
        return;
    }

    int desired_sectors = vhd_geometry->cyl * vhd_geometry->heads * vhd_geometry->spt;
    if (desired_sectors > 267321600)
        desired_sectors = 267321600;

    const int remainder = desired_sectors % 85680;
    if (remainder > 0)
        desired_sectors -= remainder;

    geometry->cyl   = desired_sectors / (16 * 63);
    geometry->heads = 16;
    geometry->spt   = 63;
}

/* What the creation thread gets. */
struct CreateJob {
    HarddiskDialog *dialog;
    std::string     file_name;
    std::string     parent;
    int             format;
    Geom            geometry;
    int             block_size;
};

/* mvhd_create_fixed() reports its progress here; one image at a time. */
static HarddiskDialog *progress_dialog = nullptr;

static void
vhd_progress(uint32_t current_sector, uint32_t total_sectors)
{
    if ((progress_dialog != nullptr) && (total_sectors > 0))
        progress_dialog->Progress((int) (((uint64) current_sector * 100) / total_sectors));
}

static bool
write_le32(FILE *fp, uint32 value)
{
    const uint32 le = B_HOST_TO_LENDIAN_INT32(value);
    return fwrite(&le, sizeof(le), 1, fp) == 1;
}

static bool
write_le64(FILE *fp, uint64 value)
{
    const uint64 le = B_HOST_TO_LENDIAN_INT64(value);
    return fwrite(&le, sizeof(le), 1, fp) == 1;
}

/* A raw, HDI or HDX image: the header, then the zeroed data. */
static bool
create_plain_image(const CreateJob *job)
{
    const uint32 sector_size = 512;
    const uint64 size        = (uint64) job->geometry.cyl * job->geometry.heads * job->geometry.spt * sector_size;

    FILE *fp = plat_fopen(job->file_name.c_str(), "wb");
    if (fp == nullptr)
        return false;

    bool ok = true;
    if (job->format == IMG_FMT_HDI) {
        ok &= write_le32(fp, 0);                    /* 00000000: Zero/unknown */
        ok &= write_le32(fp, 0);                    /* 00000004: Zero/unknown */
        ok &= write_le32(fp, 0x1000);               /* 00000008: Offset at which data starts */
        ok &= write_le32(fp, (uint32) size);        /* 0000000C: Full size of the data (32-bit) */
        ok &= write_le32(fp, sector_size);          /* 00000010: Sector size in bytes */
        ok &= write_le32(fp, job->geometry.spt);    /* 00000014: Sectors per cylinder */
        ok &= write_le32(fp, job->geometry.heads);  /* 00000018: Heads per cylinder */
        ok &= write_le32(fp, job->geometry.cyl);    /* 0000001C: Cylinders */
        for (int i = 0; ok && (i < 0x3f8); i++)
            ok &= write_le32(fp, 0);
    } else if (job->format == IMG_FMT_HDX) {
        ok &= write_le64(fp, 0xD778A82044445459ULL); /* 00000000: Signature */
        ok &= write_le64(fp, size);                  /* 00000008: Full size of the data (64-bit) */
        ok &= write_le32(fp, sector_size);           /* 00000010: Sector size in bytes */
        ok &= write_le32(fp, job->geometry.spt);     /* 00000014: Sectors per cylinder */
        ok &= write_le32(fp, job->geometry.heads);   /* 00000018: Heads per cylinder */
        ok &= write_le32(fp, job->geometry.cyl);     /* 0000001C: Cylinders */
        ok &= write_le32(fp, 0);                     /* 00000020: [Translation] Sectors per cylinder */
        ok &= write_le32(fp, 0);                     /* 00000024: [Translation] Heads per cylinder */
    }

    std::vector<char> buf(1048576, 0);
    const uint64      blocks = size >> 20;
    const uint64      rest   = size & 0xfffff;

    if (ok && rest)
        ok = fwrite(buf.data(), 1, rest, fp) == rest;
    for (uint64 i = 0; ok && (i < blocks); i++) {
        ok = fwrite(buf.data(), 1, buf.size(), fp) == buf.size();
        job->dialog->Progress((int) ((i * 100) / blocks));
    }
    job->dialog->Progress(100);

    ok &= (fclose(fp) == 0);
    return ok;
}

static Geom
create_vhd(const CreateJob *job)
{
    Geom     geometry = job->geometry;
    MVHDGeom vhd_geometry;
    int      vhd_error = 0;
    MVHDMeta *vhd      = nullptr;

    std::string path   = job->file_name;
    std::string parent = job->parent;

    switch (job->format) {
        case IMG_FMT_VHD_FIXED:
            adjust_86box_geometry_for_vhd(&geometry, &vhd_geometry);
            progress_dialog = job->dialog;
            vhd             = mvhd_create_fixed(&path[0], vhd_geometry, &vhd_error, vhd_progress);
            progress_dialog = nullptr;
            break;

        case IMG_FMT_VHD_DYNAMIC:
            {
                adjust_86box_geometry_for_vhd(&geometry, &vhd_geometry);
                MVHDCreationOptions options;
                memset(&options, 0, sizeof(options));
                options.block_size_in_sectors = job->block_size;
                options.path                  = &path[0];
                options.size_in_bytes         = 0;
                options.geometry              = vhd_geometry;
                options.type                  = MVHD_TYPE_DYNAMIC;
                vhd                           = mvhd_create_ex(options, &vhd_error);
                break;
            }

        case IMG_FMT_VHD_DIFF:
            {
                MVHDCreationOptions options;
                memset(&options, 0, sizeof(options));
                options.block_size_in_sectors = job->block_size;
                options.path                  = &path[0];
                options.parent_path           = &parent[0];
                options.type                  = MVHD_TYPE_DIFF;
                vhd                           = mvhd_create_ex(options, &vhd_error);
                if (vhd != nullptr) {
                    vhd_geometry = mvhd_get_geometry(vhd);
                    if (vhd_geometry.spt > 63) {
                        geometry.cyl   = mvhd_calc_size_sectors(&vhd_geometry) / (16 * 63);
                        geometry.heads = 16;
                        geometry.spt   = 63;
                    } else {
                        geometry.cyl   = vhd_geometry.cyl;
                        geometry.heads = vhd_geometry.heads;
                        geometry.spt   = vhd_geometry.spt;
                    }
                }
                break;
            }

        default:
            break;
    }

    if (vhd == nullptr)
        geometry = { 0, 0, 0 };
    else
        mvhd_close(vhd);

    return geometry;
}

int32
HarddiskDialog::CreateThread(void *data)
{
    CreateJob *job = static_cast<CreateJob *>(data);
    BMessage   done(MSG_HD_CREATED);

    if (job->format >= IMG_FMT_VHD_FIXED) {
        const Geom geometry = create_vhd(job);
        done.AddBool("ok", (geometry.cyl != 0) || (geometry.heads != 0) || (geometry.spt != 0));
        done.AddInt32("cylinders", geometry.cyl);
        done.AddInt32("heads", geometry.heads);
        done.AddInt32("sectors", geometry.spt);
    } else {
        done.AddBool("ok", create_plain_image(job));
        done.AddInt32("cylinders", job->geometry.cyl);
        done.AddInt32("heads", job->geometry.heads);
        done.AddInt32("sectors", job->geometry.spt);
    }

    BMessenger(job->dialog).SendMessage(&done);
    delete job;
    return 0;
}

/* ------------------------------------------------------ HarddiskDialog */

static BTextControl *
number_field(const char *name, const char *label, uint32 what)
{
    BTextControl *field = new BTextControl(name, label, "", nullptr);
    field->SetModificationMessage(new BMessage(what));
    field->SetExplicitMinSize(BSize(be_plain_font->StringWidth("0000000") + 10, B_SIZE_UNSET));
    for (int c = 0; c < 256; c++) {
        if ((c < '0') || (c > '9'))
            field->TextView()->DisallowChar(c);
    }
    return field;
}

static void
error_alert(const char *title, const char *text)
{
    BAlert *alert = new BAlert(title, text, "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT);
    alert->Go();
}

static void
info_alert(const char *title, const char *text)
{
    BAlert *alert = new BAlert(title, text, "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_INFO_ALERT);
    alert->Go();
}

static bool
yes_no_alert(const char *title, const char *text)
{
    BAlert *alert = new BAlert(title, text, "No", "Yes", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    alert->SetShortcut(0, B_ESCAPE);
    return alert->Go() == 1;
}

static const char *const write_failed = "Failed to write the disk image. Make sure the file is being saved to a writable directory.";
static const char *const read_failed  = "Failed to read the disk image. Make sure the file exists and is readable.";

HarddiskDialog::HarddiskDialog(BWindow *parent, bool existing, const BusTracking &tracking)
    : ModalDialog(existing ? "Add Existing Hard Disk" : "Add New Hard Disk", parent)
    , existing(existing)
    , tracking(tracking)
    , panel(nullptr)
    , parent_panel(nullptr)
    , cylinders(0)
    , heads(0)
    , sectors(0)
    , max_cylinders(0)
    , max_heads(0)
    , max_sectors(0)
    , file_ok(false)
    , busy(false)
    , last_progress(-1)
{
    file_name = new BTextControl("file_name", "File name:", "", new BMessage(MSG_HD_FILE_DONE));
    file_name->SetModificationMessage(new BMessage(MSG_HD_FILE_TEXT));
    file_name->SetExplicitMinSize(BSize(260, B_SIZE_UNSET));
    browse = new BButton("browse", "Browse…", new BMessage(MSG_HD_BROWSE));

    cylinders_text = number_field("cylinders", "Cylinders:", MSG_HD_CYLINDERS);
    heads_text     = number_field("heads", "Heads:", MSG_HD_HEADS);
    sectors_text   = number_field("sectors", "Sectors:", MSG_HD_SECTORS);
    size_text      = number_field("size", "Size (MB):", MSG_HD_SIZE);

    type       = new Choice("type", "Type:", MSG_HD_TYPE);
    bus        = new Choice("bus", "Bus:", MSG_HD_BUS);
    channel    = new Choice("channel", "Channel:", MSG_SETTINGS_IGNORE);
    speed      = new Choice("speed", "Model:", MSG_SETTINGS_IGNORE);
    format     = new Choice("format", "Image Format:", MSG_HD_FORMAT);
    block_size = new Choice("block_size", "Block Size:", MSG_SETTINGS_IGNORE);

    progress = new BStatusBar("progress");
    progress->SetMaxValue(100);

    ok     = new BButton("ok", "OK", new BMessage(MSG_HD_OK));
    cancel = new BButton("cancel", "Cancel", new BMessage(MSG_HD_CANCEL));

    BLayoutBuilder::Group<>(this, B_VERTICAL)
        .SetInsets(B_USE_WINDOW_SPACING)
        .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
            .AddTextControl(file_name, 0, 0, B_ALIGN_HORIZONTAL_UNSET, 1, 5)
            .Add(browse, 6, 0)
            .AddTextControl(cylinders_text, 0, 1)
            .AddTextControl(heads_text, 2, 1)
            .AddTextControl(sectors_text, 4, 1)
            .AddTextControl(size_text, 0, 2)
            .AddMenuField(type->field, 2, 2, B_ALIGN_HORIZONTAL_UNSET, 1, 4)
            .AddMenuField(bus->field, 0, 3, B_ALIGN_HORIZONTAL_UNSET, 1, 3)
            .AddMenuField(channel->field, 4, 3, B_ALIGN_HORIZONTAL_UNSET, 1, 2)
            .AddMenuField(speed->field, 0, 4, B_ALIGN_HORIZONTAL_UNSET, 1, 6)
            .AddMenuField(format->field, 0, 5, B_ALIGN_HORIZONTAL_UNSET, 1, 6)
            .AddMenuField(block_size->field, 0, 6, B_ALIGN_HORIZONTAL_UNSET, 1, 6)
        .End()
        .Add(progress)
        .AddGlue()
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel)
            .Add(ok)
        .End();

    SetDefaultButton(ok);

    for (int i = IMG_FMT_RAW; i <= IMG_FMT_VHD_DIFF; i++)
        format->Add(format_names[i], i);
    block_size->Add("Large blocks (2 MB)", MVHD_BLOCK_LARGE);
    block_size->Add("Small blocks (512 KB)", MVHD_BLOCK_SMALL);
    block_size->Select(MVHD_BLOCK_LARGE);

    for (int i = 0; i < TYPE_CUSTOM; i++) {
        char           text[128];
        const uint64_t size = ((uint64_t) hdd_table[i][0]) * hdd_table[i][1] * hdd_table[i][2];
        snprintf(text, sizeof(text), "%u MB (CHS: %u, %u, %u)", (unsigned) (size >> 11), hdd_table[i][0], hdd_table[i][1], hdd_table[i][2]);
        type->Add(text, i);
    }
    type->Add("Custom…", TYPE_CUSTOM);
    type->Add("Custom (large)…", TYPE_CUSTOM_LARGE);

    Harddrives::PopulateBuses(bus);
    bus->Select(HDD_BUS_IDE);

    /* The first preset, as Qt's line edits start (the type pop-up shows it). */
    cylinders = hdd_table[0][0];
    heads     = hdd_table[0][1];
    sectors   = hdd_table[0][2];
    type->Select(0);
    BusChanged();
    SetGeometryText();

    progress->Hide();
    ok->SetEnabled(false);

    if (existing) {
        EnableGeometry(false);
        format->field->Hide();
        block_size->field->Hide();
    } else {
        format->Select(DEFAULT_DISK_FORMAT);
        FormatChanged();
    }
}

HarddiskDialog::~HarddiskDialog()
{
    delete panel;
    delete parent_panel;
    delete type;
    delete bus;
    delete channel;
    delete speed;
    delete format;
    delete block_size;
}

uint8
HarddiskDialog::Bus() const
{
    return (uint8) bus->Value(HDD_BUS_IDE);
}

uint8
HarddiskDialog::Channel() const
{
    return (uint8) channel->Value(0);
}

std::string
HarddiskDialog::FileName() const
{
    return file_name->Text();
}

uint32
HarddiskDialog::Speed() const
{
    return (uint32) speed->Value(0);
}

void
HarddiskDialog::Progress(int percent)
{
    if (percent == last_progress)
        return;
    last_progress = percent;

    BMessage msg(MSG_HD_PROGRESS);
    msg.AddInt32("percent", percent);
    PostMessage(&msg);
}

void
HarddiskDialog::EnableGeometry(bool enabled)
{
    cylinders_text->SetEnabled(enabled);
    heads_text->SetEnabled(enabled);
    sectors_text->SetEnabled(enabled);
    size_text->SetEnabled(enabled);
    type->SetEnabled(enabled);
}

void
HarddiskDialog::SetGeometryText()
{
    cylinders_text->SetText(std::to_string(cylinders).c_str());
    heads_text->SetText(std::to_string(heads).c_str());
    sectors_text->SetText(std::to_string(sectors).c_str());
    RecalcSize();
}

void
HarddiskDialog::RecalcSize()
{
    const uint64 size = ((uint64) cylinders * heads * sectors) << 9;
    size_text->SetText(std::to_string(size >> 20).c_str());
}

void
HarddiskDialog::RecalcSelection()
{
    int selection = TYPE_CUSTOM;
    for (int i = 0; i < TYPE_CUSTOM; i++) {
        if ((cylinders == hdd_table[i][0]) && (heads == hdd_table[i][1]) && (sectors == hdd_table[i][2]))
            selection = i;
    }
    if ((selection == TYPE_CUSTOM) && (heads == 16) && (sectors == 63))
        selection = TYPE_CUSTOM_LARGE;
    type->Select(selection);
}

/* Qt's checkAndAdjust*(): clamp to what the bus takes. */
void
HarddiskDialog::CheckAndAdjust()
{
    bool changed = false;
    if (cylinders > max_cylinders) {
        cylinders = max_cylinders;
        cylinders_text->SetText(std::to_string(cylinders).c_str());
        changed = true;
    }
    if (heads > max_heads) {
        heads = max_heads;
        heads_text->SetText(std::to_string(heads).c_str());
        changed = true;
    }
    if (sectors > max_sectors) {
        sectors = max_sectors;
        sectors_text->SetText(std::to_string(sectors).c_str());
        changed = true;
    }
    if (changed) {
        RecalcSize();
        RecalcSelection();
    }
}

void
HarddiskDialog::BusChanged()
{
    const int b = bus->Value(HDD_BUS_IDE);

    switch (b) {
        case HDD_BUS_MFM:
            max_sectors   = 26; /* 17 for MFM, 26 for RLL. */
            max_heads     = 15;
            max_cylinders = 2047;
            break;
        case HDD_BUS_XTA:
            max_sectors   = 63;
            max_heads     = 16;
            max_cylinders = 1023;
            break;
        case HDD_BUS_ESDI:
            max_sectors   = 99; /* ESDI drives usually had 32 to 43 sectors per track. */
            max_heads     = 16;
            max_cylinders = 266305;
            break;
        case HDD_BUS_IDE:
        case HDD_BUS_LPT:
        case HDD_BUS_ATAPI:
        case HDD_BUS_SCSI:
            max_sectors   = 255;
            max_heads     = 255;
            max_cylinders = 266305;
            break;
        default:
            max_sectors = max_heads = max_cylinders = 0;
            break;
    }

    if ((format->Value(DEFAULT_DISK_FORMAT) != IMG_FMT_VHD_DIFF) || existing)
        CheckAndAdjust();

    Harddrives::PopulateBusChannels(channel, b, tracking);
    Harddrives::PopulateSpeeds(speed, b);
    speed->Select(0);

    const uint8 next = tracking.NextFree(b);
    channel->Select((next == CHANNEL_NONE) ? 0 : next);
}

void
HarddiskDialog::FormatChanged()
{
    const int index = format->Value(DEFAULT_DISK_FORMAT);

    if (index == IMG_FMT_VHD_DIFF) {
        /* The geometry is the parent's. */
        EnableGeometry(false);
        cylinders_text->SetText("(N/A)");
        heads_text->SetText("(N/A)");
        sectors_text->SetText("(N/A)");
        size_text->SetText("(N/A)");
    } else {
        EnableGeometry(true);
        SetGeometryText();
    }

    if (index < IMG_FMT_VHD_DYNAMIC) {
        if (!block_size->field->IsHidden())
            block_size->field->Hide();
    } else if (block_size->field->IsHidden()) {
        block_size->field->Show();
    }
}

void
HarddiskDialog::UpdateOk()
{
    const bool has_name = file_name->Text()[0] != '\0';
    ok->SetEnabled(!busy && has_name && (!existing || file_ok));
}

void
HarddiskDialog::SetBusy(bool on)
{
    busy = on;
    file_name->SetEnabled(!on);
    browse->SetEnabled(!on);
    bus->SetEnabled(!on);
    channel->SetEnabled(!on);
    speed->SetEnabled(!on);
    format->SetEnabled(!on);
    block_size->SetEnabled(!on);
    cancel->SetEnabled(!on);
    if (on) {
        EnableGeometry(false);
        progress->Reset();
        progress->SetMaxValue(100);
        if (progress->IsHidden())
            progress->Show();
    } else {
        if (!progress->IsHidden())
            progress->Hide();
        if (format->Value(DEFAULT_DISK_FORMAT) != IMG_FMT_VHD_DIFF)
            EnableGeometry(true);
        /* Restore what the pop-ups' own rules say (one item: disabled). */
        bus->Select(bus->Value(HDD_BUS_IDE));
        channel->Select(channel->Value(0));
        speed->Select(speed->Value(0));
        format->Select(format->Value(DEFAULT_DISK_FORMAT));
        block_size->Select(block_size->Value(MVHD_BLOCK_LARGE));
    }
    UpdateOk();
}

void
HarddiskDialog::Browse()
{
    entry_ref dir;
    const bool has_dir = (get_ref_for_path(usr_path, &dir) == B_OK);

    if (panel == nullptr) {
        BMessenger target(this);
        if (existing)
            panel = new BFilePanel(B_OPEN_PANEL, &target, has_dir ? &dir : nullptr, B_FILE_NODE, false,
                                   new BMessage(MSG_HD_FILE_OPEN), nullptr, true, true);
        else
            panel = new BFilePanel(B_SAVE_PANEL, &target, has_dir ? &dir : nullptr, B_FILE_NODE, false,
                                   new BMessage(MSG_HD_FILE_SAVE), nullptr, true, true);
        if (panel->Window()->Lock()) {
            panel->Window()->SetTitle(existing ? "Open hard disk image" : "Create hard disk image");
            panel->Window()->Unlock();
        }
    }
    panel->Show();
}

/* Qt's onExistingFileSelected(): read the image's geometry. With precheck
   (the name was typed), a file that cannot be read says nothing. */
void
HarddiskDialog::ExistingFileSelected(const std::string &name, bool precheck)
{
    uint64 size        = 0;
    uint32 sector_size = 0;
    uint32 s           = 0;
    uint32 h           = 0;
    uint32 c           = 0;
    int    vhd_error   = 0;

    file_ok = false;
    UpdateOk();
    if (name.empty())
        return;

    std::string path = name;
    if (plat_is_block_device(&path[0])) {
        const int64_t dev_size = plat_get_block_device_size(&path[0]);
        if (dev_size <= 0) {
            if (!precheck)
                error_alert("Unable to read device", "Could not determine block device size. Make sure you have permission to access this device.");
            return;
        }
        size = (uint64) dev_size;
        s    = 63;
        h    = 16;
        c    = ((size >> 9) / h) / s;
    } else {
        FILE *fp = plat_fopen(path.c_str(), "rb");
        if (fp == nullptr) {
            if (!precheck)
                error_alert("Unable to read file", read_failed);
            return;
        }

        if (image_is_hdi(path.c_str()) || image_is_hdx(path.c_str(), 1)) {
            uint32 header[4] = { 0, 0, 0, 0 };
            fseek(fp, 0x10, SEEK_SET);
            const size_t got = fread(header, sizeof(uint32), 4, fp);
            fclose(fp);
            if (got != 4) {
                error_alert("Unable to read file", read_failed);
                return;
            }
            sector_size = B_LENDIAN_TO_HOST_INT32(header[0]);
            if (sector_size != 512) {
                error_alert("Unsupported disk image", "HDI or HDX images with a sector size other than 512 are not supported.");
                return;
            }
            s = B_LENDIAN_TO_HOST_INT32(header[1]);
            h = B_LENDIAN_TO_HOST_INT32(header[2]);
            c = B_LENDIAN_TO_HOST_INT32(header[3]);
        } else if (image_is_vhd(path.c_str(), 1)) {
            fclose(fp);
            MVHDMeta *vhd = mvhd_open(&path[0], 0, &vhd_error);
            if (vhd == nullptr) {
                error_alert("Unable to read file", read_failed);
                return;
            } else if (vhd_error == MVHD_ERR_TIMESTAMP) {
                if (!yes_no_alert("Parent and child disk timestamps do not match",
                                  "Parent and child disk timestamps do not match. This could mean that the parent image was "
                                  "modified after the differencing image was created.\n\nIt can also happen if the image files "
                                  "were moved or copied, or by a bug in the program that created this disk.\n\nDo you want to "
                                  "fix the timestamps?")) {
                    mvhd_close(vhd);
                    return;
                }
                if (mvhd_diff_update_par_timestamp(vhd, &vhd_error) != 0) {
                    error_alert("Error", "Could not fix VHD timestamp");
                    mvhd_close(vhd);
                    return;
                }
            }

            const MVHDGeom vhd_geom = mvhd_get_geometry(vhd);
            Geom           geom;
            adjust_vhd_geometry_for_86box(&vhd_geom, &geom);
            c = geom.cyl;
            h = geom.heads;
            s = geom.spt;
            mvhd_close(vhd);
        } else {
            fseeko(fp, 0, SEEK_END);
            const off_t end = ftello(fp);
            fclose(fp);
            size = (end > 0) ? (uint64) end : 0;

            if (((size % 17) == 0) && (size <= 142606336)) {
                s = 17;
                if (size <= 26738688)
                    h = 4;
                else if (((size % 3072) == 0) && (size <= 53477376))
                    h = 6;
                else {
                    uint32 i;
                    for (i = 5; i < 16; i++) {
                        if (((size % (i << 9)) == 0) && (size <= ((uint64) (i * 17) << 19)))
                            break;
                        if (i == 5)
                            i++;
                    }
                    h = i;
                }
            } else {
                s = 63;
                h = 16;
            }
            c = ((size >> 9) / h) / s;
        }
    }

    if ((s > max_sectors) || (h > max_heads) || (c > max_cylinders)) {
        error_alert("Unable to read file", read_failed);
        return;
    }

    cylinders = c;
    heads     = h;
    sectors   = s;
    SetGeometryText();
    RecalcSelection();
    EnableGeometry(true);
    file_ok = true;
    UpdateOk();
}

void
HarddiskDialog::Accept()
{
    if (busy)
        return;

    if (existing) {
        if ((cylinders == 0) || (heads == 0) || (sectors == 0))
            return;
        Done(true);
        return;
    }

    if (format->Value(DEFAULT_DISK_FORMAT) == IMG_FMT_VHD_DIFF) {
        /* The parent first. */
        if (parent_panel == nullptr) {
            BMessenger target(this);
            entry_ref  dir;
            const bool has_dir = (get_ref_for_path(usr_path, &dir) == B_OK);
            parent_panel       = new BFilePanel(B_OPEN_PANEL, &target, has_dir ? &dir : nullptr, B_FILE_NODE, false,
                                                new BMessage(MSG_HD_PARENT_OPEN), nullptr, true, true);
            if (parent_panel->Window()->Lock()) {
                parent_panel->Window()->SetTitle("Select the parent VHD");
                parent_panel->Window()->Unlock();
            }
        }
        parent_panel->Show();
        return;
    }

    CreateNewFile("");
}

/* Qt's onCreateNewFile(), with the writing done by CreateThread(). */
void
HarddiskDialog::CreateNewFile(const std::string &vhd_parent)
{
    std::string name = file_name->Text();
    if (name.empty())
        return;

    /* A block device is used as it is, like an existing disk. */
    if (plat_is_block_device(&name[0])) {
        const int64_t dev_size = plat_get_block_device_size(&name[0]);
        if (dev_size <= 0) {
            error_alert("Unable to read device", "Could not determine block device size. Make sure you have permission to access this device.");
            return;
        }
        const uint64 size = (uint64) dev_size;
        const uint32 c    = ((size >> 9) / 16) / 63;
        if (c > max_cylinders) {
            error_alert("Device too large", "Block device is too large to be used as a hard disk.");
            return;
        }
        cylinders = c;
        heads     = 16;
        sectors   = 63;
        SetGeometryText();

        char text[2048];
        snprintf(text, sizeof(text), "Detected block device: %s\nSize: %llu MB\nGeometry: %u cylinders, %u heads, %u sectors",
                 name.c_str(), (unsigned long long) (size >> 20), (unsigned) cylinders, (unsigned) heads, (unsigned) sectors);
        info_alert("Block device detected", text);
        Done(true);
        return;
    }

    const int img_format = format->Value(DEFAULT_DISK_FORMAT);

    const uint64 size = (uint64) cylinders * heads * sectors * 512;
    if ((img_format != IMG_FMT_VHD_DIFF) && (size == 0))
        return;
    if (size > 0x1FFFFFFE00LL) {
        error_alert("Disk image too large", "Disk images cannot be larger than 127 GB.");
        return;
    }
    if ((img_format == IMG_FMT_HDI) && (size >= 0x100000000LL)) {
        error_alert("Disk image too large", "HDI disk images cannot be larger than 4 GB.");
        return;
    }

    const char *suffix = nullptr;
    switch (img_format) {
        case IMG_FMT_HDI:
            suffix = "hdi";
            break;
        case IMG_FMT_HDX:
            suffix = "hdx";
            break;
        case IMG_FMT_VHD_FIXED:
        case IMG_FMT_VHD_DYNAMIC:
        case IMG_FMT_VHD_DIFF:
            suffix = "vhd";
            break;
        default:
            break;
    }
    if (suffix != nullptr) {
        const size_t dot   = name.rfind('.');
        const size_t slash = name.rfind('/');
        const bool   has   = (dot != std::string::npos) && ((slash == std::string::npos) || (dot > slash)) &&
                             !strcasecmp(name.c_str() + dot + 1, suffix);
        if (!has)
            name += std::string(".") + suffix;
    }
    if (name[0] != '/')
        name = std::string(usr_path) + name;
    file_name->SetText(name.c_str());

    FILE *fp = plat_fopen(name.c_str(), "rb");
    if (fp != nullptr) {
        fclose(fp);
        if (!yes_no_alert("Disk image file already exists", "The selected file will be overwritten. Are you sure you want to use it?"))
            return;
    }

    CreateJob *job  = new CreateJob;
    job->dialog     = this;
    job->file_name  = name;
    job->parent     = vhd_parent;
    job->format     = img_format;
    job->geometry   = { cylinders, heads, sectors };
    job->block_size = block_size->Value(MVHD_BLOCK_LARGE);

    last_progress = -1;
    SetBusy(true);

    const thread_id thread = spawn_thread(CreateThread, "hard disk image", B_NORMAL_PRIORITY, job);
    if ((thread < 0) || (resume_thread(thread) != B_OK)) {
        delete job;
        SetBusy(false);
        error_alert("Unable to write file", write_failed);
    }
}

void
HarddiskDialog::CreationDone(BMessage *message)
{
    SetBusy(false);

    if (!message->GetBool("ok", false)) {
        error_alert("Unable to write file", write_failed);
        return;
    }

    cylinders = message->GetInt32("cylinders", 0);
    heads     = message->GetInt32("heads", 0);
    sectors   = message->GetInt32("sectors", 0);
    if (format->Value(DEFAULT_DISK_FORMAT) != IMG_FMT_VHD_DIFF) {
        SetGeometryText();
        info_alert("Disk image created", "The disk image has been successfully created.\n\nRemember to partition and format the newly-created drive.");
    }

    Done(true);
}

/* Not while an image is being written: its thread uses the dialog. */
bool
HarddiskDialog::QuitRequested()
{
    if (busy)
        return false;
    return ModalDialog::QuitRequested();
}

void
HarddiskDialog::MessageReceived(BMessage *message)
{
    const bool diff = !existing && (format->Value(DEFAULT_DISK_FORMAT) == IMG_FMT_VHD_DIFF);

    switch (message->what) {
        case MSG_HD_FILE_TEXT:
            if (existing)
                file_ok = false;
            UpdateOk();
            break;

        case MSG_HD_FILE_DONE:
            if (existing && !file_ok)
                ExistingFileSelected(file_name->Text(), true);
            break;

        case MSG_HD_BROWSE:
            Browse();
            break;

        case MSG_HD_FILE_OPEN:
            {
                entry_ref ref;
                BPath     path;
                if ((message->FindRef("refs", &ref) == B_OK) && (path.SetTo(&ref) == B_OK)) {
                    file_name->SetText(path.Path());
                    ExistingFileSelected(path.Path(), false);
                }
                break;
            }

        case MSG_HD_FILE_SAVE:
            {
                entry_ref   dir;
                const char *leaf = nullptr;
                BPath       path;
                if ((message->FindRef("directory", &dir) == B_OK) && (message->FindString("name", &leaf) == B_OK) &&
                    (path.SetTo(&dir) == B_OK) && (path.Append(leaf) == B_OK)) {
                    file_name->SetText(path.Path());
                    /* The extension picks the format, as Qt's file filter does. */
                    const char *dot = strrchr(leaf, '.');
                    int         fmt = -1;
                    if (dot != nullptr) {
                        if (!strcasecmp(dot, ".img"))
                            fmt = IMG_FMT_RAW;
                        else if (!strcasecmp(dot, ".hdi"))
                            fmt = IMG_FMT_HDI;
                        else if (!strcasecmp(dot, ".hdx"))
                            fmt = IMG_FMT_HDX;
                        else if (!strcasecmp(dot, ".vhd") && (format->Value(DEFAULT_DISK_FORMAT) < IMG_FMT_VHD_FIXED))
                            fmt = DEFAULT_DISK_FORMAT;
                    }
                    if ((fmt >= 0) && (fmt != format->Value(DEFAULT_DISK_FORMAT))) {
                        format->Select(fmt);
                        FormatChanged();
                    }
                    UpdateOk();
                }
                break;
            }

        case MSG_HD_PARENT_OPEN:
            {
                entry_ref ref;
                BPath     path;
                if ((message->FindRef("refs", &ref) == B_OK) && (path.SetTo(&ref) == B_OK))
                    CreateNewFile(path.Path());
                break;
            }

        /* Typed geometry. Setting a field's text also reports it as
           modified, later: what the fields already hold is ignored. */
        case MSG_HD_CYLINDERS:
        case MSG_HD_HEADS:
        case MSG_HD_SECTORS:
            {
                if (busy || diff)
                    break;
                BTextControl *field = (message->what == MSG_HD_CYLINDERS) ? cylinders_text : (message->what == MSG_HD_HEADS) ? heads_text
                                                                                                                              : sectors_text;
                uint32       *value = (message->what == MSG_HD_CYLINDERS) ? &cylinders : (message->what == MSG_HD_HEADS) ? &heads
                                                                                                                         : &sectors;
                const uint32  typed = (uint32) strtoul(field->Text(), nullptr, 10);
                if (typed == *value)
                    break;
                *value = typed;
                CheckAndAdjust();
                RecalcSize();
                RecalcSelection();
                break;
            }

        case MSG_HD_SIZE:
            {
                if (busy || diff)
                    break;
                const uint32 typed = (uint32) strtoul(size_text->Text(), nullptr, 10);
                if (typed == (uint32) ((((uint64) cylinders * heads * sectors) << 9) >> 20))
                    break;
                /* hdd_image_calc_chs() keeps to what a VHD can hold. The
                   size typed stays as it is. */
                hdd_image_calc_chs(&cylinders, &heads, &sectors, typed);
                cylinders_text->SetText(std::to_string(cylinders).c_str());
                heads_text->SetText(std::to_string(heads).c_str());
                sectors_text->SetText(std::to_string(sectors).c_str());
                RecalcSelection();
                if ((cylinders > max_cylinders) || (heads > max_heads) || (sectors > max_sectors))
                    CheckAndAdjust();
                break;
            }

        case MSG_HD_TYPE:
            {
                const int index = message->GetInt32("value", TYPE_CUSTOM);
                if ((index >= 0) && (index < TYPE_CUSTOM)) {
                    cylinders = hdd_table[index][0];
                    heads     = hdd_table[index][1];
                    sectors   = hdd_table[index][2];
                    SetGeometryText();
                } else if (index == TYPE_CUSTOM_LARGE) {
                    heads   = 16;
                    sectors = 63;
                    SetGeometryText();
                }
                CheckAndAdjust();
                break;
            }

        case MSG_HD_BUS:
            BusChanged();
            break;

        case MSG_HD_FORMAT:
            FormatChanged();
            break;

        case MSG_HD_OK:
            Accept();
            break;

        case MSG_HD_CANCEL:
            if (!busy)
                Done(false);
            break;

        case MSG_HD_PROGRESS:
            {
                const float value = (float) message->GetInt32("percent", 0);
                progress->Update(value - progress->CurrentValue());
                break;
            }

        case MSG_HD_CREATED:
            CreationDone(message);
            break;

        case B_CANCEL: /* a file panel closed */
        case MSG_SETTINGS_IGNORE:
            break;

        default:
            ModalDialog::MessageReceived(message);
            break;
    }
}

/* ----------------------------------------------------- identity dialog */

namespace {

class IdentityDialog : public ModalDialog {
public:
    IdentityDialog(BWindow *parent, const char *vendor, const char *model, const char *version)
        : ModalDialog("[Generic HDD] Device Configuration", parent)
    {
        vendor_text  = new BTextControl("vendor", "Brand:", vendor, nullptr);
        model_text   = new BTextControl("model", "Model:", model, nullptr);
        version_text = new BTextControl("version", "Revision:", version, nullptr);
        vendor_text->TextView()->SetMaxBytes(8);
        model_text->TextView()->SetMaxBytes(40);
        version_text->TextView()->SetMaxBytes(4);
        model_text->SetExplicitMinSize(BSize(280, B_SIZE_UNSET));

        BButton *ok     = new BButton("ok", "OK", new BMessage(MSG_ID_OK));
        BButton *cancel = new BButton("cancel", "Cancel", new BMessage(MSG_ID_CANCEL));

        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_WINDOW_SPACING)
            .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
                .AddTextControl(vendor_text, 0, 0)
                .AddTextControl(model_text, 0, 1)
                .AddTextControl(version_text, 0, 2)
            .End()
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(cancel)
                .Add(ok)
            .End();
        SetDefaultButton(ok);
    }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
            case MSG_ID_OK:
                Done(true);
                break;
            case MSG_ID_CANCEL:
                Done(false);
                break;
            default:
                ModalDialog::MessageReceived(message);
                break;
        }
    }

    BTextControl *vendor_text;
    BTextControl *model_text;
    BTextControl *version_text;
};

}

bool
harddisk_identity_dialog(BWindow *parent, char *vendor, char *model, char *version)
{
    IdentityDialog *dialog   = new IdentityDialog(parent, vendor, model, version);
    const bool      accepted = dialog->Go();

    if (accepted && dialog->Lock()) {
        snprintf(vendor, 9, "%s", dialog->vendor_text->Text());
        snprintf(model, 41, "%s", dialog->model_text->Text());
        snprintf(version, 5, "%s", dialog->version_text->Text());
        dialog->Unlock();
    }
    dialog->Dismiss();
    return accepted;
}

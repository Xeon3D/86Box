/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Storage controllers Settings page
 *          (qt_settingsstoragecontrollers.cpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Box.h>
#include <Button.h>
#include <CheckBox.h>
#include <GridView.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <TabView.h>

#include <cstdio>
#include <string>

#include "haiku_deviceconfig.hpp"
#include "haiku_settings_common.hpp"
#include "haiku_settings_storage.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/hdc.h>
#include <86box/hdc_ide.h>
#include <86box/fdc_ext.h>
#include <86box/cdrom_interface.h>
#include <86box/scsi.h>
#include <86box/scsi_device.h>
#include <86box/cassette.h>
#include <86box/fdd.h>
}

/* The slot is added to the per-slot codes ('sch0' + i ...). */
enum : uint32 {
    MSG_SC_FDC        = 'scfd',
    MSG_SC_FDC_CFG    = 'scfc',
    MSG_SC_CDIF       = 'scci',
    MSG_SC_CDIF_CFG   = 'sccc',
    MSG_SC_HDC        = 'sch0',
    MSG_SC_HDC_CFG    = 'scH0',
    MSG_SC_SCSI       = 'scs0',
    MSG_SC_SCSI_CFG   = 'scS0',
};

/* --- Storage controllers (qt_settingsstoragecontrollers.cpp) ----------- */

class StorageControllersPage;
static StorageControllersPage *storage_page = nullptr;

class StorageControllersPage : public SettingsPage {
public:
    StorageControllersPage()
        : SettingsPage("Storage controllers")
        , fdc(new Choice("fdc", "Floppy disk controller:", MSG_SC_FDC))
        , cdif(new Choice("cdif", "CD-ROM controller:", MSG_SC_CDIF))
        , fdc_button(new BButton("fdc_cfg", "Configure", new BMessage(MSG_SC_FDC_CFG)))
        , cdif_button(new BButton("cdif_cfg", "Configure", new BMessage(MSG_SC_CDIF_CFG)))
        , ide_turbo_box(new BCheckBox("ide_turbo", "Turbo IDE", nullptr))
        , cassette(new BCheckBox("cassette", "Cassette", nullptr))
        , fdc_cfg_changed(false)
        , cdif_cfg_changed(false)
    {
        char name[32];
        char label[32];

        /* General tab: FDC and CD-ROM controller, then the hard disk
           controllers in a group box, then the cassette. */
        BGridView *top = new BGridView(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
        BLayoutBuilder::Grid<>(top)
            .AddMenuField(fdc->field, 0, 0)
            .Add(fdc_button, 2, 0)
            .AddMenuField(cdif->field, 0, 1)
            .Add(cdif_button, 2, 1);
        top->GridLayout()->SetColumnWeight(1, 1.0f);

        BGridView *hd_grid = new BGridView(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
        BLayoutBuilder::Grid<> hd_builder(hd_grid);
        hd_builder.SetInsets(B_USE_DEFAULT_SPACING);
        for (int i = 0; i < HDC_MAX; i++) {
            snprintf(name, sizeof(name), "hdc%i", i);
            snprintf(label, sizeof(label), "Controller %i:", i + 1);
            hdc[i] = new Choice(name, label, MSG_SC_HDC + i);
            snprintf(name, sizeof(name), "hdc%i_cfg", i);
            hdc_button[i]      = new BButton(name, "Configure", new BMessage(MSG_SC_HDC_CFG + i));
            hdc_cfg_changed[i] = false;
            hd_builder.AddMenuField(hdc[i]->field, 0, i).Add(hdc_button[i], 2, i);
        }
        hd_builder.Add(ide_turbo_box, 0, HDC_MAX, 3);
        hd_grid->GridLayout()->SetColumnWeight(1, 1.0f);
        ide_turbo_box->SetToolTip("Removes the IDE transfer mode speed limit. "
                                  "Hard disk and CD-ROM drive timings still apply.");

        BBox *hd_box = new BBox("hd_box");
        hd_box->SetLabel("Hard disk controllers");
        hd_box->AddChild(hd_grid);

        BGroupView *general = new BGroupView("General", B_VERTICAL, B_USE_DEFAULT_SPACING);
        BLayoutBuilder::Group<>(general)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .Add(top)
            .Add(hd_box)
            .Add(cassette)
            .AddGlue();

        /* SCSI controllers tab. */
        BGridView *scsi_view = new BGridView("SCSI controllers", B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
        BLayoutBuilder::Grid<> scsi_builder(scsi_view);
        scsi_builder.SetInsets(B_USE_DEFAULT_SPACING);
        for (int i = 0; i < SCSI_CARD_MAX; i++) {
            snprintf(name, sizeof(name), "scsi%i", i);
            snprintf(label, sizeof(label), "Controller %i:", i + 1);
            scsi[i] = new Choice(name, label, MSG_SC_SCSI + i);
            snprintf(name, sizeof(name), "scsi%i_cfg", i);
            scsi_button[i]      = new BButton(name, "Configure", new BMessage(MSG_SC_SCSI_CFG + i));
            scsi_cfg_changed[i] = false;
            scsi_builder.AddMenuField(scsi[i]->field, 0, i).Add(scsi_button[i], 2, i);
        }
        scsi_builder.AddGlue(0, SCSI_CARD_MAX, 3);
        scsi_view->GridLayout()->SetColumnWeight(1, 1.0f);

        BTabView *tabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);
        tabs->AddTab(general);
        tabs->AddTab(scsi_view);

        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .Add(tabs);

        ide_turbo_box->SetValue((ide_turbo > 0) ? B_CONTROL_ON : B_CONTROL_OFF);

        storage_page = this;
    }

    ~StorageControllersPage() override
    {
        if (storage_page == this)
            storage_page = nullptr;
        delete fdc;
        delete cdif;
        for (int i = 0; i < HDC_MAX; i++)
            delete hdc[i];
        for (int i = 0; i < SCSI_CARD_MAX; i++)
            delete scsi[i];
    }

    /* The current selection of a slot, or the core's value while the
       lists are not filled yet. */
    int CurrentHdc(int i) const { return hdc[i]->Value(hdc_current[i]); }
    int CurrentScsiCard(int i) const { return scsi[i]->Value(scsi_card_current[i]); }

    void MachineChanged(int m) override
    {
        /* Keep the selections where the new machine allows them (Qt's
           fdcCurrent[], hdcCurrent[] ...). */
        const int fdc_cur  = fdc->Value(fdc_current[0]);
        const int cdif_cur = cdif->Value(cdrom_interface_current);
        int       hdc_cur[HDC_MAX];
        int       scsi_cur[SCSI_CARD_MAX];

        for (int i = 0; i < HDC_MAX; i++)
            hdc_cur[i] = CurrentHdc(i);
        for (int i = 0; i < SCSI_CARD_MAX; i++)
            scsi_cur[i] = CurrentScsiCard(i);

        /* Floppy disk controller. */
        fdc->Clear();
        for (int c = 0;; c++) {
            const device_t   *dev  = fdc_card_getdevice(c);
            const std::string name = settings_device_name(dev, fdc_card_get_internal_name(c), 1);
            if (name.empty())
                break;
            if (fdc_card_available(c) && device_is_valid(dev, m))
                fdc->Add(name.c_str(), c);
        }
        fdc->Select(fdc_cur);
        fdc->SetEnabled(fdc->Count() > 0);

        /* CD-ROM interface. */
        cdif->Clear();
        for (int c = 0;; c++) {
            const device_t   *dev  = cdrom_interface_get_device(c);
            const std::string name = settings_device_name(dev, cdrom_interface_get_internal_name(c), 1);
            if (name.empty())
                break;
            if (cdrom_interface_available(c) && device_is_valid(dev, m))
                cdif->Add(name.c_str(), c);
        }
        cdif->Select(cdif_cur);
        cdif->SetEnabled(cdif->Count() > 0);

        /* Hard disk controllers: "Internal device" only in the first slot,
           on machines with an on-board controller. */
        for (int i = 0; i < HDC_MAX; i++)
            hdc[i]->Clear();
        for (int c = 0;; c++) {
            const device_t   *dev  = hdc_get_device(c);
            const std::string name = settings_device_name(dev, hdc_get_internal_name(c), 1);
            if (name.empty())
                break;
            if (!hdc_available(c) || !device_is_valid(dev, m))
                continue;
            for (int i = 0; i < HDC_MAX; i++) {
                if ((c == 1) && ((i > 0) || !machine_has_flags(m, MACHINE_HDC)))
                    continue;
                hdc[i]->Add(name.c_str(), c);
            }
        }
        for (int i = 0; i < HDC_MAX; i++)
            hdc[i]->Select(hdc_cur[i]);

        /* SCSI controllers. */
        for (int i = 0; i < SCSI_CARD_MAX; i++)
            scsi[i]->Clear();
        for (int c = 0;; c++) {
            const device_t   *dev  = scsi_card_getdevice(c);
            const std::string name = settings_device_name(dev, scsi_card_get_internal_name(c), 1);
            if (name.empty())
                break;
            if (!scsi_card_available(c) || !device_is_valid(dev, m))
                continue;
            for (int i = 0; i < SCSI_CARD_MAX; i++)
                scsi[i]->Add(name.c_str(), c);
        }
        for (int i = 0; i < SCSI_CARD_MAX; i++)
            scsi[i]->Select(scsi_cur[i]);

        /* Like Qt, the cassette goes back to the saved value. */
        if (machine_has_bus(m, MACHINE_BUS_CASSETTE)) {
            cassette->SetValue((cassette_enable > 0) ? B_CONTROL_ON : B_CONTROL_OFF);
            cassette->SetEnabled(true);
        } else {
            cassette->SetValue(B_CONTROL_OFF);
            cassette->SetEnabled(false);
        }

        UpdateButtons();
    }

    bool Handle(BMessage *message) override
    {
        const uint32 what = message->what;

        if ((what == MSG_SC_FDC) || (what == MSG_SC_CDIF)
            || ((what >= MSG_SC_HDC) && (what < MSG_SC_HDC + HDC_MAX))
            || ((what >= MSG_SC_SCSI) && (what < MSG_SC_SCSI + SCSI_CARD_MAX))) {
            UpdateButtons();
            return true;
        }
        if (what == MSG_SC_FDC_CFG) {
            fdc_cfg_changed |= haiku_device_configure(Window(), fdc_card_getdevice(fdc->Value(0)));
            return true;
        }
        if (what == MSG_SC_CDIF_CFG) {
            cdif_cfg_changed |= haiku_device_configure(Window(), cdrom_interface_get_device(cdif->Value(0)));
            return true;
        }
        if ((what >= MSG_SC_HDC_CFG) && (what < MSG_SC_HDC_CFG + HDC_MAX)) {
            const int i = what - MSG_SC_HDC_CFG;
            hdc_cfg_changed[i] |= haiku_device_configure(Window(), hdc_get_device(CurrentHdc(i)), i + 1);
            return true;
        }
        if ((what >= MSG_SC_SCSI_CFG) && (what < MSG_SC_SCSI_CFG + SCSI_CARD_MAX)) {
            const int i = what - MSG_SC_SCSI_CFG;
            scsi_cfg_changed[i] |= haiku_device_configure(Window(), scsi_card_getdevice(CurrentScsiCard(i)), i + 1);
            return true;
        }
        return false;
    }

    bool Changed() override
    {
        bool changed = false;

        for (int i = 0; i < HDC_MAX; i++)
            changed |= (hdc_current[i] != CurrentHdc(i)) || hdc_cfg_changed[i];
        for (int i = 0; i < SCSI_CARD_MAX; i++)
            changed |= (scsi_card_current[i] != CurrentScsiCard(i)) || scsi_cfg_changed[i];
        changed |= (fdc_current[0] != fdc->Value(fdc_current[0])) || fdc_cfg_changed;
        changed |= (cdrom_interface_current != cdif->Value(cdrom_interface_current)) || cdif_cfg_changed;
        changed |= (cassette_enable != ((cassette->Value() == B_CONTROL_ON) ? 1 : 0));
        /* A soft change in Qt; the window always hard resets. */
        changed |= (ide_turbo != ((ide_turbo_box->Value() == B_CONTROL_ON) ? 1 : 0));
        return changed;
    }

    void Save() override
    {
        ide_turbo = (ide_turbo_box->Value() == B_CONTROL_ON) ? 1 : 0;
        ide_turbo_update();

        for (int i = 0; i < HDC_MAX; i++)
            hdc_current[i] = CurrentHdc(i);
        for (int i = 0; i < SCSI_CARD_MAX; i++)
            scsi_card_current[i] = CurrentScsiCard(i);
        fdc_current[0]          = fdc->Value(fdc_current[0]);
        cdrom_interface_current = cdif->Value(cdrom_interface_current);
        cassette_enable         = (cassette->Value() == B_CONTROL_ON) ? 1 : 0;
    }

private:
    /* Qt's on_comboBox*_currentIndexChanged(): Configure follows the
       selected device. */
    void UpdateButtons()
    {
        fdc_button->SetEnabled((fdc->Count() > 0) && haiku_device_has_config(fdc_card_getdevice(fdc->Value(0))));
        cdif_button->SetEnabled((cdif->Count() > 0) && haiku_device_has_config(cdrom_interface_get_device(cdif->Value(0))));
        for (int i = 0; i < HDC_MAX; i++)
            hdc_button[i]->SetEnabled((hdc[i]->Count() > 0) && haiku_device_has_config(hdc_get_device(CurrentHdc(i))));
        for (int i = 0; i < SCSI_CARD_MAX; i++)
            scsi_button[i]->SetEnabled((scsi[i]->Count() > 0) && haiku_device_has_config(scsi_card_getdevice(CurrentScsiCard(i))));
    }

    Choice    *fdc;
    Choice    *cdif;
    Choice    *hdc[HDC_MAX];
    Choice    *scsi[SCSI_CARD_MAX];
    BButton   *fdc_button;
    BButton   *cdif_button;
    BButton   *hdc_button[HDC_MAX];
    BButton   *scsi_button[SCSI_CARD_MAX];
    BCheckBox *ide_turbo_box;
    BCheckBox *cassette;
    bool       fdc_cfg_changed;
    bool       cdif_cfg_changed;
    bool       hdc_cfg_changed[HDC_MAX];
    bool       scsi_cfg_changed[SCSI_CARD_MAX];
};

int
settings_current_hdc(int i)
{
    if ((i < 0) || (i >= HDC_MAX))
        return 0;
    return (storage_page != nullptr) ? storage_page->CurrentHdc(i) : hdc_current[i];
}

int
settings_current_scsi_card(int i)
{
    if ((i < 0) || (i >= SCSI_CARD_MAX))
        return 0;
    return (storage_page != nullptr) ? storage_page->CurrentScsiCard(i) : scsi_card_current[i];
}

SettingsPage *
create_storage_controllers_page()
{
    return new StorageControllersPage();
}

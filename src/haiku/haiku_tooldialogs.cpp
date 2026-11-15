/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the CGA composite settings
 *          (qt_cgasettingsdialog.cpp) and MCA devices (qt_mcadevicelist.cpp)
 *          dialogs.
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
#include <Slider.h>
#include <StringView.h>
#include <Window.h>

#include <cstdio>

#include "haiku_dialogs.hpp"
#include "haiku_tooldialogs.hpp"

/* The core headers are not self-contained: keep this order. */
extern "C" {
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/plat.h>
#include <86box/video.h>
#include <86box/mca.h>
#include <86box/runctl.h>
#include <86box/vid_cga_comp.h>
}

/* ------------------------------------------------------------- modality */

/* A window that releases a semaphore when it goes away, so its creator can
   wait for it with haiku_run_modal() (as in haiku_dialogs.cpp). */
class ToolModalWindow : public BWindow {
public:
    ToolModalWindow(BWindow *parent, const char *title)
        : BWindow(BRect(0, 0, 100, 100), title, B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
                  B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS
                      | B_CLOSE_ON_ESCAPE)
        , done(create_sem(0, "86Box modal dialog"))
        , parent(parent)
    {
        if (parent != nullptr)
            AddToSubset(parent);
    }

    ~ToolModalWindow() override { release_sem(done); }

    void Go()
    {
        if (parent != nullptr)
            CenterIn(parent->Frame());
        haiku_run_modal(this, parent, done);
    }

protected:
    sem_id   done;
    BWindow *parent;
};

enum : uint32 {
    MSG_TD_OK     = 'tdok',
    MSG_TD_CANCEL = 'tdcn',
    MSG_TD_RESET  = 'tdrs',
    MSG_TD_SLIDER = 'tdsl',
};

/* --------------------------------------------------- CGA composite settings */

class CGASettingsWindow : public ToolModalWindow {
public:
    explicit CGASettingsWindow(BWindow *parent)
        : ToolModalWindow(parent, "CGA composite settings")
        , cga_hue(vid_cga_comp_hue)
        , cga_saturation(vid_cga_comp_saturation)
        , cga_brightness(vid_cga_comp_brightness)
        , cga_contrast(vid_cga_comp_contrast)
        , cga_sharpness(vid_cga_comp_sharpness)
        , accepted(false)
    {
        /* The ranges of Qt's .ui file. */
        hue        = NewSlider("hue", "Hue", -360, 360, vid_cga_comp_hue);
        saturation = NewSlider("saturation", "Saturation", 0, 360, vid_cga_comp_saturation);
        brightness = NewSlider("brightness", "Brightness", -100, 100, vid_cga_comp_brightness);
        contrast   = NewSlider("contrast", "Contrast", 0, 360, vid_cga_comp_contrast);
        sharpness  = NewSlider("sharpness", "Sharpness", -50, 50, vid_cga_comp_sharpness);

        BButton *ok = new BButton("ok", "OK", new BMessage(MSG_TD_OK));
        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_WINDOW_SPACING)
            .Add(hue)
            .Add(saturation)
            .Add(brightness)
            .Add(contrast)
            .Add(sharpness)
            .AddGroup(B_HORIZONTAL)
                .Add(new BButton("reset", "Reset", new BMessage(MSG_TD_RESET)))
                .AddGlue()
                .Add(new BButton("cancel", "Cancel", new BMessage(MSG_TD_CANCEL)))
                .Add(ok)
            .End();
        SetDefaultButton(ok);
    }

    /* Qt's on_buttonBox_accepted() / on_buttonBox_rejected(); closing the
       window any other way is a Cancel. */
    ~CGASettingsWindow() override
    {
        if (accepted) {
            vid_cga_comp_hue        = hue->Value();
            vid_cga_comp_saturation = saturation->Value();
            vid_cga_comp_brightness = brightness->Value();
            vid_cga_comp_contrast   = contrast->Value();
            vid_cga_comp_sharpness  = sharpness->Value();
        } else {
            vid_cga_comp_hue        = cga_hue;
            vid_cga_comp_saturation = cga_saturation;
            vid_cga_comp_brightness = cga_brightness;
            vid_cga_comp_contrast   = cga_contrast;
            vid_cga_comp_sharpness  = cga_sharpness;
        }
        cga_comp_reload(vid_cga_comp_brightness, vid_cga_comp_saturation, vid_cga_comp_sharpness, vid_cga_comp_hue,
                        vid_cga_comp_contrast);
    }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
            case MSG_TD_SLIDER:
                UpdateDisplay();
                break;
            case MSG_TD_RESET:
                hue->SetValue(0);
                saturation->SetValue(100);
                brightness->SetValue(0);
                contrast->SetValue(100);
                sharpness->SetValue(0);
                UpdateDisplay();
                break;
            case MSG_TD_OK:
                accepted = true;
                Quit();
                break;
            case MSG_TD_CANCEL:
                Quit();
                break;
            default:
                ToolModalWindow::MessageReceived(message);
                break;
        }
    }

private:
    static BSlider *NewSlider(const char *name, const char *label, int32 min, int32 max, int32 value)
    {
        BSlider *slider = new BSlider(name, label, new BMessage(MSG_TD_SLIDER), min, max, B_HORIZONTAL);
        slider->SetModificationMessage(new BMessage(MSG_TD_SLIDER));
        slider->SetValue(value);
        slider->SetExplicitMinSize(BSize(320, B_SIZE_UNSET));
        char min_text[16];
        char max_text[16];
        snprintf(min_text, sizeof(min_text), "%i", static_cast<int>(min));
        snprintf(max_text, sizeof(max_text), "%i", static_cast<int>(max));
        slider->SetLimitLabels(min_text, max_text);
        return slider;
    }

    /* Qt's updateDisplay(): the picture follows the sliders. */
    void UpdateDisplay()
    {
        cga_comp_reload(brightness->Value(), saturation->Value(), sharpness->Value(), hue->Value(),
                        contrast->Value());
    }

    BSlider *hue;
    BSlider *saturation;
    BSlider *brightness;
    BSlider *contrast;
    BSlider *sharpness;
    int      cga_hue;
    int      cga_saturation;
    int      cga_brightness;
    int      cga_contrast;
    int      cga_sharpness;
    bool     accepted;
};

/* Like MainWindow::on_actionCGA_composite_settings_triggered() in Qt: the
   dialog doesn't auto-pause (isNonPause there), so the changes can be seen
   live, and the configuration is saved whatever the outcome. */
void
haiku_cga_settings_dialog(BWindow *parent)
{
    (new CGASettingsWindow(parent))->Go();
    config_save();
}

/* ------------------------------------------------------------ MCA devices */

class MCADevicesWindow : public ToolModalWindow {
public:
    explicit MCADevicesWindow(BWindow *parent)
        : ToolModalWindow(parent, "MCA devices")
    {
        BListView *list = new BListView("devices", B_SINGLE_SELECTION_LIST);
        BScrollView *scroll = new BScrollView("devices_scroll", list, 0, false, true);
        scroll->SetExplicitMinSize(BSize(320, 160));

        startblit();
        if (mca_get_nr_cards() == 0) {
            BStringItem *item = new BStringItem("No MCA devices.");
            item->SetEnabled(false);
            list->AddItem(item);
        } else {
            for (int i = 0; i < mca_get_nr_cards(); i++) {
                const uint32_t device_id = mca_read_index(0x00, i) | (mca_read_index(0x01, i) << 8);
                if (device_id != 0xffff) {
                    char text[64];
                    snprintf(text, sizeof(text), "Slot %i: 0x%04X (@%04X.ADF)", i + 1, static_cast<unsigned>(device_id),
                             static_cast<unsigned>(device_id));
                    list->AddItem(new BStringItem(text));
                }
            }
        }
        endblit();

        BButton *ok = new BButton("ok", "OK", new BMessage(MSG_TD_OK));
        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_WINDOW_SPACING)
            .Add(new BStringView("label", "List of MCA devices:"))
            .Add(scroll, 1)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(ok)
            .End();
        SetDefaultButton(ok);
    }

    void MessageReceived(BMessage *message) override
    {
        if (message->what == MSG_TD_OK)
            Quit();
        else
            ToolModalWindow::MessageReceived(message);
    }
};

/* Like MainWindow::on_actionMCA_devices_triggered() in Qt; a dialog over
   the main window pauses the emulation when "Auto-pause on dialog boxes"
   is set (MainWindow::eventFilter() there). */
void
haiku_mca_devices_dialog(BWindow *parent)
{
    if (do_auto_dialog_pause > 0)
        runctl_pause(RUN_PAUSE_DIALOG);
    (new MCADevicesWindow(parent))->Go();
    runctl_resume(RUN_PAUSE_DIALOG);
}

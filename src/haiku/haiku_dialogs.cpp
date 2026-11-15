/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the main window's small dialogs.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Alert.h>
#include <Button.h>
#include <CheckBox.h>
#include <Entry.h>
#include <LayoutBuilder.h>
#include <Roster.h>
#include <Slider.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

#include "haiku_dialogs.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/plat.h>
#include <86box/sound.h>
#include <86box/version.h>
}

/* ------------------------------------------------------------- modality */

void
haiku_run_modal(BWindow *dialog, BWindow *parent, int done_sem)
{
    dialog->Show();
    while (acquire_sem_etc(done_sem, 1, B_RELATIVE_TIMEOUT, 50000) == B_TIMED_OUT) {
        if (parent != nullptr)
            parent->UpdateIfNeeded();
    }
    delete_sem(done_sem);
}

/* A window that releases a semaphore when it goes away, so its creator can
   wait for it with haiku_run_modal(). */
class ModalWindow : public BWindow {
public:
    ModalWindow(BWindow *parent, const char *title)
        : BWindow(BRect(0, 0, 100, 100), title, B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
                  B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS
                      | B_CLOSE_ON_ESCAPE)
        , done(create_sem(0, "86Box modal dialog"))
        , parent(parent)
    {
        if (parent != nullptr)
            AddToSubset(parent);
    }

    ~ModalWindow() override { release_sem(done); }

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
    MSG_DLG_OK     = 'dgok',
    MSG_DLG_CANCEL = 'dgcn',
    MSG_DLG_SLIDER = 'dgsl',
    MSG_DLG_SITE   = 'dgws',
};

/* ----------------------------------------------------------- sound gain */

class SoundGainWindow : public ModalWindow {
public:
    explicit SoundGainWindow(BWindow *parent)
        : ModalWindow(parent, "Sound gain")
        , original(sound_gain)
        , accepted(false)
    {
        /* Qt's slider goes from 0 to 18 (dB of gain). */
        slider = new BSlider("gain", "Gain", new BMessage(MSG_DLG_SLIDER), 0, 18, B_VERTICAL);
        slider->SetModificationMessage(new BMessage(MSG_DLG_SLIDER));
        slider->SetValue(sound_gain);
        slider->SetHashMarks(B_HASH_MARKS_BOTH);
        slider->SetHashMarkCount(19);
        slider->SetExplicitMinSize(BSize(B_SIZE_UNSET, 220));
        value = new BStringView("value", "");
        UpdateLabel();

        BButton *ok = new BButton("ok", "OK", new BMessage(MSG_DLG_OK));
        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_WINDOW_SPACING)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(slider)
                .AddGlue()
            .End()
            .Add(value)
            .AddGroup(B_HORIZONTAL)
                .Add(new BButton("cancel", "Cancel", new BMessage(MSG_DLG_CANCEL)))
                .Add(ok)
            .End();
        SetDefaultButton(ok);
    }

    ~SoundGainWindow() override
    {
        if (accepted)
            config_save();
        else
            sound_gain = original;
    }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
            case MSG_DLG_SLIDER:
                sound_gain = slider->Value();
                UpdateLabel();
                break;
            case MSG_DLG_OK:
                accepted = true;
                Quit();
                break;
            case MSG_DLG_CANCEL:
                Quit();
                break;
            default:
                ModalWindow::MessageReceived(message);
                break;
        }
    }

private:
    void UpdateLabel()
    {
        char text[32];
        snprintf(text, sizeof(text), "%i dB", static_cast<int>(slider->Value()));
        value->SetText(text);
        value->SetAlignment(B_ALIGN_CENTER);
    }

    BSlider     *slider;
    BStringView *value;
    int          original;
    bool         accepted;
};

void
haiku_sound_gain_dialog(BWindow *parent)
{
    (new SoundGainWindow(parent))->Go();
}

/* --------------------------------------------------- specify dimensions */

class DimensionsWindow : public ModalWindow {
public:
    DimensionsWindow(BWindow *parent, int w, int h, bool lock, bool *accepted, int *out_w, int *out_h, bool *out_lock)
        : ModalWindow(parent, "Specify dimensions")
        , accepted(accepted)
        , out_w(out_w)
        , out_h(out_h)
        , out_lock(out_lock)
    {
        char text[16];
        snprintf(text, sizeof(text), "%i", w);
        width = new BTextControl("width", "Width:", text, nullptr);
        snprintf(text, sizeof(text), "%i", h);
        height = new BTextControl("height", "Height:", text, nullptr);
        for (BTextControl *control : { width, height }) {
            for (int c = 0; c < 256; c++) {
                if ((c < '0') || (c > '9'))
                    control->TextView()->DisallowChar(c);
            }
        }
        lock_box = new BCheckBox("lock", "Lock to this size", nullptr);
        lock_box->SetValue(lock ? B_CONTROL_ON : B_CONTROL_OFF);

        BButton *ok = new BButton("ok", "OK", new BMessage(MSG_DLG_OK));
        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_WINDOW_SPACING)
            .AddGrid()
                .AddTextControl(width, 0, 0)
                .AddTextControl(height, 0, 1)
            .End()
            .Add(lock_box)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(new BButton("cancel", "Cancel", new BMessage(MSG_DLG_CANCEL)))
                .Add(ok)
            .End();
        SetDefaultButton(ok);
    }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
            case MSG_DLG_OK:
                {
                    /* Qt's spin boxes go from 16 to 2048. */
                    int w = atoi(width->Text());
                    int h = atoi(height->Text());
                    *out_w    = (w < 16) ? 16 : ((w > 2048) ? 2048 : w);
                    *out_h    = (h < 16) ? 16 : ((h > 2048) ? 2048 : h);
                    *out_lock = lock_box->Value() == B_CONTROL_ON;
                    *accepted = true;
                    Quit();
                    break;
                }
            case MSG_DLG_CANCEL:
                Quit();
                break;
            default:
                ModalWindow::MessageReceived(message);
                break;
        }
    }

private:
    BTextControl *width;
    BTextControl *height;
    BCheckBox    *lock_box;
    bool         *accepted;
    int          *out_w;
    int          *out_h;
    bool         *out_lock;
};

bool
haiku_specify_dimensions_dialog(BWindow *parent, int *width, int *height, bool *lock)
{
    bool accepted = false;
    (new DimensionsWindow(parent, *width, *height, *lock, &accepted, width, height, lock))->Go();
    return accepted;
}

/* ---------------------------------------------------------------- about */

void
haiku_about_dialog(BWindow *)
{
    char title[64];
    char text[2048];

    snprintf(title, sizeof(title), "About %s", EMU_NAME);
    snprintf(text, sizeof(text),
             "%s v%s [%s, %s]\n\n"
             "An emulator of old computers\n\n"
             "Authors: Miran Gr\xc4\x8d" "a (OBattler), RichardG867, Jasmine Iwanek, TC1995, coldbrewed, "
             "Teemu Korhonen (Manaatti), Joakim L. Gilje, Adrien Moulin (elyosh), Daniel Balsom (gloriouscow), "
             "Cacodemon345, Fred N. van Kempen (waltje), Tiseno100, reenigne, and others.\n\n"
             "With previous core contributions from Sarah Walker, leilei, JohnElliott, greatpsycho, and others.\n\n"
             "Native Haiku user interface.\n\n"
             "Released under the GNU General Public License version 2 or later. See LICENSE for more information.",
             EMU_NAME, EMU_VERSION_FULL, "x86_64",
#ifdef USE_DYNAREC
             "Dynamic Recompiler"
#else
             "Interpreter"
#endif
    );

    BAlert *alert = new BAlert(title, text, EMU_SITE, "OK", nullptr, B_WIDTH_FROM_LABEL, B_INFO_ALERT);
    alert->SetShortcut(1, B_ESCAPE);
    if (alert->Go() == 0)
        haiku_open_url("https://" EMU_SITE);
}

/* -------------------------------------------------------------- desktop */

void
haiku_open_url(const char *url)
{
    char *args[] = { const_cast<char *>(url), nullptr };
    be_roster->Launch("application/x-vnd.Be.URL.https", 1, args);
}

void
haiku_open_folder(const char *path)
{
    mkdir(path, 0755);

    entry_ref ref;
    if (get_ref_for_path(path, &ref) != B_OK)
        return;

    BMessage refs(B_REFS_RECEIVED);
    refs.AddRef("refs", &ref);
    be_roster->Launch("application/x-vnd.Be-TRAK", &refs);
}

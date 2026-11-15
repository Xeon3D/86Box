/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: what the VM manager's window, details
 *          panel (qt_vmmanager_details.cpp, qt_vmmanager_detailsection.cpp)
 *          and dialogs (qt_vmmanager_addmachine.cpp and the small dialogs
 *          of qt_vmmanager_main.cpp) share.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_VMMANAGER_UI_HPP
#define HAIKU_VMMANAGER_UI_HPP

#include <Control.h>
#include <View.h>

#include <string>
#include <vector>

class BBitmap;
class BStringView;
class BTextView;
class BWindow;
class VmmSystem;
class VmmScreenshotView;
class VmmDetailSection;

/* Messages handled by the manager window; buttons and menu items act on
   the selected machine. */
enum : uint32 {
    MSG_VMM_NEW              = 'vmnw',
    MSG_VMM_START_PAUSE      = 'vmsp', /* start, or pause/resume when running */
    MSG_VMM_START            = 'vmst',
    MSG_VMM_PAUSE            = 'vmps',
    MSG_VMM_RESET            = 'vmrs',
    MSG_VMM_FORCE_SHUTDOWN   = 'vmfs',
    MSG_VMM_CAD              = 'vmcd',
    MSG_VMM_SETTINGS         = 'vmse',
    MSG_VMM_RENAME           = 'vmrn',
    MSG_VMM_SET_ICON         = 'vmic',
    MSG_VMM_RESET_ICON       = 'vmri',
    MSG_VMM_ICON_CHOSEN      = 'vmih', /* B_REFS_RECEIVED-like, int32 "vm" */
    MSG_VMM_CLONE            = 'vmcn',
    MSG_VMM_KILL             = 'vmkl',
    MSG_VMM_WIPE_NVRAM       = 'vmnv',
    MSG_VMM_DELETE           = 'vmdl',
    MSG_VMM_OPEN_FOLDER      = 'vmof',
    MSG_VMM_OPEN_PRINTER     = 'vmop',
    MSG_VMM_OPEN_SCREENSHOTS = 'vmos',
    MSG_VMM_SHOW_CONFIG      = 'vmcf',
    MSG_VMM_HIDE_TOOLBAR     = 'vmht',
    MSG_VMM_PREFERENCES      = 'vmpf',
    MSG_VMM_GENERAL_PREFS    = 'vmgp',
    MSG_VMM_DOCS             = 'vmdc',
    MSG_VMM_ABOUT            = 'vmab',
    MSG_VMM_SEARCH           = 'vmsr',
    MSG_VMM_SELECTION        = 'vmsl',
    MSG_VMM_INVOKE           = 'vmiv',
    MSG_VMM_CONTEXT          = 'vmcx', /* BPoint "where" (screen), bool "item" */
    MSG_VMM_LOAD             = 'vmld',
    MSG_VMM_SHOT_NEXT        = 'vmsn',
    MSG_VMM_SHOT_PREVIOUS    = 'vmsv',
    MSG_VMM_NOTES            = 'vmnt', /* the notes lost the focus */
};

/* A flat button with an icon (and the text under it, as in Qt's tool bar),
   like a QToolButton. Shows the text alone when the icon is missing. */
class VmmToolButton : public BControl {
public:
    VmmToolButton(const char *name, const char *icon, const char *label, const char *tip, BMessage *message,
                  bool show_label);

    void SetIcon(const char *icon);
    void SetLabelAndTip(const char *label, const char *tip);

    void  AttachedToWindow() override;
    void  Draw(BRect update) override;
    void  MouseDown(BPoint where) override;
    void  MouseUp(BPoint where) override;
    void  MouseMoved(BPoint where, uint32 transit, const BMessage *drag) override;
    BSize MinSize() override;
    BSize MaxSize() override;
    BSize PreferredSize() override;

private:
    BBitmap    *bitmap;
    std::string text;
    bool        show_label;
    bool        inside;
    bool        pressed;
};

/* The right side of the manager (VMManagerDetails). */
class VmmDetailsView : public BView {
public:
    VmmDetailsView();

    /* No machine (VMManagerDetails::reset()). */
    void Reset();
    /* A machine was selected (updateData()). */
    void SetSystem(VmmSystem *system);
    VmmSystem *System() const { return system; }
    /* The configuration changed (onConfigUpdated()). */
    void UpdateConfig();
    /* The process or window status changed (updateProcessStatus()). */
    void UpdateProcessStatus();
    /* Stores the notes in the machine's settings if they changed. */
    void SaveNotes();
    void NextScreenshot();
    void PreviousScreenshot();

private:
    void UpdateScreenshots();
    void ShowScreenshot();

    VmmSystem                     *system;
    BStringView                   *system_label;
    BStringView                   *status_label;
    std::vector<VmmDetailSection *> sections;
    VmmScreenshotView             *screenshot;
    VmmToolButton                 *previous_button;
    VmmToolButton                 *next_button;
    BTextView                     *notes;
    VmmToolButton                 *start_pause_button;
    VmmToolButton                 *reset_button;
    VmmToolButton                 *stop_button;
    VmmToolButton                 *cad_button;
    VmmToolButton                 *configure_button;
    std::vector<std::string>       screenshots;
    size_t                         screenshot_index;
};

/* ------------------------------------------------------------ dialogs */

/* "Set display name": false on Cancel. */
bool vmm_display_name_dialog(BWindow *parent, std::string &name);

/* "Clone": the new folder name under vmm_path; false on Cancel. */
bool vmm_clone_dialog(BWindow *parent, const VmmSystem *system, std::string &name);

/* The add-machine wizard (VMManagerAddMachine): false on Cancel. */
struct VmmNewMachine {
    std::string name;          /* the folder name */
    std::string display_name;  /* optional */
    std::string configuration; /* existing configuration text, or empty */
};
bool vmm_add_machine_wizard(BWindow *parent, VmmNewMachine &result);

/* The VM manager page of the preferences (PreferencesManager): changes the
   vmm_* globals; true on OK. */
bool vmm_preferences_dialog(BWindow *parent, bool machines_running);

/* Shows a text file read-only (VMManagerMain::showTextFileContents()). */
void vmm_show_text_file(BWindow *parent, const std::string &title, const std::string &path);

/* Runs `work` on a thread while a small window says what is going on,
   keeping `parent` redrawn; returns the result of `work`. */
bool vmm_run_with_progress(BWindow *parent, const char *title, const char *text, bool (*work)(void *), void *data);

/* A machine's icon: the image file in its settings, or nullptr for the
   default icon. Cached; owned by the cache. */
BBitmap *vmm_machine_icon(const std::string &icon);

#endif

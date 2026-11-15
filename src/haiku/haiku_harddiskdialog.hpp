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
 *          dialog of the Hard disks page.
 *
 *          The dialogs are modal to the window that opens them, the way
 *          BAlert::Go() is: Go() shows the dialog and waits for it, keeping
 *          the opener's window drawn, and returns whether it was accepted.
 *          Read the results, then Dismiss() the dialog.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_HARDDISKDIALOG_HPP
#define HAIKU_HARDDISKDIALOG_HPP

#include <OS.h>
#include <Window.h>

#include <string>

class BButton;
class BFilePanel;
class BStatusBar;
class BStringView;
class BTextControl;
class BusTracking;
class Choice;

/* A window run modally by Go(), from the thread of the parent window. */
class ModalDialog : public BWindow {
public:
    ModalDialog(const char *title, BWindow *parent);

    bool Go();
    void Dismiss();

    bool QuitRequested() override;

protected:
    /* Ends Go() with this result; the window stays until Dismiss(). */
    void Done(bool accept);

    BWindow *parent;

private:
    sem_id done_sem;
    bool   accepted;
};

class HarddiskDialog : public ModalDialog {
public:
    HarddiskDialog(BWindow *parent, bool existing, const BusTracking &tracking);
    ~HarddiskDialog() override;

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override;

    uint8       Bus() const;
    uint8       Channel() const;
    std::string FileName() const;
    uint32      Cylinders() const { return cylinders; }
    uint32      Heads() const { return heads; }
    uint32      Sectors() const { return sectors; }
    uint32      Speed() const;

    /* Progress of the image being created (0 .. 100), from its thread. */
    void Progress(int percent);

private:
    void BusChanged();
    void FormatChanged();
    void SetGeometryText();
    void RecalcSize();
    void RecalcSelection();
    void CheckAndAdjust();
    void EnableGeometry(bool enabled);
    void UpdateOk();

    void Browse();
    void ExistingFileSelected(const std::string &name, bool precheck);
    void Accept();
    void CreateNewFile(const std::string &vhd_parent);
    void CreationDone(BMessage *message);
    void SetBusy(bool busy);

    static int32 CreateThread(void *data);

    bool               existing;
    const BusTracking &tracking;

    BTextControl *file_name;
    BButton      *browse;
    BTextControl *cylinders_text;
    BTextControl *heads_text;
    BTextControl *sectors_text;
    BTextControl *size_text;
    Choice       *type;
    Choice       *bus;
    Choice       *channel;
    Choice       *speed;
    Choice       *format;
    Choice       *block_size;
    BStatusBar   *progress;
    BButton      *ok;
    BButton      *cancel;
    BFilePanel   *panel;
    BFilePanel   *parent_panel;

    uint32 cylinders;
    uint32 heads;
    uint32 sectors;
    uint32 max_cylinders;
    uint32 max_heads;
    uint32 max_sectors;
    bool   file_ok;
    bool   busy;
    int    last_progress;
};

/* The vendor, model and revision a generic hard disk reports (Qt's
   "[Generic HDD] Device Configuration"). The buffers hold 9, 41 and 5
   bytes, as in hard_disk_t; they are changed only when accepted. */
bool harddisk_identity_dialog(BWindow *parent, char *vendor, char *model, char *version);

#endif

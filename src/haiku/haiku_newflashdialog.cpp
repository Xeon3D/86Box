/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the New USB flash drive dialog, like the
 *          Qt NewFlashDialog (qt_newflashdialog.cpp). The image is written
 *          by a worker thread so the dialog keeps drawing, where Qt shows
 *          a wait cursor.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Alert.h>
#include <Button.h>
#include <Directory.h>
#include <Entry.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <OS.h>
#include <Path.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "haiku_newflashdialog.hpp"
#include "haiku_newfloppydialog.hpp"
#include "haiku_settings_common.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/mem.h>
#include <86box/plat.h>
#include <86box/usb.h>
#include <86box/version.h>
}

enum : uint32 {
    MSG_NFL_OK     = 'nlok',
    MSG_NFL_CANCEL = 'nlcn',
    MSG_NFL_BROWSE = 'nlbr',
    MSG_NFL_FILE   = 'nlfp', /* from the save panel */
    MSG_NFL_NAME   = 'nlnm', /* the file name was edited */
    MSG_NFL_SIZE   = 'nlsz', /* size or custom size changed */
    MSG_NFL_UNIT   = 'nlun', /* custom size unit changed */
    MSG_NFL_FORMAT = 'nlft',
    MSG_NFL_DONE   = 'nldn', /* from the worker: "ok" */
};

static const char *const DIALOG_TITLE = "New USB flash drive";

/* Size presets in MB; 0 is Custom. */
static const int flash_presets[] = { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 0 };

struct NewFlashState {
    sem_id      done     = -1;
    bool        accepted = false;
    std::string file_name;
};

class NewFlashWindow : public BWindow {
public:
    NewFlashWindow(BWindow *parent, NewFlashState *state);
    ~NewFlashWindow() override;

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override;

private:
    void     Build();
    int32    CustomLow() const;
    int32    CustomHigh() const;
    int32    CustomValue() const;
    void     SetCustomText();
    uint64_t SizeBytes() const;
    void     UpdateSize();
    void     UpdateOk();
    void     Browse();
    void     FileChosen(BMessage *message);
    void     Accept();
    void     Done(bool ok);
    void     SetBusy(bool busy);

    NewFlashState              *state;
    BTextControl               *file;
    BButton                    *browse;
    std::unique_ptr<Choice>     size;
    BTextControl               *custom;
    std::unique_ptr<Choice>     unit;
    std::unique_ptr<Choice>     format;
    BStringView                *busy_label;
    BButton                    *ok;
    BButton                    *cancel;
    ImageRefFilter              filter; /* outlives the panel that uses it */
    std::unique_ptr<BFilePanel> panel;
    std::thread                 worker;
    bool                        creating;
    std::string                 target;
    std::string                 confirmed; /* replacing it was confirmed by the save panel */
};

NewFlashWindow::NewFlashWindow(BWindow *parent, NewFlashState *s)
    : BWindow(BRect(0, 0, 100, 100), DIALOG_TITLE, B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
              B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_V_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS
                  | B_ASYNCHRONOUS_CONTROLS | B_CLOSE_ON_ESCAPE)
    , state(s)
    , creating(false)
{
    if (parent != nullptr)
        AddToSubset(parent);

    Build();

    if (parent != nullptr)
        CenterIn(parent->Frame());
    else
        CenterOnScreen();
}

NewFlashWindow::~NewFlashWindow()
{
    if (worker.joinable())
        worker.join();

    /* The caller waits for this; nothing of the window is used after it. */
    release_sem(state->done);
}

void
NewFlashWindow::Build()
{
    file = new BTextControl("file", "File name:", "", nullptr);
    file->SetModificationMessage(new BMessage(MSG_NFL_NAME));
    file->TextView()->SetExplicitMinSize(BSize(be_plain_font->StringWidth("M") * 28, B_SIZE_UNSET));
    browse = new BButton("browse", "Specify" B_UTF8_ELLIPSIS, new BMessage(MSG_NFL_BROWSE));

    size.reset(new Choice("size", "Size:", MSG_NFL_SIZE));
    for (const int mb : flash_presets) {
        char text[32];
        if (mb == 0)
            snprintf(text, sizeof(text), "Custom" B_UTF8_ELLIPSIS);
        else if (mb >= 1024)
            snprintf(text, sizeof(text), "%i GB", mb / 1024);
        else
            snprintf(text, sizeof(text), "%i MB", mb);
        size->Add(text, mb);
    }
    size->Select(256);

    /* Whole MB or GB, like Qt's spin box: digits only, kept in range. */
    custom = new BTextControl("custom", "Custom size:", "8", nullptr);
    custom->SetModificationMessage(new BMessage(MSG_NFL_SIZE));
    for (uint32 c = 0; c < 256; c++) {
        if ((c < '0') || (c > '9'))
            custom->TextView()->DisallowChar(c);
    }
    custom->TextView()->SetMaxBytes(5);
    custom->TextView()->SetExplicitMinSize(BSize(be_plain_font->StringWidth("0") * 8, B_SIZE_UNSET));

    unit.reset(new Choice("unit", "", MSG_NFL_UNIT));
    unit->Add("MB", 1);
    unit->Add("GB", 1024);
    unit->Select(1);

    format.reset(new Choice("format", "Format:", MSG_NFL_FORMAT));
    format->Add("None (blank)", USB_FLASH_FS_BLANK);
    format->Add("FAT16", USB_FLASH_FS_FAT16);
    format->Add("FAT32", USB_FLASH_FS_FAT32);
    format->Select(USB_FLASH_FS_BLANK);

    busy_label = new BStringView("busy", "Creating image" B_UTF8_ELLIPSIS);

    ok     = new BButton("ok", "OK", new BMessage(MSG_NFL_OK));
    cancel = new BButton("cancel", "Cancel", new BMessage(MSG_NFL_CANCEL));

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_SPACING)
        .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
            .Add(file->CreateLabelLayoutItem(), 0, 0)
            .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING, 1, 0)
                .Add(file->CreateTextViewLayoutItem())
                .Add(browse)
            .End()
            .AddMenuField(size->field, 0, 1)
            .Add(custom->CreateLabelLayoutItem(), 0, 2)
            .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING, 1, 2)
                .Add(custom->CreateTextViewLayoutItem())
                .Add(unit->field->CreateMenuBarLayoutItem())
                .AddGlue()
            .End()
            .AddMenuField(format->field, 0, 3)
        .End()
        .AddGroup(B_HORIZONTAL)
            .Add(busy_label)
            .AddGlue()
            .Add(cancel)
            .Add(ok)
        .End();

    busy_label->Hide();

    SetDefaultButton(ok);
    file->MakeFocus(true);

    UpdateSize();
}

int32
NewFlashWindow::CustomLow() const
{
    return (unit->Value(1) == 1) ? 8 : 1;
}

int32
NewFlashWindow::CustomHigh() const
{
    return (unit->Value(1) == 1) ? 32768 : 32;
}

/* The custom size as the spin box would hold it: clamped to the range. */
int32
NewFlashWindow::CustomValue() const
{
    const long v = strtol(custom->Text(), nullptr, 10);

    if (v < CustomLow())
        return CustomLow();
    if (v > CustomHigh())
        return CustomHigh();
    return (int32) v;
}

void
NewFlashWindow::SetCustomText()
{
    char text[16];
    snprintf(text, sizeof(text), "%" B_PRId32, CustomValue());
    custom->SetText(text);
}

uint64_t
NewFlashWindow::SizeBytes() const
{
    uint64_t mb = (uint64_t) size->Value(0);

    if (mb == 0)
        mb = (uint64_t) CustomValue() * (uint64_t) unit->Value(1);

    return mb << 20;
}

/* The custom size is whole MB or GB, 8 MB to 32 GB. The formats the size
   cannot hold are grayed out, and a format that no longer fits gives way
   to the default for the size (as in Qt, so does "None"). */
void
NewFlashWindow::UpdateSize()
{
    const bool is_custom = (size->Value(0) == 0);

    custom->SetEnabled(is_custom);
    unit->SetEnabled(is_custom);

    const uint64_t bytes = SizeBytes();
    BMenu         *menu  = format->field->Menu();

    for (int32 i = 0; i < menu->CountItems(); i++) {
        BMenuItem *item = menu->ItemAt(i);
        if (item->Message() == nullptr)
            continue;
        const int fs = item->Message()->GetInt32("value", USB_FLASH_FS_BLANK);
        item->SetEnabled((fs == USB_FLASH_FS_BLANK) || usb_flash_fs_ok(bytes, fs));
    }

    const int cur = format->Value(USB_FLASH_FS_BLANK);
    if ((cur == USB_FLASH_FS_BLANK) || !usb_flash_fs_ok(bytes, cur))
        format->Select(usb_flash_fs_default(bytes));

    UpdateOk();
}

void
NewFlashWindow::UpdateOk()
{
    ok->SetEnabled(!creating && (file->Text()[0] != '\0'));
}

void
NewFlashWindow::Browse()
{
    filter.SetPatterns({ "img" });

    if (!panel) {
        BMessenger target(this);
        BMessage   msg(MSG_NFL_FILE);
        panel.reset(new BFilePanel(B_SAVE_PANEL, &target, nullptr, B_FILE_NODE, false, &msg, &filter, true));
        if (panel->Window()->Lock()) {
            panel->Window()->SetTitle(EMU_NAME ": Create" B_UTF8_ELLIPSIS);
            panel->Window()->Unlock();
        }
    }

    BPath current(file->Text());
    BPath dir;
    if ((file->Text()[0] == '/') && (current.InitCheck() == B_OK) && (current.GetParent(&dir) == B_OK)
        && BEntry(dir.Path()).IsDirectory()) {
        panel->SetPanelDirectory(dir.Path());
        panel->SetSaveText(current.Leaf());
    } else {
        if (usr_path[0] != '\0')
            panel->SetPanelDirectory(usr_path);
        panel->SetSaveText(file->Text());
    }

    panel->Refresh();
    panel->Show();
}

void
NewFlashWindow::FileChosen(BMessage *message)
{
    entry_ref   ref;
    const char *name = nullptr;

    if ((message->FindRef("directory", &ref) != B_OK) || (message->FindString("name", &name) != B_OK))
        return;

    BDirectory dir(&ref);
    BPath      path(&dir, name);
    if (path.InitCheck() != B_OK)
        return;

    std::string fn   = path.Path();
    const char *leaf = strrchr(fn.c_str(), '/');
    if (strchr((leaf != nullptr) ? leaf : fn.c_str(), '.') == nullptr)
        fn += ".img";
    else
        confirmed = fn; /* the panel asked before replacing exactly this file */

    file->SetText(fn.c_str());
    UpdateOk();
}

void
NewFlashWindow::SetBusy(bool busy)
{
    creating = busy;
    file->SetEnabled(!busy);
    browse->SetEnabled(!busy);
    size->SetEnabled(!busy);
    format->SetEnabled(!busy);
    cancel->SetEnabled(!busy);
    if (busy) {
        custom->SetEnabled(false);
        unit->SetEnabled(false);
        busy_label->Show();
    } else {
        busy_label->Hide();
        UpdateSize();
    }
    UpdateOk();
}

/* NewFlashDialog::accept(): a typed name without a folder is in the VM's
   folder, where the image is created, so that is also where an existing
   image is looked for. */
void
NewFlashWindow::Accept()
{
    std::string name = file->Text();
    if (name.empty())
        return;
    if (name[0] != '/')
        name = std::string(usr_path) + name;

    if (size->Value(0) == 0)
        SetCustomText(); /* show the size that is used */

    const uint64_t bytes = SizeBytes();
    const int      fs    = format->Value(USB_FLASH_FS_BLANK);

    if (BEntry(name.c_str()).Exists() && (name != confirmed)) {
        const std::string text  = name + " already exists. Replace it?";
        BAlert           *alert = new BAlert(DIALOG_TITLE, text.c_str(), "No", "Yes", nullptr, B_WIDTH_AS_USUAL,
                                             B_WARNING_ALERT);
        alert->SetShortcut(0, B_ESCAPE);
        if (alert->Go() != 1)
            return;
    }

    target = name;
    SetBusy(true);

    BMessenger self(this);
    worker = std::thread([self, name, bytes, fs]() {
        const int result = usb_flash_create(name.c_str(), bytes, fs);

        BMessage done(MSG_NFL_DONE);
        done.AddBool("ok", result != 0);
        self.SendMessage(&done);
    });
}

void
NewFlashWindow::Done(bool result)
{
    if (worker.joinable())
        worker.join();
    SetBusy(false);

    if (!result) {
        const std::string text  = "Could not create " + target + ".";
        BAlert           *alert = new BAlert(DIALOG_TITLE, text.c_str(), "OK", nullptr, nullptr, B_WIDTH_AS_USUAL,
                                             B_STOP_ALERT);
        alert->Go();
        return;
    }

    state->accepted  = true;
    state->file_name = target;
    Quit();
}

bool
NewFlashWindow::QuitRequested()
{
    /* The image being written cannot be stopped. */
    return !creating;
}

void
NewFlashWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_NFL_OK:
            if (!creating)
                Accept();
            break;
        case MSG_NFL_CANCEL:
            if (!creating)
                Quit();
            break;
        case MSG_NFL_BROWSE:
            Browse();
            break;
        case MSG_NFL_FILE:
            FileChosen(message);
            break;
        case MSG_NFL_NAME:
            UpdateOk();
            break;
        case MSG_NFL_SIZE:
            if (!creating)
                UpdateSize();
            break;
        case MSG_NFL_UNIT:
            /* A new unit brings the number into its range, as the spin box
               does; typing is left alone. */
            if (!creating) {
                SetCustomText();
                UpdateSize();
            }
            break;
        case MSG_NFL_DONE:
            Done(message->GetBool("ok", false));
            break;
        case MSG_NFL_FORMAT:
            break;
        default:
            BWindow::MessageReceived(message);
            break;
    }
}

/* ------------------------------------------------------------- entry */

bool
haiku_new_flash_dialog(BWindow *parent, std::string *file_name)
{
    NewFlashState state;
    state.done = create_sem(0, "new flash drive");
    if (state.done < B_OK)
        return false;

    NewFlashWindow *window = new NewFlashWindow(parent, &state);
    window->Show();

    /* Like BAlert::Go(): wait, keeping the parent window redrawn. */
    status_t err;
    if (parent != nullptr) {
        do {
            err = acquire_sem_etc(state.done, 1, B_RELATIVE_TIMEOUT, 50000);
            if ((err == B_TIMED_OUT) || (err == B_INTERRUPTED))
                parent->UpdateIfNeeded();
        } while ((err == B_TIMED_OUT) || (err == B_INTERRUPTED));
    } else {
        do {
            err = acquire_sem(state.done);
        } while (err == B_INTERRUPTED);
    }
    delete_sem(state.done);

    if (!state.accepted)
        return false;

    if (file_name != nullptr)
        *file_name = state.file_name;
    return true;
}

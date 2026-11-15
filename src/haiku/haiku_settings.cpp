/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Settings window.
 *
 *          The window follows the Qt Settings dialog (qt_settings.cpp): a
 *          list of pages on the left, the selected page on the right, OK and
 *          Cancel below. The pages are in haiku_settings_*.cpp. On OK, if
 *          anything changed, the pages write their values to the core and
 *          the main window hard-resets the machine (MSG_SETTINGS_CLOSED), as
 *          Qt does.
 *
 *          The emulation is paused while the window is open, so the core's
 *          tables and globals can be read and written from this thread.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <CardLayout.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <ScrollView.h>

#include "haiku_frontend.hpp"
#include "haiku_settings.hpp"
#include "haiku_settings_common.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/version.h>
}

enum : uint32 {
    MSG_PAGE_SELECTED = 'swpg',
    MSG_OK            = 'swok',
    MSG_CANCEL        = 'swcn',
};

SettingsWindow::SettingsWindow(BWindow *owner)
    : BWindow(BRect(0, 0, 720, 480), "Settings", B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
              B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS | B_CLOSE_ON_ESCAPE)
    , owner(owner)
    , finished(false)
{
    /* Without an owner (settings-only mode, -S) it is an ordinary window
       and quits the application when closed. */
    if (owner != nullptr)
        AddToSubset(owner);
    else
        SetFeel(B_NORMAL_WINDOW_FEEL);
    settings_set_current_machine(machine);

    SettingsPage *const created[] = {
        create_machine_page(this),
        create_display_page(),
        create_input_page(),
        create_sound_page(),
        create_network_page(),
        create_ports_page(),
        create_storage_controllers_page(),
        create_hard_disks_page(),
        create_floppy_cdrom_page(),
        create_other_removable_page(),
        create_other_peripherals_page(),
    };
    /* A page not written yet returns nullptr and is left out. */
    for (SettingsPage *page : created) {
        if (page != nullptr)
            pages.push_back(page);
    }

    page_list = new BListView("pages");
    page_list->SetSelectionMessage(new BMessage(MSG_PAGE_SELECTED));
    BScrollView *list_scroll = new BScrollView("pages_scroll", page_list, 0, false, false);
    list_scroll->SetExplicitMinSize(BSize(170, B_SIZE_UNSET));
    list_scroll->SetExplicitMaxSize(BSize(170, B_SIZE_UNLIMITED));

    cards               = new BCardLayout();
    BView *cards_holder = new BView("cards", 0, cards);
    cards_holder->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
    for (SettingsPage *page : pages) {
        page_list->AddItem(new BStringItem(page->Name()));
        cards->AddView(page);
    }

    BButton *ok     = new BButton("ok", "OK", new BMessage(MSG_OK));
    BButton *cancel = new BButton("cancel", "Cancel", new BMessage(MSG_CANCEL));

    BLayoutBuilder::Group<>(this, B_VERTICAL)
        .SetInsets(B_USE_WINDOW_SPACING)
        .AddGroup(B_HORIZONTAL)
            .Add(list_scroll)
            .Add(cards_holder, 1)
        .End()
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel)
            .Add(ok)
        .End();

    SetDefaultButton(ok);

    /* The Machine page goes first: starting it tells the others which
       machine to offer devices for. */
    for (SettingsPage *page : pages)
        page->Start();
    page_list->Select(0);
    page_list->MakeFocus(true);

    if (owner != nullptr)
        CenterIn(owner->Frame());
    else
        CenterOnScreen();
}

void
SettingsWindow::MachineChanged(int machine_id)
{
    for (SettingsPage *page : pages)
        page->MachineChanged(machine_id);
}

void
SettingsWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_PAGE_SELECTED:
            {
                /* A message with a "page" index (scripting) selects that page. */
                int32 wanted = -1;
                if ((message->FindInt32("page", &wanted) == B_OK) && (wanted >= 0)
                    && (wanted != page_list->CurrentSelection())) {
                    page_list->Select(wanted);
                    break;
                }
                const int32 index = page_list->CurrentSelection();
                if (index >= 0)
                    cards->SetVisibleItem(index);
                break;
            }
        case MSG_OK:
            Accept();
            break;
        case MSG_CANCEL:
            Finish(false);
            break;
        case MSG_SETTINGS_IGNORE:
            break;
        default:
            for (SettingsPage *page : pages) {
                if (page->Handle(message))
                    return;
            }
            BWindow::MessageReceived(message);
            break;
    }
}

/* Like Settings::accept(): nothing changed closes the window; otherwise
   confirm the hard reset, write the pages to the core and let the main
   window reset the machine. */
void
SettingsWindow::Accept()
{
    bool changed = false;
    for (SettingsPage *page : pages)
        changed |= page->Changed();

    if (!changed) {
        Finish(false);
        return;
    }

    /* There is no machine to reset in settings-only mode. */
    if (confirm_save && (owner != nullptr)) {
        BAlert *alert = new BAlert(EMU_NAME, "Do you want to save the settings?\n\n"
                                             "This will hard reset the emulated machine.",
                                   "Cancel", "Save", nullptr, B_WIDTH_AS_USUAL, B_IDEA_ALERT);
        alert->SetShortcut(0, B_ESCAPE);
        if (alert->Go() != 1)
            return;
    }

    for (SettingsPage *page : pages)
        page->Save();

    Finish(true);
}

void
SettingsWindow::Notify(bool apply)
{
    if (finished)
        return;
    finished = true;

    if (owner != nullptr) {
        BMessage msg(MSG_SETTINGS_CLOSED);
        msg.AddBool("apply", apply);
        owner->PostMessage(&msg);
    } else {
        /* Settings-only mode: save and leave, as Qt's main() does. */
        if (apply)
            config_save();
        be_app->PostMessage(B_QUIT_REQUESTED);
    }
}

void
SettingsWindow::Finish(bool apply)
{
    Notify(apply);
    Quit();
}

bool
SettingsWindow::QuitRequested()
{
    /* Closing the window is Cancel. */
    Notify(false);
    return true;
}

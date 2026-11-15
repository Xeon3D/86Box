/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Settings window, like the Qt Settings
 *          dialog (qt_settings.cpp). The pages are in haiku_settings_*.cpp.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_SETTINGS_HPP
#define HAIKU_SETTINGS_HPP

#include <Window.h>

#include <vector>

class BCardLayout;
class BListView;
class PageHeader;
class SettingsPage;

class SettingsWindow : public BWindow {
public:
    /* `owner` is the main window, or nullptr in settings-only mode. */
    explicit SettingsWindow(BWindow *owner);

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override;

    /* Called by the Machine page when the selected machine changes, so the
       device pages offer only what that machine can take. */
    void MachineChanged(int machine_id);

private:
    void Accept();
    void Finish(bool apply);
    void Notify(bool apply);

    BWindow                    *owner;
    BListView                  *page_list;
    BCardLayout                *cards;
    PageHeader                 *header;
    std::vector<SettingsPage *> pages;
    bool                        finished;
};

#endif

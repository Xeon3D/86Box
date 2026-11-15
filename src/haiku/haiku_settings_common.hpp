/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: what the Settings pages share.
 *
 *          Each page is a SettingsPage (one file per page, like the Qt
 *          qt_settings*.cpp files) created by a create_*_page() function.
 *          A page:
 *            - fills its choices from the core in its constructor and in
 *              MachineChanged(), the way its Qt counterpart does;
 *            - Changed() says whether its values differ from the core's;
 *            - Save() writes them to the core's globals (only after OK);
 *            - Handle() gets the window's messages; return true for the
 *              page's own. Every control's message goes to the Settings
 *              window, which offers it to each page in turn, so each page
 *              uses its own message codes (see the prefixes below).
 *
 *          The emulation is paused while the Settings window is open, so
 *          the core's tables and globals may be read and written from the
 *          window's thread.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_SETTINGS_COMMON_HPP
#define HAIKU_SETTINGS_COMMON_HPP

#include <ListItem.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <View.h>

#include <string>
#include <vector>

class BBitmap;
class BListView;
class BTab;
class SettingsWindow;
struct _device_;

/* Message codes: every page's codes start with its own letter pair so they
   never collide (the window passes messages to all pages):
     'sm..' Machine          'sd..' Display        'si..' Input
     'ss..' Sound            'sn..' Network        'sp..' Ports
     'sc..' Storage ctrl.    'sh..' Hard disks     'sf..' Floppy & CD-ROM
     'sr..' Other removable  'so..' Other periph.  'sw..' the window   */
enum : uint32 {
    MSG_SETTINGS_IGNORE = 'swig', /* a control that needs no reaction */
};

/* A labelled pop-up choice with an int32 value per item, like a Qt combo box
   with item data. Its messages carry "value" and go to the window. */
class Choice {
public:
    Choice(const char *name, const char *label, uint32 what);

    void  Clear();
    void  Add(const char *text, int32 value);
    void  AddSeparator();
    void  Select(int32 value);   /* the item with this value, or the first */
    int32 Value(int32 fallback = -1) const;
    bool  Has(int32 value) const;
    int32 Count() const;
    void  SetEnabled(bool enabled);

    BMenuField *field;

private:
    BPopUpMenu *menu;
    uint32      what;
};

/* A row of a drive list shown like Qt's QTreeView: an icon (Qt's
   DecorationRole) in the first column, then one text per column. The
   columns take the given fractions of the width; settings_column_list()
   puts the matching header above the list. */
class SettingsColumnItem : public BListItem {
public:
    explicit SettingsColumnItem(const std::vector<float> &fractions);

    void SetIcon(BBitmap *icon) { this->icon = icon; }
    void SetText(size_t column, const std::string &text);

    void DrawItem(BView *owner, BRect frame, bool complete) override;
    void Update(BView *owner, const BFont *font) override;

private:
    std::vector<float>       fractions;
    std::vector<std::string> texts;
    BBitmap                 *icon;
};

/* The column titles and the list in its scroll view, as one view. */
BView *settings_column_list(const char *name, BListView *list, const std::vector<const char *> &titles,
                            const std::vector<float> &fractions);

/* A list item with an icon before its text (the Settings page list). */
class SettingsIconItem : public BListItem {
public:
    SettingsIconItem(const char *text, BBitmap *icon);

    const char *Text() const { return text.c_str(); }
    BBitmap    *Icon() const { return icon; }

    void DrawItem(BView *owner, BRect frame, bool complete) override;
    void Update(BView *owner, const BFont *font) override;

private:
    std::string text;
    BBitmap    *icon;
};

/* A tab with an icon before its label, like Qt's tab icons. */
/* The label is the view's name unless one is given. */
BTab *settings_icon_tab(BView *view, BBitmap *icon, const char *label = nullptr);

/* The name the Qt UI shows for a device (DeviceConfig::DeviceName): "None"
   and "Internal device" for those internal names, "[bus] name" for bus 1,
   otherwise the name without " (On-Board)". Empty when there is no device,
   which ends a device list. */
std::string settings_device_name(const struct _device_ *dev, const char *internal_name, int bus);

class SettingsPage : public BView {
public:
    explicit SettingsPage(const char *name);

    virtual bool Changed() = 0;
    virtual void Save()    = 0;
    virtual void MachineChanged(int /* machine_id */) { }
    /* Called once all pages exist, before the window is shown. */
    virtual void Start() { }
    virtual bool Handle(BMessage *) { return false; }

    /* The machine selected on the Machine page (not yet saved). */
    static int CurrentMachine();
};

/* Used by the Machine page to publish its selection. */
void settings_set_current_machine(int m);

/* Page factories, in the order of the page list (as in Qt). */
SettingsPage *create_machine_page(SettingsWindow *window);
SettingsPage *create_display_page();
SettingsPage *create_input_page();
SettingsPage *create_sound_page();
SettingsPage *create_network_page();
SettingsPage *create_ports_page();
SettingsPage *create_storage_controllers_page();
SettingsPage *create_hard_disks_page();
SettingsPage *create_floppy_cdrom_page();
SettingsPage *create_other_removable_page();
SettingsPage *create_other_peripherals_page();

#endif

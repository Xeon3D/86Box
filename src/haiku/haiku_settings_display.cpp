/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Display Settings page (qt_settingsdisplay.cpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Alert.h>
#include <Box.h>
#include <Button.h>
#include <CheckBox.h>
#include <Directory.h>
#include <Entry.h>
#include <FilePanel.h>
#include <GroupView.h>
#include <GridView.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <Path.h>
#include <RadioButton.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "haiku_deviceconfig.hpp"
#include "haiku_settings.hpp"
#include "haiku_icons.hpp"
#include "haiku_settings_common.hpp"
#include "haiku_settings_display.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/video.h>
#include <86box/vid_ega.h>
#include <86box/vid_8514a_device.h>
#include <86box/vid_xga_device.h>
#include <86box/vid_ps55da2.h>
#include <86box/vid_ddc.h>
#include <86box/version.h>
}

enum : uint32 {
    MSG_VIDEO           = 'sdvi',
    MSG_VIDEO_CFG       = 'sdvc',
    MSG_SECONDARY       = 'sd2v',
    MSG_SECONDARY_CFG   = 'sd2c',
    MSG_VOODOO          = 'sdvo',
    MSG_VOODOO_CFG      = 'sdvC',
    MSG_8514            = 'sd85',
    MSG_8514_CFG        = 'sd8C',
    MSG_XGA             = 'sdxg',
    MSG_XGA_CFG         = 'sdxC',
    MSG_DA2             = 'sdda',
    MSG_DA2_CFG         = 'sddC',
    MSG_POWERVR         = 'sdpv',
    MSG_POWERVR_CFG     = 'sdpC',
    MSG_EDID_MODE       = 'sdem',
    MSG_EDID_PATH       = 'sdep',
    MSG_EDID_BROWSE     = 'sdeb',
    MSG_EDID_CHOSEN     = 'sdef',
    MSG_EDID_EXPORT     = 'sdex',
    MSG_EDID_EXPORT_TO  = 'sdeX',
};

/* Qt matches the video card's name against 3dfx|voodoo|banshee|raven. */
static bool
is_voodoo_name(const std::string &name)
{
    std::string lower = name;
    for (char &c : lower)
        c = (char) tolower((unsigned char) c);
    for (const char *word : { "3dfx", "voodoo", "banshee", "raven" }) {
        if (lower.find(word) != std::string::npos)
            return true;
    }
    return false;
}

/* The path a file panel's message names: "refs" from an open panel,
   "directory" and "name" from a save panel. Empty if none. */
static std::string
panel_path(BMessage *message)
{
    entry_ref   ref;
    const char *name = nullptr;
    BPath       path;

    if (message->FindRef("refs", &ref) == B_OK)
        path.SetTo(&ref);
    else if ((message->FindRef("directory", &ref) == B_OK) && (message->FindString("name", &name) == B_OK)) {
        BDirectory dir(&ref);
        path.SetTo(&dir, name);
    } else
        return "";

    return (path.InitCheck() == B_OK) ? path.Path() : "";
}

static void
lightpen_check_name(int card, bool &usable)
{
    const char *name = video_get_internal_name(card);
    if ((name != nullptr) && (!strcmp(name, "cga") || !strcmp(name, "plantronics")))
        usable = true;
}

/* --- Display (qt_settingsdisplay.cpp) --------------------------------- */

class DisplayPage;
static DisplayPage *display_page = nullptr;

class DisplayPage : public SettingsPage {
public:
    DisplayPage()
        : SettingsPage("Display")
        , video(new Choice("video", "Video:", MSG_VIDEO))
        , video_cfg(new BButton("video_cfg", "Configure", new BMessage(MSG_VIDEO_CFG)))
        , secondary(new Choice("secondary", "Video #2:", MSG_SECONDARY))
        , secondary_cfg(new BButton("secondary_cfg", "Configure", new BMessage(MSG_SECONDARY_CFG)))
        , voodoo(new BCheckBox("voodoo", "Voodoo 1 or 2 Graphics", new BMessage(MSG_VOODOO)))
        , voodoo_cfg(new BButton("voodoo_cfg", "Configure", new BMessage(MSG_VOODOO_CFG)))
        , ibm8514(new BCheckBox("8514", "IBM 8514/A Graphics", new BMessage(MSG_8514)))
        , ibm8514_cfg(new BButton("8514_cfg", "Configure", new BMessage(MSG_8514_CFG)))
        , xga(new BCheckBox("xga", "XGA Graphics", new BMessage(MSG_XGA)))
        , xga_cfg(new BButton("xga_cfg", "Configure", new BMessage(MSG_XGA_CFG)))
        , da2(new BCheckBox("da2", "IBM PS/55 Display Adapter Graphics", new BMessage(MSG_DA2)))
        , da2_cfg(new BButton("da2_cfg", "Configure", new BMessage(MSG_DA2_CFG)))
        , powervr(new BCheckBox("powervr", "PowerVR PCX1/PCX2 Graphics", new BMessage(MSG_POWERVR)))
        , powervr_cfg(new BButton("powervr_cfg", "Configure", new BMessage(MSG_POWERVR_CFG)))
        , screen_type(new Choice("screen_type", "VGA screen type:", MSG_SETTINGS_IGNORE))
        , conversion(new Choice("conversion", "Grayscale conversion type:", MSG_SETTINGS_IGNORE))
        , edid_default(new BRadioButton("edid_default", "Default", new BMessage(MSG_EDID_MODE)))
        , edid_export(new BButton("edid_export", "Export" B_UTF8_ELLIPSIS, new BMessage(MSG_EDID_EXPORT)))
        , edid_custom(new BRadioButton("edid_custom", "Custom" B_UTF8_ELLIPSIS, new BMessage(MSG_EDID_MODE)))
        , edid_path(new BTextControl("edid_path", nullptr, "", new BMessage(MSG_EDID_PATH)))
        , edid_browse(new BButton("edid_browse", "Browse" B_UTF8_ELLIPSIS, new BMessage(MSG_EDID_BROWSE)))
        , overscan(new BCheckBox("overscan", "Overscan", new BMessage(MSG_SETTINGS_IGNORE)))
        , inverted(new BCheckBox("inverted", "Inverted VGA monitor", new BMessage(MSG_SETTINGS_IGNORE)))
        , contrast(new BCheckBox("contrast", "Change contrast for monochrome display",
                                 new BMessage(MSG_SETTINGS_IGNORE)))
    {
        for (int i = 0; i < GFXCARD_MAX; i++)
            gfxcard_cfg_changed[i] = false;
        for (int i = 0; i < 2; i++)
            video_card[i] = gfxcard[i];

        BuildLayout();

        screen_type->Add("RGB Color", 0);
        screen_type->Add("RGB Grayscale", 1);
        screen_type->Add("Amber monitor", 2);
        screen_type->Add("Green monitor", 3);
        screen_type->Add("White monitor", 4);
        screen_type->Select(video_grayscale);

        conversion->Add("BT601 (NTSC/PAL)", 0);
        conversion->Add("BT709 (HDTV)", 1);
        conversion->Add("Average", 2);
        conversion->Select(video_graytype);

        overscan->SetValue(enable_overscan ? B_CONTROL_ON : B_CONTROL_OFF);
        contrast->SetValue(vid_cga_contrast ? B_CONTROL_ON : B_CONTROL_OFF);
        inverted->SetValue(invert_display ? B_CONTROL_ON : B_CONTROL_OFF);

        /* Qt sets these again on every machine change; here they keep what
           the user chose. */
        edid_default->SetValue((monitor_edid == 0) ? B_CONTROL_ON : B_CONTROL_OFF);
        edid_custom->SetValue((monitor_edid == 1) ? B_CONTROL_ON : B_CONTROL_OFF);
        edid_path->SetText(monitor_edid_path);
        previous_edid_path = monitor_edid_path;
        UpdateEdidControls();

        display_page = this;
    }

    ~DisplayPage() override
    {
        if (display_page == this)
            display_page = nullptr;
        delete video;
        delete secondary;
        delete screen_type;
        delete conversion;
    }

    void MachineChanged(int m) override
    {
        machine_id         = m;
        const int current  = video_card[0];

        filling = true;
        video->Clear();
        for (int c = 0;; c++) {
            const device_t   *dev  = video_card_getdevice(c);
            const std::string name = settings_device_name(dev, video_get_internal_name(c), 1);
            if (name.empty())
                break;

            /* The external display entry belongs to the secondary list. */
            if (c == VID_EXTERNAL)
                continue;
            /* "Internal" only on machines with on-board video. */
            if ((c == VID_INTERNAL) && !machine_has_flags(m, MACHINE_VIDEO))
                continue;
            if (!video_card_available(c) || !device_is_valid(dev, m))
                continue;

            std::string     label   = name;
            const device_t *onboard = machine_get_vid_device(m);
            if ((c == VID_INTERNAL) && (onboard != nullptr) && device_available(onboard))
                label += " (" + settings_device_name(onboard, onboard->internal_name, 0) + ")";
            video->Add(label.c_str(), c);
        }

        if (machine_has_flags(m, MACHINE_VIDEO_ONLY)) {
            secondary_allowed = machine_has_flags(m, MACHINE_VIDEO_EXT);
            video->Select(VID_INTERNAL);
            video->SetEnabled(false);
        } else {
            secondary_allowed = true;
            video->Select(current);
            video->SetEnabled(true);
        }
        secondary->SetEnabled(secondary_allowed);
        secondary_cfg->SetEnabled(secondary_allowed);
        filling = false;

        VideoChanged();

        for (int i = 1; i < GFXCARD_MAX; i++) {
            if (gfxcard[i] == 0)
                secondary_cfg->SetEnabled(false);
        }
    }

    bool Handle(BMessage *message) override
    {
        switch (message->what) {
            case MSG_VIDEO:
                VideoChanged();
                return true;
            case MSG_VIDEO_CFG:
                ConfigurePrimary();
                return true;
            case MSG_SECONDARY:
                SecondaryChanged();
                return true;
            case MSG_SECONDARY_CFG:
                {
                    const device_t *dev = video_card_getdevice(secondary->Value(0));
                    if (dev != nullptr)
                        gfxcard_cfg_changed[1] |= haiku_device_configure(Window(), dev, 2);
                    return true;
                }
            case MSG_VOODOO:
                voodoo_cfg->SetEnabled(voodoo->Value() == B_CONTROL_ON);
                return true;
            case MSG_VOODOO_CFG:
                voodoo_cfg_changed |= haiku_device_configure(Window(), &voodoo_device, 0);
                return true;
            case MSG_8514:
                ibm8514_cfg->SetEnabled(ibm8514->Value() == B_CONTROL_ON);
                return true;
            case MSG_8514_CFG:
                if (machine_has_bus(machine_id, MACHINE_BUS_MCA) > 0)
                    ibm8514_cfg_changed |= haiku_device_configure(Window(), &ibm8514_mca_device, 0);
                else
                    ibm8514_cfg_changed |= haiku_device_configure(Window(), &gen8514_isa_device, 0);
                return true;
            case MSG_XGA:
                xga_cfg->SetEnabled(xga->Value() == B_CONTROL_ON);
                return true;
            case MSG_XGA_CFG:
                if (machine_has_bus(machine_id, MACHINE_BUS_MCA) > 0)
                    xga_cfg_changed |= haiku_device_configure(Window(), &xga_device, 0);
                return true;
            case MSG_DA2:
                da2_cfg->SetEnabled(da2->Value() == B_CONTROL_ON);
                return true;
            case MSG_DA2_CFG:
                ps55da2_cfg_changed |= haiku_device_configure(Window(), &ps55da2_device, 0);
                return true;
            case MSG_POWERVR:
                powervr_cfg->SetEnabled(powervr->Value() == B_CONTROL_ON);
                return true;
            case MSG_POWERVR_CFG:
                powervr_cfg_changed |= haiku_device_configure(Window(), &powervr_pcx_device, 0);
                return true;
            case MSG_EDID_MODE:
                UpdateEdidControls();
                return true;
            case MSG_EDID_PATH:
                EdidSelected(edid_path->Text());
                return true;
            case MSG_EDID_BROWSE:
                BrowseEdid();
                return true;
            case MSG_EDID_CHOSEN:
                {
                    const std::string path = panel_path(message);
                    if (!path.empty()) {
                        edid_path->SetText(path.c_str());
                        EdidSelected(path.c_str());
                    }
                    return true;
                }
            case MSG_EDID_EXPORT:
                ExportEdid();
                return true;
            case MSG_EDID_EXPORT_TO:
                WriteDefaultEdid(panel_path(message));
                return true;
            default:
                return false;
        }
    }

    bool Changed() override
    {
        bool changed = false;

        changed |= (gfxcard[0] != video->Value(gfxcard[0]));
        for (int i = 1; i < GFXCARD_MAX; i++)
            changed |= (gfxcard[1] != secondary->Value(0));

        changed |= (voodoo_enabled != Checked(voodoo));
        changed |= (powervr_enabled != Checked(powervr));
        changed |= (ibm8514_standalone_enabled != Checked(ibm8514));
        changed |= (xga_standalone_enabled != Checked(xga));
        changed |= (da2_standalone_enabled != Checked(da2));
        changed |= (monitor_edid != Checked(edid_custom));
        changed |= (strcmp(monitor_edid_path, edid_path->Text()) != 0);

        for (int i = 0; i < GFXCARD_MAX; i++)
            changed |= gfxcard_cfg_changed[i];
        changed |= voodoo_cfg_changed;
        changed |= powervr_cfg_changed;
        changed |= ibm8514_cfg_changed;
        changed |= xga_cfg_changed;
        changed |= ps55da2_cfg_changed;

        /* Qt counts these as changes that need no hard reset. */
        changed |= (video_grayscale != screen_type->Value(0));
        changed |= (video_graytype != conversion->Value(0));
        changed |= (enable_overscan != Checked(overscan));
        changed |= (vid_cga_contrast != Checked(contrast));
        changed |= (invert_display != Checked(inverted));

        return changed;
    }

    void Save() override
    {
        video_grayscale = screen_type->Value(0);
        video_graytype  = conversion->Value(0);

        update_overscan = 1;

        enable_overscan  = Checked(overscan);
        vid_cga_contrast = Checked(contrast);
        invert_display   = Checked(inverted);

        gfxcard[0] = video->Value(gfxcard[0]);
        for (int i = 1; i < GFXCARD_MAX; i++)
            gfxcard[i] = secondary->Value(0);

        voodoo_enabled             = Checked(voodoo);
        powervr_enabled            = Checked(powervr);
        ibm8514_standalone_enabled = Checked(ibm8514);
        xga_standalone_enabled     = Checked(xga);
        da2_standalone_enabled     = Checked(da2);
        monitor_edid               = Checked(edid_custom);

        strncpy(monitor_edid_path, edid_path->Text(), sizeof(monitor_edid_path) - 1);
        monitor_edid_path[sizeof(monitor_edid_path) - 1] = '\0';

        for (int i = 0; i < MONITORS_NUM; i++)
            cgapal_rebuild_monitor(i);
    }

    /* Qt: isLightPenUsable(). */
    bool LightPenUsable() const
    {
        if (video->Count() == 0)
            return false;

        bool usable = false;
        lightpen_check_name(video->Value(0), usable);
        lightpen_check_name(secondary->Value(0), usable);
        return usable;
    }

private:
    static int Checked(const BControl *control) { return (control->Value() == B_CONTROL_ON) ? 1 : 0; }

    void BuildLayout()
    {
        BTabView *tabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);

        BGroupView *general = new BGroupView("General", B_VERTICAL);
        BLayoutBuilder::Group<>(general, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
                .AddMenuField(video->field, 0, 0)
                .Add(video_cfg, 2, 0)
                .AddMenuField(secondary->field, 0, 1)
                .Add(secondary_cfg, 2, 1)
                .Add(voodoo, 0, 2, 2)
                .Add(voodoo_cfg, 2, 2)
                .Add(ibm8514, 0, 3, 2)
                .Add(ibm8514_cfg, 2, 3)
                .Add(xga, 0, 4, 2)
                .Add(xga_cfg, 2, 4)
                .Add(da2, 0, 5, 2)
                .Add(da2_cfg, 2, 5)
                .Add(powervr, 0, 6, 2)
                .Add(powervr_cfg, 2, 6)
                .SetColumnWeight(1, 1.0f)
            .End()
            .AddGlue();
        tabs->AddTab(general, settings_icon_tab(general, haiku_icon("general_display")));

        /* The two radio buttons share the box as their parent, which makes
           them exclude each other. */
        BBox *edid_box = new BBox("edid_box");
        edid_box->SetLabel("Monitor EDID");
        BGridView *edid_box_view = new BGridView(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
        BLayoutBuilder::Grid<>(edid_box_view)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .Add(edid_default, 0, 0)
            .Add(edid_export, 2, 0)
            .Add(edid_custom, 0, 1)
            .Add(edid_path, 1, 1)
            .Add(edid_browse, 2, 1)
            .SetColumnWeight(1, 1.0f);
        /* A child view, so the box keeps room for its label. */
        edid_box->AddChild(edid_box_view);
        edid_path->SetExplicitMinSize(BSize(be_plain_font->StringWidth("M") * 20, B_SIZE_UNSET));

        BGroupView *monitor = new BGroupView("Monitor", B_VERTICAL);
        BLayoutBuilder::Group<>(monitor, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
                .AddMenuField(screen_type->field, 0, 0)
                .AddMenuField(conversion->field, 0, 1)
                .SetColumnWeight(1, 1.0f)
            .End()
            .Add(edid_box)
            .Add(overscan)
            .Add(inverted)
            .Add(contrast)
            .AddGlue();
        tabs->AddTab(monitor, settings_icon_tab(monitor, haiku_icon("monitor")));

        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .Add(tabs);
    }

    /* The text of the selected primary card, as the list shows it. */
    std::string PrimaryLabel() const
    {
        BMenuItem *item = video->field->Menu()->FindMarked();
        return (item != nullptr) ? item->Label() : "";
    }

    /* Qt: on_comboBoxVideo_currentIndexChanged(). */
    void VideoChanged()
    {
        if (filling || (video->Count() == 0))
            return;

        const int m           = machine_id;
        const int current_2nd = video_card[1];
        video_card[0]         = video->Value(0);

        if (video_card[0] == VID_INTERNAL) {
            const device_t *onboard = machine_get_vid_device(m);
            video_cfg->SetEnabled(machine_has_flags(m, MACHINE_VIDEO) && (onboard != nullptr)
                                  && haiku_device_has_config(onboard) && device_available(onboard));
        } else
            video_cfg->SetEnabled(video_card_has_config(video_card[0]) > 0);

        const bool pci = machine_has_bus(m, MACHINE_BUS_PCI) > 0;
        powervr->SetEnabled(pci);
        powervr->SetValue((powervr_enabled && pci) ? B_CONTROL_ON : B_CONTROL_OFF);
        powervr_cfg->SetEnabled(pci && (powervr->Value() == B_CONTROL_ON));

        const bool isa16 = machine_has_bus(m, MACHINE_BUS_ISA16) > 0;
        const bool mca   = machine_has_bus(m, MACHINE_BUS_MCA) > 0;
        const int  flags = video_get_primary_flags(m, video_card[0]);

        const bool supports_8514 = (isa16 || mca) && (flags != VIDEO_FLAG_TYPE_8514);
        const bool supports_xga  = mca && device_available(&xga_device) && (flags != VIDEO_FLAG_TYPE_XGA);
        const bool supports_da2  = mca && device_available(&ps55da2_device) && (flags != VIDEO_FLAG_TYPE_DA2);

        ibm8514->SetEnabled(supports_8514);
        ibm8514->SetValue((ibm8514_standalone_enabled && supports_8514) ? B_CONTROL_ON : B_CONTROL_OFF);
        ibm8514_cfg->SetEnabled(supports_8514 && (ibm8514->Value() == B_CONTROL_ON));

        xga->SetEnabled(supports_xga);
        xga->SetValue((xga_standalone_enabled && supports_xga) ? B_CONTROL_ON : B_CONTROL_OFF);
        da2->SetEnabled(supports_da2);
        da2->SetValue((da2_standalone_enabled && supports_da2) ? B_CONTROL_ON : B_CONTROL_OFF);
        xga_cfg->SetEnabled(supports_xga && (xga->Value() == B_CONTROL_ON));
        da2_cfg->SetEnabled(supports_da2 && (da2->Value() == B_CONTROL_ON));

        /* The secondary list: None, the machine's external display when
           its fixed video can drive one, then the cards that can drive a
           second monitor next to the primary one. */
        secondary->Clear();
        secondary->Add("None", 0);
        if (machine_has_flags(m, MACHINE_VIDEO_EXT))
            secondary->Add("External", VID_EXTERNAL);

        const device_t *primary_dev    = video_card_getdevice(video_card[0]);
        const bool      primary_is_agp = (primary_dev != nullptr) && (primary_dev->flags & DEVICE_AGP);
        for (int c = 0;; c++) {
            const device_t   *dev  = video_card_getdevice(c);
            const std::string name = settings_device_name(dev, video_get_internal_name(c), 1);
            if (name.empty())
                break;

            if ((c == VID_NONE) || (c == VID_INTERNAL) || (c == VID_EXTERNAL))
                continue;

            const bool secondary_is_agp = (dev != nullptr) && (dev->flags & DEVICE_AGP);
            if (video_card_available(c) && device_is_valid(dev, m) && video_card_is_secondary(m, video_card[0], c)
                && !(primary_is_agp && secondary_is_agp))
                secondary->Add(name.c_str(), c);
        }
        secondary->Select(current_2nd);
        secondary->SetEnabled(secondary_allowed);
        SecondaryChanged();

        if ((video_card[1] == 0)
            || (machine_has_flags(m, MACHINE_VIDEO_ONLY) && !machine_has_flags(m, MACHINE_VIDEO_EXT))) {
            secondary->Select(0);
            secondary->SetEnabled(secondary_allowed);
            SecondaryChanged();
            secondary_cfg->SetEnabled(false);
        }

        /* A Voodoo as the primary card leaves no room for the add-on one,
           unless the saved card is a Voodoo too (set by hand). */
        if (is_voodoo_name(PrimaryLabel())) {
            const std::string saved = settings_device_name(video_card_getdevice(gfxcard[0]),
                                                           video_get_internal_name(gfxcard[0]), 1);
            if ((voodoo->Value() == B_CONTROL_ON) && !is_voodoo_name(saved))
                voodoo->SetValue(B_CONTROL_OFF);
            voodoo->SetEnabled(false);
        } else {
            voodoo->SetEnabled(pci);
            if (pci)
                voodoo->SetValue(voodoo_enabled ? B_CONTROL_ON : B_CONTROL_OFF);
        }
        voodoo_cfg->SetEnabled(pci && (voodoo->Value() == B_CONTROL_ON));
    }

    /* Qt: on_comboBoxVideoSecondary_currentIndexChanged(). */
    void SecondaryChanged()
    {
        video_card[1] = secondary->Value(0);
        secondary_cfg->SetEnabled((video_card[1] != 0) && (video_card_has_config(video_card[1]) > 0));
    }

    void ConfigurePrimary()
    {
        const int card = video->Value(0);
        if (card == VID_INTERNAL) {
            const device_t *dev = machine_get_vid_device(machine_id);
            if (dev != nullptr)
                gfxcard_cfg_changed[0] |= haiku_device_configure(Window(), dev, 0);
        } else if (const device_t *dev = video_card_getdevice(card))
            gfxcard_cfg_changed[0] |= haiku_device_configure(Window(), dev, 1);
    }

    void UpdateEdidControls()
    {
        const bool custom = (edid_custom->Value() == B_CONTROL_ON);
        edid_path->SetEnabled(custom);
        edid_browse->SetEnabled(custom);
    }

    /* Qt: the FileField's fileSelected handler. An EDID that cannot be
       used is refused and the previous path comes back. */
    void EdidSelected(const char *path)
    {
        char    name[1024];
        uint8_t buffer[384] = { 0 };
        snprintf(name, sizeof(name), "%s", path);
        const size_t size = ddc_load_edid(name, buffer, sizeof(buffer));
        if ((size == 0) || (size > 256)) {
            char text[1200];
            snprintf(text, sizeof(text), (size == 0) ? "EDID file \"%s\" is invalid." : "EDID file \"%s\" is too large.",
                     path);
            BAlert *alert = new BAlert("EDID", text, "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT);
            alert->Go();
            edid_path->SetText(previous_edid_path.c_str());
        } else
            previous_edid_path = path;
    }

    /* The panels are asynchronous: they send their result to the Settings
       window, which hands it back to Handle(). */
    void BrowseEdid()
    {
        BMessenger target(Window());
        BMessage   msg(MSG_EDID_CHOSEN);

        edid_panel.reset(new BFilePanel(B_OPEN_PANEL, &target, nullptr, B_FILE_NODE, false, &msg, nullptr, true));
        edid_panel->Window()->SetTitle(EMU_NAME ": EDID");

        BPath current(edid_path->Text());
        BPath dir;
        if ((edid_path->Text()[0] != '\0') && (current.InitCheck() == B_OK) && (current.GetParent(&dir) == B_OK)
            && BEntry(dir.Path()).IsDirectory())
            edid_panel->SetPanelDirectory(dir.Path());
        else if (usr_path[0] != '\0')
            edid_panel->SetPanelDirectory(usr_path);

        edid_panel->Show();
    }

    void ExportEdid()
    {
        BMessenger target(Window());
        BMessage   msg(MSG_EDID_EXPORT_TO);

        export_panel.reset(new BFilePanel(B_SAVE_PANEL, &target, nullptr, B_FILE_NODE, false, &msg, nullptr, true));
        export_panel->Window()->SetTitle("Export EDID");
        if (usr_path[0] != '\0')
            export_panel->SetPanelDirectory(usr_path);
        export_panel->Show();
    }

    static void WriteDefaultEdid(const std::string &path)
    {
        if (path.empty())
            return;

        FILE *file = fopen(path.c_str(), "wb");
        if (file == nullptr)
            return;

        uint8_t     *bytes = nullptr;
        const size_t size  = ddc_create_default_edid(&bytes);
        if (bytes != nullptr) {
            fwrite(bytes, 1, size, file);
            free(bytes);
        }
        fclose(file);
    }

    Choice       *video;
    BButton      *video_cfg;
    Choice       *secondary;
    BButton      *secondary_cfg;
    BCheckBox    *voodoo;
    BButton      *voodoo_cfg;
    BCheckBox    *ibm8514;
    BButton      *ibm8514_cfg;
    BCheckBox    *xga;
    BButton      *xga_cfg;
    BCheckBox    *da2;
    BButton      *da2_cfg;
    BCheckBox    *powervr;
    BButton      *powervr_cfg;
    Choice       *screen_type;
    Choice       *conversion;
    BRadioButton *edid_default;
    BButton      *edid_export;
    BRadioButton *edid_custom;
    BTextControl *edid_path;
    BButton      *edid_browse;
    BCheckBox    *overscan;
    BCheckBox    *inverted;
    BCheckBox    *contrast;

    std::unique_ptr<BFilePanel> edid_panel;
    std::unique_ptr<BFilePanel> export_panel;
    std::string                 previous_edid_path;

    int  machine_id        = 0;
    int  video_card[2]     = { 0, 0 };
    bool secondary_allowed = true;
    /* The primary list is being filled: the secondary one is built once,
       at the end. */
    bool filling = false;

    bool gfxcard_cfg_changed[GFXCARD_MAX];
    bool voodoo_cfg_changed  = false;
    bool powervr_cfg_changed = false;
    bool ibm8514_cfg_changed = false;
    bool xga_cfg_changed     = false;
    bool ps55da2_cfg_changed = false;
};

bool
settings_display_light_pen_usable()
{
    if (display_page != nullptr)
        return display_page->LightPenUsable();

    bool usable = false;
    lightpen_check_name(gfxcard[0], usable);
    lightpen_check_name(gfxcard[1], usable);
    return usable;
}


SettingsPage *
create_display_page()
{
    return new DisplayPage();
}

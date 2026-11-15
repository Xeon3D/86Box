/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Other peripherals Settings page
 *          (qt_settingsotherperipherals.cpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Button.h>
#include <CheckBox.h>
#include <GridView.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <TabView.h>

#include <cstdio>
#include <string>

#include "haiku_deviceconfig.hpp"
#include "haiku_icons.hpp"
#include "haiku_settings_common.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/mcamem.h>
#include <86box/isamem.h>
#include <86box/isarom.h>
#include <86box/isartc.h>
#include <86box/unittester.h>
#include <86box/softpower.h>
#include <86box/novell_cardkey.h>
}

/* The slot is added to the per-slot codes ('som0' + i ...). */
enum : uint32 {
    MSG_SO_RTC           = 'sort',
    MSG_SO_RTC_CFG       = 'sorc',
    MSG_SO_UNITTESTER    = 'sout',
    MSG_SO_UT_CFG        = 'souc',
    MSG_SO_KEYCARD       = 'sokc',
    MSG_SO_KEYCARD_CFG   = 'sokC',
    MSG_SO_SOFTPOWER     = 'sosp',
    MSG_SO_SOFTPOWER_CFG = 'sosC',
    MSG_SO_MEM           = 'som0',
    MSG_SO_MEM_CFG       = 'soM0',
    MSG_SO_ROM           = 'sor0',
    MSG_SO_ROM_CFG       = 'soR0',
};

static bool
has_isa_or_sidecar_bus(int m)
{
    return machine_has_bus(m, MACHINE_BUS_ISA | MACHINE_BUS_SIDECAR) > 0;
}

/* Memory expansion: one shared set of slots, fed from the MCA board list
   (mcamem) on MCA machines and from the ISA one (isamem) otherwise. */
static const device_t *
memexp_device(int board, bool mca_bus)
{
    return mca_bus ? mcamem_get_device(board) : isamem_get_device(board);
}

static const char *
memexp_internal_name(int board, bool mca_bus)
{
    return mca_bus ? mcamem_get_internal_name(board) : isamem_get_internal_name(board);
}

/* --- Other peripherals (qt_settingsotherperipherals.cpp) --------------- */

class OtherPeripheralsPage : public SettingsPage {
public:
    OtherPeripheralsPage()
        : SettingsPage("Other peripherals")
        , machine_id(0)
        , rtc(new Choice("rtc", "ISA RTC:", MSG_SO_RTC))
        , rtc_button(new BButton("rtc_cfg", "Configure", new BMessage(MSG_SO_RTC_CFG)))
        , bugger(new BCheckBox("bugger", "ISABugger device", nullptr))
        , postcard(new BCheckBox("postcard", "POST card", nullptr))
        , unittester(new BCheckBox("unittester", "86Box Unit Tester", new BMessage(MSG_SO_UNITTESTER)))
        , ut_button(new BButton("ut_cfg", "Configure", new BMessage(MSG_SO_UT_CFG)))
        , keycard(new BCheckBox("keycard", "Novell NetWare 2.x Key Card", new BMessage(MSG_SO_KEYCARD)))
        , keycard_button(new BButton("keycard_cfg", "Configure", new BMessage(MSG_SO_KEYCARD_CFG)))
        , softpower(new BCheckBox("softpower", "Software-initiated poweroff device (IBM 5140)",
                                  new BMessage(MSG_SO_SOFTPOWER)))
        , softpower_button(new BButton("softpower_cfg", "Configure", new BMessage(MSG_SO_SOFTPOWER_CFG)))
        , rtc_cfg_changed(false)
        , ut_cfg_changed(false)
        , keycard_cfg_changed(false)
        , softpower_cfg_changed(false)
        , softpower_card_enabled(softpower_enabled > 0)
    {
        char name[32];
        char label[32];

        /* General tab. */
        BGridView *general = new BGridView("General", B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
        BLayoutBuilder::Grid<>(general)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .AddMenuField(rtc->field, 0, 0)
            .Add(rtc_button, 2, 0)
            .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING, 0, 1, 3)
                .Add(bugger)
                .Add(postcard)
                .AddGlue()
            .End()
            .Add(unittester, 0, 2, 2)
            .Add(ut_button, 2, 2)
            .Add(keycard, 0, 3, 2)
            .Add(keycard_button, 2, 3)
            .Add(softpower, 0, 4, 2)
            .Add(softpower_button, 2, 4)
            .AddGlue(0, 5, 3);
        general->GridLayout()->SetColumnWeight(1, 1.0f);

        /* Memory expansion and ROM card tabs. */
        BGridView *mem_view = new BGridView("ISA Memory Expansion", B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
        BLayoutBuilder::Grid<> mem_builder(mem_view);
        mem_builder.SetInsets(B_USE_DEFAULT_SPACING);
        for (int i = 0; i < ISAMEM_MAX; i++) {
            snprintf(name, sizeof(name), "mem%i", i);
            snprintf(label, sizeof(label), "Card %i:", i + 1);
            mem[i] = new Choice(name, label, MSG_SO_MEM + i);
            snprintf(name, sizeof(name), "mem%i_cfg", i);
            mem_button[i]      = new BButton(name, "Configure", new BMessage(MSG_SO_MEM_CFG + i));
            mem_cfg_changed[i] = false;
            mem_builder.AddMenuField(mem[i]->field, 0, i).Add(mem_button[i], 2, i);
        }
        mem_builder.AddGlue(0, ISAMEM_MAX, 3);
        mem_view->GridLayout()->SetColumnWeight(1, 1.0f);

        BGridView *rom_view = new BGridView("ISA ROM Cards", B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
        BLayoutBuilder::Grid<> rom_builder(rom_view);
        rom_builder.SetInsets(B_USE_DEFAULT_SPACING);
        for (int i = 0; i < ISAROM_MAX; i++) {
            snprintf(name, sizeof(name), "rom%i", i);
            snprintf(label, sizeof(label), "Card %i:", i + 1);
            rom[i] = new Choice(name, label, MSG_SO_ROM + i);
            snprintf(name, sizeof(name), "rom%i_cfg", i);
            rom_button[i]      = new BButton(name, "Configure", new BMessage(MSG_SO_ROM_CFG + i));
            rom_cfg_changed[i] = false;
            rom_builder.AddMenuField(rom[i]->field, 0, i).Add(rom_button[i], 2, i);
        }
        rom_builder.AddGlue(0, ISAROM_MAX, 3);
        rom_view->GridLayout()->SetColumnWeight(1, 1.0f);

        BTabView *tabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);
        tabs->AddTab(general, settings_icon_tab(general, haiku_icon("general_other_peripherals")));
        tabs->AddTab(mem_view, settings_icon_tab(mem_view, haiku_icon("isa_memory")));
        tabs->AddTab(rom_view, settings_icon_tab(rom_view, haiku_icon("isa_rom")));

        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .Add(tabs);
    }

    ~OtherPeripheralsPage() override
    {
        delete rtc;
        for (int i = 0; i < ISAMEM_MAX; i++)
            delete mem[i];
        for (int i = 0; i < ISAROM_MAX; i++)
            delete rom[i];
    }

    /* Like Qt, a machine change shows the saved values again (filtered for
       the new machine); only the soft power card choice is kept. */
    void MachineChanged(int m) override
    {
        machine_id = m;

        const bool has_isa          = (machine_has_bus(m, MACHINE_BUS_ISA) > 0);
        const bool has_isa_sidecar  = has_isa_or_sidecar_bus(m);
        const bool native_softpower = NativeSoftPower();

        bugger->SetEnabled(has_isa);
        keycard->SetEnabled(has_isa);
        softpower->SetEnabled(has_isa && !native_softpower);

        SetCheck(bugger, has_isa && (bugger_enabled > 0));
        SetCheck(postcard, postcard_enabled > 0);
        SetCheck(unittester, unittester_enabled > 0);
        SetCheck(keycard, has_isa && (novell_keycard_enabled > 0));
        /* Native power hardware must not overwrite the optional card
           preference. */
        SetCheck(softpower, native_softpower || (has_isa && softpower_card_enabled));

        /* ISA RTC cards. */
        rtc->Clear();
        for (int c = 0;; c++) {
            const device_t   *dev  = isartc_get_device(c);
            const std::string name = settings_device_name(dev, isartc_get_internal_name(c), 0);
            if (name.empty())
                break;
            if (device_is_valid(dev, m))
                rtc->Add(name.c_str(), c);
        }
        rtc->Select(isartc_type);
        rtc->SetEnabled(has_isa_sidecar);

        /* Memory expansion cards. */
        const bool mca_bus = (machine_has_bus(m, MACHINE_BUS_MCA) > 0);
        const bool isa_bus = !mca_bus && has_isa_sidecar;

        for (int i = 0; i < ISAMEM_MAX; i++)
            mem[i]->Clear();
        if (mca_bus || isa_bus) {
            for (int c = 0;; c++) {
                const device_t   *dev  = memexp_device(c, mca_bus);
                const std::string name = settings_device_name(dev, memexp_internal_name(c, mca_bus), 0);
                if (name.empty())
                    break;
                if (!device_available(dev) || !device_is_valid(dev, m))
                    continue;
                for (int i = 0; i < ISAMEM_MAX; i++)
                    mem[i]->Add(name.c_str(), c);
            }
        }
        for (int i = 0; i < ISAMEM_MAX; i++)
            mem[i]->Select(mca_bus ? mcamem_type[i] : isamem_type[i]);

        /* ISA ROM cards. */
        for (int i = 0; i < ISAROM_MAX; i++)
            rom[i]->Clear();
        for (int c = 0;; c++) {
            const device_t   *dev  = isarom_get_device(c);
            const std::string name = settings_device_name(dev, isarom_get_internal_name(c), 0);
            if (name.empty())
                break;
            if (!device_is_valid(dev, m))
                continue;
            for (int i = 0; i < ISAROM_MAX; i++)
                rom[i]->Add(name.c_str(), c);
        }
        for (int i = 0; i < ISAROM_MAX; i++)
            rom[i]->Select(isarom_type[i]);

        UpdateButtons();
    }

    bool Handle(BMessage *message) override
    {
        const uint32 what = message->what;

        switch (what) {
            case MSG_SO_RTC:
            case MSG_SO_UNITTESTER:
            case MSG_SO_KEYCARD:
                UpdateButtons();
                return true;
            case MSG_SO_SOFTPOWER:
                if (OptionalSoftPower())
                    softpower_card_enabled = (softpower->Value() == B_CONTROL_ON);
                UpdateButtons();
                return true;
            case MSG_SO_RTC_CFG:
                rtc_cfg_changed |= haiku_device_configure(Window(), isartc_get_device(rtc->Value(0)));
                return true;
            case MSG_SO_UT_CFG:
                ut_cfg_changed |= haiku_device_configure(Window(), &unittester_device);
                return true;
            case MSG_SO_KEYCARD_CFG:
                keycard_cfg_changed |= haiku_device_configure(Window(), &novell_keycard_device);
                return true;
            case MSG_SO_SOFTPOWER_CFG:
                softpower_cfg_changed |= haiku_device_configure(Window(), &softpower_device);
                return true;
            default:
                break;
        }

        if (((what >= MSG_SO_MEM) && (what < MSG_SO_MEM + ISAMEM_MAX))
            || ((what >= MSG_SO_ROM) && (what < MSG_SO_ROM + ISAROM_MAX))) {
            UpdateButtons();
            return true;
        }
        if ((what >= MSG_SO_MEM_CFG) && (what < MSG_SO_MEM_CFG + ISAMEM_MAX)) {
            const int i = what - MSG_SO_MEM_CFG;
            mem_cfg_changed[i] |= haiku_device_configure(Window(), memexp_device(mem[i]->Value(0), McaBus()), i + 1);
            return true;
        }
        if ((what >= MSG_SO_ROM_CFG) && (what < MSG_SO_ROM_CFG + ISAROM_MAX)) {
            const int i = what - MSG_SO_ROM_CFG;
            rom_cfg_changed[i] |= haiku_device_configure(Window(), isarom_get_device(rom[i]->Value(0)), i + 1);
            return true;
        }
        return false;
    }

    bool Changed() override
    {
        bool changed = false;

        changed |= (isartc_type != rtc->Value(isartc_type)) || rtc_cfg_changed;
        changed |= (bugger_enabled != Checked(bugger));
        changed |= (postcard_enabled != Checked(postcard));
        changed |= (unittester_enabled != Checked(unittester)) || ut_cfg_changed;
        changed |= (softpower_enabled != (softpower_card_enabled ? 1 : 0)) || softpower_cfg_changed;
        changed |= (novell_keycard_enabled != Checked(keycard)) || keycard_cfg_changed;

        const bool mca_bus = McaBus();
        if (mca_bus || has_isa_or_sidecar_bus(machine_id)) {
            for (int i = 0; i < ISAMEM_MAX; i++) {
                const int cur = mca_bus ? mcamem_type[i] : isamem_type[i];
                changed |= (cur != mem[i]->Value(cur)) || mem_cfg_changed[i];
            }
        }

        for (int i = 0; i < ISAROM_MAX; i++)
            changed |= (isarom_type[i] != rom[i]->Value(isarom_type[i])) || rom_cfg_changed[i];

        return changed;
    }

    void Save() override
    {
        isartc_type            = rtc->Value(isartc_type);
        bugger_enabled         = Checked(bugger);
        postcard_enabled       = Checked(postcard);
        unittester_enabled     = Checked(unittester);
        softpower_enabled      = softpower_card_enabled ? 1 : 0;
        novell_keycard_enabled = Checked(keycard);

        /* Write the active memory board family and clear the other one. */
        const bool mca_bus = McaBus();
        const bool isa_bus = !mca_bus && has_isa_or_sidecar_bus(machine_id);
        for (int i = 0; i < ISAMEM_MAX; i++) {
            const int val = (mca_bus || isa_bus) ? mem[i]->Value(0) : 0;
            if (mca_bus) {
                mcamem_type[i] = val;
                isamem_type[i] = 0;
            } else {
                isamem_type[i] = val;
                mcamem_type[i] = 0;
            }
        }

        for (int i = 0; i < ISAROM_MAX; i++)
            isarom_type[i] = rom[i]->Value(isarom_type[i]);
    }

private:
    static void SetCheck(BCheckBox *box, bool on) { box->SetValue(on ? B_CONTROL_ON : B_CONTROL_OFF); }
    static int  Checked(const BCheckBox *box) { return (box->Value() == B_CONTROL_ON) ? 1 : 0; }

    bool McaBus() const { return machine_has_bus(machine_id, MACHINE_BUS_MCA) > 0; }
    bool NativeSoftPower() const { return machines[machine_id].init == machine_ibm5140_init; }
    bool OptionalSoftPower() const
    {
        return !NativeSoftPower() && (machine_has_bus(machine_id, MACHINE_BUS_ISA) > 0);
    }

    /* Configure follows the selected device / checked box (Qt's
       on_comboBox*_currentIndexChanged and on_checkBox*_stateChanged). */
    void UpdateButtons()
    {
        const bool has_isa_sidecar = has_isa_or_sidecar_bus(machine_id);
        const bool mca_bus         = McaBus();

        const int rtc_cur = rtc->Value(0);
        rtc_button->SetEnabled((rtc_cur != 0) && haiku_device_has_config(isartc_get_device(rtc_cur)) && has_isa_sidecar);
        ut_button->SetEnabled(unittester->Value() == B_CONTROL_ON);
        keycard_button->SetEnabled(keycard->Value() == B_CONTROL_ON);
        softpower_button->SetEnabled(OptionalSoftPower() && (softpower->Value() == B_CONTROL_ON));

        for (int i = 0; i < ISAMEM_MAX; i++) {
            const device_t *dev = (mem[i]->Count() > 0) ? memexp_device(mem[i]->Value(0), mca_bus) : nullptr;
            mem_button[i]->SetEnabled((dev != nullptr) && device_is_valid(dev, machine_id) && haiku_device_has_config(dev));
        }
        for (int i = 0; i < ISAROM_MAX; i++) {
            const int cur = rom[i]->Value(0);
            rom_button[i]->SetEnabled((cur != 0) && haiku_device_has_config(isarom_get_device(cur)) && has_isa_sidecar);
        }
    }

    int        machine_id;
    Choice    *rtc;
    BButton   *rtc_button;
    BCheckBox *bugger;
    BCheckBox *postcard;
    BCheckBox *unittester;
    BButton   *ut_button;
    BCheckBox *keycard;
    BButton   *keycard_button;
    BCheckBox *softpower;
    BButton   *softpower_button;
    Choice    *mem[ISAMEM_MAX];
    BButton   *mem_button[ISAMEM_MAX];
    Choice    *rom[ISAROM_MAX];
    BButton   *rom_button[ISAROM_MAX];
    bool       rtc_cfg_changed;
    bool       ut_cfg_changed;
    bool       keycard_cfg_changed;
    bool       softpower_cfg_changed;
    bool       softpower_card_enabled;
    bool       mem_cfg_changed[ISAMEM_MAX];
    bool       rom_cfg_changed[ISAROM_MAX];
};

SettingsPage *
create_other_peripherals_page()
{
    return new OtherPeripheralsPage();
}

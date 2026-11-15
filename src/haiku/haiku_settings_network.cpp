/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Network Settings page (qt_settingsnetwork.cpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <SeparatorView.h>
#include <TabView.h>
#include <TextControl.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "haiku_deviceconfig.hpp"
#include "haiku_icons.hpp"
#include "haiku_settings_common.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/timer.h>
#include <86box/thread.h>
#include <86box/network.h>
}

/* One code per adapter: the last character is '0' + the adapter index. */
enum : uint32 {
    MSG_NET_CARD      = 'snc0', /* adapter model */
    MSG_NET_TYPE      = 'snt0', /* mode */
    MSG_NET_CONFIGURE = 'snb0', /* "Configure" */
};

static void
set_visible(BView *view, bool visible)
{
    if (visible && view->IsHidden(view))
        view->Show();
    else if (!visible && !view->IsHidden(view))
        view->Hide();
}

/* Copy a text control's text into a fixed-size core string. */
static void
copy_text(char *dest, size_t size, const BTextControl *control)
{
    memset(dest, '\0', size);
    strncpy(dest, control->Text(), size - 1);
}

/* --- Network (qt_settingsnetwork.cpp) --------------------------------- */

class NetworkPage : public SettingsPage {
public:
    NetworkPage()
        : SettingsPage("Network")
        , machine_id(-1)
    {
        BTabView *tabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);

        for (int i = 0; i < NET_CARD_MAX; i++) {
            Adapter &a = adapters[i];
            char     name[32];

            a.cfg_changed = false;

            snprintf(name, sizeof(name), "net%i", i);
            a.type = new Choice(name, "Mode:", MSG_NET_TYPE + i);
            snprintf(name, sizeof(name), "nic%i", i);
            a.card = new Choice(name, "Adapter:", MSG_NET_CARD + i);
            snprintf(name, sizeof(name), "intf%i", i);
            a.intf = new Choice(name, "Interface:", MSG_SETTINGS_IGNORE);

            a.configure = new BButton("configure", "Configure", new BMessage(MSG_NET_CONFIGURE + i));
            a.options   = new BSeparatorView("Options");
            a.vde       = new BTextControl("vde", "VDE Socket:", "", nullptr);
            a.bridge    = new BTextControl("bridge", "TAP Bridge Device:", "", nullptr);
            a.secret    = new BTextControl("secret", "Shared secret:", "", nullptr);
            a.promisc   = new BCheckBox("promisc", "Hub Mode", new BMessage(MSG_SETTINGS_IGNORE));
            a.hostname  = new BTextControl("hostname", "Hostname:", "", nullptr);

            BGroupView *tab = new BGroupView(B_VERTICAL);
            BLayoutBuilder::Group<>(tab, B_VERTICAL, B_USE_SMALL_SPACING)
                .SetInsets(B_USE_DEFAULT_SPACING)
                .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
                    .AddMenuField(a.type->field, 0, 0)
                    .AddMenuField(a.card->field, 0, 1)
                    .Add(a.configure, 2, 1)
                    .SetColumnWeight(1, 1.0f)
                .End()
                .Add(a.options)
                .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
                    .AddMenuField(a.intf->field, 0, 0)
                    .AddTextControl(a.vde, 0, 1)
                    .AddTextControl(a.bridge, 0, 2)
                    .AddTextControl(a.secret, 0, 3)
                    .Add(a.promisc, 1, 4)
                    .AddTextControl(a.hostname, 0, 5)
                    .SetColumnWeight(1, 1.0f)
                .End()
                .AddGlue();

            snprintf(name, sizeof(name), "Adapter %i", i + 1);
            tabs->AddTab(tab, settings_icon_tab(tab, haiku_icon("network"), name));

            FillTypes(i);
        }

        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .Add(tabs)
            .SetInsets(B_USE_DEFAULT_SPACING);
    }

    void MachineChanged(int m) override
    {
        machine_id = m;

        const int m_has_net = (!!machine_has_flags_64(m, MACHINE_NIC_PRI))
                              | ((!!machine_has_flags_64(m, MACHINE_NIC_SEC)) << 1);
        const device_t *onboard = machine_get_net_device(m);
        int             current[NET_CARD_MAX];

        for (int i = 0; i < NET_CARD_MAX; i++) {
            current[i] = adapters[i].card->Count() ? adapters[i].card->Value(net_cards_conf[i].device_num)
                                                   : net_cards_conf[i].device_num;
            adapters[i].card->Clear();
        }

        for (int c = 0;; c++) {
            const device_t   *dev  = network_card_getdevice(c);
            const std::string name = settings_device_name(dev, network_card_get_internal_name(c), 1);
            if (name.empty())
                break;
            if (!network_card_available(c) || !device_is_valid(dev, m))
                continue;

            for (int i = 0; i < NET_CARD_MAX; i++) {
                /* "Internal" only in the slots the machine has an on-board
                   NIC for. */
                if ((c == NET_INTERNAL) && !(m_has_net & (1 << i)))
                    continue;
                std::string label = name;
                if ((i == 0) && (c == NET_INTERNAL) && (onboard != nullptr))
                    label += " (" + settings_device_name(onboard, onboard->internal_name, 0) + ")";
                adapters[i].card->Add(label.c_str(), c);
            }
        }

        for (int i = 0; i < NET_CARD_MAX; i++) {
            adapters[i].card->Select(current[i]);
            UpdateAdapter(i);
        }
    }

    bool Changed() override
    {
        bool changed = false;

        for (int i = 0; i < NET_CARD_MAX; i++) {
            const Adapter &a        = adapters[i];
            const int      net_type = a.type->Value(net_cards_conf[i].net_type);
            char           host_dev_name[128];
            char           secret[256];
            char           nrs_hostname[128];

            changed |= (net_cards_conf[i].device_num != a.card->Value(net_cards_conf[i].device_num));
            changed |= a.cfg_changed;
            changed |= (net_cards_conf[i].net_type != net_type);

            HostDevName(i, net_type, host_dev_name, sizeof(host_dev_name));
            memcpy(secret, net_cards_conf[i].secret, sizeof(secret));
            memcpy(nrs_hostname, net_cards_conf[i].nrs_hostname, sizeof(nrs_hostname));
            if (net_type == NET_TYPE_NRSWITCH) {
                copy_text(nrs_hostname, sizeof(nrs_hostname), a.hostname);
                copy_text(secret, sizeof(secret), a.secret);
            } else if (net_type == NET_TYPE_NLSWITCH) {
                changed |= (net_cards_conf[i].promisc_mode != (a.promisc->Value() == B_CONTROL_ON));
                copy_text(secret, sizeof(secret), a.secret);
            }

            changed |= (strcmp(host_dev_name, net_cards_conf[i].host_dev_name[0] ? net_cards_conf[i].host_dev_name : "none") != 0);
            changed |= (strncmp(secret, net_cards_conf[i].secret, sizeof(secret)) != 0);
            changed |= (strncmp(nrs_hostname, net_cards_conf[i].nrs_hostname, sizeof(nrs_hostname)) != 0);
        }

        return changed;
    }

    void Save() override
    {
        for (int i = 0; i < NET_CARD_MAX; i++) {
            const Adapter &a    = adapters[i];
            netcard_conf_t *cfg = &net_cards_conf[i];

            cfg->device_num = a.card->Value(cfg->device_num);
            cfg->net_type   = a.type->Value(cfg->net_type);
            HostDevName(i, cfg->net_type, cfg->host_dev_name, sizeof(cfg->host_dev_name));
            if (cfg->net_type == NET_TYPE_NRSWITCH) {
                copy_text(cfg->nrs_hostname, sizeof(cfg->nrs_hostname), a.hostname);
                copy_text(cfg->secret, sizeof(cfg->secret), a.secret);
            } else if (cfg->net_type == NET_TYPE_NLSWITCH) {
                cfg->promisc_mode = (a.promisc->Value() == B_CONTROL_ON);
                copy_text(cfg->secret, sizeof(cfg->secret), a.secret);
            }
        }
    }

    bool Handle(BMessage *message) override
    {
        const uint32 what = message->what;

        for (int i = 0; i < NET_CARD_MAX; i++) {
            if ((what == MSG_NET_CARD + i) || (what == MSG_NET_TYPE + i)) {
                UpdateAdapter(i);
                return true;
            }
            if (what == MSG_NET_CONFIGURE + i) {
                Configure(i);
                return true;
            }
        }
        return false;
    }

private:
    struct Adapter {
        Choice         *type;
        Choice         *card;
        Choice         *intf;
        BButton        *configure;
        BSeparatorView *options;
        BTextControl   *vde;
        BTextControl   *bridge;
        BTextControl   *secret;
        BCheckBox      *promisc;
        BTextControl   *hostname;
        bool            cfg_changed;
    };

    /* The modes and host interfaces do not depend on the machine: filled
       once, from the saved values (Qt: onCurrentMachineChanged). */
    void FillTypes(int i)
    {
        Adapter              &a   = adapters[i];
        const netcard_conf_t &cfg = net_cards_conf[i];

        a.type->Add("Null Driver", NET_TYPE_NONE);
        a.type->Add("SLiRP", NET_TYPE_SLIRP);
        if (network_ndev > 1)
            a.type->Add("PCap", NET_TYPE_PCAP);
#ifdef HAS_VDE
        if (network_devmap.has_vde)
            a.type->Add("VDE", NET_TYPE_VDE);
#endif
#ifdef HAS_TAP
        if (network_devmap.has_tap)
            a.type->Add("TAP", NET_TYPE_TAP);
#endif
        a.type->Add("Local Switch", NET_TYPE_NLSWITCH);
#ifdef ENABLE_NET_NRSWITCH
        a.type->Add("Remote Switch", NET_TYPE_NRSWITCH);
#endif
        a.type->Select(cfg.net_type);

        int selected = 0;
        for (int c = 0; c < network_ndev; c++) {
            a.intf->Add(network_devs[c].description, c);
            if (!strcmp(network_devs[c].device, cfg.host_dev_name))
                selected = c;
        }
        a.intf->Select(selected);

        if (cfg.net_type == NET_TYPE_VDE)
            a.vde->SetText(cfg.host_dev_name);
        else if (cfg.net_type == NET_TYPE_TAP)
            a.bridge->SetText(cfg.host_dev_name);
        a.secret->SetText(cfg.secret);
        a.hostname->SetText(cfg.nrs_hostname);
        a.promisc->SetValue(cfg.promisc_mode == 1 ? B_CONTROL_ON : B_CONTROL_OFF);
    }

    /* The host device a mode connects to, "none" if it has none (Qt:
       save()). */
    void HostDevName(int i, int net_type, char *dest, size_t size) const
    {
        const Adapter &a = adapters[i];

        memset(dest, '\0', size);
        if ((net_type == NET_TYPE_PCAP) && (network_ndev > 0)) {
            const int intf = a.intf->Value(0);
            if ((intf >= 0) && (intf < network_ndev))
                strncpy(dest, network_devs[intf].device, size - 1);
        }
#ifdef HAS_VDE
        else if (net_type == NET_TYPE_VDE)
            strncpy(dest, a.vde->Text(), size - 1);
#endif
#ifdef HAS_TAP
        else if (net_type == NET_TYPE_TAP)
            strncpy(dest, a.bridge->Text(), size - 1);
#endif
        if (dest[0] == '\0')
            strncpy(dest, "none", size - 1);
    }

    /* The device a "Configure" button opens, and its instance (Qt:
       on_pushButtonConfN_clicked). */
    const device_t *ConfigDevice(int i, int *instance) const
    {
        const int nic = adapters[i].card->Value(0);

        *instance = i + 1;
        if ((nic == NET_INTERNAL) && (machine_id >= 0)) {
            if ((i == 0) && !machine_has_flags_64(machine_id, MACHINE_NIC_SEC))
                *instance = 0;
            return machine_get_net_device(machine_id);
        }
        return network_card_getdevice(nic);
    }

    void Configure(int i)
    {
        int             instance;
        const device_t *dev = ConfigDevice(i, &instance);

        if (dev != nullptr)
            adapters[i].cfg_changed |= haiku_device_configure(Window(), dev, instance);
    }

    /* Qt: enableElements(), for one adapter. */
    void UpdateAdapter(int i)
    {
        Adapter  &a        = adapters[i];
        const int nic      = a.card->Value(0);
        const int net_type = a.type->Value(NET_TYPE_NONE);
        int       instance;

        a.configure->SetEnabled(haiku_device_has_config(ConfigDevice(i, &instance)));

        bool intf = false, vde = false, bridge = false, secret = false, promisc = false, hostname = false;
        if (nic != 0) {
            switch (net_type) {
#ifdef HAS_VDE
                case NET_TYPE_VDE:
                    vde = true;
                    break;
#endif
                case NET_TYPE_PCAP:
                    intf = true;
                    break;
#ifdef HAS_TAP
                case NET_TYPE_TAP:
                    bridge = true;
                    break;
#endif
                case NET_TYPE_NLSWITCH:
                    secret  = true;
                    promisc = true;
                    break;
                case NET_TYPE_NRSWITCH:
                    secret   = true;
                    hostname = true;
                    break;
                default:
                    break;
            }
        }

        a.intf->field->SetEnabled(net_type == NET_TYPE_PCAP);
        set_visible(a.options, intf || vde || bridge || secret || promisc || hostname);
        set_visible(a.intf->field, intf);
        set_visible(a.vde, vde);
        set_visible(a.bridge, bridge);
        set_visible(a.secret, secret);
        set_visible(a.promisc, promisc);
        set_visible(a.hostname, hostname);
    }

    Adapter adapters[NET_CARD_MAX];
    int     machine_id;
};


SettingsPage *
create_network_page()
{
    return new NetworkPage();
}

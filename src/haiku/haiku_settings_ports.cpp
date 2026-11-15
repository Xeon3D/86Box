/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Ports Settings page (qt_settingsports.cpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <TabView.h>
#include <TextView.h>
#include <Window.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "haiku_deviceconfig.hpp"
#include "haiku_settings.hpp"
#include "haiku_settings_common.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/lpt.h>
#include <86box/mouse.h>
#include <86box/network.h>
#include <86box/scsi_device.h>
#include <86box/scsi_tape.h>
#include <86box/serial.h>
#include <86box/char.h>
#include <86box/mem.h>
#include <86box/usb.h>
}

/* The serial ports the page shows (Qt: SERIAL_MAX_UI). */
#define SERIAL_MAX_UI 4

/* Per-port codes: the last character is '0' + the port index. */
enum : uint32 {
    MSG_LPT_ENABLE    = 'spe0',
    MSG_LPT_DEVICE    = 'spd0',
    MSG_LPT_CONFIGURE = 'spf0',
    MSG_COM_ENABLE    = 'spg0',
    MSG_COM_DEVICE    = 'sph0',
    MSG_COM_CONFIGURE = 'spi0',
    MSG_USB_CARD      = 'spuc',
    MSG_USB_MSD       = 'spum',
    MSG_USB_HDD       = 'spuh',
    MSG_USB_CDROM     = 'spuo',
    MSG_USB_RDISK     = 'spur',
    MSG_USB_BUS       = 'spub', /* the USB bus changed: refresh the other pages */
};

/* The "port" a COM/LPT network card or mouse is configured for. */
static int
configured_port(const device_t *device, int instance)
{
    if (!device)
        return -1;

    if (instance > 0)
        device_context_inst(device, instance);
    else
        device_context(device);

    const int port = device_get_config_int("port");
    device_context_restore();
    return port;
}

/* A port taken by a serial/parallel network card or mouse (as saved). */
static bool
port_used_by_network_or_mouse(bool parallel, int port)
{
    const uint32_t bus_flag = parallel ? DEVICE_LPT : DEVICE_COM;

    for (int i = 0; i < NET_CARD_MAX; i++) {
        const device_t *device = network_card_getdevice(net_cards_conf[i].device_num);
        if (device && (device->flags & bus_flag) && (configured_port(device, i + 1) == port))
            return true;
    }

    const device_t *mouse_devices[] = {
        mouse_get_device(mouse_type),
        tablet_get_device(tablet_type)
    };
    for (const device_t *device : mouse_devices) {
        if (device && (device->flags & bus_flag) && (configured_port(device, 0) == port))
            return true;
    }

    return false;
}

/* --- Ports (qt_settingsports.cpp) ------------------------------------- */

class PortsPage : public SettingsPage {
public:
    PortsPage()
        : SettingsPage("Ports (COM & LPT)")
        , machine_id(-1)
        , ecp_dma(new Choice("ecp_dma", "Internal LPT ECP DMA:", MSG_SETTINGS_IGNORE))
        , usb_card(new Choice("usb_card", "Card:", MSG_USB_CARD))
        , usb_hub(new Choice("usb_hub", "Hub:", MSG_SETTINGS_IGNORE))
        , usb_msd(new Choice("usb_msd", "Mass storage:", MSG_USB_MSD))
    {
        BTabView *tabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);

        /* Parallel ports */
        BGroupView *parallel = new BGroupView(B_VERTICAL);
        {
            BLayoutBuilder::Grid<> grid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
            grid.AddMenuField(ecp_dma->field, 0, 0, B_ALIGN_LEFT, 1, 2);
            for (int i = 0; i < PARALLEL_MAX; i++) {
                char label[16];
                char name[16];
                snprintf(label, sizeof(label), "LPT%i:", i + 1);
                snprintf(name, sizeof(name), "lpt%i", i);
                lpt[i].enable    = new BCheckBox(name, label, new BMessage(MSG_LPT_ENABLE + i));
                lpt[i].device    = new Choice(name, nullptr, MSG_LPT_DEVICE + i);
                lpt[i].configure = new BButton("configure", "Configure", new BMessage(MSG_LPT_CONFIGURE + i));
                lpt[i].changed   = false;
                lpt[i].enable->SetValue((lpt_ports[i].enabled > 0) ? B_CONTROL_ON : B_CONTROL_OFF);
                grid.Add(lpt[i].enable, 0, i + 1)
                    .Add(lpt[i].device->field, 1, i + 1, 2)
                    .Add(lpt[i].configure, 3, i + 1);
            }
            grid.SetColumnWeight(2, 1.0f);
            BLayoutBuilder::Group<>(parallel, B_VERTICAL)
                .SetInsets(B_USE_DEFAULT_SPACING)
                .Add(grid)
                .AddGlue();
        }
        AddTab(tabs, parallel, "Parallel ports");

        /* Serial ports */
        BGroupView *serial = new BGroupView(B_VERTICAL);
        {
            BLayoutBuilder::Grid<> grid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
            for (int i = 0; i < SERIAL_MAX_UI; i++) {
                char label[16];
                char name[16];
                snprintf(label, sizeof(label), "COM%i:", i + 1);
                snprintf(name, sizeof(name), "com%i", i);
                com[i].enable    = new BCheckBox(name, label, new BMessage(MSG_COM_ENABLE + i));
                com[i].device    = new Choice(name, nullptr, MSG_COM_DEVICE + i);
                com[i].configure = new BButton("configure", "Configure", new BMessage(MSG_COM_CONFIGURE + i));
                com[i].changed   = false;
                com[i].enable->SetValue((com_ports[i].enabled > 0) ? B_CONTROL_ON : B_CONTROL_OFF);
                grid.Add(com[i].enable, 0, i)
                    .Add(com[i].device->field, 1, i)
                    .Add(com[i].configure, 2, i);
            }
            grid.SetColumnWeight(1, 1.0f);
            BLayoutBuilder::Group<>(serial, B_VERTICAL)
                .SetInsets(B_USE_DEFAULT_SPACING)
                .Add(grid)
                .AddGlue();
        }
        AddTab(tabs, serial, "Serial ports");

        /* USB */
        msd_drives = new BTextView("msd_drives");
        msd_drives->SetText("Its drives are hard disks, CD-ROM drives and other removable devices "
                            "set up on their own pages, on bus SCSI, in the channels named "
                            "\"USB Mass Storage\". Each drive is one logical unit, in channel order.");
        msd_drives->MakeEditable(false);
        msd_drives->MakeSelectable(false);
        msd_drives->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
        msd_drives->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
        rgb_color text_color = ui_color(B_PANEL_TEXT_COLOR);
        msd_drives->SetFontAndColor(be_plain_font, B_FONT_ALL, &text_color);
        msd_drives->SetWordWrap(true);
        msd_drives->SetExplicitMinSize(BSize(200, 50));

        msd_hdd       = new BButton("msd_hdd", "Hard disks" B_UTF8_ELLIPSIS, new BMessage(MSG_USB_HDD));
        msd_cdrom     = new BButton("msd_cdrom", "CD-ROM drives" B_UTF8_ELLIPSIS, new BMessage(MSG_USB_CDROM));
        msd_removable = new BButton("msd_removable", "Other removable devices" B_UTF8_ELLIPSIS, new BMessage(MSG_USB_RDISK));

        BGroupView *usb = new BGroupView(B_VERTICAL);
        BLayoutBuilder::Group<>(usb, B_VERTICAL, B_USE_SMALL_SPACING)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
                .AddMenuField(usb_card->field, 0, 0)
                .AddMenuField(usb_hub->field, 0, 1)
                .AddMenuField(usb_msd->field, 0, 2)
                .SetColumnWeight(1, 1.0f)
            .End()
            .Add(msd_drives)
            .AddGroup(B_HORIZONTAL)
                .Add(msd_hdd)
                .Add(msd_cdrom)
                .Add(msd_removable)
                .AddGlue()
            .End()
            .AddGlue();
        AddTab(tabs, usb, "USB");

        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .Add(tabs)
            .SetInsets(B_USE_DEFAULT_SPACING);
    }

    ~PortsPage() override
    {
        /* The card picked here no longer applies once the window closes;
           the saved usb_card_type decides which machines have a USB bus. */
        usb_card_pending = -1;
    }

    void MachineChanged(int m) override
    {
        machine_id = m;

        /* Internal LPT ECP DMA */
        const int has_jumpers = !!machine_has_jumpered_ecp_dma(m, DMA_ANY);
        const int cur_dma     = ecp_dma->Count() ? ecp_dma->Value(jumpered_internal_ecp_dma) : jumpered_internal_ecp_dma;
        int       n           = 0;

        ecp_dma->Clear();
        for (int i = 0; i < 9; i++) {
            const int j = machine_map_jumpered_ecp_dma(i);
            if ((has_jumpers && ((j == DMA_NONE) || !machine_has_jumpered_ecp_dma(m, j))) || (!has_jumpers && (j != DMA_NONE)))
                continue;
            ecp_dma->Add(machine_get_jumpered_ecp_dma_name(i), j);
            n++;
        }
        ecp_dma->Select(cur_dma);
        ecp_dma->SetEnabled((n != 1) && has_jumpers);

        /* LPT and COM devices: the character devices for each bus. */
        int cur_lpt[PARALLEL_MAX];
        int cur_com[SERIAL_MAX_UI];
        for (int i = 0; i < PARALLEL_MAX; i++) {
            cur_lpt[i] = lpt[i].device->Count() ? lpt[i].device->Value(lpt_ports[i].device) : lpt_ports[i].device;
            lpt[i].device->Clear();
        }
        for (int i = 0; i < SERIAL_MAX_UI; i++) {
            cur_com[i] = com[i].device->Count() ? com[i].device->Value(com_ports[i].device) : com_ports[i].device;
            com[i].device->Clear();
        }

        for (int c = 0;; c++) {
            const device_t   *dev  = char_get_device(c);
            const std::string name = settings_device_name(dev, (dev != nullptr) ? dev->internal_name : nullptr, -1);
            if (name.empty() || (dev == nullptr))
                break;
            if (!device_available(dev) || !device_is_valid(dev, m))
                continue;
            if (dev->flags & DEVICE_LPT) {
                for (int i = 0; i < PARALLEL_MAX; i++)
                    lpt[i].device->Add(name.c_str(), c);
            }
            if (dev->flags & DEVICE_COM) {
                for (int i = 0; i < SERIAL_MAX_UI; i++)
                    com[i].device->Add(name.c_str(), c);
            }
        }

        for (int i = 0; i < PARALLEL_MAX; i++) {
            lpt[i].device->Select(cur_lpt[i]);
            UpdatePort(lpt[i], true, i);
        }
        for (int i = 0; i < SERIAL_MAX_UI; i++) {
            com[i].device->Select(cur_com[i]);
            UpdatePort(com[i], false, i);
        }

        /* USB card, then the hub and mass storage it may give a bus to. */
        const int cur_card = usb_card->Count() ? usb_card->Value(usb_card_type) : usb_card_type;
        usb_card->Clear();
        for (int i = 0; i < usb_card_get_ndev(); i++) {
            const device_t *dev = usb_card_get_device(i);
            if (!device_is_valid(dev, m))
                continue;
            usb_card->Add(settings_device_name(dev, usb_card_get_internal_name(i), 0).c_str(), i);
        }
        usb_card->Select(cur_card);
        UsbCardChanged();
    }

    bool Changed() override
    {
        bool changed = false;

        changed |= (jumpered_internal_ecp_dma != ecp_dma->Value(jumpered_internal_ecp_dma));
        changed |= (usb_card_type != usb_card->Value(usb_card_type));
        changed |= (usb_hub_type != usb_hub->Value(usb_hub_type));
        changed |= (usb_msd_type != usb_msd->Value(usb_msd_type));

        for (int i = 0; i < PARALLEL_MAX; i++) {
            changed |= (lpt_ports[i].enabled != (lpt[i].enable->Value() == B_CONTROL_ON ? 1 : 0));
            changed |= lpt[i].changed;
            changed |= (lpt_ports[i].device != lpt[i].device->Value(lpt_ports[i].device));
        }
        for (int i = 0; i < SERIAL_MAX_UI; i++) {
            changed |= (com_ports[i].enabled != (com[i].enable->Value() == B_CONTROL_ON ? 1 : 0));
            changed |= com[i].changed;
            changed |= (com_ports[i].device != com[i].device->Value(com_ports[i].device));
        }

        return changed;
    }

    void Save() override
    {
        for (int i = 0; i < PARALLEL_MAX; i++) {
            lpt_ports[i].device  = lpt[i].device->Value(lpt_ports[i].device);
            lpt_ports[i].enabled = (lpt[i].enable->Value() == B_CONTROL_ON) ? 1 : 0;
        }
        for (int i = 0; i < SERIAL_MAX_UI; i++) {
            com_ports[i].device  = com[i].device->Value(com_ports[i].device);
            com_ports[i].enabled = (com[i].enable->Value() == B_CONTROL_ON) ? 1 : 0;
        }

        jumpered_internal_ecp_dma = ecp_dma->Value(jumpered_internal_ecp_dma);
        usb_card_type             = usb_card->Value(usb_card_type);
        usb_hub_type              = usb_hub->Value(usb_hub_type);
        usb_msd_type              = usb_msd->Value(usb_msd_type);
    }

    bool Handle(BMessage *message) override
    {
        const uint32 what = message->what;

        for (int i = 0; i < PARALLEL_MAX; i++) {
            if ((what == MSG_LPT_ENABLE + i) || (what == MSG_LPT_DEVICE + i)) {
                UpdatePort(lpt[i], true, i);
                return true;
            }
            if (what == MSG_LPT_CONFIGURE + i) {
                Configure(lpt[i], i);
                return true;
            }
        }
        for (int i = 0; i < SERIAL_MAX_UI; i++) {
            if ((what == MSG_COM_ENABLE + i) || (what == MSG_COM_DEVICE + i)) {
                UpdatePort(com[i], false, i);
                return true;
            }
            if (what == MSG_COM_CONFIGURE + i) {
                Configure(com[i], i);
                return true;
            }
        }

        switch (what) {
            case MSG_USB_CARD:
                UsbCardChanged();
                return true;
            case MSG_USB_MSD:
                UsbMsdChanged();
                return true;
            case MSG_USB_BUS:
                /* Like Qt's usbBusChanged: the pages listing USB devices
                   (all of them, here) refill for the card picked. */
                if (SettingsWindow *window = dynamic_cast<SettingsWindow *>(Window()))
                    window->MachineChanged(CurrentMachine());
                return true;
            case MSG_USB_HDD:
                ShowPage("Hard disks");
                return true;
            case MSG_USB_CDROM:
                ShowPage("Floppy & CD-ROM drives");
                return true;
            case MSG_USB_RDISK:
                ShowPage("Other removable devices");
                return true;
            default:
                return false;
        }
    }

private:
    struct Port {
        BCheckBox *enable;
        Choice    *device;
        BButton   *configure;
        bool       changed; /* its configuration was changed */
    };

    static void AddTab(BTabView *tabs, BView *view, const char *label)
    {
        BTab *tab = new BTab();
        tabs->AddTab(view, tab);
        tab->SetLabel(label);
    }

    /* Qt: updateLptPortControls() / updateComPortControls(). */
    void UpdatePort(Port &port, bool parallel, int i)
    {
        const bool enabled = (port.enable->Value() == B_CONTROL_ON);

        port.device->SetEnabled(enabled && (port.device->Count() > 1) && !port_used_by_network_or_mouse(parallel, i));
        port.configure->SetEnabled(enabled && haiku_device_has_config(char_get_device(port.device->Value(0))));
    }

    void Configure(Port &port, int i)
    {
        const device_t *dev = char_get_device(port.device->Value(0));

        /* The ports attach their device as instance i + 1 (lpt.c,
           serial_host.c). */
        if (dev != nullptr)
            port.changed |= haiku_device_configure(Window(), dev, i + 1);
    }

    /* Qt: on_comboBoxUsbCard_currentIndexChanged(). */
    void UsbCardChanged()
    {
        const int old_card = (usb_card_pending >= 0) ? usb_card_pending : usb_card_type;

        usb_card_pending = usb_card->Value(0);
        FillUsbList(usb_hub, usb_hub_type, usb_hub_get_ndev, usb_hub_get_device, usb_hub_get_internal_name);
        FillUsbList(usb_msd, usb_msd_type, usb_msd_get_ndev, usb_msd_get_device, usb_msd_get_internal_name);
        UsbMsdChanged();

        /* The other pages are refilled from the window's message loop, not
           while it may be telling every page about a new machine. */
        if ((usb_card_pending != old_card) && (Window() != nullptr))
            Window()->PostMessage(MSG_USB_BUS);
    }

    /* The hub and the mass storage are USB devices: offered only while the
       machine, or the USB card chosen, has a USB bus. */
    void FillUsbList(Choice *choice, int saved, int (*ndev)(void), const device_t *(*get_device)(int),
                     const char *(*get_internal_name)(int))
    {
        const int current = choice->Count() ? choice->Value(saved) : saved;

        choice->Clear();
        for (int i = 0; i < ndev(); i++) {
            const device_t *dev = get_device(i);
            if (!device_is_valid(dev, machine_id))
                continue;
            choice->Add(settings_device_name(dev, get_internal_name(i), 0).c_str(), i);
        }
        choice->Select(current);
    }

    /* Qt: on_comboBoxUsbMsd_currentIndexChanged(). */
    void UsbMsdChanged()
    {
        const bool on = (usb_msd->Value(0) > 0);

        rgb_color color = on ? ui_color(B_PANEL_TEXT_COLOR)
                             : tint_color(ui_color(B_PANEL_TEXT_COLOR), B_DISABLED_LABEL_TINT);
        msd_drives->SetFontAndColor(be_plain_font, B_FONT_ALL, &color);
        msd_hdd->SetEnabled(on);
        msd_cdrom->SetEnabled(on);
        msd_removable->SetEnabled(on);
    }

    /* Qt: drivePageRequested. Selects the page by its name in the window's
       page list; does nothing if that page is not there. */
    void ShowPage(const char *name)
    {
        BListView *list = (Window() != nullptr) ? dynamic_cast<BListView *>(Window()->FindView("pages")) : nullptr;
        if (list == nullptr)
            return;
        for (int32 i = 0; i < list->CountItems(); i++) {
            const BStringItem *item = dynamic_cast<BStringItem *>(list->ItemAt(i));
            if ((item != nullptr) && !strcmp(item->Text(), name)) {
                list->Select(i);
                list->ScrollToSelection();
                return;
            }
        }
    }

    int machine_id;

    Choice *ecp_dma;
    Port    lpt[PARALLEL_MAX];
    Port    com[SERIAL_MAX_UI];

    Choice    *usb_card;
    Choice    *usb_hub;
    Choice    *usb_msd;
    BTextView *msd_drives;
    BButton   *msd_hdd;
    BButton   *msd_cdrom;
    BButton   *msd_removable;
};


SettingsPage *
create_ports_page()
{
    return new PortsPage();
}

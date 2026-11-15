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
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <cstring>

#include "haiku_settings_common.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/device.h>
}

/* ---------------------------------------------------------------- Choice */

Choice::Choice(const char *name, const char *label, uint32 what)
    : what(what)
{
    menu  = new BPopUpMenu("", true, true);
    field = new BMenuField(name, label, menu);
}

void
Choice::Clear()
{
    while (BMenuItem *item = menu->RemoveItem((int32) 0))
        delete item;
}

void
Choice::Add(const char *text, int32 value)
{
    BMessage *msg = new BMessage(what);
    msg->AddInt32("value", value);
    menu->AddItem(new BMenuItem(text, msg));
}

void
Choice::AddSeparator()
{
    menu->AddSeparatorItem();
}

void
Choice::Select(int32 value)
{
    BMenuItem *pick = nullptr;
    for (int32 i = 0; i < menu->CountItems(); i++) {
        BMenuItem *item = menu->ItemAt(i);
        if (item->Message() == nullptr)
            continue;
        if (pick == nullptr)
            pick = item;
        if (item->Message()->GetInt32("value", -1) == value) {
            pick = item;
            break;
        }
    }
    if (pick != nullptr)
        pick->SetMarked(true);
    field->SetEnabled(Count() > 1);
}

int32
Choice::Value(int32 fallback) const
{
    BMenuItem *item = menu->FindMarked();
    return ((item != nullptr) && (item->Message() != nullptr)) ? item->Message()->GetInt32("value", fallback) : fallback;
}

bool
Choice::Has(int32 value) const
{
    for (int32 i = 0; i < menu->CountItems(); i++) {
        const BMessage *msg = menu->ItemAt(i)->Message();
        if ((msg != nullptr) && (msg->GetInt32("value", -1) == value))
            return true;
    }
    return false;
}

int32
Choice::Count() const
{
    int32 n = 0;
    for (int32 i = 0; i < menu->CountItems(); i++) {
        if (menu->ItemAt(i)->Message() != nullptr)
            n++;
    }
    return n;
}

void
Choice::SetEnabled(bool enabled)
{
    field->SetEnabled(enabled);
}

/* ------------------------------------------------------- device names */

std::string
settings_device_name(const device_t *dev, const char *internal_name, int bus)
{
    if ((internal_name != nullptr) && !strcmp(internal_name, "none"))
        return "None";
    if ((internal_name != nullptr) && !strcmp(internal_name, "internal"))
        return "Internal device";
    if (dev == nullptr)
        return "";

    char temp[512];
    if (bus == 1) {
        device_get_name(dev, -1, temp);
        const char *bus_name = device_get_bus_name(dev);
        if (bus_name != nullptr)
            return std::string("[") + bus_name + "] " + temp;
        return temp;
    }

    device_get_name(dev, bus, temp);
    std::string name = temp;
    for (const char *suffix : { " (On-Board)", " On-Board" }) {
        const size_t at = name.find(suffix);
        if (at != std::string::npos)
            name.erase(at, strlen(suffix));
    }
    if (const char *m = device_get_machine(dev)) {
        const std::string tail = std::string(" (") + m + ")";
        const size_t      at   = name.find(tail);
        if (at != std::string::npos)
            name.erase(at, tail.size());
    }
    return name;
}

/* ---------------------------------------------------------- SettingsPage */

static int selected_machine = 0;

SettingsPage::SettingsPage(const char *name)
    : BView(name, 0)
{
    SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
}

int
SettingsPage::CurrentMachine()
{
    return selected_machine;
}

/* Set by the Machine page (haiku_settings_machine.cpp). */
void
settings_set_current_machine(int m)
{
    selected_machine = m;
}

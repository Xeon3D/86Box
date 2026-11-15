/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: what the hard disk page and dialog share
 *          (qt_harddrive_common.cpp and qt_settings_bus_tracking.cpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_HARDDRIVE_COMMON_HPP
#define HAIKU_HARDDRIVE_COMMON_HPP

#include <SupportDefs.h>

#include <string>
#include <vector>

class Choice;

#ifndef CHANNEL_NONE
#    define CHANNEL_NONE 0xff
#endif

/* The channels taken on each kind of bus, like Qt's SettingsBusTracking.
   Channels are the list's: on bus LPT the port (the bridge does not
   matter). IDE and ATAPI share their channels. */
class BusTracking {
public:
    BusTracking();

    void Clear();
    void Track(bool set, int bus, int channel);
    bool InUse(int bus, int channel) const;
    std::vector<int> InUse(int bus) const;

    /* The CD-ROM, removable, MO and tape drives as saved in the core (the
       pages that edit them keep their own copies). */
    void AddSavedDrives();

    /* The channel a new drive on this bus gets: like next_free_*() in Qt,
       the list channel (on LPT the port's first bridge), or CHANNEL_NONE. */
    uint8 NextFree(int bus) const;

private:
    int Slot(int bus) const;

    enum { SLOTS = 6, CHANNELS = 160 };
    uint8 use[SLOTS][CHANNELS];
};

namespace Harddrives {

void        PopulateBuses(Choice *choice);
void        PopulateSpeeds(Choice *choice, int bus);
/* channel: on bus LPT, port | bridge << 2. Disabled entries are channels
   in use; for IDE, the controllers left without boards follow. */
void        PopulateBusChannels(Choice *choice, int bus, const BusTracking &tracking);
std::string BusChannelName(uint8 bus, uint8 channel);
std::string LptBridgeName(int bridge);
bool        LptPortEnabled(int port);

}

#endif

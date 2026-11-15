/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: what the drive pages of Settings share
 *          (qt_settings_bus_tracking.cpp and qt_harddrive_common.cpp).
 *
 *          The bus tracking says which channel of each bus a drive has, for
 *          the values on the pages (not yet saved): the Hard disks, Floppy &
 *          CD-ROM and Other removable pages share one. Each page acquires it
 *          in its constructor and releases it in its destructor; the first
 *          to acquire it fills it from the core's drives, so it holds what
 *          is saved until a page changes a drive.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_SETTINGS_DRIVES_HPP
#define HAIKU_SETTINGS_DRIVES_HPP

#include <SupportDefs.h>

#include <string>
#include <vector>

#include "haiku_settings_common.hpp"

struct bus_owner_t;
struct ide_conflict_t;

#define DEV_HDD      0x01
#define DEV_CDROM    0x02
#define DEV_RDISK    0x04
#define DEV_MO       0x08
#define DEV_TAPE     0x10
#define DEV_FDD      0x20
#define DEV_LPT      0x40

#define CHANNEL_NONE 0xff

/* SettingsBusTracking. */
class BusTracking {
public:
    std::vector<int> BusChannelsInUse(int bus) const;

    /* These return CHANNEL_NONE if none is free. */
    uint8 NextFreeHitachiChannel() const;
    uint8 NextFreeMkeChannel() const;
    uint8 NextFreeMfmChannel() const;
    uint8 NextFreeEsdiChannel() const;
    uint8 NextFreeXtaChannel() const;
    uint8 NextFreeIdeChannel() const;
    uint8 NextFreeScsiId() const;
    uint8 NextFreeFdcUnit() const;
    uint8 NextFreeLptPort() const;

    /* set: 0 = clear the device from the tracking, 1 = set it. */
    void DeviceTrack(int set, uint8 dev_type, int bus, int channel);

private:
    uint64 hitachi_tracking { 0 };
    uint64 mke_tracking { 0 };
    uint64 mfm_tracking { 0 };
    uint64 esdi_tracking { 0 };
    uint64 xta_tracking { 0 };
    uint64 ide_tracking[4] { 0, 0, 0, 0 };
    uint64 scsi_tracking[32] {};
    uint64 fdc_tracking { 0 };
    uint64 lpt_tracking { 0 };
    bool   philips_tracking { false };
    bool   cm100_tracking { false };
    bool   sony_tracking { false };
    bool   mitsumi_tracking { false };
};

BusTracking *settings_bus_tracking_acquire();
void         settings_bus_tracking_release();

/* Harddrives:: */
enum { LptBridgesNone = 0, LptBridgesHdd = 1, LptBridgesCdrom = 2 };

void        drives_populate_bus_channels(Choice *channel, int bus, const BusTracking *sbt, int lpt_bridges = LptBridgesNone);
std::string drives_bus_channel_name(uint8 bus, uint8 channel);
std::string drives_lpt_bridge_name(int bridge);
bool        drives_lpt_port_enabled(int port);
int         drives_ide_plan(bus_owner_t *owners, ide_conflict_t *conflicts = nullptr, int *conflict_count = nullptr);
int         drives_scsi_plan(bus_owner_t *owners);

/* What a Choice lacks for these pages: its items by row, as a combo box
   index, and enabling one item. Selecting keeps the field's enabled state
   (Choice::Select() sets it from the item count). */
int32 choice_index(const Choice *choice);
int32 choice_value_at(const Choice *choice, int32 row);
void  choice_pick(Choice *choice, int32 value);
void  choice_pick_index(Choice *choice, int32 row); /* -1 or out of range: none */
void  choice_set_item_enabled(Choice *choice, int32 row, bool enabled);

#endif

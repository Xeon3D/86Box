/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the device configuration dialog, like
 *          the Qt DeviceConfig (qt_deviceconfig.cpp). It shows one control
 *          per entry of a device's device_config_t list and writes the
 *          values to the configuration with config_set_*().
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_DEVICECONFIG_HPP
#define HAIKU_DEVICECONFIG_HPP

class BWindow;
struct _device_;

/* True if the device has anything to configure (Qt: device_has_config). */
bool haiku_device_has_config(const struct _device_ *device);

/* Shows the dialog for this device (instance as for device_set_context /
   the "#n" section suffix; 0 for single-instance devices) and waits until
   it is closed, keeping `parent` redrawn meanwhile. Returns true when the
   user pressed OK and a value changed. Call it from the parent window's
   thread, like BAlert::Go(). */
bool haiku_device_configure(BWindow *parent, const struct _device_ *device, int instance = 0);

#endif

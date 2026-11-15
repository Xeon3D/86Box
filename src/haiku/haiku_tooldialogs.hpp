/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Tools and View dialogs (Qt's
 *          Preferences, CGASettingsDialog and MCADeviceList).
 *
 *          Each one runs modally from the main window's thread and returns
 *          when it is closed.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_TOOLDIALOGS_HPP
#define HAIKU_TOOLDIALOGS_HPP

class BWindow;

/* Preferences (Qt Preferences): General, Emulator, Input and Key bindings.
   OK writes the global configuration (config_save_global()). */
void haiku_preferences_dialog(BWindow *parent);

/* CGA composite settings (Qt CGASettingsDialog): changes are shown live,
   Cancel restores; the machine configuration is saved afterwards. */
void haiku_cga_settings_dialog(BWindow *parent);

/* MCA devices (Qt MCADeviceList): the cards in the MCA slots. */
void haiku_mca_devices_dialog(BWindow *parent);

#endif

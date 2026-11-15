/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: keyboard grab stopgap (see the .cpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_KEYBOARDGRAB_HPP
#define HAIKU_KEYBOARDGRAB_HPP

/* Grab (on) or release (off) the keyboard for the main window while the
   mouse is captured: with BWindow::SetKeyboardGrab() on a Haiku that has
   it and the user chose it, otherwise by unassigning the Command modifier
   keys for the time being. Call on the main window's thread. */
void haiku_keyboard_grab(bool on);

/* Whether this Haiku has BWindow::SetKeyboardGrab() (a patched system). */
bool haiku_keyboard_grab_system_available(void);

/* The user's choice (Preferences -> Input), saved in
   ~/config/settings/86Box/haiku-frontend: use the system's keyboard grab
   when there is one, instead of unassigning the Command keys. */
bool haiku_keyboard_grab_use_system(void);
void haiku_keyboard_grab_set_use_system(bool use);

/* At start-up: restore the Command keys if a previous run died holding
   them. */
void haiku_keyboard_restore_after_crash(void);

#endif

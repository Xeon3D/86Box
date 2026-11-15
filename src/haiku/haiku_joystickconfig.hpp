/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the joystick configuration dialog, like
 *          the Qt JoystickConfiguration (qt_joystickconfiguration.cpp). It
 *          picks the host joystick for one emulated joystick and maps its
 *          axes, buttons and POV hats.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_JOYSTICKCONFIG_HPP
#define HAIKU_JOYSTICKCONFIG_HPP

class BWindow;

/* Shows the dialog for joystick `joystick_nr` of game port `gameport_nr`,
   emulating joystick type `type` (an index for joystick_get_name()), and
   waits until it is closed, keeping `parent` redrawn meanwhile. On OK the
   choices are written to joystick_state[gameport_nr][joystick_nr] (as Qt's
   updateJoystickConfig() does) and true is returned. Call it from the
   parent window's thread, like BAlert::Go(). */
bool haiku_joystick_configure(BWindow *parent, int type, int gameport_nr, int joystick_nr);

#endif

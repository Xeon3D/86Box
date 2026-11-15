/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: what the Display Settings page tells the
 *          other pages.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_SETTINGS_DISPLAY_HPP
#define HAIKU_SETTINGS_DISPLAY_HPP

/* Qt: SettingsDisplay::isLightPenUsable(). True if the video cards selected
   on the Display page (not yet saved) include a CGA or Plantronics card,
   which a light pen can be attached to. Without a Display page (the window
   is not open) it answers for the saved cards. */
bool settings_display_light_pen_usable();

#endif

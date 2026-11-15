/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the main window's small dialogs (Qt's
 *          SoundGain, SpecifyDimensions and About) and desktop helpers.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_DIALOGS_HPP
#define HAIKU_DIALOGS_HPP

class BWindow;

/* Runs `dialog` (already set up, not shown) until it quits, keeping `parent`
   redrawn, like BAlert::Go(). The dialog must release `done` (see
   haiku_modal_done()) from its destructor. */
void haiku_run_modal(BWindow *dialog, BWindow *parent, int done_sem);

/* Sound gain (Qt SoundGain): a slider that changes sound_gain live; OK
   saves, Cancel restores. */
void haiku_sound_gain_dialog(BWindow *parent);

/* Specify dimensions (Qt SpecifyDimensions). Returns false on Cancel;
   otherwise the chosen size and whether to lock the window to it. */
bool haiku_specify_dimensions_dialog(BWindow *parent, int *width, int *height, bool *lock);

/* About 86Box (Qt About). */
void haiku_about_dialog(BWindow *parent);

/* Open a URL in the preferred browser, or a folder in Tracker (created if
   missing). */
void haiku_open_url(const char *url);
void haiku_open_folder(const char *path);

#endif

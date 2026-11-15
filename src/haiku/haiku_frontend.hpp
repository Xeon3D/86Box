/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku (Be API) frontend: shared declarations.
 *
 *          The frontend mirrors the structure of the Qt one:
 *            haiku_main.cpp         <-> qt_main.cpp
 *            haiku_mainwindow.cpp   <-> qt_mainwindow.cpp
 *            haiku_rendererview.cpp <-> qt_softwarerenderer.cpp / qt_rendererstack.cpp
 *            haiku_ui.cpp           <-> qt_ui.cpp
 *            haiku_platform.cpp     <-> the parts of qt_platform.cpp not shared
 *                                       with src/unix
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_FRONTEND_HPP
#define HAIKU_FRONTEND_HPP

#include <SupportDefs.h>

class MainWindow;
class RendererView;

/* The main window, or nullptr before it exists and after it is gone. */
extern MainWindow *main_window;

/* Messages handled by MainWindow. */
enum : uint32 {
    MSG_PAUSE           = 'paus',
    MSG_HARD_RESET      = 'hrst',
    MSG_CTRL_ALT_DEL    = 'cad ',
    MSG_CTRL_ALT_ESC    = 'cae ',
    MSG_NMI             = 'nmi ',
    MSG_ACPI_SHUTDOWN   = 'acpi',
    MSG_KBD_CAPTURE     = 'kcap',
    MSG_RCTRL_IS_LALT   = 'rclt',
    MSG_SCALE           = 'scal', /* int32 "scale" */
    MSG_FILTER          = 'filt', /* int32 "filter" */
    MSG_HIDE_STATUS_BAR = 'hsbr',
    MSG_SCREENSHOT      = 'shot',
    MSG_ABOUT           = 'abou',
    MSG_RESIZE          = 'rsiz', /* int32 "w", "h" */
    MSG_OSD_TICK        = 'osdt', /* redraw the OSD (RendererView) */
    MSG_EMU_STATUS      = 'emus', /* string "text" */
    MSG_STATUS_TEXT     = 'sbtx', /* string "text" */
    MSG_TITLE           = 'titl', /* string "text" */
    MSG_FRAME           = 'fram',
    MSG_MOUSE_CAPTURE   = 'mcap', /* bool "on" */
    MSG_SB_PANES        = 'sbpn', /* rebuild the status-bar drive icons */
    MSG_SETTINGS        = 'sets', /* open the Settings window */
    MSG_SETTINGS_CLOSED = 'setc', /* bool "apply": the pages were saved */
    MSG_MUTE              = 'mwmu', /* toggle sound_muted */
    MSG_SOUND_GAIN        = 'mwsg', /* open the Sound gain dialog */
    MSG_FORCE_INTERPRETER = 'mwfi', /* toggle cpu_force_interpreter */
    MSG_MEDIA_OPEN      = 'mopn', /* int32 "type", "id" */
    MSG_MEDIA_EJECT     = 'mejc', /* int32 "type", "id" */
    MSG_ONE_SECOND      = '1sec',
};

/* Media types for MSG_MEDIA_OPEN / MSG_MEDIA_EJECT. */
enum {
    MEDIA_FLOPPY = 0,
    MEDIA_CDROM  = 1,
};

/* The blit hook given to video_setblit(); runs on the video blit thread. */
extern "C" void haiku_blit(int x, int y, int w, int h, int monitor_index);

/* Sets the window title (haiku_ui.cpp); with nullptr returns it. ui_notify()
   in src/unix/sdl_plat.c uses it to mark the title while paused, so the
   main window sets its title through it too. */
extern "C" char *ui_window_title(char *str);

#endif

/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the memory viewer and debugger window
 *          (Qt's MemoryViewer, qt_memoryviewer.cpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_MEMORYVIEWER_HPP
#define HAIKU_MEMORYVIEWER_HPP

class BWindow;

/* Opens the memory viewer, or activates it if it is already open. There is
   only ever one; it runs in its own window and thread, so keys typed into it
   never reach the guest. While it is open the debugger is attached, which
   turns the dynamic recompiler off. Call from the main window's thread. */
void haiku_memory_viewer_open(BWindow *main_window);

#endif

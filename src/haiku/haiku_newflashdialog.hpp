/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the New USB flash drive dialog, like the
 *          Qt NewFlashDialog (qt_newflashdialog.cpp). It creates the raw
 *          image of a flash drive, sized in MB or GB and formatted FAT16
 *          or FAT32, or left blank.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_NEWFLASHDIALOG_HPP
#define HAIKU_NEWFLASHDIALOG_HPP

#include <string>

class BWindow;

/* Shows the dialog and waits until it is closed, keeping `parent` redrawn
   meanwhile; call it from the parent window's thread, like BAlert::Go().
   Returns true once an image was created; `file_name` gets its path. */
bool haiku_new_flash_dialog(BWindow *parent, std::string *file_name);

#endif

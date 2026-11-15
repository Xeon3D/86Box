/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the storage controllers selected on the
 *          Storage controllers Settings page, for the other pages (like
 *          the Qt Settings::currentHdc() / currentScsiCard()).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_SETTINGS_STORAGE_HPP
#define HAIKU_SETTINGS_STORAGE_HPP

/* The hard disk controller in slot i (0 .. HDC_MAX - 1) as selected on the
   page (not yet saved), or hdc_current[i] while the page does not exist. */
int settings_current_hdc(int i);

/* The SCSI card in slot i (0 .. SCSI_CARD_MAX - 1) as selected on the page,
   or scsi_card_current[i] while the page does not exist. */
int settings_current_scsi_card(int i);

#endif

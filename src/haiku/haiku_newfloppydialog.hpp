/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the New Image dialog, like the Qt
 *          NewFloppyDialog (qt_newfloppydialog.cpp). It creates a blank
 *          floppy, removable disk, MO or tape image, with a progress bar
 *          while the file is written.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_NEWFLOPPYDIALOG_HPP
#define HAIKU_NEWFLOPPYDIALOG_HPP

#include <FilePanel.h>

#include <string>
#include <vector>

class BWindow;

enum class NewImageMedia {
    Floppy,
    RDisk,
    Mo,
    Tape,
};

/* Shows the dialog and waits until it is closed, keeping `parent` redrawn
   meanwhile; call it from the parent window's thread, like BAlert::Go().
   drive_type (the drive's type, or -1) limits the media offered, as in Qt.
   Returns true once an image was created: `file_name` gets its path and
   `media_type` the index of the medium in the core's table (disk_sizes,
   rdisk_types, mo_types or tape_types). */
bool haiku_new_floppy_dialog(BWindow *parent, NewImageMedia type, int drive_type,
                             std::string *file_name, int *media_type);

/* A file panel filter from Qt-style extension patterns ("86f", "im?",
   "*fd?"): a file is shown when its name matches "*.<pattern>", ignoring
   case. Folders are always shown; no patterns shows everything. */
class ImageRefFilter : public BRefFilter {
public:
    void SetPatterns(const std::vector<std::string> &patterns);
    bool Filter(const entry_ref *ref, BNode *node, struct stat_beos *st, const char *filetype) override;

private:
    std::vector<std::string> globs;
};

#endif

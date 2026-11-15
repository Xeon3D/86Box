/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: where 86Box keeps its files, the
 *          plat_get_*_dir() / plat_init_*_paths() functions of
 *          qt_platform.cpp with Haiku's standard folders in place of
 *          QStandardPaths:
 *            global configuration  ~/config/settings/86Box
 *            global data           ~/config/non-packaged/data/86Box
 *            virtual machines      ~/config/non-packaged/data/86Box/Virtual Machines
 *            ROMs and assets       86Box/roms and 86Box/assets in each of
 *                                  the user and system data folders
 *          In portable mode everything is next to the executable, as in Qt.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Directory.h>
#include <FindDirectory.h>
#include <Path.h>

#include <cstring>
#include <sys/stat.h>

extern "C" {
#include <86box/86box.h>
#include <86box/mem.h>
#include <86box/path.h>
#include <86box/plat.h>
#include <86box/rom.h>
#include <86box/version.h>
}

/* <folder>/86Box, created if needed; false if Haiku has no such folder. */
static bool
app_folder(directory_which which, char *outbuf, size_t len)
{
    BPath path;
    if (find_directory(which, &path, true) != B_OK)
        return false;
    path.Append(EMU_NAME);
    create_directory(path.Path(), 0755);
    strlcpy(outbuf, path.Path(), len);
    return true;
}

extern "C" {

void
plat_get_global_config_dir(char *outbuf, const size_t len)
{
    if (portable_mode || !app_folder(B_USER_SETTINGS_DIRECTORY, outbuf, len))
        strlcpy(outbuf, exe_path, len);
    path_slash(outbuf);
}

void
plat_get_global_data_dir(char *outbuf, const size_t len)
{
    if (portable_mode || !app_folder(B_USER_NONPACKAGED_DATA_DIRECTORY, outbuf, len))
        strlcpy(outbuf, exe_path, len);
    path_slash(outbuf);
}

void
plat_get_temp_dir(char *outbuf, const uint8_t len)
{
    BPath path;
    if (find_directory(B_SYSTEM_TEMP_DIRECTORY, &path) == B_OK)
        strlcpy(outbuf, path.Path(), len);
    else
        strlcpy(outbuf, "/tmp", len);
    path_slash(outbuf);
}

void
plat_get_vmm_dir(char *outbuf, const size_t len)
{
    char base[1024];

    if (portable_mode)
        strlcpy(base, exe_path, sizeof(base));
    else
        plat_get_global_data_dir(base, sizeof(base));

    path_append_filename(outbuf, base, VMM_PATH);
    path_slash(outbuf);
}

/* Qt adds <GenericDataLocation>/86Box/roms for each data location; on Haiku
   those are the user and system data folders, packaged and not. */
void
plat_init_rom_paths(void)
{
    static const directory_which folders[] = {
        B_USER_NONPACKAGED_DATA_DIRECTORY,
        B_USER_DATA_DIRECTORY,
        B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
        B_SYSTEM_DATA_DIRECTORY,
    };

    for (directory_which which : folders) {
        BPath path;
        if (find_directory(which, &path) != B_OK)
            continue;
        path.Append(EMU_NAME "/roms");
        rom_add_path(path.Path());
    }
}

void
plat_init_asset_paths(void)
{
    static const directory_which folders[] = {
        B_USER_NONPACKAGED_DATA_DIRECTORY,
        B_USER_DATA_DIRECTORY,
        B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
        B_SYSTEM_DATA_DIRECTORY,
    };

    for (directory_which which : folders) {
        BPath path;
        if (find_directory(which, &path) != B_OK)
            continue;
        path.Append(EMU_NAME "/assets");
        asset_add_path(path.Path());
    }
}

} /* extern "C" */

/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the machine UUID helpers of the Qt util
 *          namespace (qt_util.cpp), used to notice a machine that was moved
 *          or copied.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_UTIL_HPP
#define HAIKU_UTIL_HPP

#include <string>

/* The UUID Qt derives for a configuration file: a version 5 UUID (nil
   namespace) of its folder's canonical path with a trailing slash. */
std::string haiku_uuid_for_config(const char *config_path);

/* util::compareUuid(): true if the stored uuid matches this location (or
   was missing/invalid, or there is no network card, and was updated). */
bool haiku_compare_uuid(void);

/* util::storeCurrentUuid() and util::generateNewMacAdresses(). */
void haiku_store_current_uuid(void);
void haiku_generate_new_mac_addresses(void);

#endif

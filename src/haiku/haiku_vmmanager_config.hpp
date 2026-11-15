/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the VM manager's configuration
 *          (qt_vmmanager_config.cpp): vmm.ini in the global configuration
 *          directory, written in the QSettings INI format so the Qt and
 *          native managers share it, and a reader for machine
 *          configuration files.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_VMMANAGER_CONFIG_HPP
#define HAIKU_VMMANAGER_CONFIG_HPP

#include <string>
#include <utility>
#include <vector>

/* An INI file kept in order. With `qsettings` the values are escaped and
   unescaped like QSettings does (quotes, backslash escapes, lists);
   otherwise (86Box's own files) they are taken as they are, trimmed. */
class VmmIni {
public:
    typedef std::vector<std::pair<std::string, std::string>> keys_t;

    explicit VmmIni(bool qsettings);

    bool Load(const std::string &path);
    bool Save(const std::string &path) const;

    std::vector<std::string> Sections() const;
    const keys_t            *Keys(const std::string &section) const;
    bool                     Has(const std::string &section, const std::string &key) const;
    std::string              Get(const std::string &section, const std::string &key, const std::string &fallback = "") const;
    void                     Set(const std::string &section, const std::string &key, const std::string &value);
    void                     Remove(const std::string &section, const std::string &key);
    void                     RemoveSection(const std::string &section);

private:
    struct Section {
        std::string name;
        keys_t      keys;
    };

    Section *Find(const std::string &section);

    std::vector<Section> sections;
    bool                 qsettings;
};

/* VMManagerConfig: one section of vmm.ini ("General" for the manager's own
   settings, a machine's UUID for that machine). Every call reads the file
   and every change writes it, so other managers' changes are kept. */
class VmmConfig {
public:
    explicit VmmConfig(const std::string &section = "General");

    std::string GetString(const char *key) const;
    /* An empty value removes the key, as in Qt. */
    void SetString(const char *key, const std::string &value) const;
    void Remove(const char *key) const;

    static std::string Path();

    /* Every machine section: UUID and "config_file". */
    static std::vector<std::pair<std::string, std::string>> Machines();
    static void RemoveSection(const std::string &section);

    /* VMManagerConfig::migratePreferences(). */
    static void MigratePreferences();

private:
    std::string section;
};

#endif

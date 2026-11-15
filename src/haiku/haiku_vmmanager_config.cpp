/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the VM manager's configuration
 *          (qt_vmmanager_config.cpp) and INI files.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "haiku_vmmanager_config.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/config.h>
}

/* ------------------------------------------------------------ escaping */

static std::string
trim(const std::string &text)
{
    size_t start = 0;
    size_t end   = text.size();
    while ((start < end) && ((text[start] == ' ') || (text[start] == '\t') || (text[start] == '\r')))
        start++;
    while ((end > start) && ((text[end - 1] == ' ') || (text[end - 1] == '\t') || (text[end - 1] == '\r')))
        end--;
    return text.substr(start, end - start);
}

static int
hex_value(char c)
{
    if ((c >= '0') && (c <= '9'))
        return c - '0';
    if ((c >= 'a') && (c <= 'f'))
        return c - 'a' + 10;
    if ((c >= 'A') && (c <= 'F'))
        return c - 'A' + 10;
    return -1;
}

static void
append_utf8(std::string &out, unsigned int cp)
{
    if (cp < 0x80)
        out += (char) cp;
    else if (cp < 0x800) {
        out += (char) (0xc0 | (cp >> 6));
        out += (char) (0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += (char) (0xe0 | (cp >> 12));
        out += (char) (0x80 | ((cp >> 6) & 0x3f));
        out += (char) (0x80 | (cp & 0x3f));
    } else {
        out += (char) (0xf0 | ((cp >> 18) & 0x07));
        out += (char) (0x80 | ((cp >> 12) & 0x3f));
        out += (char) (0x80 | ((cp >> 6) & 0x3f));
        out += (char) (0x80 | (cp & 0x3f));
    }
}

/* QSettings' iniUnescapedKey(): %XX escapes, and '\' for '/'. */
static std::string
unescape_key(const std::string &key)
{
    std::string out;
    for (size_t i = 0; i < key.size(); i++) {
        if ((key[i] == '%') && ((i + 2) < key.size()) && (hex_value(key[i + 1]) >= 0) && (hex_value(key[i + 2]) >= 0)) {
            out += (char) ((hex_value(key[i + 1]) << 4) | hex_value(key[i + 2]));
            i += 2;
        } else if (key[i] == '\\')
            out += '/';
        else
            out += key[i];
    }
    return out;
}

/* QSettings' iniUnescapedStringList(); a list is joined with ", ", as the
   Qt manager does when it reads one. */
static std::string
unescape_value(const std::string &raw)
{
    const std::string        value = trim(raw);
    std::vector<std::string> parts;
    std::string              current;
    bool                     quoted     = false;
    bool                     had_quotes = false;

    if (value.compare(0, 2, "@@") == 0)
        return value.substr(1);

    for (size_t i = 0; i < value.size(); i++) {
        const char c = value[i];
        if (c == '"') {
            quoted     = !quoted;
            had_quotes = true;
            continue;
        }
        if ((c == ',') && !quoted) {
            parts.push_back(had_quotes ? current : trim(current));
            current.clear();
            had_quotes = false;
            continue;
        }
        if ((c != '\\') || ((i + 1) >= value.size())) {
            current += c;
            continue;
        }

        const char e = value[++i];
        switch (e) {
            case 'a':
                current += '\a';
                break;
            case 'b':
                current += '\b';
                break;
            case 'f':
                current += '\f';
                break;
            case 'n':
                current += '\n';
                break;
            case 'r':
                current += '\r';
                break;
            case 't':
                current += '\t';
                break;
            case 'v':
                current += '\v';
                break;
            case 'x':
                {
                    unsigned int cp = 0;
                    while (((i + 1) < value.size()) && (hex_value(value[i + 1]) >= 0))
                        cp = (cp << 4) | hex_value(value[++i]);
                    append_utf8(current, cp);
                    break;
                }
            default:
                if ((e >= '0') && (e <= '7')) {
                    unsigned int cp = e - '0';
                    while (((i + 1) < value.size()) && (value[i + 1] >= '0') && (value[i + 1] <= '7'))
                        cp = (cp << 3) | (value[++i] - '0');
                    append_utf8(current, cp);
                } else
                    current += e;
                break;
        }
    }
    parts.push_back(had_quotes ? current : trim(current));

    std::string joined;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0)
            joined += ", ";
        joined += parts[i];
    }
    return joined;
}

/* QSettings' iniEscapedString() (UTF-8). */
static std::string
escape_value(const std::string &value)
{
    std::string out;
    bool        needs_quotes        = false;
    bool        escape_next_if_hex = false;

    if (!value.empty() && (value[0] == '@'))
        out += '@';

    for (const unsigned char c : value) {
        if ((c == ';') || (c == ',') || (c == '='))
            needs_quotes = true;
        if (escape_next_if_hex && (hex_value((char) c) >= 0)) {
            char temp[8];
            snprintf(temp, sizeof(temp), "\\x%x", c);
            out += temp;
            continue;
        }
        escape_next_if_hex = false;

        switch (c) {
            case '\0':
                out += "\\0";
                escape_next_if_hex = true;
                break;
            case '\a':
                out += "\\a";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\v':
                out += "\\v";
                break;
            case '"':
            case '\\':
                out += '\\';
                out += (char) c;
                break;
            default:
                if (c <= 0x1f) {
                    char temp[8];
                    snprintf(temp, sizeof(temp), "\\x%x", c);
                    out += temp;
                    escape_next_if_hex = true;
                } else
                    out += (char) c;
                break;
        }
    }

    if (needs_quotes || (!out.empty() && ((out[0] == ' ') || (out[out.size() - 1] == ' '))))
        out = "\"" + out + "\"";
    return out;
}

/* QSettings' iniEscapedKey(). */
static std::string
escape_key(const std::string &key)
{
    std::string out;
    for (const unsigned char c : key) {
        if (c == '/')
            out += '\\';
        else if (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || (c == '_')
                 || (c == '-') || (c == '.') || (c == ' ') || (c >= 0x80))
            out += (char) c;
        else {
            char temp[8];
            snprintf(temp, sizeof(temp), "%%%02X", c);
            out += temp;
        }
    }
    return out;
}

/* ------------------------------------------------------------- VmmIni */

VmmIni::VmmIni(bool qsettings)
    : qsettings(qsettings)
{
}

bool
VmmIni::Load(const std::string &path)
{
    sections.clear();

    std::ifstream file(path);
    if (!file.is_open())
        return false;

    Section    *current = nullptr;
    std::string line;
    bool        first = true;
    while (std::getline(file, line)) {
        /* Skip a UTF-8 byte order mark. */
        if (first && (line.compare(0, 3, "\xef\xbb\xbf") == 0))
            line.erase(0, 3);
        first = false;

        const std::string text = trim(line);
        if (text.empty() || (text[0] == ';') || (text[0] == '#'))
            continue;

        if (text[0] == '[') {
            const size_t close = text.find(']');
            std::string  name  = text.substr(1, (close == std::string::npos) ? std::string::npos : close - 1);
            if (qsettings)
                name = unescape_key(name);
            current = Find(name);
            if (current == nullptr) {
                sections.push_back({ name, {} });
                current = &sections.back();
            }
            continue;
        }

        const size_t equals = text.find('=');
        if (equals == std::string::npos)
            continue;
        std::string key   = trim(text.substr(0, equals));
        std::string value = text.substr(equals + 1);
        if (qsettings) {
            key   = unescape_key(key);
            value = unescape_value(value);
        } else
            value = trim(value);

        if (current == nullptr) {
            sections.push_back({ "General", {} });
            current = &sections.back();
        }
        bool replaced = false;
        for (auto &pair : current->keys) {
            if (pair.first == key) {
                pair.second = value;
                replaced    = true;
                break;
            }
        }
        if (!replaced)
            current->keys.emplace_back(key, value);
    }
    return true;
}

bool
VmmIni::Save(const std::string &path) const
{
    const std::string temp_path = path + ".tmp";
    {
        std::ofstream file(temp_path, std::ios::trunc);
        if (!file.is_open())
            return false;

        bool first = true;
        for (const auto &section : sections) {
            if (section.keys.empty())
                continue;
            if (!first)
                file << "\n";
            first = false;
            file << "[" << (qsettings ? escape_key(section.name) : section.name) << "]\n";
            for (const auto &pair : section.keys) {
                if (qsettings)
                    file << escape_key(pair.first) << "=" << escape_value(pair.second) << "\n";
                else
                    file << pair.first << " = " << pair.second << "\n";
            }
        }
        if (!file.good())
            return false;
    }
    if (rename(temp_path.c_str(), path.c_str()) != 0) {
        remove(temp_path.c_str());
        return false;
    }
    return true;
}

VmmIni::Section *
VmmIni::Find(const std::string &section)
{
    for (auto &entry : sections) {
        if (entry.name == section)
            return &entry;
    }
    return nullptr;
}

std::vector<std::string>
VmmIni::Sections() const
{
    std::vector<std::string> names;
    for (const auto &entry : sections)
        names.push_back(entry.name);
    return names;
}

const VmmIni::keys_t *
VmmIni::Keys(const std::string &section) const
{
    for (const auto &entry : sections) {
        if (entry.name == section)
            return &entry.keys;
    }
    return nullptr;
}

bool
VmmIni::Has(const std::string &section, const std::string &key) const
{
    const keys_t *keys = Keys(section);
    if (keys == nullptr)
        return false;
    for (const auto &pair : *keys) {
        if (pair.first == key)
            return true;
    }
    return false;
}

std::string
VmmIni::Get(const std::string &section, const std::string &key, const std::string &fallback) const
{
    const keys_t *keys = Keys(section);
    if (keys != nullptr) {
        for (const auto &pair : *keys) {
            if (pair.first == key)
                return pair.second;
        }
    }
    return fallback;
}

void
VmmIni::Set(const std::string &section, const std::string &key, const std::string &value)
{
    Section *entry = Find(section);
    if (entry == nullptr) {
        sections.push_back({ section, {} });
        entry = &sections.back();
    }
    for (auto &pair : entry->keys) {
        if (pair.first == key) {
            pair.second = value;
            return;
        }
    }
    entry->keys.emplace_back(key, value);
}

void
VmmIni::Remove(const std::string &section, const std::string &key)
{
    Section *entry = Find(section);
    if (entry == nullptr)
        return;
    for (auto it = entry->keys.begin(); it != entry->keys.end(); ++it) {
        if (it->first == key) {
            entry->keys.erase(it);
            return;
        }
    }
}

void
VmmIni::RemoveSection(const std::string &section)
{
    for (auto it = sections.begin(); it != sections.end(); ++it) {
        if (it->name == section) {
            sections.erase(it);
            return;
        }
    }
}

/* ---------------------------------------------------------- VmmConfig */

VmmConfig::VmmConfig(const std::string &section)
    : section(section.empty() ? std::string("General") : section)
{
}

std::string
VmmConfig::Path()
{
    char dir[1024] = { 0 };
    plat_get_global_config_dir(dir, sizeof(dir));
    std::string path = dir;
    if (!path.empty() && (path[path.size() - 1] != '/'))
        path += '/';
    return path + "vmm.ini";
}

std::string
VmmConfig::GetString(const char *key) const
{
    VmmIni ini(true);
    ini.Load(Path());
    /* VMManagerConfig::generalDefaults */
    const char *fallback = ((section == "General") && !strcmp(key, "hide_tool_bar")) ? "0" : "";
    return ini.Get(section, key, fallback);
}

void
VmmConfig::SetString(const char *key, const std::string &value) const
{
    VmmIni ini(true);
    ini.Load(Path());
    if (value.empty())
        ini.Remove(section, key);
    else
        ini.Set(section, key, value);
    ini.Save(Path());
}

void
VmmConfig::Remove(const char *key) const
{
    SetString(key, "");
}

std::vector<std::pair<std::string, std::string>>
VmmConfig::Machines()
{
    std::vector<std::pair<std::string, std::string>> machines;
    VmmIni                                           ini(true);
    if (!ini.Load(Path()))
        return machines;
    for (const auto &name : ini.Sections()) {
        if (name.empty() || (name == "General"))
            continue;
        machines.emplace_back(name, ini.Get(name, "config_file"));
    }
    return machines;
}

void
VmmConfig::RemoveSection(const std::string &section)
{
    VmmIni ini(true);
    if (!ini.Load(Path()))
        return;
    ini.RemoveSection(section);
    ini.Save(Path());
}

void
VmmConfig::MigratePreferences()
{
    const struct {
        const char *key;
        int        *value;
        int         default_value;
    } preferences[] = {
        { "update_check",    &vmm_update_check,    1 },
        { "regex_search",    &vmm_regex_search,    0 },
        { "window_remember", &vmm_window_remember, 0 },
        { "delete_to_trash", &vmm_delete_to_trash, 0 },
    };

    VmmIni ini(true);
    if (!ini.Load(Path()))
        return;

    bool migrated = false;
    for (const auto &preference : preferences) {
        if (!ini.Has("General", preference.key))
            continue;
        if (*preference.value == preference.default_value)
            *preference.value = atoi(ini.Get("General", preference.key).c_str());
        /* Only this preference is saved, keeping the rest of the file. */
        config_save_global_setting("Manager", preference.key);
        ini.Remove("General", preference.key);
        migrated = true;
    }
    if (migrated)
        ini.Save(Path());
}

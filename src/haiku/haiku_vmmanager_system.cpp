/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: one virtual machine of the VM manager
 *          (qt_vmmanager_system.cpp and qt_vmmanager_serversocket.cpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Message.h>
#include <Roster.h>
#include <image.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <regex>
#include <set>
#include <strings.h>

#include <dirent.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "haiku_settings_common.hpp"
#include "haiku_util.hpp"
#include "haiku_vmmanager_config.hpp"
#include "haiku_vmmanager_protocol.hpp"
#include "haiku_vmmanager_system.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/video.h>
#include <86box/sound.h>
#include <86box/midi.h>
#include <86box/thread.h>
#include <86box/network.h>
#include <86box/keyboard.h>
#include <86box/mouse.h>
#include <86box/gameport.h>
#include <86box/fdd.h>
#include <86box/fdc_ext.h>
#include <86box/hdc.h>
#include <86box/cdrom.h>
#include <86box/cdrom_interface.h>
#include <86box/hdd.h>
#include <86box/scsi.h>
#include <86box/scsi_device.h>
#include <86box/rdisk.h>
#include <86box/mo.h>
#include <86box/scsi_tape.h>
#include <86box/isartc.h>
#include <86box/isamem.h>
#include <86box/isarom.h>
#include <86box/char.h>
#include "../cpu/cpu.h"
}

extern char **environ;

/* --------------------------------------------------------------- helpers */

std::string
vmm_executable_path(void)
{
    image_info info;
    int32      cookie = 0;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
        if (info.type == B_APP_IMAGE)
            return info.name;
    }
    return std::string(exe_path) + "86Box";
}

std::string
vmm_shorten_path(const std::string &path)
{
    const char *home = getenv("HOME");
    if ((home == nullptr) || (home[0] == '\0'))
        return path;
    const size_t len = strlen(home);
    if ((path.compare(0, len, home) == 0) && ((path.size() == len) || (path[len] == '/')))
        return "~" + path.substr(len);
    return path;
}

static std::vector<std::string>
split(const std::string &text, char separator)
{
    std::vector<std::string> parts;
    size_t                   start = 0;
    for (;;) {
        const size_t pos = text.find(separator, start);
        parts.push_back(text.substr(start, (pos == std::string::npos) ? std::string::npos : pos - start));
        if (pos == std::string::npos)
            break;
        start = pos + 1;
    }
    return parts;
}

static std::string
join(const std::vector<std::string> &parts, const char *separator)
{
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0)
            out += separator;
        out += parts[i];
    }
    return out;
}

static std::string
trimmed(const std::string &text)
{
    size_t start = 0;
    size_t end   = text.size();
    while ((start < end) && isspace((unsigned char) text[start]))
        start++;
    while ((end > start) && isspace((unsigned char) text[end - 1]))
        end--;
    return text.substr(start, end - start);
}

static std::string
upper(std::string text)
{
    for (auto &c : text)
        c = (char) toupper((unsigned char) c);
    return text;
}

/* The piece `index` of a comma-separated value, trimmed ("" if missing). */
static std::string
field(const std::string &text, size_t index)
{
    const auto parts = split(text, ',');
    return (index < parts.size()) ? trimmed(parts[index]) : std::string();
}

static bool
key_matches(const std::string &key, const char *pattern)
{
    const std::regex regex(pattern, std::regex::icase);
    return std::regex_search(key, regex);
}

static std::string
device_name(const device_t *dev, const char *internal_name, int bus)
{
    return settings_device_name(dev, internal_name, bus);
}

static std::string
number_string(long value)
{
    char temp[32];
    snprintf(temp, sizeof(temp), "%ld", value);
    return temp;
}

std::string
VmmSystem::Canonical(const std::string &path)
{
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved) != nullptr)
        return resolved;
    return path;
}

/* ------------------------------------------------------------ discovery */

std::vector<std::string>
VmmSystem::CachedConfigPaths()
{
    std::vector<std::string> paths;
    std::set<std::string>    seen;

    for (const auto &machine : VmmConfig::Machines()) {
        const std::string &path = machine.second;
        if (path.empty())
            continue;
        struct stat st;
        if ((stat(path.c_str(), &st) != 0) || !S_ISREG(st.st_mode))
            continue;
        const size_t slash = path.rfind('/');
        const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
        if (strcasecmp(name.c_str(), CONFIG_FILE) != 0)
            continue;
        /* The section must be the UUID of the path; anything else is a
           stale duplicate. */
        if (strcasecmp(machine.first.c_str(), haiku_uuid_for_config(path.c_str()).c_str()) != 0)
            continue;
        const std::string key = Canonical(path);
        if (seen.insert(key).second)
            paths.push_back(path);
    }
    return paths;
}

static void
scan_directory(const std::string &dir, const std::string &root_cfg, std::vector<std::string> &found, int depth)
{
    if (depth > 32)
        return;
    DIR *handle = opendir(dir.c_str());
    if (handle == nullptr)
        return;

    std::vector<std::string> subdirs;
    struct dirent           *entry;
    while ((entry = readdir(handle)) != nullptr) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        const std::string path = dir + "/" + entry->d_name;
        struct stat       st;
        if (lstat(path.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            subdirs.push_back(path);
        else if (S_ISREG(st.st_mode) && !strcmp(entry->d_name, CONFIG_FILE) && (strcasecmp(path.c_str(), root_cfg.c_str()) != 0))
            found.push_back(path);
    }
    closedir(handle);

    for (const auto &sub : subdirs)
        scan_directory(sub, root_cfg, found, depth + 1);
}

std::vector<std::string>
VmmSystem::ScanForConfigs(const std::string &root)
{
    std::vector<std::string> found;
    std::string              dir = root;
    while ((dir.size() > 1) && (dir[dir.size() - 1] == '/'))
        dir.erase(dir.size() - 1);
    if (dir.empty())
        return found;
    scan_directory(dir, dir + "/" CONFIG_FILE, found, 0);
    return found;
}

/* ---------------------------------------------------------------- system */

VmmSystem::VmmSystem(const std::string &config_path, const BMessenger &target)
    : id(0)
    , status(VmmStatus::Stopped)
    , window_obscured(false)
    , settings_only(false)
    , killed(false)
    , pid(-1)
    , target(target)
    , listen_fd(-1)
    , client_fd(-1)
    , wake_pipe { -1, -1 }
    , server(nullptr)
{
    static int32 next_id = 1;
    id = next_id++;

    config_file = config_path;
    if (!config_file.empty() && (config_file[0] != '/')) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != nullptr)
            config_file = std::string(cwd) + "/" + config_file;
    }
    const size_t slash = config_file.rfind('/');
    config_dir         = (slash == std::string::npos) ? std::string(".") : config_file.substr(0, slash);
    if (config_dir.empty())
        config_dir = "/";
    const size_t name_slash = config_dir.rfind('/');
    config_name   = (name_slash == std::string::npos) ? config_dir : config_dir.substr(name_slash + 1);
    shortened_dir = vmm_shorten_path(config_dir);
    uuid          = haiku_uuid_for_config(config_file.c_str());

    /* QLocalServer puts its sockets in the temporary folder; the name is
       unique per machine folder. */
    std::string tail;
    for (const char c : uuid) {
        if (c != '-')
            tail += c;
    }
    socket_path = "/tmp/86Box.socket." + tail.substr(tail.size() > 9 ? tail.size() - 9 : 0);

    LoadSettings();
    SetupVars();
    SaveSettings();
}

VmmSystem::~VmmSystem()
{
    StopServer();
}

bool
VmmSystem::IsValid() const
{
    struct stat st;
    return (stat(config_file.c_str(), &st) == 0) && S_ISREG(st.st_mode) && (st.st_size != 0);
}

void
VmmSystem::LoadSettings()
{
    config_hash.clear();
    VmmIni cfg(false);
    if (cfg.Load(config_file)) {
        for (const auto &section : cfg.Sections()) {
            const VmmIni::keys_t *keys = cfg.Keys(section);
            auto                 &out  = config_hash[section];
            for (const auto &pair : *keys)
                out[pair.first] = pair.second;
        }
    }

    VmmIni ini(true);
    ini.Load(VmmConfig::Path());
    const std::string loaded_name = ini.Get(uuid, "display_name");
    display_name                  = loaded_name.empty() ? config_name : loaded_name;
    notes                         = ini.Get(uuid, "notes");
    timestamp                     = ini.Get(uuid, "timestamp");
    icon                          = ini.Get(uuid, "icon");
}

void
VmmSystem::SaveSettings()
{
    if (!IsValid())
        return;

    VmmIni ini(true);
    ini.Load(VmmConfig::Path());

    auto set = [&ini, this](const char *key, const std::string &value) {
        if (value.empty())
            ini.Remove(uuid, key);
        else
            ini.Set(uuid, key, value);
    };

    const std::string canonical = Canonical(config_file);
    const size_t      slash     = canonical.rfind('/');
    set("system_name", config_name);
    set("config_file", canonical);
    set("config_dir", (slash == std::string::npos) ? config_dir : canonical.substr(0, slash));
    set("display_name", (display_name != config_name) ? display_name : std::string());
    set("notes", notes);
    if (!timestamp.empty())
        set("timestamp", timestamp);
    set("icon", icon);

    /* Drop any other section for this configuration (stale duplicates). */
    for (const auto &other : ini.Sections()) {
        if (other.empty() || (other == "General") || !strcasecmp(other.c_str(), uuid.c_str()))
            continue;
        const std::string other_path = ini.Get(other, "config_file");
        if (!other_path.empty() && (Canonical(other_path) == canonical))
            ini.RemoveSection(other);
    }
    ini.Save(VmmConfig::Path());

    GenerateSearchTerms();
}

void
VmmSystem::RemoveFromCache()
{
    VmmIni ini(true);
    if (!ini.Load(VmmConfig::Path()))
        return;
    const std::string target_path = Canonical(config_file);
    for (const auto &section : ini.Sections()) {
        if (section.empty() || (section == "General"))
            continue;
        const std::string path = ini.Get(section, "config_file");
        if ((section == uuid) || (!path.empty() && ((Canonical(path) == target_path) || (path == config_file))))
            ini.RemoveSection(section);
    }
    ini.Save(VmmConfig::Path());
}

void
VmmSystem::GenerateSearchTerms()
{
    search_terms.clear();
    for (const auto &value : display) {
        for (const auto &part : split(value, ';'))
            search_terms.push_back(part);
    }
    search_terms.push_back(display_name);
    search_terms.push_back(config_name);
    std::string word;
    for (const char c : notes) {
        if (isspace((unsigned char) c)) {
            search_terms.push_back(word);
            word.clear();
        } else
            word += c;
    }
    search_terms.push_back(word);
}

void
VmmSystem::UpdateTimestamp()
{
    const time_t now = time(nullptr);
    struct tm    utc;
    gmtime_r(&now, &utc);
    char text[32];
    strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    timestamp = text;
    SaveSettings();
}

void
VmmSystem::SetDisplayName(const std::string &name)
{
    display_name = name.empty() ? config_name : name;
    SaveSettings();
}

void
VmmSystem::SetNotes(const std::string &text)
{
    notes = text;
    SaveSettings();
}

void
VmmSystem::SetIcon(const std::string &name)
{
    icon = name;
    SaveSettings();
}

void
VmmSystem::ReloadConfig()
{
    LoadSettings();
    SetupVars();
    GenerateSearchTerms();
}

std::vector<std::string>
VmmSystem::Screenshots() const
{
    std::vector<std::string> files;
    const std::string        dir    = config_dir + "/screenshots";
    DIR                     *handle = opendir(dir.c_str());
    if (handle == nullptr)
        return files;
    struct dirent *entry;
    while ((entry = readdir(handle)) != nullptr) {
        if (strncmp(entry->d_name, "Monitor_1", 9) != 0)
            continue;
        const std::string path = dir + "/" + entry->d_name;
        struct stat       st;
        if ((stat(path.c_str(), &st) == 0) && S_ISREG(st.st_mode))
            files.push_back(path);
    }
    closedir(handle);
    std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    });
    return files;
}

std::string
VmmSystem::StatusName(VmmStatus status)
{
    switch (status) {
        case VmmStatus::Stopped:
            return "Powered Off";
        case VmmStatus::Running:
            return "Running";
        case VmmStatus::Paused:
            return "Paused";
        case VmmStatus::PausedWaiting:
            return "Paused (Waiting)";
        case VmmStatus::RunningWaiting:
            return "Running (Waiting)";
        default:
            return "Unknown Status";
    }
}

std::string
VmmSystem::StatusString() const
{
    return StatusName(status);
}

/* VMManagerSystem::setupVars(): the summary shown in the details. */
void
VmmSystem::SetupVars()
{
    for (auto &value : display)
        value.clear();

    auto category = [this](const char *name) -> std::map<std::string, std::string> {
        const auto it = config_hash.find(name);
        return (it == config_hash.end()) ? std::map<std::string, std::string>() : it->second;
    };
    auto get = [](const std::map<std::string, std::string> &cat, const std::string &key) -> std::string {
        const auto it = cat.find(key);
        return (it == cat.end()) ? std::string() : it->second;
    };
    auto has = [](const std::map<std::string, std::string> &cat, const std::string &key) {
        return cat.find(key) != cat.end();
    };

    const auto machine_config      = category("Machine");
    const auto video_config        = category("Video");
    const auto disk_config         = category("Hard disks");
    const auto audio_config        = category("Sound");
    const auto network_config      = category("Network");
    const auto input_config        = category("Input devices");
    const auto floppy_cdrom_config = category("Floppy and CD-ROM drives");
    const auto removable_config    = category("Other removable devices");
    const auto storage_config      = category("Storage controllers");
    const auto ports_config        = category("Ports (COM & LPT)");
    const auto other_config        = category("Other peripherals");

    /* Machine */
    std::string machine_name;
    int         ram_granularity = 0;
    const int   ci              = machine_get_machine_from_internal_name(get(machine_config, "machine").c_str());
    if ((ci != -1) && machine_available(ci)) {
        machine_name    = machines[ci].name;
        ram_granularity = machines[ci].ram.step;
    }
    display[DISPLAY_MACHINE] = machine_name;

    /* CPU: name with speed and FPU */
    std::string cpu_name = "Unknown";
    for (int i = 0; cpu_families[i].package != 0; i++) {
        if (get(machine_config, "cpu_family") != cpu_families[i].internal_name)
            continue;
        cpu_name                     = std::string(cpu_families[i].manufacturer) + " " + cpu_families[i].name;
        const unsigned long speed    = strtoul(get(machine_config, "cpu_speed").c_str(), nullptr, 10);
        for (int j = 0; cpu_families[i].cpus[j].cpu_type != 0; j++) {
            if ((unsigned long) cpu_families[i].cpus[j].rspeed != speed)
                continue;
            std::string cpu_speed = split(cpu_families[i].cpus[j].name, '/')[0];
            const size_t paren    = cpu_speed.find(" (");
            if (paren != std::string::npos)
                cpu_speed.erase(paren);
            cpu_name += " / " + cpu_speed + " MHz";
            const std::string fpu = get(machine_config, "fpu_type");
            if (has(machine_config, "fpu_type") && (fpu != "none") && (fpu != "internal")) {
                for (int k = 0; cpu_families[i].cpus[j].fpus[k].internal_name != nullptr; k++) {
                    if (fpu == cpu_families[i].cpus[j].fpus[k].internal_name) {
                        cpu_name += std::string(", ") + cpu_families[i].cpus[j].fpus[k].name + " FPU";
                        break;
                    }
                }
            }
            break;
        }
        break;
    }
    display[DISPLAY_CPU] = cpu_name;

    /* Memory */
    const int divisor       = (ram_granularity < 1024) ? 1 : 1024;
    display[DISPLAY_MEMORY] = number_string(atoi(get(machine_config, "mem_size").c_str()) / divisor) + ((divisor == 1) ? " KB" : " MB");

    /* Video cards, migrating old names first */
    auto video_name = [&](const std::string &name, std::string &internal) -> const device_t * {
        std::string     temp = name;
        const device_t *dev  = video_get_video_from_old_internal_name(&temp[0]);
        if (dev == nullptr) {
            temp               = name;
            const int card     = video_get_video_from_internal_name(&temp[0]);
            dev                = video_card_getdevice(card);
            const char *intern = video_get_internal_name(card);
            internal           = (intern != nullptr) ? intern : "";
        } else
            internal = (dev->internal_name != nullptr) ? dev->internal_name : "";
        return dev;
    };
    std::string     video_internal;
    const device_t *video_dev = video_name(get(video_config, "gfxcard"), video_internal);
    std::string     video     = device_name(video_dev, video_internal.c_str(), 1);
    if ((ci != -1) && (video_internal == "internal")) {
        const device_t *internal_device = machine_get_vid_device(ci);
        if ((internal_device != nullptr) && device_available(internal_device))
            video += " (" + device_name(internal_device, internal_device->internal_name, 0) + ")";
    }
    if (has(video_config, "gfxcard_2")) {
        std::string     video2_internal;
        const device_t *video2_dev = video_name(get(video_config, "gfxcard_2"), video2_internal);
        video += VMM_SECTION_SEPARATOR + device_name(video2_dev, video2_internal.c_str(), 1);
    }
    if (atoi(get(video_config, "8514a").c_str()) != 0)
        video += VMM_SECTION_SEPARATOR "IBM 8514/A Graphics";
    if (atoi(get(video_config, "xga").c_str()) != 0)
        video += VMM_SECTION_SEPARATOR "XGA Graphics";
    if (atoi(get(video_config, "da2").c_str()) != 0)
        video += VMM_SECTION_SEPARATOR "IBM PS/55 Display Adapter Graphics";
    if (atoi(get(video_config, "powervr").c_str()) != 0)
        video += VMM_SECTION_SEPARATOR "PowerVR PCX1/PCX2 Graphics";
    display[DISPLAY_VIDEO] = video;

    /* Voodoo */
    std::string voodoo_name;
    if (atoi(get(video_config, "voodoo").c_str()) != 0) {
        char temp[512];
        device_get_name(&voodoo_device, 0, temp);
        const auto voodoo_config = category(temp);
        switch (atoi(get(voodoo_config, "type").c_str())) {
            default:
                voodoo_name = "3Dfx Voodoo Graphics";
                break;
            case 1:
                voodoo_name = "Obsidian SB50 + Amethyst (2 TMUs)";
                break;
            case 2:
                voodoo_name = "3Dfx Voodoo 2";
                break;
        }
        if (atoi(get(voodoo_config, "sli").c_str()) == 1)
            voodoo_name += " (SLI)";
    }
    display[DISPLAY_VOODOO] = voodoo_name;

    /* Hard disks: hdd_NN_* keys */
    std::set<std::string> disks;
    for (const auto &pair : disk_config) {
        const auto pieces = split(pair.first, '_');
        if (pieces.size() >= 2)
            disks.insert(pieces[0] + "_" + pieces[1]);
    }
    std::vector<std::string> disk_lines;
    for (const auto &key : disks) {
        const std::string parameters = get(disk_config, key + "_parameters");
        const auto        pieces     = split(parameters, ',');
        const long        sectors    = atol(field(parameters, 0).c_str());
        const long        heads      = atol(field(parameters, 1).c_str());
        const long        cylinders  = atol(field(parameters, 2).c_str());
        const std::string bus        = trimmed(pieces.back());
        const long        size_raw   = (cylinders * heads * sectors) >> 11;
        char              size[32];
        const char       *unit = "MiB";
        if (size_raw > 1000) {
            unit = "GiB";
            snprintf(size, sizeof(size), "%.1f", size_raw / 1000.0);
        } else
            snprintf(size, sizeof(size), "%ld", size_raw);
        std::string prefix;
        if (disks.size() > 1)
            prefix = "Disk " + number_string(atol(split(key, '_').back().c_str())) + ": ";
        disk_lines.push_back(prefix + size + " " + unit + " (" + upper(bus) + ")");
    }
    display[DISPLAY_DISKS] = disk_lines.empty() ? std::string("No disks") : join(disk_lines, VMM_SECTION_SEPARATOR);

    /* Floppy and CD-ROM drives */
    std::vector<std::string> floppies;
    std::vector<std::string> cdroms;
    /* The first two 5.25" 360k drives are not saved to the file. */
    for (int i = 0; i < 2; i++) {
        if (!has(floppy_cdrom_config, "fdd_0" + number_string(i + 1) + "_type")) {
            char internal[] = "525_2dd";
            floppies.push_back(fdd_getname(fdd_get_from_internal_name(internal)));
        }
    }
    for (const auto &pair : floppy_cdrom_config) {
        const std::string &key = pair.first;
        if (key_matches(key, "fdd_\\d\\d_type")) {
            std::string internal = pair.second;
            if (internal == "none")
                continue;
            const char *name = fdd_getname(fdd_get_from_internal_name(&internal[0]));
            if ((name != nullptr) && (name[0] != '\0'))
                floppies.push_back(name);
        }
        if (key_matches(key, "cdrom_\\d\\d_parameters")) {
            const std::string number   = split(key, '_')[1];
            std::string       bus      = upper(field(pair.second, 1));
            std::string       internal = get(floppy_cdrom_config, "cdrom_" + number + "_type");
            if (internal.empty())
                internal = "86cd";
            const int   type  = cdrom_get_from_internal_name(internal.c_str());
            std::string speed = get(floppy_cdrom_config, "cdrom_" + number + "_speed");
            if (speed.empty())
                speed = "8";
            if ((bus != "NONE") && (type != -1)) {
                speed = (atoi(speed.c_str()) > 72) ? std::string("Turbo ") : (speed + "x ");
                cdroms.push_back(speed + cdrom_drive_types[type].vendor + " " + cdrom_drive_types[type].model + " "
                                 + cdrom_drive_types[type].revision + " (" + bus + ")");
            }
        }
    }
    display[DISPLAY_FLOPPY] = join(floppies, VMM_SECTION_SEPARATOR);
    display[DISPLAY_CD]     = join(cdroms, VMM_SECTION_SEPARATOR);

    /* Other removable devices */
    std::vector<std::string> rdisks;
    std::vector<std::string> mos;
    std::vector<std::string> tapes;
    for (const auto &pair : removable_config) {
        const std::string &key = pair.first;
        const bool         zip = key_matches(key, "zip_\\d\\d_parameters"); /* legacy ZIP entries */
        if (zip || key_matches(key, "rdisk_\\d\\d_parameters")) {
            uint32_t config_id = (uint32_t) atoi(field(pair.second, 0).c_str());
            if (zip)
                config_id++;
            std::string bus = upper(field(pair.second, 1));
            /* The file has the number of rdisk_type_config_ids[]. */
            const uint32_t type = rdisk_type_from_config(config_id);
            if ((type != RDISK_TYPE_GENERIC) || (config_id == 0)) {
                if (!bus.empty())
                    bus = " (" + bus + ")";
                rdisks.push_back(std::string(rdisk_drive_types[type].vendor) + " " + rdisk_drive_types[type].model + bus);
            }
        }
        if (key_matches(key, "mo_\\d\\d_parameters")) {
            const int   type = atoi(field(pair.second, 0).c_str());
            std::string bus  = upper(field(pair.second, 1));
            if ((type >= 0) && (type < KNOWN_MO_DRIVE_TYPES)) {
                if (!bus.empty())
                    bus = " (" + bus + ")";
                mos.push_back(std::string(mo_drive_types[type].vendor) + " " + mo_drive_types[type].model + bus);
            }
        }
        if (key_matches(key, "tape_\\d\\d_parameters")) {
            const int   type = atoi(field(pair.second, 0).c_str());
            std::string bus  = upper(field(pair.second, 1));
            if ((type >= 0) && (type < KNOWN_TAPE_DRIVE_TYPES)) {
                if (!bus.empty())
                    bus = " (" + bus + ")";
                tapes.push_back(std::string(tape_drive_types[type].vendor) + " " + tape_drive_types[type].model + bus);
            }
        }
    }
    display[DISPLAY_RDISK] = join(rdisks, VMM_SECTION_SEPARATOR);
    display[DISPLAY_MO]    = join(mos, VMM_SECTION_SEPARATOR);
    display[DISPLAY_TAPE]  = join(tapes, VMM_SECTION_SEPARATOR);

    /* SCSI controllers */
    std::vector<std::string> scsi;
    for (const auto &pair : storage_config) {
        if (!key_matches(pair.first, "scsicard_\\d"))
            continue;
        std::string internal = pair.second;
        const int   card     = scsi_card_get_from_internal_name(&internal[0]);
        const std::string name = device_name(scsi_card_getdevice(card), scsi_card_get_internal_name(card), 1);
        if (!name.empty())
            scsi.push_back(name);
    }
    display[DISPLAY_SCSI_CONTROLLER] = join(scsi, VMM_SECTION_SEPARATOR);

    /* Hard and floppy disk controllers */
    std::vector<std::string> controllers;
    for (const auto &pair : storage_config) {
        std::string internal = pair.second;
        const bool  usable   = !internal.empty() && (internal != "none") && (internal != "internal");
        if (key_matches(pair.first, "fdc(_\\d)?") && usable) {
            const int         card = fdc_card_get_from_internal_name(&internal[0]);
            const std::string name = device_name(fdc_card_getdevice(card), fdc_card_get_internal_name(card), 1);
            if (!name.empty())
                controllers.push_back(name);
        }
        if (key_matches(pair.first, "hdc(_\\d)?") && usable) {
            const int         hdc  = hdc_get_from_internal_name(internal.c_str());
            const std::string name = device_name(hdc_get_device(hdc), hdc_get_internal_name(hdc), 1);
            if (!name.empty())
                controllers.push_back(name);
        }
    }
    if (has(storage_config, "cdrom_interface")) {
        const std::string internal = get(storage_config, "cdrom_interface");
        if (!internal.empty() && (internal != "none") && (internal != "internal")) {
            const int intf = cdrom_interface_get_from_internal_name(internal.c_str());
            controllers.push_back(device_name(cdrom_interface_get_device(intf), cdrom_interface_get_internal_name(intf), 1));
        }
    }
    /* Legacy tertiary/quaternary IDE */
    for (const char *ide : { "ide_ter", "ide_qua" }) {
        if (atoi(get(storage_config, ide).c_str()) != 0)
            controllers.push_back(device_name(hdc_get_device(hdc_get_from_internal_name(ide)), ide, 1));
    }
    display[DISPLAY_STORAGE_CONTROLLER] = join(controllers, VMM_SECTION_SEPARATOR);

    /* Sound cards */
    std::vector<std::string> sound_cards;
    for (const auto &pair : audio_config) {
        if (!key_matches(pair.first, "sndcard\\d?"))
            continue;
        const std::string &internal = pair.second;
        const int          card     = sound_card_get_from_internal_name(internal.c_str());
        std::string        name     = device_name(sound_card_getdevice(card), sound_card_get_internal_name(card), 1);
        if ((ci != -1) && (internal == "internal")) {
            const device_t *internal_device = machine_get_snd_device(ci);
            if (internal_device != nullptr)
                name += " (" + device_name(internal_device, internal_device->internal_name, 0) + ")";
        }
        if (!name.empty())
            sound_cards.push_back(name);
    }
    if (has(audio_config, "mpu401_standalone"))
        sound_cards.push_back("Standalone MPU-401");
    if (sound_cards.empty())
        sound_cards.push_back("None");
    display[DISPLAY_AUDIO] = join(sound_cards, VMM_SECTION_SEPARATOR);

    /* MIDI output */
    if (has(audio_config, "midi_device")) {
        std::string     internal = get(audio_config, "midi_device");
        const device_t *midi_out = midi_out_device_getdevice(midi_out_device_get_from_internal_name(&internal[0]));
        if ((midi_out != nullptr) && (midi_out->name != nullptr))
            display[DISPLAY_MIDI_OUT] = midi_out->name;
    }

    /* Network cards */
    std::vector<std::string> nics;
    for (const auto &pair : network_config) {
        if (!key_matches(pair.first, "net_\\d\\d_card"))
            continue;
        const std::string number   = split(pair.first, '_')[1];
        std::string       internal = pair.second;
        const int         card     = network_card_get_from_internal_name(&internal[0]);
        std::string       name     = device_name(network_card_getdevice(card), network_card_get_internal_name(card), 1);
        if ((ci != -1) && (pair.second == "internal")) {
            const device_t *internal_device = machine_get_net_device(ci);
            if (internal_device != nullptr)
                name += " (" + device_name(internal_device, internal_device->internal_name, 0) + ")";
        }
        std::string net_type = get(network_config, "net_" + number + "_net_type");
        if (!net_type.empty()) {
            if (net_type == "slirp")
                net_type = "SLiRP";
            else if (net_type == "pcap")
                net_type = "PCap";
            else if ((net_type == "nlswitch") || (net_type == "nmswitch"))
                net_type = "Local Switch";
            else if (net_type == "nrswitch")
                net_type = "Remote Switch";
            else
                net_type = upper(net_type);
            nics.push_back(name + " (" + net_type + ")");
        } else
            nics.push_back(name);
    }
    if (nics.empty())
        nics.push_back("None");
    display[DISPLAY_NIC] = join(nics, VMM_SECTION_SEPARATOR);

    /* Input devices */
    if (has(input_config, "keyboard_type")) {
        std::string internal        = get(input_config, "keyboard_type");
        const int   keyboard        = keyboard_get_from_internal_name(&internal[0]);
        display[DISPLAY_KEYBOARD]   = device_name(keyboard_get_device(keyboard), keyboard_get_internal_name(keyboard), 0);
    }
    {
        std::string internal   = get(input_config, "mouse_type");
        const int   mouse      = mouse_get_from_internal_name(&internal[0]);
        display[DISPLAY_MOUSE] = device_name(mouse_get_device(mouse), mouse_get_internal_name(mouse), 0);
    }
    if (has(input_config, "tablet_type")) {
        std::string internal    = get(input_config, "tablet_type");
        const int   tablet      = tablet_get_from_internal_name(&internal[0]);
        display[DISPLAY_TABLET] = device_name(tablet_get_device(tablet), tablet_get_internal_name(tablet), 0);
    }
    if (has(input_config, "joystick_type")) {
        std::string internal = get(input_config, "joystick_type");
        const char *name     = joystick_get_name(joystick_get_from_internal_name(&internal[0]));
        if ((name != nullptr) && (name[0] != '\0'))
            display[DISPLAY_JOYSTICK] = name;
    }

    /* Ports: COM1-2 and LPT1 are enabled unless the file says otherwise */
    bool serial_enabled[SERIAL_MAX] = { true, true };
    bool lpt_enabled[PARALLEL_MAX]  = { true };
    for (const auto &pair : ports_config) {
        const std::string &key = pair.first;
        if (key_matches(key, "serial\\d_enabled") || key_matches(key, "lpt\\d_enabled")) {
            const std::string dev = split(key, '_')[0];
            if (dev.empty())
                continue;
            const char last = dev[dev.size() - 1];
            if (!isdigit((unsigned char) last))
                continue;
            const int num = last - '0';
            if (key_matches(key, "serial\\d_enabled")) {
                if ((num >= 1) && (num <= SERIAL_MAX))
                    serial_enabled[num - 1] = (atoi(pair.second.c_str()) == 1);
            } else if ((num >= 1) && (num <= PARALLEL_MAX))
                lpt_enabled[num - 1] = (atoi(pair.second.c_str()) == 1);
        }
    }
    auto ports = [&](const bool *enabled, int count, const char *prefix, const char *device_key, int bus) -> std::string {
        std::vector<std::string> list;
        bool                     has_devices = false;
        for (int i = 0; i < count; i++) {
            if (!enabled[i])
                continue;
            const std::string key = std::string(device_key) + number_string(i + 1) + "_device";
            std::string       dev_name;
            if (has(ports_config, key)) {
                const int id = char_get_from_internal_name(get(ports_config, key).c_str(), bus);
                if (id) {
                    const device_t *dev = char_get_device(id);
                    dev_name            = " (" + device_name(dev, (dev != nullptr) ? dev->internal_name : nullptr, -1) + ")";
                    has_devices         = true;
                }
            }
            list.push_back(std::string(prefix) + number_string(i + 1) + dev_name);
        }
        if (list.empty())
            return "None";
        return join(list, has_devices ? VMM_SECTION_SEPARATOR : ", ");
    };
    display[DISPLAY_SERIAL]   = ports(serial_enabled, SERIAL_MAX, "COM", "serial", DEVICE_COM);
    display[DISPLAY_PARALLEL] = ports(lpt_enabled, PARALLEL_MAX, "LPT", "lpt", DEVICE_LPT);

    /* ISA RTC, memory and ROM cards */
    if (has(other_config, "isartc_type")) {
        const int rtc            = isartc_get_from_internal_name(get(other_config, "isartc_type").c_str());
        display[DISPLAY_ISA_RTC] = device_name(isartc_get_device(rtc), isartc_get_internal_name(rtc), 0);
    }
    std::vector<std::string> mem_cards;
    std::vector<std::string> rom_cards;
    for (const auto &pair : other_config) {
        if (key_matches(pair.first, "isamem\\d_type")) {
            const int         card = isamem_get_from_internal_name(pair.second.c_str());
            const std::string name = device_name(isamem_get_device(card), isamem_get_internal_name(card), 0);
            if (!name.empty())
                mem_cards.push_back(name);
        }
        if (key_matches(pair.first, "isarom\\d_type")) {
            const int         card = isarom_get_from_internal_name(pair.second.c_str());
            const std::string name = device_name(isarom_get_device(card), isarom_get_internal_name(card), 0);
            if (!name.empty())
                rom_cards.push_back(name);
        }
    }
    display[DISPLAY_ISA_MEM] = join(mem_cards, VMM_SECTION_SEPARATOR);
    display[DISPLAY_ISA_ROM] = join(rom_cards, VMM_SECTION_SEPARATOR);
}

/* --------------------------------------------------------------- server */

bool
VmmSystem::StartServer()
{
    if (server != nullptr)
        return true;

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path))
        return false;
    strcpy(address.sun_path, socket_path.c_str());

    /* Remove an old socket file first. */
    struct stat st;
    if ((lstat(socket_path.c_str(), &st) == 0) && !S_ISDIR(st.st_mode) && (unlink(socket_path.c_str()) != 0)) {
        pclog("VMM: failed to remove the old socket file %s: %s\n", socket_path.c_str(), strerror(errno));
        return false;
    }

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0)
        return false;
    if ((bind(listen_fd, (struct sockaddr *) &address, sizeof(address)) < 0) || (listen(listen_fd, 4) < 0)) {
        pclog("VMM: failed to start the server on %s: %s\n", socket_path.c_str(), strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return false;
    }
    if (pipe(wake_pipe) < 0) {
        close(listen_fd);
        listen_fd = -1;
        return false;
    }

    pclog("VMM: socket path is %s\n", socket_path.c_str());
    server = new std::thread(&VmmSystem::ServerThread, this);
    return true;
}

void
VmmSystem::StopServer()
{
    if (server == nullptr)
        return;

    const char wake = 1;
    if (write(wake_pipe[1], &wake, 1) < 0)
        pclog("VMM: could not wake the server thread\n");
    server->join();
    delete server;
    server = nullptr;

    close(listen_fd);
    listen_fd = -1;
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    wake_pipe[0] = wake_pipe[1] = -1;
    unlink(socket_path.c_str());
}

/* Accepts the machine's connections, one at a time (QLocalServer keeps the
   latest), and posts what it sends to the window. */
void
VmmSystem::ServerThread()
{
    for (;;) {
        if (!vmm_wait_readable(listen_fd, wake_pipe[0]))
            return;
        const int fd = accept(listen_fd, nullptr, nullptr);
        if (fd < 0) {
            if ((errno == EINTR) || (errno == EAGAIN) || (errno == ECONNABORTED))
                continue;
            return;
        }

        {
            std::lock_guard<std::mutex> guard(lock);
            client_fd    = fd;
            VmmJson json = vmm_protocol_message(true, "ManagerIdentity");
            json["params"] = vmm_manager_identity();
            vmm_send(fd, json);
        }

        VmmFrameReader reader;
        char           buffer[4096];
        bool           stop = false;
        for (;;) {
            if (!vmm_wait_readable(fd, wake_pipe[0])) {
                stop = true;
                break;
            }
            const ssize_t got = read(fd, buffer, sizeof(buffer));
            if (got < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (got == 0)
                break;
            reader.Append(buffer, (size_t) got);
            std::string payload;
            while (reader.Next(payload)) {
                BMessage message(MSG_VMM_CLIENT);
                message.AddInt32("vm", id);
                message.AddString("json", payload.c_str());
                target.SendMessage(&message);
            }
        }

        {
            std::lock_guard<std::mutex> guard(lock);
            client_fd = -1;
        }
        close(fd);
        if (stop)
            return;
    }
}

void
VmmSystem::Send(const char *message)
{
    std::lock_guard<std::mutex> guard(lock);
    if (client_fd < 0) {
        pclog("VMM: cannot send %s: not connected\n", message);
        return;
    }
    vmm_send(client_fd, vmm_protocol_message(true, message));
}

/* VMManagerServerSocket::jsonReceived() and the VMManagerSystem slots. */
uint32
VmmSystem::HandleClientMessage(const std::string &text)
{
    VmmJson json;
    if (!VmmJson::Parse(text, json) || (json.type != VmmJson::Object) || !vmm_protocol_valid(json))
        return 0;

    const std::string message = json.Str("message");
    if (message == "WindowBlocked") {
        window_obscured = true;
        return CHANGED_STATUS;
    }
    if (message == "WindowUnblocked") {
        window_obscured = false;
        return CHANGED_STATUS;
    }
    if (message == "RunningStateChanged") {
        const VmmJson  params = vmm_protocol_params(json);
        const VmmJson *state  = params.Find("status");
        if ((state == nullptr) || (state->type != VmmJson::Number))
            return 0;
        switch ((int) state->number) {
            case 0:
                status          = VmmStatus::Running;
                window_obscured = false;
                break;
            case 1:
                status          = VmmStatus::Paused;
                window_obscured = false;
                break;
            case 2:
                status          = VmmStatus::RunningWaiting;
                window_obscured = true;
                break;
            case 3:
                status          = VmmStatus::PausedWaiting;
                window_obscured = true;
                break;
            default:
                status = VmmStatus::Unknown;
                break;
        }
        return CHANGED_STATUS;
    }
    if (message == "ConfigurationChanged") {
        ReloadConfig();
        return CHANGED_CONFIG;
    }
    if (message == "GlobalConfigurationChanged")
        return CHANGED_GLOBAL;
    /* Status, WinIdMessage and WindowActivated need nothing here. */
    return 0;
}

/* ------------------------------------------------------------- process */

bool
VmmSystem::Launch(bool settings, std::string &error)
{
    const std::string program = vmm_executable_path();
    if (access(program.c_str(), X_OK) != 0) {
        error = "The 86Box executable was not found.";
        return false;
    }

    if (!StartServer()) {
        error = "Failed to start the VM manager server.";
        return false;
    }

    /* Already running: bring it to the front, or have it show its settings. */
    if (pid >= 0) {
        be_roster->ActivateApp(pid);
        if (settings)
            Send("ShowSettings");
        return true;
    }

    std::vector<std::string> args = { program, "--vmpath", config_dir, "--vmname", display_name };
    if (settings)
        args.push_back("--settings");
    if (rom_path[0] != '\0') {
        args.push_back("--rompath");
        args.push_back(rom_path);
    }
    if (asset_path[0] != '\0') {
        args.push_back("--assetpath");
        args.push_back(asset_path);
    }
    if (global_cfg_overridden) {
        args.push_back("--global");
        args.push_back(global_cfg_path);
    }
    if (!hook_enabled)
        args.push_back("--nohook");
    if (start_in_fullscreen)
        args.push_back("--fullscreen");
    if (!confirm_exit_cmdl)
        args.push_back("--noconfirm");

    std::vector<char *> argv;
    for (auto &arg : args)
        argv.push_back(&arg[0]);
    argv.push_back(nullptr);

    /* The environment, with the socket path. */
    std::vector<std::string> env_strings;
    const size_t             name_len = strlen(VMM_SOCKET_ENV);
    for (char **env = environ; (env != nullptr) && (*env != nullptr); env++) {
        if ((strncmp(*env, VMM_SOCKET_ENV, name_len) == 0) && ((*env)[name_len] == '='))
            continue;
        env_strings.push_back(*env);
    }
    env_strings.push_back(std::string(VMM_SOCKET_ENV "=") + socket_path);
    std::vector<char *> envp;
    for (auto &entry : env_strings)
        envp.push_back(&entry[0]);
    envp.push_back(nullptr);

    pid_t      child  = -1;
    const int  result = posix_spawn(&child, program.c_str(), nullptr, nullptr, argv.data(), envp.data());
    if (result != 0) {
        error = std::string("Could not start the virtual machine: ") + strerror(result);
        return false;
    }

    pid             = child;
    status          = VmmStatus::Running;
    window_obscured = settings;
    settings_only   = settings;
    killed          = false;
    UpdateTimestamp();

    /* Wait for the process like QProcess::finished. */
    const BMessenger messenger = target;
    const int32      vm        = id;
    std::thread([messenger, vm, child]() {
        int status = 0;
        while ((waitpid(child, &status, 0) < 0) && (errno == EINTR))
            ;
        BMessage message(MSG_VMM_EXITED);
        message.AddInt32("vm", vm);
        message.AddInt32("pid", child);
        message.AddInt32("code", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        message.AddBool("crashed", !WIFEXITED(status));
        BMessenger(messenger).SendMessage(&message);
    }).detach();
    return true;
}

bool
VmmSystem::ProcessExited(team_id team)
{
    if ((pid < 0) || (team != pid))
        return false;
    pid             = -1;
    status          = VmmStatus::Stopped;
    window_obscured = false;
    settings_only   = false;
    return true;
}

void
VmmSystem::Kill()
{
    if (pid >= 0) {
        killed = true;
        kill(pid, SIGKILL);
    }
}

/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: one virtual machine of the VM manager
 *          (qt_vmmanager_system.cpp and qt_vmmanager_serversocket.cpp):
 *          its configuration and summary, its settings in vmm.ini, the
 *          86Box process running it and the socket the process connects
 *          to.
 *
 *          A VmmSystem belongs to the manager window and is used from its
 *          thread only. Its server thread and process watcher post
 *          messages to the window (MSG_VMM_CLIENT, MSG_VMM_EXITED), which
 *          passes them back to HandleClientMessage() / ProcessExited().
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_VMMANAGER_SYSTEM_HPP
#define HAIKU_VMMANAGER_SYSTEM_HPP

#include <Messenger.h>
#include <OS.h>

#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class VmmJson;

enum : uint32 {
    MSG_VMM_CLIENT     = 'vmcl', /* int32 "vm", string "json": a client message */
    MSG_VMM_EXITED     = 'vmex', /* int32 "vm", "pid", "code", bool "crashed" */
    MSG_VMM_DISCOVERED = 'vmdi', /* string "path" (repeated), string "root" */
};

/* VMManagerSystem::ProcessStatus */
enum class VmmStatus {
    Stopped,
    Running,
    Paused,
    PausedWaiting,
    RunningWaiting,
    Unknown,
};

/* VMManager::Display::Name */
enum VmmDisplay {
    DISPLAY_MACHINE,
    DISPLAY_CPU,
    DISPLAY_MEMORY,
    DISPLAY_VIDEO,
    DISPLAY_DISKS,
    DISPLAY_FLOPPY,
    DISPLAY_CD,
    DISPLAY_RDISK,
    DISPLAY_MO,
    DISPLAY_TAPE,
    DISPLAY_SCSI_CONTROLLER,
    DISPLAY_STORAGE_CONTROLLER,
    DISPLAY_MIDI_OUT,
    DISPLAY_JOYSTICK,
    DISPLAY_SERIAL,
    DISPLAY_PARALLEL,
    DISPLAY_AUDIO,
    DISPLAY_VOODOO,
    DISPLAY_NIC,
    DISPLAY_KEYBOARD,
    DISPLAY_MOUSE,
    DISPLAY_TABLET,
    DISPLAY_ISA_RTC,
    DISPLAY_ISA_MEM,
    DISPLAY_ISA_ROM,
    DISPLAY_COUNT
};

/* The separator between the lines of a summary value
   (VMManagerDetailSection::sectionSeparator). */
#define VMM_SECTION_SEPARATOR ";"

class VmmSystem {
public:
    /* What HandleClientMessage() changed. */
    enum {
        CHANGED_STATUS = 1, /* process status or window_obscured */
        CHANGED_CONFIG = 2, /* the configuration was reloaded */
        CHANGED_GLOBAL = 4, /* the machine changed the global configuration */
    };

    VmmSystem(const std::string &config_path, const BMessenger &target);
    ~VmmSystem();

    /* Configuration files listed in vmm.ini (VMManagerSystem::cachedConfigPaths()). */
    static std::vector<std::string> CachedConfigPaths();
    /* Every machine configuration under `root` except root/86box.cfg (the
       scan of startBackgroundDiscovery()). */
    static std::vector<std::string> ScanForConfigs(const std::string &root);
    /* A path's canonical form, for comparisons. */
    static std::string Canonical(const std::string &path);

    bool IsValid() const;
    bool IsRunning() const { return pid >= 0; }

    std::string Display(VmmDisplay which) const { return display[which]; }
    std::string StatusString() const;
    static std::string StatusName(VmmStatus status);
    /* "Monitor_1*" files in the screenshots folder, sorted. */
    std::vector<std::string> Screenshots() const;
    const std::map<std::string, std::map<std::string, std::string>> &ConfigHash() const { return config_hash; }

    void SetDisplayName(const std::string &name);
    void SetNotes(const std::string &text);
    void SetIcon(const std::string &name);
    void ReloadConfig();
    /* Drops every vmm.ini section of this machine (after deleting it). */
    void RemoveFromCache();

    /* VMManagerSystem::launch86Box(): starts the process, or brings the
       running one to the front (and opens its settings). False with a
       reason when the process could not be started. */
    bool Launch(bool settings, std::string &error);

    void Pause() { Send("Pause"); }
    void Reset() { Send("ResetVM"); }
    void CtrlAltDel() { Send("CtrlAltDel"); }
    void RequestShutdown() { Send("RequestShutdown"); }
    void ForceShutdown() { Send("ForceShutdown"); }
    void SendGlobalConfigurationChanged() { Send("GlobalConfigurationChanged"); }
    void Kill();

    uint32 HandleClientMessage(const std::string &text);
    /* The process ended; returns false if it was not this machine's. */
    bool ProcessExited(team_id team);

    int32       id;
    std::string config_file;   /* absolute path of 86box.cfg */
    std::string config_name;   /* the folder's name */
    std::string config_dir;
    std::string shortened_dir; /* config_dir with ~ for the home folder */
    std::string uuid;
    std::string display_name;
    std::string notes;
    std::string icon;
    std::string timestamp;     /* last start, ISO 8601 UTC */
    std::vector<std::string> search_terms;

    VmmStatus status;
    bool      window_obscured;
    bool      settings_only;
    bool      killed;        /* Kill() was used: no crash report */
    team_id   pid;

private:
    void LoadSettings();
    void SaveSettings();
    void SetupVars();
    void GenerateSearchTerms();
    void UpdateTimestamp();

    bool StartServer();
    void StopServer();
    void ServerThread();
    void Send(const char *message);

    std::map<std::string, std::map<std::string, std::string>> config_hash;
    std::string                                               display[DISPLAY_COUNT];

    BMessenger   target;
    std::string  socket_path;
    int          listen_fd;
    int          client_fd;
    int          wake_pipe[2];
    std::thread *server;
    std::mutex   lock;
};

/* The executable to start machines with (this one). */
std::string vmm_executable_path(void);

/* A path with the home folder shown as ~. */
std::string vmm_shorten_path(const std::string &path);

#endif

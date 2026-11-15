/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the link from a running machine to the
 *          VM manager that started it (qt_vmmanager_clientsocket.cpp).
 *
 *          A reader thread receives the manager's commands and posts them
 *          to the main window as the frontend's messages; notifications
 *          are sent from the caller's thread.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Looper.h>
#include <Message.h>
#include <Messenger.h>
#include <Window.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "haiku_frontend.hpp"
#include "haiku_vmmanager.hpp"
#include "haiku_vmmanager_protocol.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/config.h>
}

static std::mutex   client_lock;
static int          client_fd     = -1;
static int          wake_pipe[2]  = { -1, -1 };
static std::thread *reader_thread = nullptr;
static BMessenger   client_target;
/* The manager is the built-in one (VMManagerClientSocket::vmm_supervised). */
static std::atomic<bool> vmm_supervised { false };

static void
client_post(uint32 what, bool force = false)
{
    BMessage message(what);
    if (force)
        message.AddBool("force", true);
    client_target.SendMessage(&message);
}

/* VMManagerClientSocket::jsonReceived(). */
static void
client_handle(const VmmJson &json)
{
    if (!vmm_protocol_valid(json)) {
        pclog("VMM client: message without the required fields\n");
        return;
    }

    const std::string message = json.Str("message");
    if (message == "Pause")
        client_post(MSG_PAUSE);
    else if (message == "ResetVM")
        client_post(MSG_HARD_RESET);
    else if (message == "ShowSettings")
        client_post(MSG_SETTINGS);
    else if (message == "CtrlAltDel")
        client_post(MSG_CTRL_ALT_DEL);
    else if (message == "RequestShutdown")
        client_post(B_QUIT_REQUESTED);
    else if (message == "ForceShutdown")
        client_post(B_QUIT_REQUESTED, true);
    else if (message == "ManagerIdentity")
        vmm_supervised = (vmm_protocol_params(json) == vmm_manager_identity());
    else if (message == "GlobalConfigurationChanged") {
        /* Reloaded under the window's lock, like Qt does on its GUI thread
           (with a timeout: the window may be waiting for this thread). */
        if (client_target.LockTargetWithTimeout(500000) == B_OK) {
            BLooper *looper = nullptr;
            client_target.Target(&looper);
            config_load_global();
            if (looper != nullptr)
                looper->Unlock();
        } else
            config_load_global();
    } else if (message != "RequestStatus")
        pclog("VMM client: unknown message \"%s\"\n", message.c_str());
}

static void
client_reader(int fd, int wake_fd)
{
    VmmFrameReader reader;
    char           buffer[4096];

    while (vmm_wait_readable(fd, wake_fd)) {
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
            VmmJson json;
            if (VmmJson::Parse(payload, json) && (json.type == VmmJson::Object))
                client_handle(json);
        }
    }

    pclog("VMM client: disconnected from the manager\n");
    vmm_supervised = false;
}

static void
client_send(const char *message, const VmmJson *params = nullptr)
{
    std::lock_guard<std::mutex> guard(client_lock);
    if (client_fd < 0)
        return;

    VmmJson json = vmm_protocol_message(false, message);
    if (params != nullptr)
        json["params"] = *params;
    vmm_send(client_fd, json);
}

void
haiku_vmm_client_start(BWindow *main_window)
{
    const char *path = getenv(VMM_SOCKET_ENV);
    if ((path == nullptr) || (path[0] == '\0') || (reader_thread != nullptr))
        return;

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path)) {
        pclog("VMM client: socket path too long: %s\n", path);
        return;
    }
    strcpy(address.sun_path, path);

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        pclog("VMM client: socket() failed: %s\n", strerror(errno));
        return;
    }
    if (connect(fd, (struct sockaddr *) &address, sizeof(address)) < 0) {
        pclog("VMM client: could not connect to %s: %s\n", path, strerror(errno));
        close(fd);
        return;
    }
    if (pipe(wake_pipe) < 0) {
        close(fd);
        return;
    }

    pclog("VMM client: connected to %s\n", path);
    client_target = BMessenger(main_window);
    {
        std::lock_guard<std::mutex> guard(client_lock);
        client_fd = fd;
    }
    reader_thread = new std::thread(client_reader, fd, wake_pipe[0]);
}

void
haiku_vmm_client_running_state(int state)
{
    VmmJson params;
    params["status"] = VmmJson::MakeNumber(state);
    client_send("RunningStateChanged", &params);
}

void
haiku_vmm_client_configuration_changed(void)
{
    client_send("ConfigurationChanged");
}

void
haiku_vmm_client_window_activated(void)
{
    client_send("WindowActivated");
}

void
haiku_vmm_client_stop(void)
{
    if (reader_thread == nullptr)
        return;

    const char wake = 1;
    if (write(wake_pipe[1], &wake, 1) < 0)
        pclog("VMM client: could not wake the reader\n");
    reader_thread->join();
    delete reader_thread;
    reader_thread = nullptr;

    {
        std::lock_guard<std::mutex> guard(client_lock);
        close(client_fd);
        client_fd = -1;
    }
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    wake_pipe[0] = wake_pipe[1] = -1;
}

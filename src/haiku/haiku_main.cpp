/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the application and start-up sequence,
 *          following qt_main.cpp:
 *            pc_init -> pc_init_roms -> (VM manager | settings only |
 *            pc_init_modules -> moved/copied check -> main window ->
 *            pc_reset_hard_init -> emulation thread (runctl_run_loop) ->
 *            pc_onesec() every second -> on quit: stop, join, pc_close).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Alert.h>
#include <Application.h>
#include <MessageRunner.h>

#include <clocale>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <thread>

#include <unistd.h>

#include "haiku_frontend.hpp"
#include "haiku_keyboardgrab.hpp"
#include "haiku_mainwindow.hpp"
#include "haiku_settings.hpp"
#include "haiku_util.hpp"
#include "haiku_vmmanager.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/runctl.h>
#include <86box/ui.h>
#include <86box/version.h>
#include "../cpu/cpu.h"
}

enum run_mode {
    MODE_MACHINE,
    MODE_VM_MANAGER,
    MODE_SETTINGS_ONLY,
};

class EmuApplication : public BApplication {
public:
    explicit EmuApplication()
        : BApplication("application/x-vnd.86Box")
        , mode(MODE_MACHINE)
        , emu_thread(nullptr)
        , one_second(nullptr)
    {
    }

    void ReadyToRun() override;
    bool QuitRequested() override;
    void MessageReceived(BMessage *message) override;

    run_mode mode;

private:
    void StopEmulation();

    std::thread    *emu_thread;
    BMessageRunner *one_second;
};

static void
emu_thread_fn()
{
    plat_set_thread_name(nullptr, "main_thread");
    runctl_run_loop(nullptr);

    /* The machine powered itself off, or a quit is under way. */
    be_app->PostMessage(B_QUIT_REQUESTED);
}

void
EmuApplication::ReadyToRun()
{
    switch (mode) {
        case MODE_VM_MANAGER:
            haiku_vmmanager_create_window();
            return;

        case MODE_SETTINGS_ONLY:
            {
                /* As Qt: tell the manager the machine is paused/waiting. */
                SettingsWindow *window = new SettingsWindow(nullptr);
                haiku_vmm_client_start(window);
                haiku_vmm_client_running_state(3);
                window->Show();
                return;
            }

        case MODE_MACHINE:
            break;
    }

    MainWindow *window = new MainWindow();
    window->Show();

    /* In a machine started by the VM manager, connect to it. */
    haiku_vmm_client_start(window);

    pc_reset_hard_init();
    if (fast_forward_on_start)
        runctl_set_mode(RUN_MODE_FAST);
    ui_emu_status(100);

    runctl_thread_starting();
    emu_thread = new std::thread(emu_thread_fn);

    one_second = new BMessageRunner(BMessenger(this), new BMessage(MSG_ONE_SECOND), 1000000);
}

void
EmuApplication::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_ONE_SECOND:
            pc_onesec();
            break;
        default:
            BApplication::MessageReceived(message);
            break;
    }
}

/* Stop the emulation before any window goes away, so no frame or status
   message is sent to a window that no longer exists. */
void
EmuApplication::StopEmulation()
{
    delete one_second;
    one_second = nullptr;

    if (emu_thread != nullptr) {
        runctl_request_stop();
        emu_thread->join();
        delete emu_thread;
        emu_thread = nullptr;

        pc_close(nullptr);
    }
}

bool
EmuApplication::QuitRequested()
{
    MainWindow::app_quitting = true;
    haiku_keyboard_grab(false);
    StopEmulation();
    haiku_vmm_client_stop();
    return BApplication::QuitRequested();
}

/* A three-way question, Qt's QMessageBox with custom buttons: returns the
   index of the button pressed (Escape picks the last). */
static int32
ask(const char *title, const char *text, const char *b0, const char *b1, const char *b2, alert_type type)
{
    BAlert *alert = new BAlert(title, text, b0, b1, b2, B_WIDTH_FROM_LABEL, type);
    alert->SetShortcut(b2 ? 2 : (b1 ? 1 : 0), B_ESCAPE);
    return alert->Go();
}

int
main(int argc, char *argv[])
{
    EmuApplication app;

    /* A previous run may have died with the Command keys unassigned. */
    haiku_keyboard_restore_after_crash();

    setlocale(LC_NUMERIC, "C");

    if (!pc_init(argc, argv))
        return 0;

    if (!pc_init_roms()) {
        ui_msgbox_header(MBX_FATAL, (char *) "No ROMs found",
                         (char *) EMU_NAME " could not find any usable ROM images.\n\n"
                                  "Please download a ROM set from " EMU_ROMS_URL
                                  " and extract it into the \"roms\" directory.");
        return 6;
    }

    if (start_vmm) {
        app.mode = MODE_VM_MANAGER;
        app.Run();
        return 0;
    }

    pc_init_modules();

    for (int i = 0; i < argc; i++) {
        if (!strcasecmp(argv[i], "--assume_copied_vm")) {
            haiku_store_current_uuid();
            haiku_generate_new_mac_addresses();
            break;
        }
        if (!strcasecmp(argv[i], "--assume_moved_vm")) {
            haiku_store_current_uuid();
            break;
        }
    }

    /* UUID / copy / move detection, as in Qt. */
    if (!haiku_compare_uuid()) {
        const int32 choice = ask(EMU_NAME,
                                 "This machine might have been moved or copied.\n\n"
                                 "In order to ensure proper networking functionality, " EMU_NAME
                                 " needs to know if this machine was moved or copied.\n\n"
                                 "Select \"I Copied It\" if you are not sure.",
                                 "I Moved It", "I Copied It", "Cancel", B_WARNING_ALERT);
        if (choice == 1) {
            haiku_store_current_uuid();
            haiku_generate_new_mac_addresses();
        } else if (choice == 0)
            haiku_store_current_uuid();
    }

    if (settings_only) {
        app.mode = MODE_SETTINGS_ONLY;
        app.Run();
        return 0;
    }

    /* Warn about unsupported configs. */
    if (cpu_override) {
        const int32 choice = ask("You are loading an unsupported configuration",
                                 "CPU type filtering based on selected machine is disabled for this emulated machine.\n\n"
                                 "This makes it possible to choose a CPU that is otherwise incompatible with the selected "
                                 "machine. However, you may run into incompatibilities with the machine BIOS or other "
                                 "software.\n\n"
                                 "Enabling this setting is not officially supported and any bug reports filed may be "
                                 "closed as invalid.",
                                 "Exit", "Continue", nullptr, B_WARNING_ALERT);
        if (choice != 1)
            return 0;
    }

    app.Run();

    /* pc_close() has saved the configuration and NVR and closed every device
       by now. Leave without destroying the BApplication: OpenAL's Haiku
       backend leaves a BSoundPlayer node running after alcCloseDevice(), and
       tearing down the Media Kit under it crashes that node's thread
       (BTimeSource::GetTime in BMediaEventLooper::ControlLoop). */
    fflush(nullptr);
    _exit(0);
}

/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the VM manager (the Qt VMManager*
 *          classes), shown when 86Box starts without a machine (start_vmm),
 *          and the link between a running machine and the manager that
 *          started it (VMManagerClientSocket in Qt).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_VMMANAGER_HPP
#define HAIKU_VMMANAGER_HPP

class BWindow;

/* Creates and shows the VM manager window. Called from the application's
   ReadyToRun() when start_vmm is set; closing the window quits the
   application (the window posts B_QUIT_REQUESTED to be_app). */
BWindow *haiku_vmmanager_create_window(void);

/* In a machine started by the manager (the VMM_86BOX_SOCKET environment
   variable is set): connect to it. Commands from the manager (pause,
   reset, settings, Ctrl+Alt+Del, shutdown requests, ...) are posted to
   `main_window` as the frontend's messages (haiku_frontend.hpp). Does
   nothing without the variable. */
void haiku_vmm_client_start(BWindow *main_window);

/* Tell the manager about the machine: running state (as Qt's
   VMManagerProtocol::RunningState: 0 running, 1 paused, 2 running/waiting,
   3 paused/waiting), the configuration changed, the window was activated.
   No-ops when not connected. */
void haiku_vmm_client_running_state(int state);
void haiku_vmm_client_configuration_changed(void);
void haiku_vmm_client_window_activated(void);

/* Closes the connection (at quit). */
void haiku_vmm_client_stop(void);

#endif

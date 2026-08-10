#ifndef PPC_SPOTI_RECEIVER_ENTRY_H
#define PPC_SPOTI_RECEIVER_ENTRY_H

/*
 * The AirPlay backend's entry point, run on its own detached pthread
 * inside the app (ppc_pod_gui_main.m), which owns the process's actual
 * main() (Cocoa's NSApplicationMain) - this is not a process main() itself.
 */

void *airplay_backend_thread_main(void *arg);

#endif

#ifndef PPC_POD_SHUTDOWN_LISTENER_H
#define PPC_POD_SHUTDOWN_LISTENER_H

#define SHUTDOWN_LISTENER_PORT 8990

void *shutdown_listener_thread_main(void *arg);

#endif

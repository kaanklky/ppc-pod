#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <CoreServices/CoreServices.h>

#include "shutdown_listener.h"
#include "app_settings.h"

#define SHUTDOWN_LISTENER_RECV_TIMEOUT_SECONDS 2
#define SHUTDOWN_LISTENER_PROCESS_SYSTEM 1
#define SHUTDOWN_LISTENER_DUPLICATE_WINDOW_SECONDS 30

static const char k_response_ok[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 14\r\n"
    "Connection: close\r\n"
    "\r\n"
    "shutting down\n";

static const char k_response_not_allowed[] =
    "HTTP/1.1 405 Method Not Allowed\r\n"
    "Allow: GET\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static int listen_tcp_any(unsigned short port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    int yes = 1;
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    if (listen(fd, 2) != 0) { close(fd); return -1; }
    return fd;
}

static int request_system_shutdown(void)
{
    ProcessSerialNumber psn;
    AEDesc target;
    AppleEvent event;
    OSStatus err;

    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN = SHUTDOWN_LISTENER_PROCESS_SYSTEM;

    err = AECreateDesc(typeProcessSerialNumber, &psn, sizeof(psn), &target);
    if (err != noErr) return -1;

    err = AECreateAppleEvent(kCoreEventClass, kAEShutDown, &target,
                             kAutoGenerateReturnID, kAnyTransactionID, &event);
    AEDisposeDesc(&target);
    if (err != noErr) return -1;

    err = AESendMessage(&event, NULL, kAENoReply | kAENeverInteract, kAEDefaultTimeout);
    AEDisposeDesc(&event);
    return (err == noErr) ? 0 : -1;
}

static void send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

void *shutdown_listener_thread_main(void *arg)
{
    const app_settings *settings = (const app_settings *)arg;
    int listen_fd;
    time_t last_shutdown_at = 0;

    if (settings == NULL) return NULL;

    listen_fd = listen_tcp_any((unsigned short)settings->shutdown_port);
    if (listen_fd < 0) {
        fprintf(stderr, "shutdown: failed to listen on port %d - exiting\n", settings->shutdown_port);
        return NULL;
    }
    fprintf(stderr, "shutdown: HTTP listener on 0.0.0.0:%d\n", settings->shutdown_port);

    for (;;) {
        char buf[16];
        struct timeval tv;
        ssize_t n;
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) continue;

        tv.tv_sec = SHUTDOWN_LISTENER_RECV_TIMEOUT_SECONDS;
        tv.tv_usec = 0;
        setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        n = recv(conn_fd, buf, sizeof(buf), 0);
        if (n >= 4 && strncmp(buf, "GET ", 4) == 0) {
            time_t now = time(NULL);
            send_all(conn_fd, k_response_ok, sizeof(k_response_ok) - 1);
            close(conn_fd);
            if (last_shutdown_at != 0 && now - last_shutdown_at < SHUTDOWN_LISTENER_DUPLICATE_WINDOW_SECONDS) {
                fprintf(stderr, "shutdown: GET received - shutdown already requested, ignoring\n");
                continue;
            }
            last_shutdown_at = now;
            fprintf(stderr, "shutdown: GET received - requesting system shutdown\n");
            if (request_system_shutdown() != 0) {
                fprintf(stderr, "shutdown: failed to send shutdown Apple Event\n");
            }
        } else {
            send_all(conn_fd, k_response_not_allowed, sizeof(k_response_not_allowed) - 1);
            close(conn_fd);
        }
    }

    return NULL;
}

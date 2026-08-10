/*
 * AirPlay 1 ("classic AirPlay") receiver entry point. The sending device
 * does all of the audio-source app's own work (decoding, decryption, etc.)
 * and streams the resulting compressed audio to this receiver, the same
 * way any AirPlay speaker works - no account/pairing concept, anyone on
 * the network can use it with whatever app they're using.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mdns.h"
#include "airplay_rtsp.h"
#include "app_state.h"
#include "receiver_entry.h"

/* 5000 is the traditional AirPlay 1 default, not a protocol requirement -
 * a real client only ever connects to whatever port this device's own
 * mDNS SRV record advertises (mdns_args.cfg.port below), so any free port
 * works. */
#define AIRPLAY_RTSP_PORT 5002
#define AIRPLAY_DEVICE_NAME "PowerPC Pod"

typedef struct { mdns_service_config cfg; } airplay_mdns_args;

/* Some senders tear down and re-establish the whole RTSP session on
 * pause/resume rather than keeping one connection open. Clearing the
 * peer/backend state immediately on disconnect would make every pause
 * flash the UI into "disconnected" for the brief reconnect window.
 * Deferring the clear by a few seconds, and skipping it if a new
 * connection has since been accepted (tracked via this generation
 * counter), rides out a normal pause/resume without a visible flicker
 * while still clearing for an actual disconnect. */
static volatile unsigned long g_airplay_session_generation = 0;

typedef struct { unsigned long generation; } airplay_deferred_clear_args;

#define AIRPLAY_DISCONNECT_GRACE_SECONDS 3

static void *airplay_deferred_clear_thread(void *arg)
{
    airplay_deferred_clear_args *a = (airplay_deferred_clear_args *)arg;
    unsigned long generation = a->generation;
    free(a);
    sleep(AIRPLAY_DISCONNECT_GRACE_SECONDS);
    if (g_airplay_session_generation == generation) {
        app_state_clear_peer(app_state_shared());
        app_state_clear_backend_if(app_state_shared(), APP_BACKEND_AIRPLAY);
    }
    return NULL;
}

static void *airplay_mdns_thread_main(void *arg)
{
    airplay_mdns_args *args = (airplay_mdns_args *)arg;
    int fd = mdns_open_socket();
    if (fd < 0) return NULL;
    mdns_responder_run(fd, &args->cfg, 0);
    return NULL;
}

static int local_ipv4_be(uint32_t *out_be)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in remote, local;
    socklen_t local_len = sizeof(local);
    if (fd < 0) return -1;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    remote.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (connect(fd, (struct sockaddr *)&remote, sizeof(remote)) != 0) { close(fd); return -1; }
    if (getsockname(fd, (struct sockaddr *)&local, &local_len) != 0) { close(fd); return -1; }
    close(fd);
    *out_be = local.sin_addr.s_addr;
    return 0;
}

static int listen_tcp(unsigned short port)
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
    if (listen(fd, 4) != 0) { close(fd); return -1; }
    return fd;
}

/* Runs on its own detached pthread inside ppc_pod_gui_main.m, which owns
 * the real process main() (Cocoa's NSApplicationMain) - see
 * receiver_entry.h. One blocking accept() loop, single-active-session
 * scope limit. */
void *airplay_backend_thread_main(void *arg)
{
    uint32_t ipv4_be;
    unsigned char device_id6[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    char instance_name[64];
    int listen_fd;
    const char *device_name = (arg != NULL) ? (const char *)arg : AIRPLAY_DEVICE_NAME;

    if (local_ipv4_be(&ipv4_be) != 0) {
        fprintf(stderr, "airplay: could not determine local IPv4 address - exiting\n");
        return NULL;
    }

    snprintf(instance_name, sizeof(instance_name), "%02X%02X%02X%02X%02X%02X@%s",
             device_id6[0], device_id6[1], device_id6[2],
             device_id6[3], device_id6[4], device_id6[5], device_name);

    {
        static airplay_mdns_args mdns_args;
        pthread_t mdns_thread;
        memset(&mdns_args, 0, sizeof(mdns_args));
        mdns_args.cfg.instance_name = instance_name;
        mdns_args.cfg.hostname = "ppc-pod-airplay";
        mdns_args.cfg.port = AIRPLAY_RTSP_PORT;
        mdns_args.cfg.ipv4_be = ipv4_be;
        mdns_args.cfg.service_type = "_raop._tcp";
        /* TXT record set for classic AirPlay 1, matching shairport-sync's
         * bonjour_strings.c non-AirPlay-2 branch. */
        mdns_args.cfg.txt_records[0] = "sf=0x4";
        mdns_args.cfg.txt_records[1] = "fv=76400.10";
        mdns_args.cfg.txt_records[2] = "am=AirPort4,107";
        mdns_args.cfg.txt_records[3] = "vs=105.1";
        mdns_args.cfg.txt_records[4] = "tp=TCP,UDP";
        mdns_args.cfg.txt_records[5] = "vn=65537";
        mdns_args.cfg.txt_records[6] = "md=0,1,2";
        mdns_args.cfg.txt_records[7] = "ss=16";
        mdns_args.cfg.txt_records[8] = "sr=44100";
        mdns_args.cfg.txt_records[9] = "da=true";
        mdns_args.cfg.txt_records[10] = "sv=false";
        mdns_args.cfg.txt_records[11] = "et=0,1";
        mdns_args.cfg.txt_records[12] = "ek=1";
        mdns_args.cfg.txt_records[13] = "cn=0,1";
        mdns_args.cfg.txt_records[14] = "ch=2";
        mdns_args.cfg.txt_records[15] = "txtvers=1";
        mdns_args.cfg.txt_record_count = 16;

        if (pthread_create(&mdns_thread, NULL, airplay_mdns_thread_main, &mdns_args) == 0) {
            pthread_detach(mdns_thread);
            fprintf(stderr, "airplay: mDNS advertising _raop._tcp as \"%s\"\n", instance_name);
        } else {
            fprintf(stderr, "airplay: failed to start mDNS thread - continuing without discovery\n");
        }
    }

    listen_fd = listen_tcp(AIRPLAY_RTSP_PORT);
    if (listen_fd < 0) {
        fprintf(stderr, "airplay: failed to listen on port %d - exiting\n", AIRPLAY_RTSP_PORT);
        return NULL;
    }
    fprintf(stderr, "airplay: RTSP server listening on 0.0.0.0:%d\n", AIRPLAY_RTSP_PORT);

    /* Scope limit: one RTSP connection at a time, handled synchronously
     * on this thread - a second sender connecting while one is already
     * active will block until the first disconnects/times out, not
     * implemented as an explicit takeover. */
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int conn_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (conn_fd < 0) continue;

        /* Peer IP is only known here (accept()'s own sockaddr) - a human
         * friendly name isn't reliably available in AirPlay 1's handshake
         * (see cocoa_ui.m's window-title comment), so app_state's
         * peer_name is left empty rather than faked. */
        g_airplay_session_generation++;
        app_state_set_backend(app_state_shared(), APP_BACKEND_AIRPLAY);
        app_state_set_peer(app_state_shared(), NULL, inet_ntoa(peer.sin_addr));

        airplay_rtsp_serve_connection(conn_fd, ipv4_be, device_id6, device_name);

        /* Only stop the playing indicator immediately; leave the last-known
         * track/art displayed until a fresh session's own metadata
         * replaces it. */
        app_state_set_stopped(app_state_shared());

        /* Deferred, cancellable clear of the "connected" indicator itself
         * (see airplay_deferred_clear_thread above) - runs on its own
         * thread so this loop can immediately go back to accept()-ing a
         * fast reconnect instead of blocking on the grace-period sleep. */
        {
            airplay_deferred_clear_args *dargs =
                (airplay_deferred_clear_args *)malloc(sizeof(airplay_deferred_clear_args));
            if (dargs != NULL) {
                pthread_t clear_thread;
                dargs->generation = g_airplay_session_generation;
                if (pthread_create(&clear_thread, NULL, airplay_deferred_clear_thread, dargs) == 0) {
                    pthread_detach(clear_thread);
                } else {
                    free(dargs);
                }
            }
        }
    }

    return NULL;
}

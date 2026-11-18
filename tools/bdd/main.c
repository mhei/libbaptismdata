#include "bdd.h"

#include <signal.h>
#include <errno.h>
#include <time.h>
#include <systemd/sd-daemon.h>
#include <stdio.h>
#include <string.h>
#include <gnutls/gnutls.h>

int main(int argc, char **argv)
{
    const char *path = NULL;
    bool explicit_path = false;
    struct bdd_config config;
    struct MHD_Daemon *daemon;
    sigset_t signals;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            puts("Usage: bdd [--config FILE]\nConfiguration: /lib/bdd, /run/bdd, /etc/bdd (bdd.conf and bdd.conf.d/*.conf)");
            return 0;
        }
        if (!strcmp(argv[i], "--config") && i + 1 < argc && !explicit_path) {
            path = argv[++i];
            explicit_path = true;
        } else {
            fprintf(stderr, "bdd: invalid arguments; use --help\n");
            return 1;
        }
    }
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &signals, NULL) < 0 || gnutls_global_init() < 0)
        return 1;
    if (bdd_config_load(&config, path, explicit_path) < 0) {
        gnutls_global_deinit();
        return 1;
    }
    daemon = bdd_server_start(&config);
    if (!daemon) {
        fprintf(stderr, "bdd: could not start HTTP/TLS listener\n");
        bdd_config_clear(&config);
        gnutls_global_deinit();
        return 1;
    }
    fprintf(stderr, "bdd: listening on %s://%s:%u%s\n", config.tls ? "https" : "http",
            config.address, config.port, config.client_ca ? " (client certificate required)" : "");
    struct bdd_mdns *mdns = bdd_mdns_start(&config);
    /* Notify only after the listener can accept requests. */
    sd_notify(0, "READY=1");
    /* Retry discovery independently of HTTP availability after Avahi restarts. */
    int rv;
    do {
        const struct timespec timeout = {.tv_sec = 5};
        rv = sigtimedwait(&signals, NULL, &timeout);
        int wait_error = errno;
        if (rv < 0 && wait_error == EAGAIN && config.zeroconf && bdd_mdns_failed(mdns)) {
            bdd_mdns_stop(mdns);
            mdns = bdd_mdns_start(&config);
        }
        errno = wait_error;
    } while (rv < 0 && (errno == EAGAIN || errno == EINTR));
    sd_notify(0, "STOPPING=1");
    bdd_mdns_stop(mdns);
    MHD_stop_daemon(daemon);
    bdd_config_clear(&config);
    gnutls_global_deinit();
    return rv >= 0 ? 0 : 1;
}

#include "bdd.h"

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/malloc.h>
#include <avahi-common/thread-watch.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

struct bdd_mdns {
    AvahiThreadedPoll *poll;
    AvahiClient *client;
    AvahiEntryGroup *group;
    const struct bdd_config *config;
    char *name;
    bool running;
    atomic_bool failed;
};

static void publish(struct bdd_mdns *mdns);

static void group_changed(AvahiEntryGroup *group, AvahiEntryGroupState state, void *userdata)
{
    struct bdd_mdns *mdns = userdata;
    if (state == AVAHI_ENTRY_GROUP_COLLISION) {
        /* DNS-SD instance names must remain unique on the local network. */
        char *name = avahi_alternative_service_name(mdns->name);
        if (!name) { atomic_store(&mdns->failed, true); return; }
        avahi_free(mdns->name);
        mdns->name = name;
        avahi_entry_group_reset(group);
        publish(mdns);
    } else if (state == AVAHI_ENTRY_GROUP_FAILURE) {
        atomic_store(&mdns->failed, true);
        fprintf(stderr, "bdd: Zeroconf registration failed\n");
    }
}

static void publish(struct bdd_mdns *mdns)
{
    if (!mdns->group)
        mdns->group = avahi_entry_group_new(mdns->client, group_changed, mdns);
    if (!mdns->group) {
        atomic_store(&mdns->failed, true);
        return;
    }
    int empty = avahi_entry_group_is_empty(mdns->group);
    if (empty < 0) { atomic_store(&mdns->failed, true); return; }
    if (!empty) return;
    if (avahi_entry_group_add_service(mdns->group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0,
            mdns->name, mdns->config->tls ? "_https._tcp" : "_http._tcp", NULL, NULL,
            (uint16_t)mdns->config->port, "path=/api/v1/baptism-data",
            "openapi=/openapi.yaml", "api=baptism-data", NULL) < 0 ||
        avahi_entry_group_commit(mdns->group) < 0) {
        atomic_store(&mdns->failed, true);
        fprintf(stderr, "bdd: could not announce HTTP API\n");
    }
}

static void client_changed(AvahiClient *client, AvahiClientState state, void *userdata)
{
    struct bdd_mdns *mdns = userdata;
    mdns->client = client;
    if (state == AVAHI_CLIENT_S_RUNNING) publish(mdns);
    else if ((state == AVAHI_CLIENT_S_REGISTERING || state == AVAHI_CLIENT_S_COLLISION) && mdns->group)
        avahi_entry_group_reset(mdns->group);
    else if (state == AVAHI_CLIENT_FAILURE) {
        atomic_store(&mdns->failed, true);
        fprintf(stderr, "bdd: Avahi connection lost; retrying\n");
    }
}

struct bdd_mdns *bdd_mdns_start(const struct bdd_config *config)
{
    if (!config->zeroconf) return NULL;
    struct bdd_mdns *mdns = calloc(1, sizeof(*mdns));
    if (!mdns) return NULL;
    atomic_init(&mdns->failed, false);
    char host[128] = "device", name[160];
    gethostname(host, sizeof(host) - 1);
    host[sizeof(host)-1] = '\0';
    snprintf(name, sizeof(name), "Baptism Data %.40s", host);
    mdns->config = config;
    mdns->name = avahi_strdup(name);
    mdns->poll = avahi_threaded_poll_new();
    if (!mdns->name || !mdns->poll) goto fail;
    int error;
    mdns->client = avahi_client_new(avahi_threaded_poll_get(mdns->poll), AVAHI_CLIENT_NO_FAIL,
                                    client_changed, mdns, &error);
    if (!mdns->client || avahi_threaded_poll_start(mdns->poll) < 0) goto fail;
    mdns->running = true;
    return mdns;
fail:
    fprintf(stderr, "bdd: Zeroconf unavailable\n");
    bdd_mdns_stop(mdns);
    return NULL;
}

void bdd_mdns_stop(struct bdd_mdns *mdns)
{
    if (!mdns) return;
    if (mdns->running) avahi_threaded_poll_stop(mdns->poll);
    /* Freeing the client withdraws its service group as well. */
    if (mdns->client) avahi_client_free(mdns->client);
    if (mdns->poll) avahi_threaded_poll_free(mdns->poll);
    avahi_free(mdns->name);
    free(mdns);
}

bool bdd_mdns_failed(const struct bdd_mdns *mdns)
{
    return !mdns || atomic_load(&mdns->failed);
}

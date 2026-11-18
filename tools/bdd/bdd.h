#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <json-c/json.h>
#include <microhttpd.h>

#define BDD_MAX_BODY (1024U * 1024U)

struct bdd_config {
    char *address;
    unsigned int port;
    struct json_object *writable_keys;
    char *password;
    bool zeroconf;
    bool tls;
    char *certificate;
    char *private_key;
    char *client_ca;
};

bool bdd_valid_key(const char *key);
struct json_object *bdd_parse_json(const char *data, size_t length);
int bdd_config_load(struct bdd_config *config, const char *path, bool explicit_path);
void bdd_config_clear(struct bdd_config *config);
bool bdd_writable(const struct bdd_config *config, const char *key);
struct MHD_Daemon *bdd_server_start(const struct bdd_config *config);

struct bdd_mdns;
struct bdd_mdns *bdd_mdns_start(const struct bdd_config *config);
void bdd_mdns_stop(struct bdd_mdns *mdns);
bool bdd_mdns_failed(const struct bdd_mdns *mdns);

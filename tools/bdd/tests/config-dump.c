#include "bdd.h"
#include <stdio.h>
#include <gnutls/gnutls.h>

/* Test-only probe exercises production precedence without opening sockets. */
int main(void)
{
    struct bdd_config config;
    if (gnutls_global_init() < 0) return 1;
    if (bdd_config_load(&config, NULL, false) < 0) {
        gnutls_global_deinit();
        return 1;
    }
    struct json_object *output = json_object_new_object();
    json_object_object_add(output, "address", json_object_new_string(config.address));
    json_object_object_add(output, "port", json_object_new_int((int)config.port));
    json_object_object_add(output, "keys", json_object_get(config.writable_keys));
    json_object_object_add(output, "password", json_object_new_boolean(config.password && *config.password));
    puts(json_object_to_json_string(output));
    json_object_put(output);
    bdd_config_clear(&config);
    gnutls_global_deinit();
    return 0;
}

#include "bdd.h"
#include <crypt.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <libeconf.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

/* Keep all file reads bounded, including certificates and configuration. */
static char *read_file(const char *path, size_t limit, size_t *length)
{
    FILE *file = fopen(path, "rb");
    char *buffer;
    size_t n;
    if (!file)
        return NULL;
    buffer = malloc(limit + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    n = fread(buffer, 1, limit, file);
    if (ferror(file) || (n == limit && fgetc(file) != EOF)) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    fclose(file);
    buffer[n] = '\0';
    *length = n;
    return buffer;
}

struct json_object *bdd_parse_json(const char *data, size_t length)
{
    /* json-c stores object member names as C strings; reject NUL before it
     * could turn e.g. "value\u0000extra" into the accepted name "value".
     * Skip escaped backslashes so a literal "\\u0000" remains valid text. */
    if (memchr(data, '\0', length))
        return NULL;
    for (size_t i = 0; i < length; ++i) {
        if (data[i] == '\\' && ++i < length && data[i] == 'u' &&
            length - i >= 5 && !memcmp(data + i + 1, "0000", 4))
            return NULL;
    }
    struct json_tokener *tok = json_tokener_new();
    struct json_object *obj;
    size_t end;
    if (!tok || length >= INT_MAX) {
        if (tok)
            json_tokener_free(tok);
        return NULL;
    }
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    obj = json_tokener_parse_ex(tok, data, (int)length);
    end = json_tokener_get_parse_end(tok);
    while (end < length && isspace((unsigned char)data[end]))
        ++end;
    if (json_tokener_get_error(tok) != json_tokener_success || end != length) {
        json_object_put(obj);
        obj = NULL;
    }
    json_tokener_free(tok);
    return obj;
}

bool bdd_valid_key(const char *key)
{
    const unsigned char *p = (const unsigned char *)key;
    if (!*p)
        return false;
    for (; *p; ++p)
        if (*p < 0x20 || *p == 0x7f || *p == '=')
            return false;
    /* Validate UTF-8 via the same parser used for JSON request bodies. */
    struct json_object *str = json_object_new_string(key);
    if (!str)
        return false;
    const char *encoded = json_object_to_json_string_ext(str, JSON_C_TO_STRING_PLAIN);
    struct json_object *checked = bdd_parse_json(encoded, strlen(encoded));
    bool ok = checked != NULL;
    json_object_put(checked);
    json_object_put(str);
    return ok;
}

static bool string_value(struct json_object *value)
{
    return json_object_is_type(value, json_type_string) &&
        strlen(json_object_get_string(value)) == (size_t)json_object_get_string_len(value);
}

static bool known_fields(struct json_object *obj, const char *const *names)
{
    json_object_object_foreach(obj, name, value) {
        size_t i;
        (void)value;
        for (i = 0; names[i]; ++i)
            if (!strcmp(name, names[i]))
                break;
        if (!names[i])
            return false;
    }
    return true;
}

static char *pem_file(struct json_object *obj, const char *field)
{
    struct json_object *value;
    size_t size;
    char *pem;
    if (!json_object_object_get_ex(obj, field, &value) || !string_value(value) ||
        json_object_get_string(value)[0] != '/')
        return NULL;
    pem = read_file(json_object_get_string(value), 4 * BDD_MAX_BODY, &size);
    if (pem && (!size || strlen(pem) != size)) {
        free(pem);
        pem = NULL;
    }
    return pem;
}

static int load_tls(struct bdd_config *config, struct json_object *tls)
{
    static const char *const fields[] = {
        "enabled", "certificate_file", "private_key_file", "client_ca_file", NULL
    };
    gnutls_certificate_credentials_t credentials = NULL;
    gnutls_x509_crt_t cert = NULL;
    int rv = -1;
    if (!json_object_is_type(tls, json_type_object) || !known_fields(tls, fields))
        return -1;
    config->tls = json_object_object_length(tls) != 0;
    /* Validate all supplied paths before loading TLS credentials. */
    for (size_t i = 1; fields[i]; ++i) {
        struct json_object *path;
        if (json_object_object_get_ex(tls, fields[i], &path) &&
            (!string_value(path) || json_object_get_string(path)[0] != '/'))
            return -1;
    }
    if (!config->tls)
        return 0;
    if (MHD_is_feature_supported(MHD_FEATURE_TLS) != MHD_YES)
        return -1;
    config->certificate = pem_file(tls, "certificate_file");
    config->private_key = pem_file(tls, "private_key_file");
    struct json_object *ca_path;
    bool mtls = json_object_object_get_ex(tls, "client_ca_file", &ca_path);
    config->client_ca = mtls ? pem_file(tls, "client_ca_file") : NULL;
    if (!config->certificate || !config->private_key || (mtls && !config->client_ca))
        return -1;
    gnutls_datum_t chain = {(unsigned char *)config->certificate, (unsigned int)strlen(config->certificate)};
    gnutls_datum_t key = {(unsigned char *)config->private_key, (unsigned int)strlen(config->private_key)};
    gnutls_datum_t ca = {(unsigned char *)config->client_ca, (unsigned int)(config->client_ca ? strlen(config->client_ca) : 0)};
    if (gnutls_certificate_allocate_credentials(&credentials) < 0)
        goto out;
    if (gnutls_certificate_set_x509_key_mem(credentials, &chain, &key, GNUTLS_X509_FMT_PEM) < 0 ||
        (mtls && gnutls_certificate_set_x509_trust_mem(credentials, &ca, GNUTLS_X509_FMT_PEM) <= 0))
        goto out;
    if (gnutls_x509_crt_init(&cert) < 0 ||
        gnutls_x509_crt_import(cert, &chain, GNUTLS_X509_FMT_PEM) < 0)
        goto out;
    time_t now = time(NULL);
    time_t activation = gnutls_x509_crt_get_activation_time(cert);
    time_t expiration = gnutls_x509_crt_get_expiration_time(cert);
    if (activation == (time_t)-1 || expiration == (time_t)-1 || now < activation || now > expiration)
        goto out;
    rv = 0;
out:
    if (cert)
        gnutls_x509_crt_deinit(cert);
    if (credentials)
        gnutls_certificate_free_credentials(credentials);
    return rv;
}

#ifndef BDD_RUN_DIR
#define BDD_RUN_DIR "/run/bdd"
#endif
#ifndef BDD_ETC_DIR
#define BDD_ETC_DIR "/etc/bdd"
#endif

/* libeconf selects the main file, merges drop-ins and resolves masks. The
 * callback bounds file size without reimplementing its parser or precedence. */
static bool config_file_check(const char *path, const void *userdata)
{
    (void)userdata;
    struct stat st, null_device;
    return stat(path, &st) == 0 &&
        ((S_ISREG(st.st_mode) && st.st_size <= BDD_MAX_BODY) ||
         (S_ISCHR(st.st_mode) && stat("/dev/null", &null_device) == 0 &&
          st.st_rdev == null_device.st_rdev));
}

static bool optional(econf_err error)
{
    return error == ECONF_SUCCESS || error == ECONF_NOKEY || error == ECONF_NOGROUP;
}

static int validate_names(econf_file *file)
{
    static const char *const fields[] = {"ListenAddress", "Port", "WritableKey", "Password",
        "Zeroconf", "CertificateFile", "PrivateKeyFile", "ClientCAFile", NULL};
    char **groups = NULL, **keys = NULL;
    size_t count = 0;
    econf_err error = econf_getKeys(file, NULL, &count, &keys);
    if (error == ECONF_SUCCESS && count) { econf_freeArray(keys); return -1; }
    if (error != ECONF_SUCCESS && error != ECONF_NOKEY && error != ECONF_NOGROUP) return -1;
    econf_freeArray(keys); keys = NULL;
    count = 0;
    error = econf_getGroups(file, &count, &groups);
    if (error != ECONF_SUCCESS && error != ECONF_NOGROUP) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(groups[i], "bdd")) { econf_freeArray(groups); return -1; }
    }
    econf_freeArray(groups);
    error = econf_getKeys(file, "bdd", &count, &keys);
    if (error == ECONF_NOGROUP || error == ECONF_NOKEY) return 0;
    if (error != ECONF_SUCCESS) return -1;
    for (size_t i = 0; i < count; ++i) {
        size_t j;
        for (j = 0; fields[j] && strcmp(keys[i], fields[j]); ++j) {}
        if (!fields[j]) { econf_freeArray(keys); return -1; }
    }
    econf_freeArray(keys);
    return 0;
}

int bdd_config_load(struct bdd_config *config, const char *path, bool explicit_path)
{
    econf_file *file = NULL;
    struct json_object *tls = NULL;
    char *keys = NULL, *value = NULL;
    econf_err error;
    int rv = -1;
    *config = (struct bdd_config){.port = 8080, .zeroconf = true};
    config->writable_keys = json_object_new_array();
    tls = json_object_new_object();
    if (!config->writable_keys || !tls) goto out;
    /* Joining repeated WritableKey entries preserves one exact key per line.
     * A drop-in replaces the complete WritableKey value from earlier files. */
    char *explicit_dir = NULL;
    const char *name = "bdd", *suffix = "conf";
    const char *options = "JOIN_SAME_ENTRIES=1;PARSING_DIRS=" BDD_VENDOR_DIR ":" BDD_RUN_DIR ":" BDD_ETC_DIR;
    char *custom_options = NULL;
    if (explicit_path) {
        explicit_dir = strdup(path);
        if (!explicit_dir) goto out;
        char *slash = strrchr(explicit_dir, '/');
        if (slash) {
            *slash = '\0';
            name = slash + 1;
        } else name = path;
        const char *directory = slash ? (*explicit_dir ? explicit_dir : "/") : ".";
        size_t length = strlen(directory) + 64;
        custom_options = malloc(length);
        if (!custom_options) { free(explicit_dir); goto out; }
        snprintf(custom_options, length, "JOIN_SAME_ENTRIES=1;PARSING_DIRS=%s", directory);
        options = custom_options;
        suffix = NULL;
    }
    error = econf_newKeyFile_with_options(&file, options);
    free(custom_options);
    if (error == ECONF_SUCCESS)
        error = econf_readConfigWithCallback(&file, NULL, BDD_VENDOR_DIR,
                                             name, suffix, "=", "#", config_file_check, NULL);
    free(explicit_dir);
    if (error == ECONF_NOFILE && !explicit_path) {
        config->address = strdup("0.0.0.0");
        rv = config->address ? 0 : -1;
        goto out;
    }
    if (error != ECONF_SUCCESS) {
        fprintf(stderr, "bdd: cannot read configuration: %s\n", econf_errString(error));
        goto out;
    }
    if (validate_names(file) < 0) goto out;
    if (!optional(econf_getStringValueDef(file, "bdd", "ListenAddress", &config->address, "0.0.0.0")))
        goto out;
    struct in_addr address;
    if (!config->address || inet_pton(AF_INET, config->address, &address) != 1) goto out;
    /* Validate the full decimal string: do not accept truncated numeric input. */
    error = econf_getStringValue(file, "bdd", "Port", &value);
    if (!optional(error)) goto out;
    if (error == ECONF_SUCCESS) {
        if (!value || !*value) goto out;
        for (const char *p = value; *p; ++p) if (!isdigit((unsigned char)*p)) goto out;
        char *end;
        errno = 0;
        unsigned long port = strtoul(value, &end, 10);
        if (errno || *end || port < 1 || port > 65535) goto out;
        config->port = (unsigned int)port;
    }
    free(value); value = NULL;
    if (!optional(econf_getBoolValue(file, "bdd", "Zeroconf", &config->zeroconf))) goto out;
    if (!optional(econf_getStringValue(file, "bdd", "Password", &config->password))) goto out;
    error = econf_getStringValue(file, "bdd", "WritableKey", &keys);
    if (!optional(error)) goto out;
    if (keys) {
        char *save = NULL;
        for (char *key = strtok_r(keys, "\n,", &save); key; key = strtok_r(NULL, "\n,", &save)) {
            while (isspace((unsigned char)*key)) ++key;
            char *end = key + strlen(key);
            while (end > key && isspace((unsigned char)end[-1])) *--end = '\0';
            if (!*key || bdd_writable(config, key)) continue;
            if (!bdd_valid_key(key)) goto out;
            json_object_array_add(config->writable_keys, json_object_new_string(key));
        }
    }
    if (config->password && *config->password) {
        /* Validate the setting at startup; never treat a stored hash as plaintext. */
        struct crypt_data state = {0};
        char *probe = config->password[0] == '$' ? crypt_r("", config->password, &state) : NULL;
        bool valid = probe && probe[0] != '*' && strlen(probe) == strlen(config->password);
        const char *digest = strrchr(config->password, '$');
        if (!digest || !digest[1]) valid = false;
        else {
            size_t prefix = (size_t)(digest + 1 - config->password);
            if (valid && strncmp(probe, config->password, prefix)) valid = false;
            for (++digest; *digest; ++digest)
                if (!strchr("./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", *digest)) valid = false;
        }
        gnutls_memset(&state, 0, sizeof(state));
        if (!valid) goto out;
    }
    const char *names[] = {"CertificateFile", "PrivateKeyFile", "ClientCAFile"};
    const char *fields[] = {"certificate_file", "private_key_file", "client_ca_file"};
    for (size_t i = 0; i < 3; ++i) {
        error = econf_getStringValue(file, "bdd", names[i], &value);
        if (!optional(error)) goto out;
        if (error == ECONF_SUCCESS) {
            if (!value) goto out;
            json_object_object_add(tls, fields[i], json_object_new_string(value));
        }
        free(value); value = NULL;
    }
    if (load_tls(config, tls) < 0) goto out;
    rv = 0;
out:
    free(keys);
    free(value);
    econf_freeFile(file);
    json_object_put(tls);
    if (rv < 0) {
        fprintf(stderr, "bdd: configuration or TLS credentials invalid/unreadable\n");
        bdd_config_clear(config);
    }
    return rv;
}

void bdd_config_clear(struct bdd_config *config)
{
    free(config->address);
    if (config->password) {
        gnutls_memset(config->password, 0, strlen(config->password));
        free(config->password);
    }
    json_object_put(config->writable_keys);
    free(config->certificate);
    if (config->private_key) {
        gnutls_memset(config->private_key, 0, strlen(config->private_key));
        free(config->private_key);
    }
    free(config->client_ca);
    *config = (struct bdd_config){0};
}

bool bdd_writable(const struct bdd_config *config, const char *key)
{
    for (size_t i = 0; i < json_object_array_length(config->writable_keys); ++i)
        if (!strcmp(key, json_object_get_string(json_object_array_get_idx(config->writable_keys, i))))
            return true;
    return false;
}

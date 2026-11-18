#include "bdd.h"
#include <crypt.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <baptismdata.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

#define API_ROOT "/api/v1/baptism-data"

struct request {
    char *uri;
    char *key;
    char *body;
    size_t length;
    bool metadata;
    bool asset;
    bool initialized;
    bool answered;
    unsigned int upload_error;
};

static enum MHD_Result respond(struct MHD_Connection *connection, struct request *request,
                               unsigned int status, struct json_object *body, const char *allow)
{
    const char *text = body ? json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN) : "";
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(text), (void *)text,
                                                                  MHD_RESPMEM_MUST_COPY);
    json_object_put(body);
    if (!response)
        return MHD_NO;
    if (status != 204)
        MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");
    MHD_add_response_header(response, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
    /* API clients receive the standard Basic challenge; the web editor handles
     * authentication inline and identifies its requests to suppress popups. */
    const char *requested_with = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-Requested-With");
    if (status == 401 && (!requested_with || strcmp(requested_with, "XMLHttpRequest")))
        MHD_add_response_header(response, "WWW-Authenticate", "Basic realm=\"bdd\", charset=\"UTF-8\"");
    if (allow)
        MHD_add_response_header(response, MHD_HTTP_HEADER_ALLOW, allow);
    if (status == 201)
        MHD_add_response_header(response, MHD_HTTP_HEADER_LOCATION, request->uri);
    enum MHD_Result result = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    request->answered = true;
    return result;
}

static enum MHD_Result error_response(struct MHD_Connection *connection, struct request *request,
                                      unsigned int status, const char *code, const char *message,
                                      const char *allow)
{
    struct json_object *body = json_object_new_object();
    struct json_object *error = json_object_new_object();
    if (!body || !error) {
        json_object_put(body);
        json_object_put(error);
        return MHD_NO;
    }
    json_object_object_add(error, "code", json_object_new_string(code));
    json_object_object_add(error, "message", json_object_new_string(message));
    json_object_object_add(body, "error", error);
    return respond(connection, request, status, body, allow);
}

static enum MHD_Result internal_error(struct MHD_Connection *connection, struct request *request)
{
    fprintf(stderr, "bdd: request failed during storage or memory operation\n");
    return error_response(connection, request, 500, "internal_error", "Storage or internal error", NULL);
}

static bool authenticated(struct MHD_Connection *connection)
{
    const union MHD_ConnectionInfo *info = MHD_get_connection_info(connection,
                                                        MHD_CONNECTION_INFO_GNUTLS_SESSION);
    unsigned int count = 0, status = 0;
    if (!info || !info->tls_session)
        return false;
    gnutls_session_t session = info->tls_session;
    if (gnutls_certificate_type_get(session) != GNUTLS_CRT_X509 ||
        !gnutls_certificate_get_peers(session, &count) || count == 0)
        return false;
    gnutls_typed_vdata_st purpose = {
        .type = GNUTLS_DT_KEY_PURPOSE_OID,
        .data = (unsigned char *)GNUTLS_KP_TLS_WWW_CLIENT,
        .size = 0
    };
    return gnutls_certificate_verify_peers(session, &purpose, 1, &status) == 0 && status == 0;
}

static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Parse the original URI, never MHD's already unescaped URL. */
static unsigned int parse_route(struct request *request)
{
    char *query = strchr(request->uri, '?');
    if (query)
        *query = '\0';
    if (!strcmp(request->uri, "/") || !strcmp(request->uri, "/index.html") ||
        !strcmp(request->uri, "/app.js") || !strcmp(request->uri, "/app.css") || !strcmp(request->uri, "/bootstrap.min.css") ||
        !strcmp(request->uri, "/openapi.yaml")) { request->asset = true; return 0; }
    if (!strcmp(request->uri, "/api/v1/metadata")) { request->metadata = true; return 0; }
    if (!strcmp(request->uri, API_ROOT))
        return 0;
    size_t prefix = strlen(API_ROOT);
    if (strncmp(request->uri, API_ROOT "/", prefix + 1))
        return 404;
    const char *source = request->uri + prefix + 1;
    if (strchr(source, '/'))
        return 404;
    request->key = malloc(strlen(source) + 1);
    if (!request->key)
        return 500;
    char *dest = request->key;
    while (*source) {
        unsigned char c = (unsigned char)*source++;
        if (c == '%') {
            if (!source[0] || !source[1])
                return 400;
            int high = hex_value((unsigned char)source[0]);
            int low = hex_value((unsigned char)source[1]);
            if (high < 0 || low < 0)
                return 400;
            c = (unsigned char)(high * 16 + low);
            source += 2;
        }
        if (!c)
            return 400;
        *dest++ = (char)c;
    }
    *dest = '\0';
    return bdd_valid_key(request->key) ? 0 : 400;
}

static bool json_content_type(const char *type)
{
    if (!type || strncasecmp(type, "application/json", 16))
        return false;
    type += 16;
    while (*type == ' ' || *type == '\t')
        ++type;
    return !*type || *type == ';';
}

static enum MHD_Result execute(struct MHD_Connection *connection, struct request *request,
                               const char *method, const struct bdd_config *config)
{
    struct baptismdata_ctx *ctx = NULL;
    struct json_object *input = NULL, *value = NULL, *output = NULL;
    unsigned int status = 200;
    bool put = !strcmp(method, "PUT");
    bool remove = !strcmp(method, "DELETE");
    if (put && request->key) {
        input = bdd_parse_json(request->body ? request->body : "", request->length);
        if (!json_object_is_type(input, json_type_object) || json_object_object_length(input) != 1 ||
            !json_object_object_get_ex(input, "value", &value) ||
            !json_object_is_type(value, json_type_string) ||
            strlen(json_object_get_string(value)) != (size_t)json_object_get_string_len(value)) {
            json_object_put(input);
            return error_response(connection, request, 400, "invalid_body",
                                  "Expected an object containing exactly one string field: value", NULL);
        }
    }
    /* Validate the entire batch before opening storage. NULL deletes a key;
     * an empty JSON string remains a stored empty string. */
    if (put && !request->key) {
        input = bdd_parse_json(request->body ? request->body : "", request->length);
        if (!json_object_is_type(input, json_type_object)) {
            json_object_put(input);
            return error_response(connection, request, 400, "invalid_body", "Expected a key/value object", NULL);
        }
        json_object_object_foreach(input, name, item) {
            if (!bdd_valid_key(name) || (item && !json_object_is_type(item, json_type_string))) {
                json_object_put(input);
                return error_response(connection, request, 400, "invalid_body", "Expected strings or null for deletion", NULL);
            }
            if (!bdd_writable(config, name)) {
                json_object_put(input);
                return error_response(connection, request, 403, "key_read_only", "Key is not writable", NULL);
            }
        }
    }
    if (baptismdata_open(&ctx) < 0)
        goto failure;
    if (put && !request->key) {
        /* Hold one library lock and commit once for the complete submission.
         * Closing without store discards staged changes when a setter fails. */
        json_object_object_foreach(input, name, item) {
            if (baptismdata_set_var(ctx, name, item ? json_object_get_string(item) : NULL) < 0)
                goto failure;
        }
        if (baptismdata_store(ctx) < 0) goto failure;
        status = 204;
    } else if (request->metadata) {
        output = json_object_new_object();
        struct json_object *keys = json_object_new_object();
        if (!output || !keys) { json_object_put(keys); goto failure; }
        json_object_object_add(output, "keys", keys);
        json_object_object_add(output, "write_authentication_required",
            json_object_new_boolean(config->password && *config->password));
        void *entry = NULL;
        while ((entry = baptismdata_iterator(ctx, entry))) {
            const char *name = baptismdata_get_name(entry);
            if (!name) goto failure;
            json_object_object_add(keys, name, json_object_new_boolean(bdd_writable(config, name)));
        }
        for (size_t i = 0; i < json_object_array_length(config->writable_keys); ++i) {
            const char *name = json_object_get_string(json_object_array_get_idx(config->writable_keys, i));
            json_object_object_add(keys, name, json_object_new_boolean(true));
        }
    } else if (!request->key) {
        output = json_object_new_object();
        if (!output)
            goto failure;
        void *entry = NULL;
        while ((entry = baptismdata_iterator(ctx, entry))) {
            const char *name = baptismdata_get_name(entry);
            const char *stored = baptismdata_get_value(entry);
            struct json_object *str = stored ? json_object_new_string(stored) : NULL;
            if (!name || !str) {
                json_object_put(str);
                goto failure;
            }
            json_object_object_add(output, name, str);
        }
    } else {
        /* Iterator distinguishes a missing key from get_var's allocation failure. */
        const char *stored = NULL;
        void *entry = NULL;
        while ((entry = baptismdata_iterator(ctx, entry))) {
            const char *name = baptismdata_get_name(entry);
            if (!name)
                goto failure;
            if (!strcmp(name, request->key)) {
                stored = baptismdata_get_value(entry);
                if (!stored)
                    goto failure;
                break;
            }
        }
        if (!put && !stored) {
            baptismdata_close(ctx);
            json_object_put(input);
            return error_response(connection, request, 404, "key_not_found", "Key does not exist", NULL);
        }
        if (put || remove) {
            status = remove ? 204 : (stored ? 200 : 201);
            if (baptismdata_set_var(ctx, request->key, put ? json_object_get_string(value) : NULL) < 0 ||
                baptismdata_store(ctx) < 0)
                goto failure;
        }
        if (!remove) {
            output = json_object_new_object();
            struct json_object *str = json_object_new_string(put ? json_object_get_string(value) : stored);
            if (!output || !str) {
                json_object_put(str);
                goto failure;
            }
            json_object_object_add(output, "value", str);
        }
    }
    baptismdata_close(ctx);
    json_object_put(input);
    return respond(connection, request, status, output, NULL);
failure:
    if (ctx)
        baptismdata_close(ctx);
    json_object_put(input);
    json_object_put(output);
    return internal_error(connection, request);
}

static void *new_request(void *cls, const char *uri, struct MHD_Connection *connection)
{
    (void)cls;
    (void)connection;
    struct request *request = calloc(1, sizeof(*request));
    if (request) {
        request->uri = strdup(uri);
        if (!request->uri) {
            free(request);
            return NULL;
        }
    }
    return request;
}

static void complete_request(void *cls, struct MHD_Connection *connection, void **context,
                             enum MHD_RequestTerminationCode reason)
{
    (void)cls;
    (void)connection;
    (void)reason;
    struct request *request = *context;
    if (request) {
        free(request->uri);
        free(request->key);
        free(request->body);
        free(request);
        *context = NULL;
    }
}

/* Serve a fixed allowlist; never derive filesystem paths from user input. */
static enum MHD_Result asset_response(struct MHD_Connection *connection, struct request *request)
{
    const char *file = "index.html", *type = "text/html; charset=utf-8";
    if (!strcmp(request->uri, "/app.js")) { file = "app.js"; type = "text/javascript; charset=utf-8"; }
    else if (!strcmp(request->uri, "/app.css")) { file = "app.css"; type = "text/css; charset=utf-8"; }
    else if (!strcmp(request->uri, "/bootstrap.min.css")) { file = "bootstrap.min.css"; type = "text/css; charset=utf-8"; }
    else if (!strcmp(request->uri, "/openapi.yaml")) { file = "openapi.yaml"; type = "application/yaml"; }
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", BDD_WEB_ROOT, file);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) close(fd);
        return error_response(connection, request, 404, "not_found", "Asset unavailable", NULL);
    }
    struct MHD_Response *response = MHD_create_response_from_fd64((uint64_t)st.st_size, fd);
    if (!response) { close(fd); return MHD_NO; }
    MHD_add_response_header(response, "Content-Type", type);
    MHD_add_response_header(response, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(response, "Cache-Control", "no-store");
    MHD_add_response_header(response, "Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'; base-uri 'none'");
    enum MHD_Result result = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);
    request->answered = true;
    return result;
}

/* Password authentication protects writes; read access remains public. */
static bool password_authenticated(struct MHD_Connection *connection, const struct bdd_config *config)
{
    if (!config->password || !*config->password) return true;
    char *password = NULL;
    char *user = MHD_basic_auth_get_username_password(connection, &password);
    struct crypt_data state = {0};
    char *hash = password ? crypt_r(password, config->password, &state) : NULL;
    bool ok = hash && hash[0] != '*' && user && password && !strcmp(user, "admin") &&
        strlen(hash) == strlen(config->password) &&
        !gnutls_memcmp(hash, config->password, strlen(hash));
    if (password) { gnutls_memset(password, 0, strlen(password)); MHD_free(password); }
    gnutls_memset(&state, 0, sizeof(state));
    MHD_free(user);
    return ok;
}

static enum MHD_Result handle(void *cls, struct MHD_Connection *connection, const char *url,
                              const char *method, const char *version, const char *upload,
                              size_t *upload_size, void **context)
{
    const struct bdd_config *config = cls;
    struct request *request = *context;
    (void)url;
    (void)version;
    if (!request)
        return MHD_NO;
    if (request->answered) {
        *upload_size = 0;
        return MHD_YES;
    }
    if (!request->initialized) {
        request->initialized = true;
        if (config->client_ca && !authenticated(connection))
            return error_response(connection, request, 403, "client_certificate_required",
                                  "A trusted, valid client certificate is required", NULL);
        unsigned int route_error = parse_route(request);
        if (route_error == 500)
            return internal_error(connection, request);
        if (route_error)
            return error_response(connection, request, route_error,
                                  route_error == 404 ? "not_found" : "invalid_key",
                                  route_error == 404 ? "Route does not exist" : "Invalid key", NULL);
        bool get = !strcmp(method, "GET");
        bool put = !strcmp(method, "PUT");
        bool remove = !strcmp(method, "DELETE");
        if (((request->asset || request->metadata) && !get) ||
            (!request->key && !get && !put) || (request->key && !get && !put && !remove))
            return error_response(connection, request, 405, "method_not_allowed", "Method not allowed",
                                  request->key ? "GET, PUT, DELETE" : (request->asset || request->metadata ? "GET" : "GET, PUT"));
        if ((put || remove) && !password_authenticated(connection, config))
            return error_response(connection, request, 401, "authentication_required", "Password required for writes", NULL);
        if (request->key && (put || remove) && !bdd_writable(config, request->key))
            return error_response(connection, request, 403, "key_read_only", "Key is not writable", NULL);
        if (put && !json_content_type(MHD_lookup_connection_value(connection, MHD_HEADER_KIND,
                                                                 MHD_HTTP_HEADER_CONTENT_TYPE)))
            return error_response(connection, request, 415, "unsupported_media_type",
                                  "Content-Type must be application/json", NULL);
        const char *length = MHD_lookup_connection_value(connection, MHD_HEADER_KIND,
                                                         MHD_HTTP_HEADER_CONTENT_LENGTH);
        if (length) {
            char *end;
            unsigned long long size = strtoull(length, &end, 10);
            if (!*length || *end || *length == '-')
                return error_response(connection, request, 400, "invalid_body", "Invalid Content-Length", NULL);
            if (size > BDD_MAX_BODY)
                return error_response(connection, request, 413, "body_too_large", "Body exceeds 1 MiB", NULL);
            if (!put && size > 0)
                return error_response(connection, request, 400, "invalid_body", "Request body is not allowed", NULL);
        }
        if (!put && MHD_lookup_connection_value(connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_TRANSFER_ENCODING))
            return error_response(connection, request, 400, "invalid_body", "Request body is not allowed", NULL);
        return MHD_YES;
    }
    if (*upload_size) {
        if (request->upload_error || *upload_size > BDD_MAX_BODY - request->length) {
            /* MHD permits queueing a response only outside upload callbacks. */
            if (!request->upload_error)
                request->upload_error = 413;
            *upload_size = 0;
            return MHD_YES;
        }
        if (strcmp(method, "PUT"))
            return error_response(connection, request, 400, "invalid_body", "Request body is not allowed", NULL);
        char *body = realloc(request->body, request->length + *upload_size + 1);
        if (!body) {
            request->upload_error = 500;
            *upload_size = 0;
            return MHD_YES;
        }
        request->body = body;
        memcpy(body + request->length, upload, *upload_size);
        request->length += *upload_size;
        body[request->length] = '\0';
        *upload_size = 0;
        return MHD_YES;
    }
    if (request->upload_error == 500)
        return internal_error(connection, request);
    if (request->upload_error == 413)
        return error_response(connection, request, 413, "body_too_large", "Body exceeds 1 MiB", NULL);
    if (request->asset) return asset_response(connection, request);
    return execute(connection, request, method, config);
}

struct MHD_Daemon *bdd_server_start(const struct bdd_config *config)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)config->port)};
    if (inet_pton(AF_INET, config->address, &address.sin_addr) != 1)
        return NULL;
    /* One polling thread serializes all handlers and libbaptismdata contexts. */
    unsigned int flags = MHD_USE_INTERNAL_POLLING_THREAD;
    struct MHD_OptionItem options[] = {
        {MHD_OPTION_SOCK_ADDR, 0, &address},
        {MHD_OPTION_URI_LOG_CALLBACK, (intptr_t)new_request, NULL},
        {MHD_OPTION_NOTIFY_COMPLETED, (intptr_t)complete_request, NULL},
        {MHD_OPTION_CONNECTION_TIMEOUT, 30, NULL},
        {MHD_OPTION_CONNECTION_LIMIT, 64, NULL},
        {MHD_OPTION_END, 0, NULL}
    };
    if (config->tls) {
        flags |= MHD_USE_TLS;
        return MHD_start_daemon(flags, (uint16_t)config->port, NULL, NULL, handle, (void *)config,
            MHD_OPTION_ARRAY, options,
            MHD_OPTION_HTTPS_MEM_CERT, config->certificate,
            MHD_OPTION_HTTPS_MEM_KEY, config->private_key,
            MHD_OPTION_HTTPS_MEM_TRUST, config->client_ca,
            MHD_OPTION_HTTPS_PRIORITIES, "NORMAL:-VERS-ALL:+VERS-TLS1.3:+VERS-TLS1.2",
            MHD_OPTION_END);
    }
    return MHD_start_daemon(flags, (uint16_t)config->port, NULL, NULL, handle, (void *)config,
                            MHD_OPTION_ARRAY, options, MHD_OPTION_END);
}

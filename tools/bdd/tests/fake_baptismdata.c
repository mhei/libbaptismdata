/* In-process transactional test double; never opens device storage. */
#include <baptismdata.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct entry {
    char *name;
    char *value;
    struct entry *next;
};
struct baptismdata_ctx { struct entry *head; };
static struct entry *committed;
static int initialized;

static void audit(const char *operation)
{
    const char *path = getenv("BDD_TEST_AUDIT");
    if (path) {
        FILE *file = fopen(path, "a");
        if (file) {
            fprintf(file, "%s\n", operation);
            fclose(file);
        }
    }
}

static int fails(const char *operation)
{
    const char *failure = getenv("BDD_TEST_FAIL");
    return failure && !strcmp(failure, operation);
}

static void release(struct entry *entry)
{
    while (entry) {
        struct entry *next = entry->next;
        free(entry->name);
        free(entry->value);
        free(entry);
        entry = next;
    }
}

static struct entry *clone(struct entry *entry)
{
    struct entry *head = NULL, **tail = &head;
    for (; entry; entry = entry->next) {
        struct entry *copy = calloc(1, sizeof(*copy));
        if (!copy)
            goto fail;
        *tail = copy;
        copy->name = strdup(entry->name);
        copy->value = strdup(entry->value);
        if (!copy->name || !copy->value)
            goto fail;
        tail = &copy->next;
    }
    return head;
fail:
    release(head);
    return NULL;
}

int baptismdata_open(struct baptismdata_ctx **ctx)
{
    audit("open");
    *ctx = NULL;
    if (fails("open"))
        return -EIO;
    if (!initialized) {
        struct entry model = {"model", "Test Device", NULL};
        struct entry serial = {"serial", "ABC123", &model};
        struct entry internal = {"serial#", "BOARD42", &serial};
        committed = clone(&internal);
        if (!committed)
            return -ENOMEM;
        initialized = 1;
    }
    *ctx = calloc(1, sizeof(**ctx));
    if (!*ctx)
        return -ENOMEM;
    (*ctx)->head = clone(committed);
    if (committed && !(*ctx)->head) {
        free(*ctx);
        *ctx = NULL;
        return -ENOMEM;
    }
    return 0;
}

void baptismdata_close(struct baptismdata_ctx *ctx)
{
    audit("close");
    if (ctx) {
        release(ctx->head);
        free(ctx);
    }
}

int baptismdata_set_var(struct baptismdata_ctx *ctx, const char *name, const char *value)
{
    audit("set");
    if (fails("set"))
        return -EIO;
    struct entry **slot = &ctx->head;
    while (*slot && strcmp((*slot)->name, name))
        slot = &(*slot)->next;
    if (!value) {
        if (*slot) {
            struct entry *old = *slot;
            *slot = old->next;
            old->next = NULL;
            release(old);
        }
        return 0;
    }
    if (!*slot) {
        *slot = calloc(1, sizeof(**slot));
        if (!*slot)
            return -ENOMEM;
        (*slot)->name = strdup(name);
        if (!(*slot)->name)
            return -ENOMEM;
    }
    char *copy = strdup(value);
    if (!copy)
        return -ENOMEM;
    free((*slot)->value);
    (*slot)->value = copy;
    return 0;
}

int baptismdata_store(struct baptismdata_ctx *ctx)
{
    audit("store");
    if (fails("store"))
        return -EIO;
    struct entry *copy = clone(ctx->head);
    if (ctx->head && !copy)
        return -ENOMEM;
    release(committed);
    committed = copy;
    return 0;
}

char *baptismdata_get_var(struct baptismdata_ctx *ctx, const char *name)
{
    for (struct entry *entry = ctx->head; entry; entry = entry->next)
        if (!strcmp(entry->name, name))
            return strdup(entry->value);
    return NULL;
}

void *baptismdata_iterator(struct baptismdata_ctx *ctx, void *next)
{
    return next ? ((struct entry *)next)->next : ctx->head;
}
const char *baptismdata_get_name(void *entry) { return ((struct entry *)entry)->name; }
const char *baptismdata_get_value(void *entry) { return ((struct entry *)entry)->value; }

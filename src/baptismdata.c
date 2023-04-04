// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Copyright © 2023 Michael Heimpold <mhei@heimpold.de>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <libuboot.h>
#include "config.h"
#include "baptismdata.h"

#ifndef CFG_FILE
#define CFG_FILE "/etc/baptism-data.config"
#endif

struct baptismdata_ctx {
	struct uboot_ctx *uboot_ctx;
	bool need_store;
};

int baptismdata_open(struct baptismdata_ctx **ctx)
{
	struct baptismdata_ctx *c = NULL;
	int rv;

	*ctx = NULL;

	/* check whether config file exists and we have permissions to read */
	rv = access(CFG_FILE, R_OK);
	if (rv < 0) {
		rv = -ENOENT;
		goto free_out;
	}

	c = calloc(1, sizeof(*c));
	if (!c)
		return -ENOMEM;

#ifdef HAVE_LIBUBOOT_READ_MULTIPLE_CONFIG
	/* older library versions do not have YAML support yet, so this is
	 * compile-time optional */
	rv = libuboot_read_multiple_config(&c->uboot_ctx, CFG_FILE);
	if (rv) {
#endif
		/* might be not a YAML file yet, try fallback to legacy parser */
		rv = libuboot_initialize(&c->uboot_ctx, NULL);
		if (rv < 0)
			goto free_out;

		rv = libuboot_read_config(c->uboot_ctx, CFG_FILE);
		if (rv < 0)
			goto uboot_exit_out;
#ifdef HAVE_LIBUBOOT_READ_MULTIPLE_CONFIG
	}
#endif

	rv = libuboot_open(c->uboot_ctx);
	if (rv == -ENODATA)
		rv = 0;
	if (rv < 0)
		goto uboot_exit_out;

	*ctx = c;
	return 0;

uboot_exit_out:
	libuboot_exit(c->uboot_ctx);

free_out:
	free(c);

	return rv;
}

void baptismdata_close(struct baptismdata_ctx *ctx)
{
	if (!ctx)
		return;

	libuboot_close(ctx->uboot_ctx);
	libuboot_exit(ctx->uboot_ctx);
	free(ctx);
}

int baptismdata_load_file(struct baptismdata_ctx *ctx, const char *filename)
{
	int rv;

	if (!ctx)
		return -EINVAL;

	rv = libuboot_load_file(ctx->uboot_ctx, filename);
	if (rv < 0)
		return rv;

	ctx->need_store = true;
	return 0;
}

int baptismdata_store(struct baptismdata_ctx *ctx)
{
	int rv;

	if (!ctx)
		return -EINVAL;

	if (!ctx->need_store)
		return 0;

	rv = libuboot_env_store(ctx->uboot_ctx);
	if (rv < 0)
		return rv;

	ctx->need_store = false;
	return 0;
}

int baptismdata_set_var(struct baptismdata_ctx *ctx, const char *varname, const char *value)
{
	char *old_value;
	bool value_changed;

	if (!ctx)
		return -EINVAL;

	old_value = libuboot_get_env(ctx->uboot_ctx, varname);

	/* if both string pointers are none-NULL we must compare the content */
	if (old_value != NULL && value != NULL)
		value_changed = strcmp(old_value, value) != 0;
	/* else it depends whether value is missing or should be deleted */
	else
		value_changed = (old_value == NULL && value != NULL) || (old_value != NULL && value == NULL);

	free(old_value);

	if (value_changed) {
		int rv;

		rv = libuboot_set_env(ctx->uboot_ctx, varname, value);
		if (rv < 0)
			return rv;

		ctx->need_store = true;
	}

	return 0;
}

char *baptismdata_get_var(struct baptismdata_ctx *ctx, const char *varname)
{
	return libuboot_get_env(ctx->uboot_ctx, varname);
}

void *baptismdata_iterator(struct baptismdata_ctx *ctx, void *next)
{
	return libuboot_iterator(ctx->uboot_ctx, next);
}

const char *baptismdata_get_name(void *entry)
{
	return libuboot_getname(entry);
}

const char *baptismdata_get_value(void *entry)
{
	return libuboot_getvalue(entry);
}

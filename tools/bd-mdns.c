// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright © 2026 Michael Heimpold <mhei@heimpold.de>
 */

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/simple-watch.h>
#include <errno.h>
#include <dirent.h>
#include <getopt.h>
#include <limits.h>
#include <net/if.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "baptismdata.h"
#include "config.h"

struct announce_ctx {
	AvahiSimplePoll *poll;
	AvahiEntryGroup *group;
	char *ifname;
	char *service_name;
	char *service_type;
	char *serial;
	char *model;
	char *vendor;
	uint16_t port;
};

static struct announce_ctx *global_ctx;

static const struct option long_options[] = {
	{ "device",        required_argument, NULL, 'd' },
	{ "name",          required_argument, NULL, 'n' },
	{ "type",          required_argument, NULL, 't' },
	{ "port",          required_argument, NULL, 'p' },
	{ "serial-key",    required_argument, NULL, 'S' },
	{ "use-serial",    no_argument,       NULL, 's' },
	{ "use-vendor",    no_argument,       NULL, 'v' },

	{ "version",       no_argument,       NULL, 'V' },
	{ "help",          no_argument,       NULL, 'h' },
	{ NULL,            0,                 NULL,  0  }
};

static const char *long_options_description[] = {
	"interface name to use as MAC address source, e.g. eth0 (default: try to auto-detect)",
	"service name (default: baptized model name)",
	"mDNS service type (default: _ssh._tcp)",
	"port announced via mDNS (default: 22)",
	"baptism-data variable name to read as serial (default: serial)",
	"use baptized serial instead of interface MAC address as individualization (in brackets)",
	"prefix the default (baptized model-based) service name with the baptized vendor",

	"print version and exit",
	"print this usage and exit",
	NULL
};

static void usage(const char *program, int exitcode)
{
	const char **description = long_options_description;
	const struct option *opt = long_options;
	FILE *f = exitcode ? stderr : stdout;

	fprintf(f, "%s (%s) -- Announce a baptism-data based mDNS service\n\n", program, PACKAGE_STRING);
	fprintf(f, "Usage: %s [<options>]\n\n", program);
	fprintf(f, "Options:\n");

	while (opt->name && description) {
		if (opt->val > 1)
			fprintf(f, "\t-%c, --%-15s%s\n", opt->val, opt->name, *description);
		else
			fprintf(f, "\t    --%-15s%s\n", opt->name, *description);
		opt++;
		description++;
	}

	fprintf(f, "\nDefaults:\n");
	fprintf(f, "\t--name <baptized model name>\n");
	fprintf(f, "\t--type _ssh._tcp\n");
	fprintf(f, "\t--port 22\n");
	fprintf(f, "\t--serial-key serial\n");
	fprintf(f, "\tbracket individualization: MAC address\n");

	exit(exitcode);
}

static void quit_loop(int signum)
{
	(void)signum;

	if (global_ctx && global_ctx->poll)
		avahi_simple_poll_quit(global_ctx->poll);
}

static int read_mac_address(const char *ifname, char *buf, size_t len)
{
	char path[PATH_MAX];
	FILE *f;

	if (snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname) >= (int)sizeof(path))
		return -ENAMETOOLONG;

	f = fopen(path, "r");
	if (!f)
		return -errno;

	if (!fgets(buf, len, f)) {
		int rv = ferror(f) ? -errno : -EIO;
		fclose(f);
		return rv;
	}

	fclose(f);
	buf[strcspn(buf, "\r\n")] = '\0';

	return 0;
}

// wild assumption: on embedded devices is e.g. br0 listed before
// eth0 because of sorting... not ensured... heuristic
static int detect_interface(char **ifname_out)
{
	DIR *dir;
	struct dirent *entry;
	char *ifname;

	dir = opendir("/sys/class/net");
	if (!dir)
		return -errno;

	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (strcmp(entry->d_name, "lo") == 0)
			continue;
		if (strncmp(entry->d_name, "can", 3) == 0)
			continue;

		errno = 0;
		if (if_nametoindex(entry->d_name) == 0)
			continue;

		ifname = strdup(entry->d_name);
		if (!ifname) {
			closedir(dir);
			return -ENOMEM;
		}

		closedir(dir);
		*ifname_out = ifname;
		return 0;
	}

	closedir(dir);
	return -ENODEV;
}

static int read_baptism_var(const char *name, char **value_out)
{
	struct baptismdata_ctx *bdctx;
	char *value;
	int rv;

	rv = baptismdata_open(&bdctx);
	if (rv < 0)
		return rv;

	value = baptismdata_get_var(bdctx, name);
	baptismdata_close(bdctx);

	if (!value || value[0] == '\0') {
		free(value);
		return -ENOENT;
	}

	*value_out = value;
	return 0;
}

static int read_baptism_var_optional(const char *name, char **value_out)
{
	int rv;

	rv = read_baptism_var(name, value_out);
	if (rv == -ENOENT) {
		*value_out = NULL;
		return 0;
	}

	return rv;
}

static int create_services(AvahiClient *client, struct announce_ctx *ctx)
{
	AvahiStringList *txt = NULL;
	int is_empty;
	int rv;

	if (!ctx->group) {
		ctx->group = avahi_entry_group_new(client, NULL, NULL);
		if (!ctx->group)
			return -avahi_client_errno(client);
	}

	is_empty = avahi_entry_group_is_empty(ctx->group);
	if (is_empty < 0)
		return is_empty;
	if (is_empty == 0)
		return 0;

	txt = avahi_string_list_add_printf(NULL, "serial=%s", ctx->serial);
	if (!txt)
		return -ENOMEM;

	if (ctx->model) {
		txt = avahi_string_list_add_printf(txt, "model=%s", ctx->model);
		if (!txt)
			return -ENOMEM;
	}

	if (ctx->vendor) {
		txt = avahi_string_list_add_printf(txt, "vendor=%s", ctx->vendor);
		if (!txt)
			return -ENOMEM;
	}

	rv = avahi_entry_group_add_service_strlst(ctx->group,
	                                          AVAHI_IF_UNSPEC,
	                                          AVAHI_PROTO_UNSPEC,
	                                          0,
	                                          ctx->service_name,
	                                          ctx->service_type,
	                                          NULL,
	                                          NULL,
	                                          ctx->port,
	                                          txt);
	avahi_string_list_free(txt);
	if (rv < 0)
		return rv;

	rv = avahi_entry_group_commit(ctx->group);
	if (rv < 0)
		return rv;

	return 0;
}

static void client_callback(AvahiClient *client, AvahiClientState state, void *userdata)
{
	struct announce_ctx *ctx = userdata;
	int rv;

	switch (state) {
	case AVAHI_CLIENT_S_RUNNING:
		rv = create_services(client, ctx);
		if (rv < 0) {
			fprintf(stderr, "Error: could not register service: %s\n", avahi_strerror(rv));
			avahi_simple_poll_quit(ctx->poll);
		}
		break;
	case AVAHI_CLIENT_FAILURE:
		fprintf(stderr, "Error: Avahi client failure: %s\n",
		        avahi_strerror(avahi_client_errno(client)));
		avahi_simple_poll_quit(ctx->poll);
		break;
	case AVAHI_CLIENT_S_COLLISION:
	case AVAHI_CLIENT_S_REGISTERING:
		if (ctx->group)
			avahi_entry_group_reset(ctx->group);
		break;
	case AVAHI_CLIENT_CONNECTING:
		break;
	}
}

int main(int argc, char **argv)
{
	struct announce_ctx ctx = {
		.port = 22,
	};
	AvahiClient *client;
	bool use_serial = false;
	bool use_vendor = false;
	bool free_ifname = false;
	bool free_service_name = false;
	char *i15n_value; // individualization_value
	char *combined_service_name = NULL;
	char *service_name = NULL;
	const char *serial_key = "serial";
	char mac[32];
	char *progname = argv[0];
	char *options = "d:n:t:p:S:svVh";
	char *endptr;
	unsigned int ifindex;
	long port;
	int error;
	int c;
	int rv;

	while ((c = getopt_long(argc, argv, options, long_options, NULL)) != EOF) {
		switch (c) {
		case 'd':
			errno = 0;
			ifindex = if_nametoindex(optarg);
			if (ifindex == 0) {
				if (errno != 0)
					fprintf(stderr, "Error: interface lookup for '%s' failed: %s\n",
					        optarg, strerror(errno));
				else
					fprintf(stderr, "Error: unknown interface '%s'\n", optarg);
				return EXIT_FAILURE;
			}
			ctx.ifname = optarg;
			break;
		case 'n':
			service_name = optarg;
			break;
		case 't':
			ctx.service_type = optarg;
			break;
		case 'p':
			errno = 0;
			endptr = NULL;
			port = strtol(optarg, &endptr, 10);
			if (errno != 0 || !endptr || *endptr != '\0' || port < 1 || port > 65535) {
				fprintf(stderr, "Error: invalid port '%s'\n", optarg);
				return EXIT_FAILURE;
			}
			ctx.port = (uint16_t)port;
			break;
		case 'S':
			serial_key = optarg;
			break;
		case 's':
			use_serial = true;
			break;
		case 'v':
			use_vendor = true;
			break;
		case 'V':
			printf("%s (%s)\n", progname, PACKAGE_STRING);
			return EXIT_SUCCESS;
		case '?':
		case 'h':
			usage(progname, (c == 'h') ? EXIT_SUCCESS : EXIT_FAILURE);
		}
	}

	if (!ctx.ifname) {
		rv = detect_interface(&ctx.ifname);
		if (rv < 0) {
			fprintf(stderr, "Error: could not auto-detect interface for MAC address source: %s\n",
				strerror(abs(rv)));
			return EXIT_FAILURE;
		}
		free_ifname = true;
	}

	if (optind != argc) {
		fprintf(stderr, "Error: unexpected positional arguments.\n\n");
		usage(progname, EXIT_FAILURE);
	}

	if (!ctx.service_type)
		ctx.service_type = "_ssh._tcp";

	rv = read_baptism_var(serial_key, &ctx.serial);
	if (rv < 0) {
		fprintf(stderr, "Error: could not read serial from baptism data key '%s': %s\n",
		        serial_key, strerror(abs(rv)));
		return EXIT_FAILURE;
	}

	rv = read_baptism_var_optional("model", &ctx.model);
	if (rv < 0) {
		free(ctx.serial);
		fprintf(stderr, "Error: could not read model from baptism data: %s\n", strerror(abs(rv)));
		return EXIT_FAILURE;
	}

	rv = read_baptism_var_optional("vendor", &ctx.vendor);
	if (rv < 0) {
		free(ctx.model);
		free(ctx.serial);
		fprintf(stderr, "Error: could not read vendor from baptism data: %s\n", strerror(abs(rv)));
		return EXIT_FAILURE;
	}

	if (!service_name) {
		if (!ctx.model) {
			free(ctx.serial);
			free(ctx.vendor);
			fprintf(stderr, "Error: could not read model from baptism data for default service name: %s\n",
			        strerror(ENOENT));
			return EXIT_FAILURE;
		}

		service_name = strdup(ctx.model);
		if (!service_name) {
			free(ctx.vendor);
			free(ctx.model);
			free(ctx.serial);
			fprintf(stderr, "Error: out of memory\n");
			return EXIT_FAILURE;
		}

		if (use_vendor) {
			if (!ctx.vendor) {
				free(ctx.serial);
				free(service_name);
				free(ctx.model);
				fprintf(stderr, "Error: could not read vendor from baptism data for default service name: %s\n",
				        strerror(ENOENT));
				return EXIT_FAILURE;
			}

			rv = snprintf(NULL, 0, "%s %s", ctx.vendor, service_name);
			if (rv < 0) {
				free(ctx.serial);
				free(ctx.vendor);
				free(ctx.model);
				free(service_name);
				fprintf(stderr, "Error: could not format vendor-prefixed service name\n");
				return EXIT_FAILURE;
			}

			combined_service_name = malloc((size_t)rv + 1);
			if (!combined_service_name) {
				free(ctx.serial);
				free(ctx.vendor);
				free(ctx.model);
				free(service_name);
				fprintf(stderr, "Error: out of memory\n");
				return EXIT_FAILURE;
			}

			snprintf(combined_service_name, (size_t)rv + 1, "%s %s", ctx.vendor, service_name);
			free(service_name);
			service_name = combined_service_name;
		}

		free_service_name = true;
	}

	if (use_serial) {
		i15n_value = ctx.serial;
	} else {
		rv = read_mac_address(ctx.ifname, mac, sizeof(mac));
		if (rv < 0) {
			free(ctx.serial);
			free(ctx.model);
			free(ctx.vendor);
			if (free_service_name)
				free(service_name);
			fprintf(stderr, "Error: could not read MAC address for interface '%s': %s\n",
			        ctx.ifname, strerror(abs(rv)));
			return EXIT_FAILURE;
		}
		i15n_value = mac;
	}

	rv = snprintf(NULL, 0, "%s [%s]", service_name, i15n_value);
	if (rv < 0) {
		free(ctx.serial);
		free(ctx.model);
		free(ctx.vendor);
		if (free_service_name)
			free(service_name);
		fprintf(stderr, "Error: could not format service name\n");
		return EXIT_FAILURE;
	}

	ctx.service_name = malloc((size_t)rv + 1);
	if (!ctx.service_name) {
		free(ctx.serial);
		free(ctx.model);
		free(ctx.vendor);
		if (free_service_name)
			free(service_name);
		fprintf(stderr, "Error: out of memory\n");
		return EXIT_FAILURE;
	}
	snprintf(ctx.service_name, (size_t)rv + 1, "%s [%s]", service_name, i15n_value);
	if (free_service_name)
		free(service_name);

	signal(SIGINT, quit_loop);
	signal(SIGTERM, quit_loop);

	ctx.poll = avahi_simple_poll_new();
	if (!ctx.poll) {
		fprintf(stderr, "Error: could not create Avahi event loop\n");
		free(ctx.service_name);
		free(ctx.serial);
		free(ctx.model);
		free(ctx.vendor);
		return EXIT_FAILURE;
	}

	global_ctx = &ctx;
	client = avahi_client_new(avahi_simple_poll_get(ctx.poll), 0, client_callback, &ctx, &error);
	if (!client) {
		fprintf(stderr, "Error: could not create Avahi client: %s\n", avahi_strerror(error));
		avahi_simple_poll_free(ctx.poll);
		free(ctx.service_name);
		free(ctx.serial);
		free(ctx.model);
		free(ctx.vendor);
		return EXIT_FAILURE;
	}

	rv = avahi_simple_poll_loop(ctx.poll);
	if (rv < 0)
		fprintf(stderr, "Error: Avahi event loop failed: %s\n", avahi_strerror(rv));

	if (ctx.group)
		avahi_entry_group_free(ctx.group);
	avahi_client_free(client);
	avahi_simple_poll_free(ctx.poll);
	free(ctx.service_name);
	free(ctx.serial);
	free(ctx.model);
	free(ctx.vendor);
	if (free_ifname)
		free(ctx.ifname);

	return rv < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

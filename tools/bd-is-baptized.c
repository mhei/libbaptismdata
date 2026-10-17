// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright © 2026 Michael Heimpold <mhei@heimpold.de>
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "baptismdata.h"
#include "config.h"

static const struct option long_options[] = {
	{ "version", no_argument, NULL, 'V' },
	{ "help",    no_argument, NULL, 'h' },
	{ NULL,      0,           NULL,  0  }
};

static void usage(const char *program, int status)
{
	FILE *f = status ? stderr : stdout;

	fprintf(f, "%s (%s) -- Check required baptism data\n\n"
		"Usage: %s [<options>]\n\n"
		"Options:\n"
		"  -V, --version  Print version and exit\n"
		"  -h, --help     Print this usage and exit\n\n"
		"Exit status 0: model and vendor are present and nonempty.\n"
		"Exit status 1: either value is missing or empty, or an error occurred.\n",
		program, PACKAGE_STRING, program);
	exit(status);
}

int main(int argc, char **argv)
{
	static const char *const required_keys[] = { "model", "vendor" };
	struct baptismdata_ctx *ctx;
	int c, rv, status = EXIT_SUCCESS;

	while ((c = getopt_long(argc, argv, "Vh", long_options, NULL)) != -1) {
		switch (c) {
		case 'V':
			printf("%s (%s)\n", argv[0], PACKAGE_STRING);
			return EXIT_SUCCESS;
		case 'h':
			usage(argv[0], EXIT_SUCCESS);
			break;
		default:
			usage(argv[0], EXIT_FAILURE);
		}
	}

	if (optind != argc) {
		fprintf(stderr, "Error: unexpected positional arguments.\n");
		usage(argv[0], EXIT_FAILURE);
	}

	rv = baptismdata_open(&ctx);
	if (rv < 0) {
		fprintf(stderr, "Error: could not open baptism data: %s\n", strerror(-rv));
		return EXIT_FAILURE;
	}

	for (size_t i = 0; i < sizeof(required_keys) / sizeof(required_keys[0]); i++) {
		char *value = baptismdata_get_var(ctx, required_keys[i]);

		if (!value || !*value) {
			fprintf(stderr, "Error: baptism data '%s' is missing, empty or unreadable\n",
				required_keys[i]);
			status = EXIT_FAILURE;
		}
		free(value);
	}

	baptismdata_close(ctx);
	return status;
}

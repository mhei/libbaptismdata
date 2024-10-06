// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright © 2023 Michael Heimpold <mhei@heimpold.de>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include "baptismdata.h"
#include "config.h"

static const struct option long_options[] = {
	{ "no-header", no_argument,       NULL, 'n' },
	{ "script",    required_argument, NULL, 's' },
	{ "version",   no_argument,       NULL, 'V' },
	{ "help",      no_argument,       NULL, 'h' },
	{ NULL,        0,                 NULL,  0  }
};

static const char *long_options_description[] = {
	"do not print variable name(s)",
	"read variables to be set/unset from a script file",
	"print version and exit",
	"print this usage and exit",
	NULL
};

static void usage(char *program, int exitcode)
{
	const char **description = long_options_description;
	const struct option *opt = long_options;
	FILE *F = exitcode ? stderr : stdout;

	fprintf(F, "%s (%s) -- Tool to get/set baptism data variables\n\n", program, PACKAGE_STRING);
	fprintf(F, "Usage: %s [<options>] <command> [<parameters>]\n\n", program);
	fprintf(F, "Commands:\n");
	fprintf(F, "\t%-40s%s\n", "[show]", "print all baptism variables");
	fprintf(F, "\t%-40s%s\n", "get varname [varname...]", "print the value(s) of the baptism variable(s)");
	fprintf(F, "\t%-40s%s\n", "set varname value [varname value...]", "set the value(s) of baptism variable(s)");
	fprintf(F, "\t%-40s%s\n", "unset varname [varname...]", "unset the given baptism variable(s)");
	fprintf(F, "\n");
	fprintf(F, "Options:\n");

	while (opt->name && description) {
		if (opt->val > 1)
			fprintf(F, "\t-%c, --%-15s%s\n", opt->val, opt->name, *description);
		else
			fprintf(F, "\t    --%-15s%s\n", opt->name, *description);
		opt++;
		description++;
	}
	fprintf(F, "\n");

	fprintf(F, "Script File Syntax:\n"
	           " key=value\n"
	           " lines starting with '#' are treated as comment\n"
	           " lines without '=' are ignored\n"
	           "\n"
	           "Script File Example:\n"
	           " serial#=4711\n"
	           " hw_revision=A0\n"
	           " manufacturer=Example Company\n"
	           " line ignored\n"
	           " delete_this_var=\n"
	           "\n"
	       );

	exit(exitcode);
}

int main(int argc, char **argv) {
	struct baptismdata_ctx *ctx;
	char *options = "ns:Vh";
	char *scriptfile = NULL;
	bool noheader = false;
	char *progname;
	int c, i;
	int rv = 0;
	void *tmp;

	/* remember progname for later use since we will modify argv */
	progname = argv[0];

	while ((c = getopt_long(argc, argv, options, long_options, NULL)) != EOF) {
		switch (c) {
		case 'n':
			noheader = true;
			break;
		case 's':
			scriptfile = optarg;
			break;
		case 'V':
			printf("%s (%s)\n", argv[0], PACKAGE_STRING);
			exit(EXIT_SUCCESS);
		case '?':
		case 'h':
			usage(progname, (c == 'h') ? EXIT_SUCCESS : EXIT_FAILURE);
		}
	}

	argc -= optind;
	argv += optind;

	if (scriptfile && strcmp(scriptfile, "-") != 0) {
		rv = access(scriptfile, R_OK);
		if (rv < 0) {
			fprintf(stderr, "Error: Cannot use script file '%s': %m", scriptfile);
			exit(EXIT_FAILURE);
		}
	}

	rv = baptismdata_open(&ctx);
	if (rv < 0) {
		fprintf(stderr, "Error: baptismdata_open() failed: %s\n", strerror(abs(rv)));
		exit(EXIT_FAILURE);
	}

	if (argc == 0 || strcasecmp(argv[0], "show") == 0) {
		if (scriptfile) {
			fprintf(stderr, "Error: Cannot use script file for 'show' command.\n");
			goto close_out;
		}

		/* iterate over all available variables */
		tmp = NULL;
		while ((tmp = baptismdata_iterator(ctx, tmp)) != NULL) {
			const char *name, *value;

			name = baptismdata_get_name(tmp);
			value = baptismdata_get_value(tmp);

			/* --no-header does not really makes sense here, but it's up to the user */
			if (noheader)
				fprintf(stdout, "%s\n", value);
			else
				fprintf(stdout, "%s=%s\n", name, value);
		}
	} else if (strcasecmp(argv[0], "get") == 0) {
		argc--;
		argv++;

		if (scriptfile) {
			fprintf(stderr, "Error: Cannot use script file for 'get' command.\n");
			goto close_out;
		}

		/* iterate over all requested variables */
		for (i = 0; i < argc; i++) {
			char *value;

			value = baptismdata_get_var(ctx, argv[i]);
			if (noheader)
				fprintf(stdout, "%s\n", value ? value : "");
			else
				fprintf(stdout, "%s=%s\n", argv[i], value ? value : "");
			free(value);
		}
	} else if (strcasecmp(argv[0], "set") == 0) {
		argc--;
		argv++;

		if (argc % 2) {
			fprintf(stderr, "Error: Parameter count must be even for 'set'.\n");
			goto close_out;
		}

		if (scriptfile) {
			rv = baptismdata_load_file(ctx, scriptfile);
			if (rv < 0) {
				fprintf(stderr, "Could not process script file: %s\n", strerror(abs(rv)));
				goto close_out;
			}
		}

		for (i = 0; i < argc; i += 2) {
			rv = baptismdata_set_var(ctx, argv[i], argv[i + 1]);
			if (rv < 0) {
				fprintf(stderr, "Error: setting '%s' failed: %s\n", argv[i], strerror(abs(rv)));
				goto close_out;
			}
		}
	} else if (strcasecmp(argv[0], "unset") == 0) {
		argc--;
		argv++;

		if (scriptfile) {
			rv = baptismdata_load_file(ctx, scriptfile);
			if (rv < 0) {
				fprintf(stderr, "Could not process script file: %s\n", strerror(abs(rv)));
				goto close_out;
			}
		}

		for (i = 0; i < argc; ++i) {
			rv = baptismdata_set_var(ctx, argv[i], NULL);
			if (rv < 0) {
				fprintf(stderr, "Error: deleting '%s' failed: %s\n", argv[i], strerror(abs(rv)));
				goto close_out;
			}
		}
	} else {
		fprintf(stderr, "Error: unknown command '%s'.\n\n", argv[0]);
		usage(progname, EXIT_FAILURE);
	}

	rv = baptismdata_store(ctx);
	if (rv < 0)
		fprintf(stderr, "Could not store the baptism data: %s", strerror(abs(rv)));

close_out:
	baptismdata_close(ctx);

	return rv ? EXIT_FAILURE : EXIT_SUCCESS;
}

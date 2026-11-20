/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BD_UPNP_MANAGER_POLICY_H
#define BD_UPNP_MANAGER_POLICY_H

#include <stdbool.h>
#include <stddef.h>

struct names {
	char **values;
	size_t count;
};

struct rule {
	char *compatible;
	struct names interfaces;
};

struct policy {
	struct rule *rules;
	size_t count;
};

void names_free(struct names *names);
int names_add(struct names *names, const char *name);
bool names_has(const struct names *names, const char *name);
void policy_free(struct policy *policy);
int policy_load(const char *path, struct policy *policy);
int policy_select(const struct policy *policy, const char *dt_path, struct names *ignored);
bool interface_name_valid(const char *name);
bool interface_eligible(const char *name, unsigned int type, unsigned int flags,
                        bool bridge_port, const struct names *ignored);
char *interface_unit(const char *name);

#endif

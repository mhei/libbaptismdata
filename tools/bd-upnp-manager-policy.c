// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright © 2026 Michael Heimpold <mhei@heimpold.de> */
#define _GNU_SOURCE
#include "bd-upnp-manager-policy.h"

#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

void names_free(struct names *names)
{
	for (size_t i = 0; i < names->count; i++)
		free(names->values[i]);
	free(names->values);
	memset(names, 0, sizeof(*names));
}

bool names_has(const struct names *names, const char *name)
{
	for (size_t i = 0; i < names->count; i++)
		if (!strcmp(names->values[i], name))
			return true;
	return false;
}

int names_add(struct names *names, const char *name)
{
	char **grown;
	char *copy;
	if (names_has(names, name))
		return 0;
	copy = strdup(name);
	if (!copy)
		return -ENOMEM;
	grown = realloc(names->values, (names->count + 1) * sizeof(*grown));
	if (!grown) {
		free(copy);
		return -ENOMEM;
	}
	names->values = grown;
	names->values[names->count++] = copy;
	return 0;
}

void policy_free(struct policy *policy)
{
	for (size_t i = 0; i < policy->count; i++) {
		free(policy->rules[i].compatible);
		names_free(&policy->rules[i].interfaces);
	}
	free(policy->rules);
	memset(policy, 0, sizeof(*policy));
}

bool interface_name_valid(const char *name)
{
	size_t len = strlen(name);
	if (!len || len >= IFNAMSIZ || !strcmp(name, ".") || !strcmp(name, ".."))
		return false;
	for (size_t i = 0; i < len; i++)
		if (name[i] == '/' || name[i] == ':' || isspace((unsigned char)name[i]))
			return false;
	return true;
}

static const char *scalar(yaml_node_t *node)
{
	if (!node || node->type != YAML_SCALAR_NODE || !node->data.scalar.length ||
	    memchr(node->data.scalar.value, 0, node->data.scalar.length))
		return NULL;
	return (const char *)node->data.scalar.value;
}

int policy_load(const char *path, struct policy *policy)
{
	yaml_parser_t parser;
	yaml_document_t doc, extra;
	yaml_node_t *root, *mapping;
	FILE *f = fopen(path, "r");
	int rv = -EINVAL;
	if (!f) {
		int saved_errno = errno;
		fprintf(stderr, "%s: %s\n", path, strerror(saved_errno));
		return -saved_errno;
	}
	if (!yaml_parser_initialize(&parser)) {
		fclose(f);
		return -ENOMEM;
	}
	yaml_parser_set_input_file(&parser, f);
	if (!yaml_parser_load(&parser, &doc))
		goto parser_error;
	root = yaml_document_get_root_node(&doc);
	if (!root || root->type != YAML_MAPPING_NODE ||
	    root->data.mapping.pairs.top - root->data.mapping.pairs.start != 1)
		goto schema_error;
	{
		yaml_node_pair_t *pair = root->data.mapping.pairs.start;
		const char *key = scalar(yaml_document_get_node(&doc, pair->key));
		if (!key || strcmp(key, "ignore_interfaces"))
			goto schema_error;
		mapping = yaml_document_get_node(&doc, pair->value);
	}
	if (!mapping || mapping->type != YAML_MAPPING_NODE)
		goto schema_error;
	for (yaml_node_pair_t *pair = mapping->data.mapping.pairs.start;
	     pair < mapping->data.mapping.pairs.top; pair++) {
		const char *compatible = scalar(yaml_document_get_node(&doc, pair->key));
		yaml_node_t *list = yaml_document_get_node(&doc, pair->value);
		struct rule *grown, *rule;
		if (!compatible || !list || list->type != YAML_SEQUENCE_NODE)
			goto schema_error;
		for (size_t i = 0; i < policy->count; i++)
			if (!strcmp(policy->rules[i].compatible, compatible))
				goto schema_error;
		grown = realloc(policy->rules, (policy->count + 1) * sizeof(*grown));
		if (!grown) {
			rv = -ENOMEM;
			goto document_out;
		}
		policy->rules = grown;
		rule = &policy->rules[policy->count++];
		memset(rule, 0, sizeof(*rule));
		rule->compatible = strdup(compatible);
		if (!rule->compatible) {
			rv = -ENOMEM;
			goto document_out;
		}
		for (yaml_node_item_t *item = list->data.sequence.items.start;
		     item < list->data.sequence.items.top; item++) {
			const char *name = scalar(yaml_document_get_node(&doc, *item));
			if (!name || !interface_name_valid(name))
				goto schema_error;
			rv = names_add(&rule->interfaces, name);
			if (rv < 0)
				goto document_out;
		}
	}
	/* A configuration is one document, not a YAML stream of overrides. */
	if (!yaml_parser_load(&parser, &extra)) {
		yaml_document_delete(&doc);
		goto parser_error;
	}
	rv = yaml_document_get_root_node(&extra) ? -EINVAL : 0;
	yaml_document_delete(&extra);
	if (rv < 0)
		goto schema_error;
	goto document_out;
schema_error:
	fprintf(stderr, "%s: expected one ignore_interfaces mapping from unique compatible strings to interface-name lists\n", path);
	rv = -EINVAL;
document_out:
	yaml_document_delete(&doc);
	goto out;
parser_error:
	fprintf(stderr, "%s:%zu:%zu: %s\n", path, parser.problem_mark.line + 1,
	        parser.problem_mark.column + 1, parser.problem ? parser.problem : "YAML parse error");
	rv = -EINVAL;
out:
	yaml_parser_delete(&parser);
	fclose(f);
	if (rv < 0)
		policy_free(policy);
	return rv;
}

int policy_select(const struct policy *policy, const char *dt_path, struct names *ignored)
{
	char *compatible = NULL;
	size_t capacity = 0;
	ssize_t len;
	int rv = 0;
	FILE *f = fopen(dt_path, "rb");
	if (!f)
		return -errno;
	while ((len = getdelim(&compatible, &capacity, '\0', f)) >= 0) {
		if (!len || compatible[len - 1] != '\0') {
			rv = -EINVAL;
			break;
		}
		for (size_t i = 0; i < policy->count; i++) {
			if (strcmp(policy->rules[i].compatible, compatible))
				continue;
			for (size_t j = 0; j < policy->rules[i].interfaces.count; j++) {
				rv = names_add(ignored, policy->rules[i].interfaces.values[j]);
				if (rv < 0)
					goto out;
			}
		}
	}
	if (ferror(f))
		rv = -EIO;
out:
	free(compatible);
	fclose(f);
	if (rv < 0)
		names_free(ignored);
	return rv;
}

bool interface_eligible(const char *name, unsigned int type, unsigned int flags,
                        bool bridge_port, const struct names *ignored)
{
	static const char *patterns[] = { "eth*", "en*", "wlan*", "wl*", "br*", "usb*" };
	if (!interface_name_valid(name) || type != ARPHRD_ETHER || !(flags & IFF_MULTICAST) ||
	    bridge_port || names_has(ignored, name))
		return false;
	for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++)
		if (!fnmatch(patterns[i], name, 0))
			return true;
	return false;
}

char *interface_unit(const char *name)
{
	static const char hex[] = "0123456789abcdef";
	size_t len = strlen(name);
	char *unit = malloc(sizeof("bd-upnp@.service") + 4 * len), *p;
	if (!unit)
		return NULL;
	p = stpcpy(unit, "bd-upnp@");
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)name[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '_' || c == ':' || (c == '.' && i)) {
			*p++ = (char)c;
		} else {
			*p++ = '\\';
			*p++ = 'x';
			*p++ = hex[c >> 4];
			*p++ = hex[c & 15];
		}
	}
	strcpy(p, ".service");
	return unit;
}

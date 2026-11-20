// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright © 2026 Michael Heimpold <mhei@heimpold.de> */
#define _GNU_SOURCE
#include "config.h"
#include "bd-upnp-manager-policy.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#ifndef BD_UPNP_MANAGER_CONFIG
#define BD_UPNP_MANAGER_CONFIG "/usr/share/bd-upnp-manager/network.yaml"
#endif
#ifndef BD_UPNP_MANAGER_DT
#define BD_UPNP_MANAGER_DT "/proc/device-tree/compatible"
#endif
#ifndef BD_UPNP_MANAGER_NET
#define BD_UPNP_MANAGER_NET "/sys/class/net"
#endif
#define SYSTEMD_DEST "org.freedesktop.systemd1"
#define SYSTEMD_PATH "/org/freedesktop/systemd1"
#define SYSTEMD_MANAGER "org.freedesktop.systemd1.Manager"
#define RETRY_USEC UINT64_C(5000000)
#define DEBOUNCE_USEC UINT64_C(100000)

enum operation { NONE, START, STOP };
struct instance {
	char *unit, *job;
	unsigned int index, applied_index, job_index;
	bool desired, owned;
	enum operation operation;
	uint64_t retry_at;
	struct instance *next;
};
struct link {
	char *unit;
	unsigned int index;
	struct link *next;
};
struct manager {
	sd_bus *bus;
	sd_event *event;
	sd_event_source *timer;
	struct names ignored;
	struct instance *instances;
	uint64_t rescan_at;
	int (*submit)(struct manager *, struct instance *, enum operation);
};

static uint64_t now_usec(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * UINT64_C(1000000) + now.tv_nsec / 1000;
}

static struct instance *get_instance(struct manager *m, const char *unit)
{
	struct instance *entry;
	for (entry = m->instances; entry; entry = entry->next)
		if (!strcmp(entry->unit, unit))
			return entry;
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return NULL;
	entry->unit = strdup(unit);
	if (!entry->unit) {
		free(entry);
		return NULL;
	}
	entry->next = m->instances;
	m->instances = entry;
	return entry;
}

static int read_number(int dir, const char *name, unsigned int *value)
{
	char buf[64], *end;
	unsigned long parsed;
	ssize_t len;
	int fd = openat(dir, name, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	len = read(fd, buf, sizeof(buf) - 1);
	int saved_errno = errno;
	close(fd);
	if (len < 0)
		return -saved_errno;
	if (!len)
		return -EINVAL;
	buf[len] = 0;
	errno = 0;
	parsed = strtoul(buf, &end, 0);
	if (errno || parsed > UINT_MAX || end == buf || (*end && strcmp(end, "\n")))
		return -EINVAL;
	*value = (unsigned int)parsed;
	return 0;
}

static void links_free(struct link *links)
{
	while (links) {
		struct link *next = links->next;
		free(links->unit);
		free(links);
		links = next;
	}
}

/* Subscribe before calling this: queued link events cover changes during a scan. */
static int scan_links(struct manager *m)
{
	DIR *dir = opendir(BD_UPNP_MANAGER_NET);
	struct dirent *entry;
	struct link *links = NULL;
	int rv = 0;
	if (!dir)
		return -errno;
	for (;;) {
		unsigned int type, flags, index;
		struct stat st;
		bool bridge;
		int fd;
		errno = 0;
		entry = readdir(dir);
		if (!entry) {
			if (errno)
				rv = -errno;
			break;
		}
		if (!interface_name_valid(entry->d_name))
			continue;
		fd = openat(dirfd(dir), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (fd < 0) {
			if (errno == ENOENT || errno == ENODEV)
				continue;
			rv = -errno;
			break;
		}
		rv = read_number(fd, "type", &type);
		if (!rv)
			rv = read_number(fd, "flags", &flags);
		if (!rv)
			rv = read_number(fd, "ifindex", &index);
		bridge = fstatat(fd, "master/bridge", &st, 0) == 0;
		if (!bridge && errno != ENOENT && !rv)
			rv = -errno;
		close(fd);
		if (rv == -ENOENT || rv == -ENODEV) {
			rv = 0;
			continue;
		}
		if (rv < 0)
			break;
		if (index && interface_eligible(entry->d_name, type, flags, bridge, &m->ignored)) {
			struct link *link = calloc(1, sizeof(*link));
			if (!link) {
				rv = -ENOMEM;
				break;
			}
			link->next = links;
			links = link;
			link->index = index;
			link->unit = interface_unit(entry->d_name);
			if (!link->unit) {
				rv = -ENOMEM;
				break;
			}
		}
	}
	closedir(dir);
	if (rv < 0)
		goto out;
	/* Do not change the desired set until the complete snapshot is available. */
	for (struct link *link = links; link; link = link->next)
		if (!get_instance(m, link->unit)) {
			rv = -ENOMEM;
			goto out;
		}
	for (struct instance *i = m->instances; i; i = i->next)
		i->desired = false;
	for (struct link *link = links; link; link = link->next) {
		struct instance *i = get_instance(m, link->unit);
		i->desired = true;
		i->index = link->index;
		/* Already active at manager startup: adopt, without restarting. */
		if (i->owned && !i->applied_index)
			i->applied_index = i->index;
	}
out:
	links_free(links);
	return rv;
}

static bool template_instance(const char *unit)
{
	size_t len = strlen(unit);
	return len > strlen("bd-upnp@.service") && !strncmp(unit, "bd-upnp@", 8) &&
	       !strcmp(unit + len - 8, ".service");
}

static int existing_instances(struct manager *m)
{
	sd_bus_message *reply = NULL;
	int rv = sd_bus_call_method(m->bus, SYSTEMD_DEST, SYSTEMD_PATH, SYSTEMD_MANAGER,
	                            "ListUnitsByPatterns", NULL, &reply, "asas",
	                            0, 1, "bd-upnp@*.service");
	if (rv < 0)
		return rv;
	rv = sd_bus_message_enter_container(reply, 'a', "(ssssssouso)");
	if (rv < 0)
		goto out;
	for (;;) {
		const char *unit, *description, *load, *active, *sub, *following, *path, *job_type, *job_path;
		uint32_t job_id;
		rv = sd_bus_message_read(reply, "(ssssssouso)", &unit, &description, &load,
		                         &active, &sub, &following, &path, &job_id, &job_type, &job_path);
		if (rv <= 0)
			break;
		if (template_instance(unit) && strcmp(active, "inactive") && strcmp(active, "failed")) {
			struct instance *i = get_instance(m, unit);
			if (!i) {
				rv = -ENOMEM;
				break;
			}
			i->owned = true;
		}
	}
out:
	sd_bus_message_unref(reply);
	return rv < 0 ? rv : 0;
}

static int schedule(struct manager *m);

static int request_job(struct manager *m, struct instance *i, enum operation operation)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	const char *path;
	const char *method = operation == START ? "StartUnit" : "StopUnit";
	int rv = sd_bus_call_method(m->bus, SYSTEMD_DEST, SYSTEMD_PATH, SYSTEMD_MANAGER,
	                            method, &error, &reply, "ss", i->unit, "replace");
	if (rv < 0) {
		if (operation == STOP && sd_bus_error_has_name(&error, SYSTEMD_DEST ".NoSuchUnit")) {
			i->owned = false;
			i->retry_at = 0;
			rv = 0;
		} else {
			fprintf(stderr, "%s %s: %s; retrying\n", method, i->unit,
			        error.message ? error.message : strerror(-rv));
			i->retry_at = now_usec() + RETRY_USEC;
		}
		goto out;
	}
	rv = sd_bus_message_read(reply, "o", &path);
	if (rv <= 0) {
		rv = rv < 0 ? rv : -EBADMSG;
		goto out;
	}
	i->operation = operation;
	i->job_index = i->index;
	i->job = strdup(path);
	if (!i->job) {
		rv = -ENOMEM;
		goto out;
	}
	i->retry_at = 0;
	fprintf(stderr, "%s %s\n", method, i->unit);
out:
	if (rv < 0 && !i->retry_at)
		i->retry_at = now_usec() + RETRY_USEC;
	sd_bus_message_unref(reply);
	sd_bus_error_free(&error);
	return rv;
}

static bool unbaptized(struct manager *m, const char *unit)
{
	sd_bus_message *reply = NULL;
	const char *path;
	int status = 0;
	int rv = sd_bus_call_method(m->bus, SYSTEMD_DEST, SYSTEMD_PATH, SYSTEMD_MANAGER,
	                            "GetUnit", NULL, &reply, "s", unit);
	if (rv >= 0 && sd_bus_message_read(reply, "o", &path) > 0)
		sd_bus_get_property_trivial(m->bus, SYSTEMD_DEST, path,
		                           "org.freedesktop.systemd1.Service", "ExecMainStatus",
		                           NULL, 'i', &status);
	sd_bus_message_unref(reply);
	return status == 10;
}

static bool needs_stop(const struct instance *i)
{
	return i->owned && (!i->desired || i->applied_index != i->index);
}

static int drive(struct manager *m)
{
	bool stopping = false;
	uint64_t now = now_usec();
	for (struct instance *i = m->instances; i; i = i->next) {
		if (!needs_stop(i))
			continue;
		if (!i->job && i->retry_at <= now) {
			int rv = m->submit(m, i, STOP);
			if (rv == -ENOMEM)
				return rv;
		}
	}
	stopping = false;
	for (struct instance *i = m->instances; i; i = i->next)
		if (needs_stop(i) || i->operation == STOP ||
		    (i->operation == START && (!i->desired || i->job_index != i->index)))
			stopping = true;
	if (!stopping)
		for (struct instance *i = m->instances; i; i = i->next) {
			if (i->desired && !i->owned && !i->job && i->retry_at <= now) {
				int rv = m->submit(m, i, START);
				if (rv == -ENOMEM)
					return rv;
			}
		}
	/* Forget disappeared interfaces, including suppressed exit-10 instances. */
	struct instance **cursor = &m->instances;
	while (*cursor) {
		struct instance *i = *cursor;
		if (!i->desired && !i->owned && !i->job) {
			*cursor = i->next;
			free(i->unit);
			free(i);
		} else {
			cursor = &i->next;
		}
	}
	return schedule(m);
}

static int on_timer(sd_event_source *source, uint64_t usec, void *userdata)
{
	struct manager *m = userdata;
	int rv;
	(void)source;
	(void)usec;
	if (m->rescan_at && m->rescan_at <= now_usec()) {
		m->rescan_at = 0;
		rv = scan_links(m);
		if (rv < 0) {
			fprintf(stderr, "Cannot scan interfaces: %s; retrying\n", strerror(-rv));
			m->rescan_at = now_usec() + RETRY_USEC;
			/* Keep the old desired set; never stop instances on a failed scan. */
			return schedule(m);
		}
	}
	return drive(m);
}

static int schedule(struct manager *m)
{
	uint64_t deadline = m->rescan_at;
	int rv;
	for (struct instance *i = m->instances; i; i = i->next)
		if (i->retry_at && !i->job && (needs_stop(i) || (i->desired && !i->owned)) &&
		    (!deadline || i->retry_at < deadline))
			deadline = i->retry_at;
	if (!deadline)
		return m->timer ? sd_event_source_set_enabled(m->timer, SD_EVENT_OFF) : 0;
	if (!m->timer)
		return sd_event_add_time(m->event, &m->timer, CLOCK_MONOTONIC, deadline,
		                         10000, on_timer, m);
	rv = sd_event_source_set_time(m->timer, deadline);
	return rv < 0 ? rv : sd_event_source_set_enabled(m->timer, SD_EVENT_ONESHOT);
}

static void finish_job(struct instance *i, const char *result, bool suppressed)
{
	if (!strcmp(result, "done") || suppressed) {
		i->owned = i->operation == START;
		i->applied_index = i->job_index;
	} else {
		fprintf(stderr, "%s job for %s: %s; retrying\n",
		        i->operation == START ? "Start" : "Stop", i->unit, result);
		i->retry_at = now_usec() + RETRY_USEC;
	}
	free(i->job);
	i->job = NULL;
	i->operation = NONE;
}

static int on_job(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
	struct manager *m = userdata;
	const char *path, *unit, *result;
	uint32_t id;
	int rv = sd_bus_message_read(message, "uoss", &id, &path, &unit, &result);
	(void)error;
	if (rv <= 0)
		return rv < 0 ? rv : -EBADMSG;
	for (struct instance *i = m->instances; i; i = i->next) {
		if (!i->job || strcmp(i->job, path))
			continue;
		bool suppressed = i->operation == START && strcmp(result, "done") && unbaptized(m, unit);
		finish_job(i, result, suppressed);
		break;
	}
	rv = drive(m);
	if (rv < 0)
		sd_event_exit(m->event, EXIT_FAILURE);
	return 0;
}

static int on_link(sd_event_source *source, int fd, uint32_t events, void *userdata)
{
	struct manager *m = userdata;
	bool changed = false;
	(void)source;
	(void)events;
	for (unsigned int received = 0; received < 64; received++) {
		char buffer[32768] __attribute__((aligned(__alignof__(struct nlmsghdr))));
		struct sockaddr_nl sender = {0};
		struct iovec iov = { .iov_base = buffer, .iov_len = sizeof(buffer) };
		struct msghdr msg = { .msg_name = &sender, .msg_namelen = sizeof(sender),
		                     .msg_iov = &iov, .msg_iovlen = 1 };
		ssize_t len = recvmsg(fd, &msg, MSG_DONTWAIT);
		if (len < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR)
				continue;
			if (errno == ENOBUFS) {
				fprintf(stderr, "Netlink receive overflow; rescanning interfaces\n");
				changed = true;
				continue;
			}
			fprintf(stderr, "Netlink receive failed: %s\n", strerror(errno));
			return -errno;
		}
		if (!len)
			return -EIO;
		if (sender.nl_pid)
			continue;
		if (msg.msg_flags & MSG_TRUNC)
			changed = true;
		for (struct nlmsghdr *h = (struct nlmsghdr *)buffer; NLMSG_OK(h, len); h = NLMSG_NEXT(h, len))
			if (h->nlmsg_type == RTM_NEWLINK || h->nlmsg_type == RTM_DELLINK ||
			    h->nlmsg_type == NLMSG_OVERRUN || h->nlmsg_type == NLMSG_ERROR)
				changed = true;
	}
	if (changed && !m->rescan_at)
		m->rescan_at = now_usec() + DEBOUNCE_USEC;
	return schedule(m);
}

static int on_signal(sd_event_source *source, const struct signalfd_siginfo *signal, void *userdata)
{
	(void)source;
	(void)signal;
	return sd_event_exit(((struct manager *)userdata)->event, EXIT_SUCCESS);
}

static int open_netlink(void)
{
	struct sockaddr_nl address = { .nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK };
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return -errno;
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		int rv = -errno;
		close(fd);
		return rv;
	}
	return fd;
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "config", required_argument, NULL, 'c' },
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ NULL, 0, NULL, 0 }
	};
	struct manager m = { .submit = request_job };
	struct policy policy = {0};
	sd_bus_slot *slot = NULL;
	sd_event_source *io = NULL, *term = NULL, *interrupt = NULL;
	const char *config = BD_UPNP_MANAGER_CONFIG;
	sigset_t signals;
	int fd = -1, rv, opt;
	while ((opt = getopt_long(argc, argv, "c:hV", options, NULL)) != -1) {
		switch (opt) {
		case 'c': config = optarg; break;
		case 'h':
			printf("Usage: %s [-c FILE]\nManage bd-upnp@.service instances on local Ethernet interfaces.\n"
			       "  -c, --config FILE  YAML configuration (default: %s)\n"
			       "  -h, --help         Show this help\n"
			       "  -V, --version      Show version\n", argv[0], BD_UPNP_MANAGER_CONFIG);
			return EXIT_SUCCESS;
		case 'V': puts(PACKAGE_STRING); return EXIT_SUCCESS;
		default: return EXIT_FAILURE;
		}
	}
	if (optind != argc || !*config)
		return EXIT_FAILURE;
	rv = policy_load(config, &policy);
	if (rv < 0)
		goto out;
	rv = policy_select(&policy, BD_UPNP_MANAGER_DT, &m.ignored);
	if (rv == -ENOMEM)
		goto out;
	if (rv < 0)
		fprintf(stderr, "Warning: cannot read %s: %s; no DT exclusions applied\n",
		        BD_UPNP_MANAGER_DT, strerror(-rv));
	for (size_t i = 0; i < m.ignored.count; i++)
		fprintf(stderr, "Ignoring interface %s (Device Tree policy)\n", m.ignored.values[i]);
	policy_free(&policy);
	sigemptyset(&signals);
	sigaddset(&signals, SIGTERM);
	sigaddset(&signals, SIGINT);
	if (sigprocmask(SIG_BLOCK, &signals, NULL) < 0) {
		rv = -errno;
		goto out;
	}
	/* No window between the initial snapshot and starting event reception. */
	fd = open_netlink();
	if (fd < 0) {
		rv = fd;
		goto out;
	}
	if ((rv = sd_event_new(&m.event)) < 0 ||
	    (rv = sd_bus_open_system(&m.bus)) < 0 ||
	    (rv = sd_bus_set_method_call_timeout(m.bus, RETRY_USEC)) < 0 ||
	    (rv = sd_bus_attach_event(m.bus, m.event, 0)) < 0 ||
	    (rv = sd_bus_set_exit_on_disconnect(m.bus, 1)) < 0 ||
	    (rv = sd_bus_match_signal(m.bus, &slot, SYSTEMD_DEST, SYSTEMD_PATH,
	                             SYSTEMD_MANAGER, "JobRemoved", on_job, &m)) < 0 ||
	    (rv = sd_bus_call_method(m.bus, SYSTEMD_DEST, SYSTEMD_PATH, SYSTEMD_MANAGER,
	                            "Subscribe", NULL, NULL, NULL)) < 0 ||
	    (rv = existing_instances(&m)) < 0 ||
	    (rv = sd_event_add_io(m.event, &io, fd, EPOLLIN, on_link, &m)) < 0 ||
	    (rv = sd_event_add_signal(m.event, &term, SIGTERM, on_signal, &m)) < 0 ||
	    (rv = sd_event_add_signal(m.event, &interrupt, SIGINT, on_signal, &m)) < 0)
		goto out;
	m.rescan_at = now_usec();
	if ((rv = schedule(&m)) >= 0)
		rv = sd_event_loop(m.event);
out:
	if (rv < 0)
		fprintf(stderr, "bd-upnp-manager: %s\n", strerror(-rv));
	while (m.instances) {
		struct instance *i = m.instances;
		m.instances = i->next;
		if (m.bus && (i->owned || i->operation == START))
			sd_bus_call_method(m.bus, SYSTEMD_DEST, SYSTEMD_PATH, SYSTEMD_MANAGER,
			                   "StopUnit", NULL, NULL, "ss", i->unit, "replace");
		free(i->unit);
		free(i->job);
		free(i);
	}
	if (m.bus)
		sd_bus_flush(m.bus);
	sd_event_source_unref(io);
	sd_event_source_unref(term);
	sd_event_source_unref(interrupt);
	sd_event_source_unref(m.timer);
	sd_bus_slot_unref(slot);
	sd_bus_unref(m.bus);
	sd_event_unref(m.event);
	if (fd >= 0)
		close(fd);
	policy_free(&policy);
	names_free(&m.ignored);
	return rv < 0 ? EXIT_FAILURE : rv;
}

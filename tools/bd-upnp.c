// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright © 2026 Michael Heimpold <mhei@heimpold.de>
 */

#define _GNU_SOURCE
#include "config.h"
#include "baptismdata.h"

#include <upnp.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <ifaddrs.h>
#include <limits.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define EXIT_FAILURE_UNBAPTIZED 10
#define RETRY_MS 5000
#define DEBOUNCE_MS 1000
#define ADVERTISEMENT_AGE 1800
#define EXIT_RETRY 75
#define DESCRIPTOR_DIR "/run/bd-upnp"

struct options {
	const char *interface;
	const char *device;
	const char *serial_keys;
	const char *http_root;
	const char *descriptor_dir;
	unsigned int http_port;
	unsigned int port;
	bool https;
	bool use_serial;
	bool use_vendor;
};

enum data_key { MODEL, VENDOR, MANUFACTURER, MANUFACTURER_URL, MODEL_URL, MODEL_NUMBER, DATA_COUNT };
static const char *const data_keys[] = {
	"model", "vendor", "manufacturer", "manufacturer_url", "model_url", "model_number"
};

struct device_data {
	char *values[DATA_COUNT];
	char *serial;
	char *name;
	char uuid[37];
};

struct network {
	unsigned int index;
	bool ready;
	char *signature;
};

struct runtime {
	char *root;
	char *private_dir;
	int lock_fd;
	int directory_fd;
	unsigned int index;
	pid_t worker;
	bool initialized;
	UpnpDevice_Handle handles[2];
	char urls[2][256];
};

enum { OPT_DESCRIPTOR_DIR = 256 };
static const struct option long_options[] = {
	{ "interface",       required_argument, NULL, 'I'                },
	{ "device",          required_argument, NULL, 'd'                },
	{ "port",            required_argument, NULL, 'p'                },
	{ "http-port",       required_argument, NULL, 'P'                },
	{ "http-root",       required_argument, NULL, 'H'                },
	{ "descriptor-dir",  required_argument, NULL, OPT_DESCRIPTOR_DIR },
	{ "serial-keys",     required_argument, NULL, 'S'                },
	{ "use-serial",      no_argument,       NULL, 's'                },
	{ "use-vendor",      no_argument,       NULL, 'v'                },
	{ "https",           no_argument,       NULL, 'T'                },
	{ "version",         no_argument,       NULL, 'V'                },
	{ "help",            no_argument,       NULL, 'h'                },
	{ NULL,              0,                 NULL, 0                  }
};

static void usage(const char *program, int status)
{
	FILE *f = status ? stderr : stdout;

	fprintf(f, "%s (%s) -- Announce a baptism-data based UPnP Basic Device\n\n"
	        "Usage: %s --interface <name> [<options>]\n\n"
	        "  -I, --interface NAME    UPnP network interface (required)\n"
	        "  -d, --device NAME       MAC address source (default: auto-detect)\n"
	        "  -p, --port PORT         PresentationURL port (default: 80, or 443 with --https)\n"
	        "  -P, --http-port PORT    External descriptor HTTP port (default: 80)\n"
	        "  -H, --http-root PATH    Local icon directory (default: %s)\n"
	        "      --descriptor-dir PATH\n"
	        "                          Descriptor output directory (default: %s)\n"
	        "  -S, --serial-keys LIST  Serial keys, first nonempty value wins (default: serial,serial#)\n"
	        "  -s, --use-serial        Append serial instead of MAC address to friendlyName\n"
	        "  -v, --use-vendor        Prefix model name with vendor\n"
	        "  -T, --https             Use https in PresentationURL\n"
	        "  -V, --version           Print version and exit\n"
	        "  -h, --help              Print this usage and exit\n\n"
	        "Descriptors: /upnp/<interface-index>/ipv4.xml and ipv6.xml\n"
	        "Icons: /upnp-static/<width>x<height>-<depth>.png\n"
	        "An external HTTP server must serve both URL prefixes.\n",
	        program, PACKAGE_STRING, program, BD_UPNP_HTTP_ROOT, DESCRIPTOR_DIR);
	exit(status);
}

static int parse_port(const char *text, unsigned int *port)
{
	char *end;
	unsigned long value;

	if (!isdigit((unsigned char)*text))
		return -EINVAL;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || *end || value == 0 || value > 65535)
		return -EINVAL;

	*port = (unsigned int)value;
	return 0;
}

static int mkdir_p(const char *path, mode_t mode)
{
	char *copy = strdup(path), *p;
	struct stat st;
	int rv = 0;

	if (!copy)
		return -ENOMEM;
	if (!*copy) {
		free(copy);
		return -EINVAL;
	}

	for (p = copy + 1; ; p++) {
		char saved = *p;

		if (saved && saved != '/')
			continue;

		*p = '\0';
		if (mkdir(copy, mode) < 0 && errno != EEXIST) {
			rv = -errno;
			break;
		}

		if (stat(copy, &st) < 0) {
			rv = -errno;
			break;
		}

		if (!S_ISDIR(st.st_mode)) {
			rv = -ENOTDIR;
			break;
		}

		*p = saved;
		if (!saved)
			break;
	}

	free(copy);
	return rv;
}

/* The staging directory is outside the web alias, but on the same filesystem.
 * Rename publishes a complete file; UUID callers additionally request durability. */
static int publish_file(int directory_fd, const char *name, const char *staging,
                        const char *contents, bool durable)
{
	char *temporary;
	FILE *f;
	int fd, rv = 0;

	if (asprintf(&temporary, "%s/.tmp-XXXXXX", staging) < 0)
		return -ENOMEM;

	fd = mkostemp(temporary, O_CLOEXEC);
	if (fd < 0) {
		rv = -errno;
		goto out;
	}

	f = fdopen(fd, "w");
	if (!f) {
		rv = -errno;
		close(fd);
		goto unlink_out;
	}

	if (fchmod(fd, 0644) < 0 || fputs(contents, f) == EOF || fflush(f) == EOF ||
	    (durable && fsync(fd) < 0))
		rv = errno ? -errno : -EIO;
	if (fclose(f) == EOF && !rv)
		rv = -errno;

	if (!rv && renameat(AT_FDCWD, temporary, directory_fd, name) < 0)
		rv = -errno;
	if (!rv && durable && fsync(directory_fd) < 0)
		rv = -errno;

unlink_out:
	unlink(temporary);
out:
	free(temporary);
	return rv;
}

static int read_uuid(const char *path, char uuid[37])
{
	char buffer[80];
	FILE *f = fopen(path, "r");
	size_t length;
	int rv = 0;

	if (!f)
		return -errno;

	length = fread(buffer, 1, sizeof(buffer), f);
	if (ferror(f))
		rv = errno ? -errno : -EIO;

	fclose(f);
	if (rv)
		return rv;

	if (length && buffer[length - 1] == '\n')
		length--;
	if (length && buffer[length - 1] == '\r')
		length--;
	if (length != 36)
		return -EINVAL;

	for (size_t i = 0; i < length; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (buffer[i] != '-')
				return -EINVAL;
		} else if (!isxdigit((unsigned char)buffer[i])) {
			return -EINVAL;
		}
	}

	memcpy(uuid, buffer, 36);
	uuid[36] = '\0';
	return 0;
}

static int load_uuid(char uuid[37])
{
	char contents[38];
	int directory_fd, lock_fd, rv;

	rv = mkdir_p(BD_UPNP_STATE_DIR, 0755);
	if (rv)
		return rv;

	directory_fd = open(BD_UPNP_STATE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (directory_fd < 0)
		return -errno;

	/* This inode is permanent. Never unlink the lock file, even on shutdown. */
	lock_fd = openat(directory_fd, "uuid.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (lock_fd < 0) {
		rv = -errno;
		goto close_directory;
	}
	if (flock(lock_fd, LOCK_EX) < 0) {
		rv = -errno;
		goto close_lock;
	}

	rv = read_uuid(BD_UPNP_STATE_DIR "/uuid", uuid);
	if (rv == -ENOENT) {
		rv = read_uuid("/proc/sys/kernel/random/uuid", uuid);
		if (!rv) {
			snprintf(contents, sizeof(contents), "%s\n", uuid);
			rv = publish_file(directory_fd, "uuid", BD_UPNP_STATE_DIR, contents, true);
		}
	}

close_lock:
	close(lock_fd);
close_directory:
	close(directory_fd);
	return rv;
}

static char *get_value(struct baptismdata_ctx *ctx, const char *name)
{
	char *value = baptismdata_get_var(ctx, name);

	if (value && !*value) {
		free(value);
		value = NULL;
	}
	return value;
}

static int load_data(const struct options *opts, struct device_data *data)
{
	struct baptismdata_ctx *ctx;
	char *keys, *key, *saveptr = NULL;
	int rv;

	keys = strdup(opts->serial_keys);
	if (!keys)
		return -ENOMEM;

	rv = baptismdata_open(&ctx);
	if (rv < 0) {
		free(keys);
		return rv;
	}

	for (key = strtok_r(keys, ",", &saveptr); key; key = strtok_r(NULL, ",", &saveptr)) {
		while (*key == ' ')
			key++;
		data->serial = get_value(ctx, key);
		if (data->serial)
			break;
	}

	for (size_t i = 0; i < DATA_COUNT; i++)
		data->values[i] = get_value(ctx, data_keys[i]);
	baptismdata_close(ctx);
	free(keys);

	if (!data->serial) {
		fprintf(stderr, "Error: no serial in baptism data key(s) '%s'\n", opts->serial_keys);
		return -ENOENT;
	}

	if (!data->values[MODEL] || (opts->use_vendor && !data->values[VENDOR])) {
		fprintf(stderr, "Error: model or requested vendor missing from baptism data\n");
		return EXIT_FAILURE_UNBAPTIZED;
	}

	return 0;
}

static int load_name(const struct options *opts, struct device_data *data)
{
	const char *device = opts->device, *individual = data->serial;
	char mac[32], *path = NULL;
	struct dirent *entry;
	DIR *dir = NULL;
	FILE *f;
	int rv = 0;

	if (!opts->use_serial) {
		if (!device) {
			dir = opendir("/sys/class/net");
			if (!dir)
				return -errno;
			while ((entry = readdir(dir))) {
				if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "lo") ||
				    !strncmp(entry->d_name, "can", 3) || !if_nametoindex(entry->d_name))
					continue;
				device = entry->d_name;
				break;
			}
		}
		if (!device) {
			rv = -ENODEV;
			goto out;
		}
		if (asprintf(&path, "/sys/class/net/%s/address", device) < 0) {
			rv = -ENOMEM;
			goto out;
		}
		f = fopen(path, "r");
		if (!f) {
			rv = -errno;
			goto out;
		}
		if (!fgets(mac, sizeof(mac), f))
			rv = errno ? -errno : -EIO;
		fclose(f);
		if (rv)
			goto out;
		mac[strcspn(mac, "\r\n")] = '\0';
		individual = mac;
	}
	if (asprintf(&data->name, "%s%s%s [%s]",
		     opts->use_vendor ? data->values[VENDOR] : "",
		     opts->use_vendor ? " " : "", data->values[MODEL], individual) < 0)
		rv = -ENOMEM;
out:
	if (dir)
		closedir(dir);
	free(path);
	return rv;
}

static int xml_element(FILE *f, const char *tag, const char *value)
{
	if (!value)
		return 0;

	fprintf(f, "<%s>", tag);
	for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
		switch (*p) {
		case '&': fputs("&amp;", f); break;
		case '<': fputs("&lt;", f); break;
		case '>': fputs("&gt;", f); break;
		case '\"': fputs("&quot;", f); break;
		case '\'': fputs("&apos;", f); break;
		default:
			if (*p < 0x20 && *p != '\t' && *p != '\n' && *p != '\r')
				return -EINVAL;
			fputc(*p, f);
		}
	}
	fprintf(f, "</%s>\n", tag);

	return ferror(f) ? -EIO : 0;
}

static int icon_number(const char **name, char separator, unsigned long *number)
{
	char *end;

	if (!isdigit((unsigned char)**name))
		return -EINVAL;

	errno = 0;
	*number = strtoul(*name, &end, 10);
	if (errno || *number == 0 || *number > INT_MAX || *end != separator)
		return -EINVAL;

	*name = end + 1;
	return 0;
}

static int load_icons(const char *root, char **xml)
{
	struct dirent **entries;
	struct stat st;
	unsigned long width, height, depth;
	char *contents = NULL;
	size_t size = 0;
	FILE *f;
	int count, fd, rv = 0;
	bool first = true;

	fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	count = scandir(root, &entries, NULL, alphasort);
	if (count < 0) {
		rv = -errno;
		close(fd);
		return rv;
	}

	f = open_memstream(&contents, &size);
	if (!f) {
		rv = -errno;
		goto out;
	}

	for (int i = 0; i < count; i++) {
		const char *name = entries[i]->d_name, *p = name;
		size_t length = strlen(name);

		if (length < 4 || strcmp(name + length - 4, ".png"))
			continue;
		if (icon_number(&p, 'x', &width) || icon_number(&p, '-', &height) ||
		    icon_number(&p, '.', &depth) || strcmp(p, "png")) {
			fprintf(stderr, "Warning: ignoring icon '%s': expected <width>x<height>-<depth>.png\n", name);
			continue;
		}
		if (fstatat(fd, name, &st, 0) < 0 || !S_ISREG(st.st_mode)) {
			fprintf(stderr, "Warning: ignoring icon '%s': not an accessible regular file\n", name);
			continue;
		}
		if (first) {
			fputs("<iconList>\n", f);
			first = false;
		}
		fprintf(f, "<icon><mimetype>image/png</mimetype><width>%lu</width>"
			"<height>%lu</height><depth>%lu</depth>"
			"<url>/upnp-static/%s</url></icon>\n", width, height, depth, name);
	}
	if (!first)
		fputs("</iconList>\n", f);
	if (ferror(f))
		rv = -EIO;
	if (fclose(f) == EOF)
		rv = -EIO;
	if (!rv) {
		*xml = contents;
		contents = NULL;
	}

out:
	for (int i = 0; i < count; i++)
		free(entries[i]);
	free(entries);
	free(contents);
	close(fd);
	return rv;
}

static int make_description(const struct device_data *data, const char *icons,
                            const char *presentation, char **xml)
{
	const char *manufacturer = data->values[MANUFACTURER];
	char *contents = NULL;
	size_t size = 0;
	FILE *f = open_memstream(&contents, &size);
	int rv = 0;

	if (!f)
		return -errno;

	if (!manufacturer)
		manufacturer = data->values[VENDOR] ? data->values[VENDOR] : "";

	fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	      "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
	      "<specVersion><major>1</major><minor>0</minor></specVersion>\n"
	      "<device>\n<deviceType>urn:schemas-upnp-org:device:Basic:1</deviceType>\n", f);

	if (xml_element(f, "friendlyName", data->name) ||
	    xml_element(f, "manufacturer", manufacturer) ||
	    xml_element(f, "manufacturerURL", data->values[MANUFACTURER_URL]) ||
	    xml_element(f, "modelName", data->values[MODEL]) ||
	    xml_element(f, "modelNumber", data->values[MODEL_NUMBER]) ||
	    xml_element(f, "modelURL", data->values[MODEL_URL]) ||
	    xml_element(f, "serialNumber", data->serial))
		rv = -EINVAL;

	fprintf(f, "<UDN>uuid:%s</UDN>\n%s", data->uuid, icons);

	if (xml_element(f, "presentationURL", presentation))
		rv = -EINVAL;

	fputs("</device>\n</root>\n", f);

	if (ferror(f))
		rv = -EIO;
	if (fclose(f) == EOF)
		rv = -EIO;

	if (rv)
		free(contents);
	else
		*xml = contents;

	return rv;
}

static const char *const filenames[] = { "ipv4.xml", "ipv6.xml" };

static void stop_announcements(struct runtime *rt)
{
	for (size_t i = 0; i < 2; i++) {
		if (rt->handles[i] >= 0) {
			UpnpUnRegisterRootDevice(rt->handles[i]);
			rt->handles[i] = -1;
		}
	}
	if (rt->initialized) {
		UpnpFinish();
		rt->initialized = false;
	}
	for (size_t i = 0; i < 2; i++) {
		if (rt->directory_fd >= 0) {
			if (unlinkat(rt->directory_fd, filenames[i], 0) < 0 && errno != ENOENT)
				fprintf(stderr, "Warning: could not remove descriptor '%s': %s\n",
					filenames[i], strerror(errno));
		}
		rt->urls[i][0] = '\0';
	}
}

static void stop_worker(struct runtime *rt)
{
	int status;
	int64_t elapsed = 0;

	if (!rt->worker)
		return;
	kill(rt->worker, SIGTERM);
	for (;;) {
		pid_t exited = waitpid(rt->worker, &status, WNOHANG);

		if (exited < 0 && errno == EINTR)
			continue;
		if (exited != 0)
			break;
		if (elapsed >= 10000) {
			fprintf(stderr, "Warning: forcing shutdown of unresponsive UPnP worker\n");
			kill(rt->worker, SIGKILL);
			while (waitpid(rt->worker, &status, 0) < 0 && errno == EINTR)
				;
			break;
		}
		poll(NULL, 0, 100);
		elapsed += 100;
	}
	rt->worker = 0;
}

static void release_interface(struct runtime *rt)
{
	/* Keep the directory and lock inode. Never unlink a lock another process
	 * might already have opened. Remove descriptors only while owning the lock. */
	stop_worker(rt);
	stop_announcements(rt);
	if (rt->directory_fd >= 0)
		close(rt->directory_fd);
	if (rt->lock_fd >= 0)
		close(rt->lock_fd);
	rt->directory_fd = -1;
	rt->lock_fd = -1;
	rt->index = 0;
}

static int claim_interface(struct runtime *rt, unsigned int index)
{
	char *path;
	int rv;

	if (rt->index == index)
		return 0;

	release_interface(rt);
	if (!index)
		return 0;

	if (asprintf(&path, "%s/%u.lock", rt->private_dir, index) < 0)
		return -ENOMEM;

	rt->lock_fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	free(path);
	if (rt->lock_fd < 0)
		return -errno;

	if (flock(rt->lock_fd, LOCK_EX | LOCK_NB) < 0) {
		rv = -errno;
		fprintf(stderr, "Error: cannot claim interface index %u (another instance may own it): %s\n",
		        index, strerror(errno));
		goto fail;
	}

	if (asprintf(&path, "%s/%u", rt->root, index) < 0) {
		rv = -ENOMEM;
		goto fail;
	}

	rv = mkdir_p(path, 0755);
	if (!rv) {
		rt->directory_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (rt->directory_fd < 0)
			rv = -errno;
	}

	free(path);
	if (rv)
		goto fail;

	rt->index = index;

	/* Clean up descriptors left by a previous crashed owner, under our lock. */
	for (size_t i = 0; i < 2; i++) {
		if (unlinkat(rt->directory_fd, filenames[i], 0) < 0 && errno != ENOENT) {
			rv = -errno;
			goto fail;
		}
	}

	return 0;
fail:
	release_interface(rt);
	return rv;
}

static int compare_strings(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int read_network(const char *interface, struct network *net)
{
	struct ifaddrs *addresses, *entry;
	char **items = NULL;
	size_t count = 0, length = 0;
	unsigned int flags = 0;
	bool ipv4 = false;
	FILE *f = NULL;
	int rv = 0;

	memset(net, 0, sizeof(*net));
	net->index = if_nametoindex(interface);

	if (getifaddrs(&addresses) < 0)
		return -errno;

	for (entry = addresses; entry; entry = entry->ifa_next) {
		char address[INET6_ADDRSTRLEN], *item, **grown;
		const void *binary;
		int family;

		if (strcmp(entry->ifa_name, interface))
			continue;

		flags = entry->ifa_flags;
		if (!entry->ifa_addr)
			continue;

		family = entry->ifa_addr->sa_family;

		if (family == AF_INET) {
			binary = &((struct sockaddr_in *)entry->ifa_addr)->sin_addr;
			if (((const struct in_addr *)binary)->s_addr != INADDR_ANY)
				ipv4 = true;
		} else if (family == AF_INET6) {
			binary = &((struct sockaddr_in6 *)entry->ifa_addr)->sin6_addr;
		} else {
			continue;
		}

		if (!inet_ntop(family, binary, address, sizeof(address))) {
			rv = -errno;
			goto out;
		}

		if (asprintf(&item, "%d:%s", family, address) < 0) {
			rv = -ENOMEM;
			goto out;
		}

		grown = realloc(items, (count + 1) * sizeof(*items));
		if (!grown) {
			free(item);
			rv = -ENOMEM;
			goto out;
		}
		items = grown;
		items[count++] = item;
	}

	if (count > 1)
		qsort(items, count, sizeof(*items), compare_strings);

	f = open_memstream(&net->signature, &length);
	if (!f) {
		rv = -errno;
		goto out;
	}

	/* Ignore unrelated flag changes (e.g. a multicast membership update). */
	flags &= IFF_UP | IFF_RUNNING | IFF_MULTICAST | IFF_LOOPBACK;
	fprintf(f, "%u:%u", net->index, flags);
	for (size_t i = 0; i < count; i++)
		fprintf(f, ";%s", items[i]);
	if (fclose(f) == EOF)
		rv = -EIO;

	net->ready = net->index && ipv4 && (flags & IFF_UP) &&
		(flags & IFF_RUNNING) && (flags & IFF_MULTICAST) && !(flags & IFF_LOOPBACK);

out:
	freeifaddrs(addresses);
	for (size_t i = 0; i < count; i++)
		free(items[i]);
	free(items);
	if (rv) {
		free(net->signature);
		net->signature = NULL;
	}
	return rv;
}

static bool usable_ipv6(const char *text, bool link_local)
{
	struct in6_addr address;

	if (!text || inet_pton(AF_INET6, text, &address) != 1 ||
	    IN6_IS_ADDR_UNSPECIFIED(&address) || IN6_IS_ADDR_LOOPBACK(&address) ||
	    IN6_IS_ADDR_MULTICAST(&address))
		return false;
	return !!IN6_IS_ADDR_LINKLOCAL(&address) == link_local;
}

static int start_announcements(const struct options *opts, const struct device_data *data,
                               struct runtime *rt)
{
	char ipv4[INET_ADDRSTRLEN], ipv6[INET6_ADDRSTRLEN];
	char *icons = NULL, *xml = NULL;
	const char *global6, *local6;
	bool have_global6, have_ipv6;
	int rv;

	rv = UpnpInit2(opts->interface, 0);
	if (rv != UPNP_E_SUCCESS) {
		fprintf(stderr, "Warning: UPnP initialization on '%s' failed (%d); retrying\n",
		        opts->interface, rv);
		if (rv == UPNP_E_OUTOF_MEMORY || rv == UPNP_E_INIT)
			return -EIO;
		return 0;
	}

	rt->initialized = true;
#if defined(UPNP_HAVE_WEBSERVER) && UPNP_HAVE_WEBSERVER
	/* Disable content serving; SDK-owned listener sockets may remain open. */
	rv = UpnpEnableWebserver(0);
	if (rv != UPNP_E_SUCCESS)
		return -EIO;
#endif
	if (!UpnpGetServerIpAddress() ||
	    strlen(UpnpGetServerIpAddress()) >= sizeof(ipv4))
		return -EIO;

	strcpy(ipv4, UpnpGetServerIpAddress());
	if (!strcmp(ipv4, "0.0.0.0"))
		return -EADDRNOTAVAIL;

	global6 = UpnpGetServerUlaGuaIp6Address();
	local6 = UpnpGetServerIp6Address();
	have_global6 = usable_ipv6(global6, false);
	have_ipv6 = usable_ipv6(local6, true);
	ipv6[0] = '\0';
	if (have_ipv6)
		snprintf(ipv6, sizeof(ipv6), "%s", have_global6 ? global6 : local6);

	rv = load_icons(opts->http_root, &icons);
	if (rv)
		return rv;

	for (size_t i = 0; i < 2; i++) {
		char host[INET6_ADDRSTRLEN + 2], presentation[256];
		const char *scheme = opts->https ? "https" : "http";

		if (i && !have_ipv6)
			continue;
		if (i && have_global6)
			snprintf(host, sizeof(host), "[%s]", ipv6);
		else
			snprintf(host, sizeof(host), "%s", ipv4);
		snprintf(presentation, sizeof(presentation), "%s://%s:%u/", scheme, host, opts->port);
		rv = make_description(data, icons, presentation, &xml);
		if (rv)
			break;
		rv = publish_file(rt->directory_fd, filenames[i], rt->private_dir, xml, false);
		free(xml);
		xml = NULL;
		if (rv)
			break;
		/* Link-local LOCATION URLs have no sender-specific zone suffix. The
		 * SDK uses its selected interface index for its own HTTP download. */
		if (i)
			snprintf(host, sizeof(host), "[%s]", ipv6);
		else
			snprintf(host, sizeof(host), "%s", ipv4);
		snprintf(rt->urls[i], sizeof(rt->urls[i]), "http://%s:%u/upnp/%u/%s",
			host, opts->http_port, rt->index, filenames[i]);
	}
	free(icons);
	return rv;
}

#if UPNP_VERSION >= 11800
#define CALLBACK_EVENT void
#else
#define CALLBACK_EVENT const void
#endif
static int device_callback(Upnp_EventType type, CALLBACK_EVENT *event, void *cookie)
{
	/* Basic:1 exposes no services, actions or event subscriptions. */
	(void)type;
	(void)event;
	(void)cookie;
	return 0;
}

static int register_descriptions(struct runtime *rt)
{
	for (size_t i = 0; i < 2; i++) {
		int rv;

		if (!rt->urls[i][0] || rt->handles[i] >= 0)
			continue;
		rv = UpnpRegisterRootDevice3(rt->urls[i], device_callback, NULL,
		                             &rt->handles[i], i ? AF_INET6 : AF_INET);
		if (rv == UPNP_E_SUCCESS) {
			rv = UpnpSendAdvertisement(rt->handles[i], ADVERTISEMENT_AGE);
			if (rv != UPNP_E_SUCCESS)
				UpnpUnRegisterRootDevice(rt->handles[i]);
		}
		if (rv != UPNP_E_SUCCESS) {
			rt->handles[i] = -1;
			fprintf(stderr, "Warning: could not announce %s (%d); check external HTTP server; retrying\n",
			        rt->urls[i], rv);
			if (rv == UPNP_E_OUTOF_MEMORY || rv == UPNP_E_INVALID_DESC ||
			    rv == UPNP_E_INVALID_URL || rv == UPNP_E_INVALID_PARAM)
				return -EIO;
		} else {
			fprintf(stderr, "Announcing %s\n", rt->urls[i]);
		}
	}
	return 0;
}

static int worker_main(const struct options *opts, const struct device_data *data,
                       struct runtime *rt, int signal_fd)
{
	struct pollfd fd = { .fd = signal_fd, .events = POLLIN };
	int rv = start_announcements(opts, data, rt);
	int status = EXIT_FAILURE;

	if (rv)
		goto out;
	if (!rt->initialized) {
		status = EXIT_RETRY;
		goto out;
	}
	for (;;) {
		int count;

		rv = register_descriptions(rt);
		if (rv)
			break;
		count = poll(&fd, 1, RETRY_MS);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			rv = -errno;
			break;
		}
		if (fd.revents & POLLIN) {
			status = EXIT_SUCCESS;
			break;
		}
		if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			rv = -EIO;
			break;
		}
	}
out:
	if (rv)
		fprintf(stderr, "Error: UPnP worker: %s\n", strerror(-rv));
	stop_announcements(rt);
	return status;
}

/* The single-threaded supervisor never initializes libupnp. Each worker gets
 * fresh SDK state, including on old SDKs which retain IPv6 addresses or fail
 * to fully unwind an unsuccessful initialization. */
static int start_worker(const struct options *opts, const struct device_data *data,
                        struct runtime *rt, int signal_fd, int netlink_fd)
{
	pid_t parent = getpid(), worker = fork();

	if (worker < 0)
		return -errno;
	if (worker == 0) {
		sigset_t signals;
		int fd, status;

		close(signal_fd);
		close(netlink_fd);
		sigemptyset(&signals);
		sigaddset(&signals, SIGTERM);
		sigaddset(&signals, SIGINT);
		fd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
		if (fd < 0 || prctl(PR_SET_PDEATHSIG, SIGTERM) < 0) {
			fprintf(stderr, "Error: could not prepare UPnP worker: %s\n", strerror(errno));
			_exit(EXIT_FAILURE);
		}
		if (getppid() != parent)
			_exit(EXIT_SUCCESS);
		/* Inherit the interface lock until the worker exits, so even a killed
		 * supervisor cannot leave a worker racing a new owner of this index. */
		status = worker_main(opts, data, rt, fd);
		close(fd);
		_exit(status);
	}
	rt->worker = worker;
	return 0;
}

static int open_netlink(void)
{
	struct sockaddr_nl address = {
		.nl_family = AF_NETLINK,
		.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR,
	};
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);

	if (fd < 0)
		return -errno;
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		int rv = -errno;
		close(fd);
		return rv;
	}
	return fd;
}

/* Drain notifications. Re-read the complete state on loss, rather than relying
 * on a possibly incomplete sequence of events. IPv6 address flags (DAD) may
 * change without changing getifaddrs()' address list; force a refresh then. */
static int drain_netlink(int fd, unsigned int index, bool *changed, bool *force)
{
	union { struct nlmsghdr alignment; char bytes[32768]; } buffer;
	int length;

	for (;;) {
		length = recv(fd, buffer.bytes, sizeof(buffer.bytes), MSG_TRUNC);
		if (length < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			if (errno == EINTR)
				continue;
			if (errno == ENOBUFS) {
				*changed = *force = true;
				continue;
			}
			return -errno;
		}
		if (!length)
			return -EIO;
		if (length > (int)sizeof(buffer.bytes)) {
			*changed = *force = true;
			continue;
		}
		for (struct nlmsghdr *h = (struct nlmsghdr *)buffer.bytes;
		     NLMSG_OK(h, length); h = NLMSG_NEXT(h, length)) {
			unsigned int event_index = 0;

			if (h->nlmsg_type == NLMSG_OVERRUN || h->nlmsg_type == NLMSG_ERROR) {
				*changed = *force = true;
				continue;
			}
			if ((h->nlmsg_type == RTM_NEWLINK || h->nlmsg_type == RTM_DELLINK) &&
			    h->nlmsg_len >= NLMSG_LENGTH(sizeof(struct ifinfomsg))) {
				event_index = ((struct ifinfomsg *)NLMSG_DATA(h))->ifi_index;
			} else if ((h->nlmsg_type == RTM_NEWADDR || h->nlmsg_type == RTM_DELADDR) &&
				   h->nlmsg_len >= NLMSG_LENGTH(sizeof(struct ifaddrmsg))) {
				struct ifaddrmsg *a = NLMSG_DATA(h);
				event_index = a->ifa_index;
				if ((!index || event_index == index) && a->ifa_family == AF_INET6)
					*force = true;
			}
			if (event_index && (!index || event_index == index))
				*changed = true;
		}
	}
}

static int64_t now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int run(const struct options *opts, const struct device_data *data,
               struct runtime *rt, int signal_fd, int netlink_fd)
{
	struct pollfd fds[] = {
		{ .fd = signal_fd, .events = POLLIN },
		{ .fd = netlink_fd, .events = POLLIN },
	};
	char *signature = NULL;
	int64_t next_check = 0, debounce = 0;
	bool force = false;
	int rv = 0;

	for (;;) {
		int64_t now = now_ms(), deadline = debounce ? debounce : next_check;
		int timeout = deadline > now ? (int)(deadline - now) : 0;
		int count = poll(fds, 2, timeout);

		if (count < 0) {
			if (errno == EINTR)
				continue;
			rv = -errno;
			break;
		}
		if (fds[0].revents & POLLIN) {
			struct signalfd_siginfo signal_info;
			bool quit = false;

			while (read(signal_fd, &signal_info, sizeof(signal_info)) == sizeof(signal_info)) {
				if (signal_info.ssi_signo == SIGINT || signal_info.ssi_signo == SIGTERM)
					quit = true;
			}
			if (quit)
				break;
		}
		if (rt->worker) {
			int status;
			pid_t exited = waitpid(rt->worker, &status, WNOHANG);

			if (exited == rt->worker) {
				rt->worker = 0;
				stop_announcements(rt);
				if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_RETRY) {
					fprintf(stderr, "Error: UPnP worker exited unexpectedly (status %d)\n", status);
					rv = -EIO;
					break;
				}
				next_check = now_ms() + RETRY_MS;
			}
		}
		if ((fds[0].revents | fds[1].revents) & (POLLHUP | POLLNVAL)) {
			rv = -EIO;
			break;
		}
		if (fds[1].revents & (POLLIN | POLLERR)) {
			bool changed = false;
			unsigned int index = rt->index ? rt->index : if_nametoindex(opts->interface);

			rv = drain_netlink(netlink_fd, index, &changed, &force);
			if (rv)
				break;
			if (changed && !debounce)
				debounce = now_ms() + DEBOUNCE_MS;
		}
		now = now_ms();
		if (debounce ? now < debounce : now < next_check)
			continue;
		{
			struct network net;
			bool changed;

			rv = read_network(opts->interface, &net);
			if (rv)
				break;
			changed = force || !signature || strcmp(signature, net.signature);
			free(signature);
			signature = net.signature;
			force = false;
			debounce = 0;
			if (changed) {
				stop_worker(rt);
				stop_announcements(rt);
			}
			rv = claim_interface(rt, net.index);
			if (rv)
				break;
			if (changed && !net.ready)
				fprintf(stderr, "Waiting for interface '%s' with multicast and IPv4 connectivity\n",
					opts->interface);
			if (net.ready && !rt->worker) {
				rv = start_worker(opts, data, rt, signal_fd, netlink_fd);
				if (rv)
					break;
			}
		}
		next_check = now_ms() + RETRY_MS;
	}
	free(signature);
	return rv;
}

int main(int argc, char **argv)
{
	struct options opts = {
		.serial_keys = "serial,serial#",
		.http_root = BD_UPNP_HTTP_ROOT,
		.descriptor_dir = DESCRIPTOR_DIR,
		.http_port = 80,
	};
	struct device_data data = { 0 };
	struct runtime rt = { .lock_fd = -1, .directory_fd = -1, .handles = { -1, -1 } };
	sigset_t signals;
	int signal_fd = -1, netlink_fd = -1;
	int c, rv, status = EXIT_FAILURE;

	while ((c = getopt_long(argc, argv, "I:d:p:P:H:S:svTVh", long_options, NULL)) != -1) {
		switch (c) {
		case 'I': opts.interface = optarg; break;
		case 'd': opts.device = optarg; break;
		case 'p':
		case 'P':
			rv = parse_port(optarg, c == 'p' ? &opts.port : &opts.http_port);
			if (rv) {
				fprintf(stderr, "Error: invalid port '%s' (expected 1–65535)\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'H': opts.http_root = optarg; break;
		case OPT_DESCRIPTOR_DIR: opts.descriptor_dir = optarg; break;
		case 'S': opts.serial_keys = optarg; break;
		case 's': opts.use_serial = true; break;
		case 'v': opts.use_vendor = true; break;
		case 'T': opts.https = true; break;
		case 'V': printf("%s (%s)\n", argv[0], PACKAGE_STRING); return EXIT_SUCCESS;
		case 'h': usage(argv[0], EXIT_SUCCESS); break;
		default: usage(argv[0], EXIT_FAILURE);
		}
	}
	if (!opts.interface || !*opts.interface || strlen(opts.interface) >= IFNAMSIZ ||
	    strchr(opts.interface, '/') || optind != argc) {
		fprintf(stderr, "Error: --interface NAME is required; positional arguments are not accepted\n");
		return EXIT_FAILURE;
	}
	if (opts.device && (!*opts.device || strlen(opts.device) >= IFNAMSIZ ||
			    strchr(opts.device, '/') || !if_nametoindex(opts.device))) {
		fprintf(stderr, "Error: invalid MAC source interface '%s'\n", opts.device);
		return EXIT_FAILURE;
	}
	if (!opts.port)
		opts.port = opts.https ? 443 : 80;
	if (!*opts.http_root || opts.descriptor_dir[0] != '/' || !*opts.serial_keys) {
		fprintf(stderr, "Error: nonempty icon directory and serial keys, and absolute descriptor directory required\n");
		return EXIT_FAILURE;
	}
	sigemptyset(&signals);
	sigaddset(&signals, SIGINT);
	sigaddset(&signals, SIGTERM);
	sigaddset(&signals, SIGCHLD);
	rv = pthread_sigmask(SIG_BLOCK, &signals, NULL);
	if (rv) {
		fprintf(stderr, "Error: could not block termination signals: %s\n", strerror(rv));
		return EXIT_FAILURE;
	}
	signal_fd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
	if (signal_fd < 0) {
		rv = -errno;
		goto out;
	}
	netlink_fd = open_netlink();
	if (netlink_fd < 0) {
		rv = netlink_fd;
		goto out;
	}
	rv = load_data(&opts, &data);
	if (rv == EXIT_FAILURE_UNBAPTIZED) {
		status = EXIT_FAILURE_UNBAPTIZED;
		rv = 0;
		goto out;
	}
	if (rv)
		goto out;
	rv = load_name(&opts, &data);
	if (rv)
		goto out;
	rv = load_uuid(data.uuid);
	if (rv) {
		fprintf(stderr, "Error: could not load/persist UUID in %s: %s\n", BD_UPNP_STATE_DIR, strerror(-rv));
		goto out;
	}
	/* Validate the icon directory even if the interface has not appeared yet. */
	{
		char *icons = NULL;
		rv = load_icons(opts.http_root, &icons);
		free(icons);
		if (rv)
			goto out;
	}
	rv = mkdir_p(opts.descriptor_dir, 0755);
	if (rv)
		goto out;
	rt.root = realpath(opts.descriptor_dir, NULL);
	if (!rt.root) {
		rv = -errno;
		goto out;
	}
	/* Reject /: there is no sibling private directory outside its web alias. */
	if (!strcmp(rt.root, "/")) {
		rv = -EINVAL;
		goto out;
	}
	if (asprintf(&rt.private_dir, "%s.state", rt.root) < 0) {
		rv = -ENOMEM;
		goto out;
	}
	rv = mkdir_p(rt.private_dir, 0700);
	if (rv)
		goto out;
	rv = run(&opts, &data, &rt, signal_fd, netlink_fd);
	if (!rv)
		status = EXIT_SUCCESS;
out:
	if (rv < 0)
		fprintf(stderr, "Error: %s\n", strerror(-rv));
	release_interface(&rt);
	if (signal_fd >= 0)
		close(signal_fd);
	if (netlink_fd >= 0)
		close(netlink_fd);
	free(rt.root);
	free(rt.private_dir);
	free(data.serial);
	free(data.name);
	for (size_t i = 0; i < DATA_COUNT; i++)
		free(data.values[i]);
	return status;
}

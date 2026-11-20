// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
static char *test_net_path;
#define BD_UPNP_MANAGER_NET test_net_path
#define main manager_main
#include "bd-upnp-manager.c"
#undef main

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned int starts, stops;
static enum operation fail_operation;

static int fake_submit(struct manager *m, struct instance *i, enum operation op)
{
	(void)m;
	if (op == fail_operation) {
		i->retry_at = now_usec() + RETRY_USEC;
		return -EIO;
	}
	if (op == START)
		starts++;
	else
		stops++;
	i->operation = op;
	i->job_index = i->index;
	i->job = strdup(i->unit);
	CHECK(i->job);
	i->retry_at = 0;
	return 1;
}

static void attribute(const char *name, const char *attr, const char *value)
{
	char path[PATH_MAX];
	CHECK(snprintf(path, sizeof(path), "%s/%s/%s", test_net_path, name, attr) < (int)sizeof(path));
	FILE *f = fopen(path, "w");
	CHECK(f);
	CHECK(fputs(value, f) >= 0);
	CHECK(!fclose(f));
}

static void make_interface(const char *name, const char *index)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", test_net_path, name);
	CHECK(!mkdir(path, 0700));
	attribute(name, "type", "1\n");
	attribute(name, "flags", "0x1000\n");
	attribute(name, "ifindex", index);
}

static void complete(struct manager *m)
{
	for (struct instance *i = m->instances; i; i = i->next)
		if (i->job)
			finish_job(i, "done", false);
	CHECK(drive(m) >= 0);
}

static void remove_interface(const char *name)
{
	char path[PATH_MAX];
	const char *attrs[] = { "type", "flags", "ifindex", "master" };
	for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s/%s", test_net_path, name, attrs[i]);
		CHECK(!unlink(path) || errno == ENOENT);
	}
	snprintf(path, sizeof(path), "%s/%s/bridge", test_net_path, name);
	CHECK(!rmdir(path) || errno == ENOENT);
	snprintf(path, sizeof(path), "%s/%s", test_net_path, name);
	CHECK(!rmdir(path));
}

int main(void)
{
	char dir[] = "/tmp/bd-upnp-state-XXXXXX", path[PATH_MAX], target[PATH_MAX];
	test_net_path = mkdtemp(dir);
	CHECK(test_net_path);
	struct manager m = { .submit = fake_submit };
	CHECK(sd_event_new(&m.event) >= 0);
	CHECK(!names_add(&m.ignored, "eth1"));
	make_interface("eth0", "2\n");
	make_interface("br0", "3\n");
	make_interface("eth1", "4\n");
	snprintf(target, sizeof(target), "%s/br0/bridge", dir);
	CHECK(!mkdir(target, 0700));
	CHECK(!scan_links(&m));
	CHECK(drive(&m) >= 0);
	CHECK(starts == 2 && stops == 0);
	complete(&m);
	CHECK(!scan_links(&m));
	CHECK(drive(&m) >= 0);
	CHECK(starts == 2 && stops == 0); /* no repeated starts, including exit 10 */

	/* Enslaving eth0 stops only its instance. */
	snprintf(path, sizeof(path), "%s/eth0/master", dir);
	snprintf(target, sizeof(target), "%s/br0", dir);
	CHECK(!symlink(target, path));
	CHECK(!scan_links(&m));
	CHECK(drive(&m) >= 0);
	CHECK(starts == 2 && stops == 1);
	complete(&m);
	CHECK(!unlink(path));
	CHECK(!scan_links(&m));
	CHECK(drive(&m) >= 0);
	CHECK(starts == 3);
	complete(&m);

	/* Recreated interface with the same name but another index: stop, then start. */
	attribute("eth0", "ifindex", "7\n");
	CHECK(!scan_links(&m));
	CHECK(drive(&m) >= 0);
	CHECK(stops == 2 && starts == 3);
	complete(&m);
	CHECK(starts == 4);
	complete(&m);

	/* Failed StopUnit cannot be followed by a new start until the retry succeeds. */
	attribute("eth0", "ifindex", "8\n");
	CHECK(!scan_links(&m));
	fail_operation = STOP;
	CHECK(drive(&m) >= 0);
	CHECK(stops == 2 && starts == 4);
	struct instance *eth = get_instance(&m, "bd-upnp@eth0.service");
	CHECK(eth->retry_at > now_usec());
	fail_operation = NONE;
	eth->retry_at = 0;
	CHECK(drive(&m) >= 0);
	CHECK(stops == 3 && starts == 4);
	complete(&m);
	CHECK(starts == 5);
	/* Failed start job retries, but a failed job with exit 10 is suppressed. */
	CHECK(eth->operation == START);
	finish_job(eth, "failed", false);
	CHECK(!eth->owned && eth->retry_at);
	eth->retry_at = 0;
	CHECK(drive(&m) >= 0);
	CHECK(starts == 6);
	finish_job(eth, "failed", true);
	CHECK(eth->owned);
	CHECK(drive(&m) >= 0);
	CHECK(starts == 6);

	/* A failed snapshot keeps the previous desired set intact. */
	attribute("eth0", "flags", "invalid\n");
	CHECK(scan_links(&m) < 0);
	CHECK(eth->desired);
	attribute("eth0", "flags", "0x1000\n");

	/* Rename: stop the old instance before starting the new instance. */
	snprintf(path, sizeof(path), "%s/eth0", dir);
	snprintf(target, sizeof(target), "%s/usb0", dir);
	CHECK(!rename(path, target));
	CHECK(!scan_links(&m));
	CHECK(drive(&m) >= 0);
	CHECK(stops == 4 && starts == 6);
	complete(&m);
	CHECK(starts == 7);
	complete(&m);

	/* Receive a link event and overflow notification without a polling timer. */
	int sockets[2];
	CHECK(!socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, sockets));
	struct nlmsghdr msg = { .nlmsg_len = NLMSG_HDRLEN, .nlmsg_type = RTM_NEWLINK };
	CHECK(write(sockets[0], &msg, sizeof(msg)) == sizeof(msg));
	CHECK(on_link(NULL, sockets[1], EPOLLIN, &m) >= 0);
	CHECK(m.rescan_at);
	m.rescan_at = 0;
	msg.nlmsg_type = NLMSG_OVERRUN;
	CHECK(write(sockets[0], &msg, sizeof(msg)) == sizeof(msg));
	CHECK(on_link(NULL, sockets[1], EPOLLIN, &m) >= 0);
	CHECK(m.rescan_at);
	close(sockets[0]);
	close(sockets[1]);
	m.rescan_at = 0;
	CHECK(schedule(&m) >= 0);
	int enabled;
	CHECK(sd_event_source_get_enabled(m.timer, &enabled) >= 0 && enabled == SD_EVENT_OFF);

	/* Shutdown/disappearance removes the corresponding desired instances. */
	remove_interface("usb0");
	CHECK(!scan_links(&m));
	CHECK(drive(&m) >= 0);
	CHECK(stops == 5);
	complete(&m);
	struct instance *stale = get_instance(&m, "bd-upnp@eth9.service");
	stale->owned = true; /* existing instance no longer backed by an interface */
	CHECK(!scan_links(&m));
	CHECK(drive(&m) >= 0);
	CHECK(stops == 6);
	complete(&m);
	while (m.instances) {
		struct instance *next = m.instances->next;
		free(m.instances->unit);
		free(m.instances->job);
		free(m.instances);
		m.instances = next;
	}
	sd_event_source_unref(m.timer);
	sd_event_unref(m.event);
	names_free(&m.ignored);
	remove_interface("eth1");
	remove_interface("br0");
	CHECK(!rmdir(dir));
	puts("UPnP manager state tests passed");
	return 0;
}

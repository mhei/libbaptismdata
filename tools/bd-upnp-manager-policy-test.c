// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "bd-upnp-manager-policy.h"
#include <errno.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void write_file(const char *path, const void *data, size_t len)
{
	FILE *f = fopen(path, "wb");
	CHECK(f);
	CHECK(fwrite(data, 1, len, f) == len);
	CHECK(!fclose(f));
}

int main(void)
{
	char yaml[] = "/tmp/bd-upnp-policy-XXXXXX", dt[] = "/tmp/bd-upnp-dt-XXXXXX";
	int fd = mkstemp(yaml);
	CHECK(fd >= 0);
	close(fd);
	fd = mkstemp(dt);
	CHECK(fd >= 0);
	close(fd);
	struct policy policy = {0};
	struct names ignored = {0};
	const char valid[] = "ignore_interfaces:\n  'vendor,board': [eth1, eth2]\n  'vendor,soc': [eth2, eth3]\n";
	const char compatible[] = "vendor,board\0vendor,soc\0";
	write_file(yaml, valid, sizeof(valid) - 1);
	write_file(dt, compatible, sizeof(compatible) - 1);
	CHECK(!policy_load(yaml, &policy));
	CHECK(!policy_select(&policy, dt, &ignored));
	CHECK(ignored.count == 3);
	CHECK(names_has(&ignored, "eth1") && names_has(&ignored, "eth2") && names_has(&ignored, "eth3"));
	CHECK(interface_eligible("eth0", ARPHRD_ETHER, IFF_MULTICAST, false, &ignored));
	CHECK(!interface_eligible("eth1", ARPHRD_ETHER, IFF_MULTICAST, false, &ignored));
	CHECK(!interface_eligible("eth0", ARPHRD_ETHER, IFF_MULTICAST, true, &ignored));
	CHECK(!interface_eligible("eth0", ARPHRD_ETHER, 0, false, &ignored));
	CHECK(!interface_eligible("lo", ARPHRD_LOOPBACK, IFF_MULTICAST, false, &ignored));
	CHECK(!interface_eligible("can0", ARPHRD_CAN, IFF_MULTICAST, false, &ignored));
	CHECK(!interface_eligible("veth0", ARPHRD_ETHER, IFF_MULTICAST, false, &ignored));
	CHECK(!interface_eligible("wwan0", ARPHRD_ETHER, IFF_MULTICAST, false, &ignored));
	const char *accepted[] = { "enp1s0", "eth9", "wlan0", "wlp2s0", "br0", "usb0" };
	for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++)
		CHECK(interface_eligible(accepted[i], ARPHRD_ETHER, IFF_MULTICAST, false, &ignored));
	CHECK(!interface_name_valid("eth/name"));
	CHECK(!interface_name_valid("eth 0"));
	CHECK(!interface_name_valid("1234567890123456"));
	char *unit = interface_unit("br-lan.1");
	CHECK(unit && !strcmp(unit, "bd-upnp@br\\x2dlan.1.service"));
	free(unit);
	unit = interface_unit(".hidden");
	CHECK(unit && !strcmp(unit, "bd-upnp@\\x2ehidden.service"));
	free(unit);
	names_free(&ignored);
	const char unknown[] = "vendor,board-other\0";
	write_file(dt, unknown, sizeof(unknown) - 1);
	CHECK(!policy_select(&policy, dt, &ignored));
	CHECK(!ignored.count);
	write_file(dt, "vendor,board", 12);
	CHECK(policy_select(&policy, dt, &ignored) == -EINVAL);
	CHECK(!ignored.count);
	CHECK(policy_select(&policy, "/nonexistent/bd-upnp-dt", &ignored) == -ENOENT);
	policy_free(&policy);
	const char *invalid[] = {
		"", "ignore_interfaces: []\n", "other: {}\n", "ignore_interfaces: null\n",
		"ignore_interfaces: {}\nignore_interfaces: {}\n",
		"ignore_interfaces: {board: [eth0], board: [eth1]}\n",
		"ignore_interfaces: {board: eth0}\n", "ignore_interfaces: {board: [[eth0]]}\n",
		"ignore_interfaces: {board: ['eth/0']}\n", "ignore_interfaces: {board: ['']}\n",
		"ignore_interfaces: {board: [\"eth\\0\"]}\n",
		"ignore_interfaces: {}\n---\nignore_interfaces: {}\n", "ignore_interfaces: [\n"
	};
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		write_file(yaml, invalid[i], strlen(invalid[i]));
		CHECK(policy_load(yaml, &policy) < 0);
		CHECK(!policy.count);
	}
	const char empty[] = "# Empty policy\nignore_interfaces: {}\n";
	write_file(yaml, empty, sizeof(empty) - 1);
	CHECK(!policy_load(yaml, &policy));
	CHECK(!policy.count);
	policy_free(&policy);
	CHECK(policy_load("/nonexistent/bd-upnp-yaml", &policy) == -ENOENT);
	unlink(yaml);
	unlink(dt);
	puts("UPnP manager policy tests passed");
	return 0;
}

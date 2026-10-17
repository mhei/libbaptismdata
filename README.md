Overview
========

libbaptismdata is a free software library to ease the access to baptism data
of an embedded device. The baptism data, for example serial number, device
specific keys or similar, is stored as key-value pairs using libubootenv.
This library is intended to allow easy access from others programs and/or
other libraries.

Since the underlying API of libubootenv already does a great job, this
library can focus on the remaining parameterization.

A command line tool allows to read/write/modify the baptism data from e.g.
shell scripts and/or human interactive usage.

The license of libbaptismdata is LGPL v2.1 or later and the licence of
programs in tools directory is GPL v3.0 or later.

The documentation is available under the Creative Commons Attribution-ShareAlike
License 3.0 (Unported) (http://creativecommons.org/licenses/by-sa/3.0/).

The library is written in C and designed to run on Linux.



Installation
------------

The shell commands are ``./autogen.sh; ./configure; make; make install``.

An (optional) mDNS announcement tool based on libavahi-client can be built with
``./configure --with-avahi-mdns``. It installs ``bd-mdns`` and requires the
``avahi-client`` and ``avahi-common`` development packages at build time.
See below for a short tool introduction.

An optional UPnP announcement tool based on libupnp can be built with
``./configure --with-upnp``. It installs ``bd-upnp`` and requires libupnp
version 1.8.4 or newer with device, SSDP and IPv6 support, including its
development files and pkg-config metadata. The option is disabled by default;
libupnp is only searched for when requested. The UPnP descriptors and optional
icons must be served by an external HTTP server, for example lighttpd.



Well-Known Baptism Data
-----------------------

The idea to have some general/common/well-known baptism data variables is, that
these variables are stored during (OEM) device manufacturing inside the baptism
data storage area. During later runtime, programs can use these variables.
However, programmers must know which variables should be used how. The following
table thus lists variable names, example content and describes example usage
and/or usage constraints.
It does not automatically means, that each variable must be present in every device.
If a programmer wants to make use of a variable, this should be negotiated which
the OEM supplier of the board and/or defined internally during the baptism process
of the device.

Important Note: Baptism Data is considered read-only during normal device lifetime!
The baptism data storage area should usually not be written when the device is running
in the field - it does not replace proper configuration management. The idea is more
that OEM suppliers and vendors have a well-known area where such kind of data is
placed - which is also retained over firmware upgrades etc.

| Variable Name    | Example Content                         | Description |
|------------------|-----------------------------------------|-------------|
| serial           | SH4CAWN00123                            | Traditional serial number as string, does not necessarily consist of digits only. Usually, this serial appears also on a label on the device. |
| serial#          | ABC12345                                | Traditional serial number as string, does not necessarily consist of digits only. Unlike `serial`, this is an internal serial number, e.g. of the internal controller board, not of the whole device. |
| board_revision   | V0R7a                                   | Revision of internal (main) PCB |
| manufacturer     | Heimpold GmbH                           | Name of vendor, may include the legal form, typically used in product property lists or as text for the following URL |
| manufacturer_url | https://example-manufacturer.com        | URL to vendor website, ideally not a deep-link so that it is available for the whole product lifetime |
| vendor           | Heimpold                                | Name of vendor, not including any legal form. Is typically used with `model` to form a unique product name aka `vendor` + `single whitespace` + `model` |
| model            | Heavy Bear Mixer 3000                   | A human-friedly name of the model, aka the product name; without vendor name, without any device specific data/numbers (serial numbers/MAC addresses) |
| model_url        | https://example-manufacturer.com/bm3000 | URL to model website (if any), as above, ideally not a deep-link |
| model_number     | BM3000                                  | A model number as string (if any) |
| revision         | A0                                      | Product revision, very often this is not defined for the first product revision so that users of this variable should consider reasonable fallback/default values. |

Note: Vendor and manufacturer refer here to the very same company - it is the brand
which appears physically on the device label and/or which is shown on device's web frontend.



Baptism Data on Storage
-----------------------

As mentioned, this library uses libubootenv to store key-value pairs as baptism data.
The physical storage is defined using a configuration file - this file name is hard-coded
as `/etc/baptism-data.config`. During the system design, it should be decided where and
how to store this baptism data and the configuration file is expected to be present
in the final system, i.e. when the library is used.

A simple example `/etc/baptism-data.config` file in traditional syntax which uses
two redundant 128 KiB blocks at the end of the two eMMC boot partitions:

```
/dev/mmcblk0boot0 -0x20000 0x20000
/dev/mmcblk0boot1 -0x20000 0x20000
```

Newer versions of libuboootenv also support configuration files in YAML syntax.
The simple example from above would look like:

```yaml
baptism-data:
    lockfile: /var/lock/baptism-data.lock
    size: 0x20000
    devices:
        - path: /dev/mmcblk0boot0
          offset: -0x20000
        - path: /dev/mmcblk0boot1
          offset: -0x20000
```



bd-is-baptized
--------------

``bd-is-baptized`` checks that the baptism-data variables ``model`` and ``vendor``
are both present and nonempty. It exits with status 0 when both values are
available, otherwise with status 1. Errors opening or reading the baptism data
also result in status 1. Success is silent; missing, empty or unreadable values
are reported on stderr. The tool only reads baptism data.
This is intended as a fixed check, useful before starting ``bd-mdns``
or ``bd-upnp`` and can be used as systemd's ``ExecCondition``.
Other stuff for the specific tools is not checked, so exit status 0 does not
guarantee that either announcement tool can start successfully.



bd-mdns
-------

This tiny tool is designed to use the baptism-data to announce a service
on the network via mDNS.
The mDNS service is announced on all interfaces (handled automatically by Avahi).


By default, ``bd-mdns`` uses the baptism-data ``model`` variable as service name,
the ``serial`` variable in the TXT record and adds ``model`` and ``vendor`` TXT
records when present.

Use ``--serial-key <name>[,<name>...]`` to read the serial value from different
baptism-data variables while still publishing it as ``serial=...`` in the TXT
record. This can be a list of keys, comma-separated - the first found value wins.

As device individualization element, it uses the MAC address in brackets
appended to the service name.
If ``--device`` is omitted, the network interface which is the source of
this MAC address is tried to be automatically detected, i.e. the first interface
found in ``/sys/class/net`` is used which is not loopback and not e.g. a CAN
interface.
Use ``--use-serial`` to place the serial number in brackets instead.

When ``--use-vendor`` is used, then the default service name (the baptized model)
is additionally prefixed with the baptizd vendor string, i.e.
``vendor + " " + model + " " + "[" + individualization element + "]"``.



bd-upnp
-------

``bd-upnp`` announces ``urn:schemas-upnp-org:device:Basic:1`` on a required
network interface. It runs in the foreground; run a separate instance for each
UPnP interface. All instances describe the same device and share its UUID.
IPv4 is required; IPv6 is announced additionally when a link-local IPv6 address
is available on the same interface. IPv6-only operation is not supported at
the moment.

The friendlyName is ``model [MAC]`` using baptism data and the selected MAC
source. ``--use-vendor`` produces ``vendor model [MAC]``; ``--use-serial``
replaces the MAC with the serial number. Without ``--device``, the MAC source
is auto-detected using the same heuristic as bd-mdns, independently of the
UPnP interface. ``model`` and the selected serial must exist; ``vendor`` is
required when ``--use-vendor`` is requested. Missing model or requested vendor
results in exit status 10, other permanent failures in exit status 1.

| Option | Meaning |
|--------|---------|
| `-I`, `--interface NAME` | Required UPnP interface, e.g. `eth0`; wait if it is not available yet. |
| `-d`, `--device NAME` | MAC address source; default: auto-detect. |
| `-p`, `--port PORT` | Web interface port in presentationURL; default: 80, or 443 with `--https`. |
| `-P`, `--http-port PORT` | External HTTP server port for descriptors and icons; default: 80. |
| `-H`, `--http-root PATH` | Local icon directory; default: `${datadir}/bd-upnp`. |
| `--descriptor-dir PATH` | Absolute descriptor output directory; default: `/run/bd-upnp`. |
| `-S`, `--serial-keys LIST` | Comma-separated serial keys; default: `serial,serial#`, first nonempty value wins. |
| `-s`, `--use-serial` | Append serial instead of MAC to friendlyName. |
| `-v`, `--use-vendor` | Prefix friendlyName with vendor. |
| `-T`, `--https` | Use HTTPS in presentationURL. |
| `-V`, `--version` | Print version and exit. |
| `-h`, `--help` | Print usage and exit. |

Both port options accept 1–65535; explicit presentation ports override the
HTTP/HTTPS default regardless of argument order. ``--https`` affects only
presentationURL, not the URL used for the descriptor serving HTTP server.

The device descriptor maps ``model`` to modelName, the selected serial to
serialNumber, ``manufacturer`` to manufacturer, ``manufacturer_url`` to
manufacturerURL, ``model_url`` to modelURL and ``model_number`` to modelNumber.
Manufacturer falls back to ``vendor``, then an empty string. Other missing
optional fields are omitted. The presentationURL points to ``/`` on the
interface IPv4 address. The IPv6 descriptor uses a ULA/global IPv6 address
when available, otherwise the IPv4 presentationURL. The reason why the IPv6
link-local address is not used here is, that it usually requires an
interface specifier (aka zone suffix) on the peer.

### External HTTP server and icons

An instance uses its numeric interface index as URL prefix. For index 2:

```text
/run/bd-upnp/2/ipv4.xml
/run/bd-upnp/2/ipv6.xml

http://192.168.10.1:80/upnp/2/ipv4.xml
http://[2001:db8::10]:80/upnp/2/ipv6.xml
```

The IPv6 LOCATION prefers a ULA/global address, falling back to the link-local
address. Link-local LOCATION URLs omit a zone suffix: receivers must use the interface
on which discovery occurred, while libupnp uses its selected interface index for
its own download. IPv6 address literals are enclosed in brackets.

The external HTTP server must provide these fixed aliases:

| URL prefix      | Directory                         |
|-----------------|-----------------------------------|
| `/upnp/`        | The `--descriptor-dir` directory. |
| `/upnp-static/` | The `--http-root` directory.      |

A lighttpd example is installed in ``${docdir}/examples/lighttpd-bd-upnp.conf``.
It includes ``mod_alias``, XML/PNG MIME types, and listener examples. Adapt both
aliases if the directories are overridden, and configure listeners for the
interface IPv4 and IPv6 addresses on ``--http-port``. A wildcard listener also
covers addresses added later. The daemon writes descriptors; the HTTP server
needs only read/traverse permissions. All instances using an output directory
must run under the same service account and in the same network namespace.

Icons are regular ``.png`` files directly inside ``--http-root``. The filename
must be ``<width>x<height>-<depth>.png``, for example ``32x32-8.png`` describes
32 by 32 pixels at 8 bits per pixel. Positive integer metadata comes exclusively
from the filename; the image is not inspected. Matching files are listed in
filename order with MIME type ``image/png`` and URLs such as
``/upnp-static/32x32-8.png``. Invalid names are skipped with a warning; they
remain accessible through the external server. Subdirectories are not scanned.
An empty directory is valid and produces no iconList. An absent or unreadable
directory is an error. Icons are rescanned at startup and network refresh,
without a filesystem watcher. Image assets are expected to be installed by
a branding specific package of the distribution.

The default icon directory is derived from autotools ``datadir``, i.e. usually
it is ``/usr/share/bd-upnp``.

### UUID, concurrency and network changes

The UUID is stored in ``${localstatedir}/lib/bd-upnp/uuid``. Configure with
``--localstatedir=/var`` for ``/var/lib/bd-upnp/uuid``; the Autoconf default
instead follows the installation prefix. Parent directories are created as
needed. A permanent ``uuid.lock`` file serializes readers and initializers.
Only if the UUID file does not exist, a UUID is read from
``/proc/sys/kernel/random/uuid`` and persisted via a temporary file, file sync,
atomic rename and directory sync. Invalid existing UUIDs or persistence errors
abort startup. The lock file is never deleted. No instance announces a UUID
before persistence has completed.

Each interface index has a separate lifetime lock in a private sibling directory
``<descriptor-dir>.state``. Temporary XML files also live there and are atomically
renamed into the published index directory; the two directories must be on the
same filesystem. The private directory is not part of the HTTP aliases. A second
instance claiming the same index fails. Descriptors are removed while still
holding the lock at shutdown or when the index changes; stale files from a
crashed previous owner are removed after acquiring that lock. Lock files and
empty index directories remain, avoiding races with processes opening old locks.

The supervisor watches interface and IPv4/IPv6 address changes using rtnetlink,
bundles notifications for one second and refreshes when the selected interface
changes. Other interfaces do not trigger a refresh. A full state read every five
seconds also covers missed notifications. It waits for the selected interface
and IPv4 connectivity if necessary.

libupnp runs in a supervised child process with a fresh SDK state for every
network refresh; this also avoids retained IPv6 addresses in older libupnp
versions. The supervisor and child retain the interface lock until the child
has stopped. SIGINT/SIGTERM withdraws advertisements, stops the SDK and removes
the descriptors. The child also stops if its supervisor dies.

URL-based registration downloads the XML from the external HTTP server.
If it is not reachable yet, registration is retried every five seconds; IPv4
can remain announced while IPv6 registration is retried. Successful registrations
are advertised with a lifetime of 1800 seconds and renewed by libupnp. There is
no separate periodic webserver health check after successful registration.

No content is served by the embedded libupnp HTTP server. Depending on the SDK
build, unused internal HTTP listeners may still be opened on automatically
selected ports; these are independent of ``--http-port``.



Report a Bug
------------

To report a bug, you can:
 * fill a bug report on the issue tracker
   http://github.com/mhei/libbaptismdata/issues
 * or send an email to mhei@heimpold.de



Legal Notice
------------

Trade names, trademarks and registered trademarks are used without special
marking throughout the source code and/or documentation. All are the property
of their respective owners.

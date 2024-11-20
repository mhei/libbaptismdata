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
| board_revision   | V0R7a                                   | Revision of internal (main) PCB |
| manufacturer     | Heimpold GmbH                           | Name of vendor, may include the legal form, typically used in product property lists or as text for the following URL |
| manufacturer_url | https://example-manufacturer.com        | URL to vendor website, ideally not a deep-link so that it is available for the whole product lifetime |
| vendor           | Heimpold                                | Name of vendor, not including any legal form. Is typically used with `model` to form a unique product name aka `vendor` + `single whitespace` + `model` |
| model            | Heavy Bear Mixer 3000                   | A human-friedly name of the model, aka the product name; without vendor name, without any device specific data/numbers (serial numbers/MAC addresses) |
| model_url        | https://example-manufacturer.com/bm3000 | URL to model website (if any), as above, ideally not a deep-link |
| model_no         | BM3000                                  | A model number as string (if any) |
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

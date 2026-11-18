# Baptism Data Daemon (bdd)

`bdd` serves device baptism data through libbaptismdata, a REST API and a small
Bootstrap frontend at `/`. The API specification is served at `/openapi.yaml`.
The default listener is `0.0.0.0:8080`. All stored keys are readable; only
explicitly configured keys can be created, edited or deleted.

## Build, test and install

From the libbaptismdata project root:

```sh
./autogen.sh
mkdir build-bdd
cd build-bdd
../configure --prefix=/usr --with-bdd
make -j
make check
make DESTDIR=/tmp/bdd-install install
```

The daemon is optional and disabled by default. Its pkg-config dependencies
are libmicrohttpd >= 0.9.71, json-c >= 0.15, GnuTLS >= 3.5.0, libsystemd and
avahi-client, libcrypt/libxcrypt and libeconf >= 0.7.1, in addition to the library's libubootenv dependency. Tests require
Python 3 and OpenSSL; they use a storage double and never access real device
storage. PyYAML and jsonschema optionally validate live response schemas.
`tests/contract.py openapi.yaml` additionally validates the OpenAPI document
when `tests/requirements.txt` dependencies are available. `make check` builds
test executables; a normal build does not require these Python packages.
Node.js optionally tests the frontend logic with a DOM test double. Python
dbus/GLib bindings and dbus-daemon optionally test Zeroconf against a private
Avahi D-Bus double, including service records, collision handling and reconnect.

The executable installs into `sbindir`, web assets and the specification into
`datadir/bdd` (by default `/usr/local/share/bdd`), which serves directly as the
webroot: `index.html`, `app.js`, `app.css`, `bootstrap.min.css` and `openapi.yaml` are
installed side by side. Vendor configuration installs into `/lib/bdd`, and the unit into
`/lib/systemd/system`. Override the latter two directories using
`--with-bdd-vendor-dir=DIR` and `--with-systemdsystemunitdir=DIR`. `DESTDIR`
staging is supported. No local `/etc` configuration is overwritten, and the
service is not automatically enabled. Only Bootstrap's minified CSS and MIT
license are included; no Bootstrap JavaScript, source maps or CDN is needed.

## Configuration

Configuration uses libeconf and the INI section `[bdd]`. libeconf reads and
merges the main file and drop-ins, resolves directory priorities and masks,
and parses quoted values and comments. libsystemd provides service readiness
and shutdown notifications through `sd_notify()`.

The first existing main file in this priority order is used:

1. `/etc/bdd/bdd.conf`
2. `/run/bdd/bdd.conf`
3. `/lib/bdd/bdd.conf` (or the configured vendor directory)

A local main file replaces the vendor main file; omitted options use built-in
defaults. Prefer drop-ins to retain vendor settings. After the main file,
`bdd.conf.d/*.conf` files from all three directories are merged by libeconf,
with vendor files first, then runtime files, then local files. Files within a
directory are processed in filename order. Local entries take precedence over
vendor entries. A same-named symlink to `/dev/null` masks a vendor drop-in.

For example, create `/etc/bdd/bdd.conf.d/90-local.conf`:

```ini
[bdd]
ListenAddress=0.0.0.0
Port=8080
Zeroconf=yes
WritableKey=serial
WritableKey="serial#"
WritableKey=model
Password="$6$bddexample$N9DL9jxlyl.VGooKEAwUzE4dVLjjeevO/j1XxeD6iqhQvnDA4rMJUfCbQ7wd9SIM7hGh6aPzrN.cUyrUqqBPf/"
```

- `ListenAddress`: numeric IPv4 address; default `0.0.0.0`.
- `Port`: integer 1–65535; default 8080.
- `WritableKey`: repeat for exact, case-sensitive key names, optionally comma-separated on each line.
  Surrounding spaces and empty list elements are ignored; duplicates are removed.
  Commas are separators and cannot be part of a configured key name. `WritableKey=""`
  resets the entries earlier in the same file. A WritableKey assignment in a
  drop-in replaces the entire list from previous files; specify the complete
  intended list there. The default list is empty (read only).
- `Password`: complete Modular Crypt Format hash; empty or omitted disables authentication.
  Unsupported or invalid hashes prevent startup. Generate a SHA-512-Crypt hash with
  `openssl passwd -6` and store its complete output, including `$6$`, in quotes.
  Username is `admin`. Protect a file containing a password with mode `0600`.
- `Zeroconf`: boolean, default `yes`. Announces `_http._tcp` or `_https._tcp`,
  the actual listener port, and TXT records `path=/api/v1/baptism-data`,
  `openapi=/openapi.yaml`, `api=baptism-data`. Requires a running Avahi daemon
  and system D-Bus. Discovery failures are logged without disabling HTTP;
  connections are retried after Avahi restarts. Name collisions are resolved
  using alternative DNS-SD instance names.
- `CertificateFile` and `PrivateKeyFile`: together enable HTTPS.
- `ClientCAFile`: optional trust bundle enabling mandatory client certificates (mTLS).

Boolean values and INI syntax follow libeconf. Quote values that contain
comment characters, for example `WritableKey="serial#"`. Repeated WritableKey
assignments in one file are joined by libeconf; a later file replaces that list.
Scalar options should occur once per file. Unknown sections/options and invalid
values prevent startup. Each configuration file is limited to 1 MiB. Changes
require a restart. `bdd --config FILE` reads that file and adjacent `FILE.d` snippets, bypassing
the standard configuration directories. The old JSON configuration is no longer used.

See the [libeconf documentation](https://github.com/openSUSE/libeconf) for
configuration file syntax and merging rules.

## Frontend and authentication

The table shows Key, R/W, Defined and Value columns. Lock and pencil symbols
indicate read-only and writable keys. Known keys have gray descriptions.
The keys vendor, model, manufacturer, manufacturer_url, model_url, model_number
and revision always appear, alongside all existing and configured writable keys.
Only WritableKey controls whether a key may be created, changed or deleted.

Defined is checked when a key exists, including when its value is an empty string.
Unchecked keys have disabled value fields. Check Defined to create a value;
an empty field stores an empty string. Uncheck Defined on an existing key to
delete it on saving. The current value is retained until successful saving so
unchecking can be undone. Read-only keys have disabled Defined controls.

Unsaved definition or value changes are highlighted yellow. Reverting changes
clears the highlight. The **Save Baptism Data to Device** button submits all
editable defined values and deletions in one batch; undefined keys that remain
undefined are omitted. The form is frozen during saving. Success updates the
comparison state, while errors preserve edits. Deleted standard keys remain
visible as undefined rows. Inline result messages clear on the next edit.

All GET endpoints, including values and metadata, are accessible without a
password. When `Password` is set, PUT and DELETE require HTTP Basic credentials
with username `admin`. The frontend remains readable and asks for the password
when saving. Edits survive failed authentication; submitting the login form
retries saving the current edits without reloading the table.

Metadata exposes `write_authentication_required` independently of per-key write
permissions. Direct API clients receive a `WWW-Authenticate: Basic` challenge on
401 responses. The frontend sends `X-Requested-With: XMLHttpRequest` to suppress
browser authentication popups; this header does not bypass authentication.
Credentials remain in page memory, never in local storage or cookies.
Use HTTPS on untrusted networks: plain HTTP exposes passwords and data.

## API

- `GET /api/v1/baptism-data`: object of existing string values.
- `GET /api/v1/metadata`: `{"keys":{"serial":true,"model":false},"write_authentication_required":true}`. Booleans
  indicate write permission; includes existing keys and configured absent keys.
- `GET /api/v1/baptism-data/{key}`: `{"value":"..."}`.
- `PUT /api/v1/baptism-data/{key}` with `{"value":"..."}`: create/update,
  returning 201/200 respectively.
- `DELETE /api/v1/baptism-data/{key}`: delete, returning 204 (404 if absent).
- `PUT /api/v1/baptism-data` with `{"serial":"ABC123","old-key":null}`:
  batch update/delete, returning 204 after successful persistence. Deleting an
  absent key in a batch is harmless. Unspecified keys remain unchanged.

All names and permissions in a batch are checked before storage is opened.
Changes are staged under one library lock and written using one
`baptismdata_store()` call. Setter failures discard staged changes. A physical
storage failure cannot guarantee unchanged storage; read back before retrying.
This does not provide optimistic concurrency: another client's change can be
overwritten by a later submission of previously read values.

PUT requires `Content-Type: application/json`; maximum request body is 1 MiB.
Keys and values are UTF-8, without NUL. Keys cannot contain `=`, ASCII control
characters or DEL. Percent-encode a key as a single path segment, including
`serial%23` for `serial#`; decoding happens exactly once. GET and DELETE accept
no body. API responses have `Cache-Control: no-store`. Failures return an
`error` object with `code` and `message`, with HTTP 400, 401 (password), 403
(permissions or client certificate), 404, 405, 413, 415 or 500.

```sh
curl -u admin http://localhost:8080/api/v1/metadata
curl -u admin -X PUT http://localhost:8080/api/v1/baptism-data \
  -H 'Content-Type: application/json' -d '{"serial":"ABC123","model":""}'
```

## Mutual TLS and service operation

A TLS drop-in can contain:

```ini
[bdd]
CertificateFile=/etc/bdd/server-chain.pem
PrivateKeyFile=/etc/bdd/server-key.pem
ClientCAFile=/etc/bdd/client-ca.pem
```

CertificateFile and PrivateKeyFile enable HTTPS. Both must be absolute PEM paths
and match; each file is limited to 4 MiB. Self-signed server certificates are
supported without a root CA file on the device. The browser/API client must trust
the server certificate or its issuing CA and verify its hostname.

ClientCAFile is optional. If present, it enables mTLS and specifies the trust
anchors for client certificates; it does not validate the server certificate.
Client chains, validity and client-auth purpose are checked on every route.
Without ClientCAFile, HTTPS does not require a client certificate. A configured
Password protects writes in either mode. Without any certificate
paths, the daemon serves HTTP. Partial or invalid TLS settings prevent startup,
with no plaintext fallback. Only TLS 1.2/1.3 are supported. Certificate rotation
requires a restart.

Storage access is configured separately through `/etc/baptism-data.config`.
The service account needs device and lock-file permissions. Requests use fresh
library contexts; a blocked storage operation can delay requests and shutdown.
The unit runs in the foreground as root by default to access device storage,
with `Type=notify`; SIGINT/SIGTERM stop it cleanly and withdraw the announcement.
Logs go to stderr without baptism data or passwords.

```sh
systemctl daemon-reload
systemctl enable --now bdd.service
journalctl -u bdd.service
```

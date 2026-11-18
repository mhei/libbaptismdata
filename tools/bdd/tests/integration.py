#!/usr/bin/env python3
"""Exercise the real HTTP/TLS server against a transactional storage double."""
import contextlib
import crypt
import http.client
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.parse import quote

try:
    import yaml
    from jsonschema import Draft4Validator
except ImportError:
    yaml = None

SERVER = str(Path(sys.argv.pop(1)).resolve())
OPENSSL = sys.argv.pop(1)
ROOT = "/api/v1/baptism-data"


def configuration_text(config):
    names = {"listen_address": "ListenAddress", "port": "Port", "password": "Password", "zeroconf": "Zeroconf"}
    lines = ["[bdd]", "Zeroconf=no"]
    for key, value in config.items():
        if key == "writable_keys":
            lines += (["WritableKey=" + json.dumps(item, ensure_ascii=False) for item in value] if value else ["WritableKey="])
        elif key == "tls":
            fields = {"enabled": "TLS", "certificate_file": "CertificateFile", "private_key_file": "PrivateKeyFile", "client_ca_file": "ClientCAFile"}
            lines += [f"{fields[name]}={str(item).lower() if isinstance(item, bool) else item}" for name, item in value.items() if name != "enabled"]
        else:
            if key == "password" and value: value = crypt.crypt(value, crypt.mksalt(crypt.METHOD_SHA512))
            lines.append(f"{names[key]}={str(value).lower() if isinstance(value, bool) else value}")
    return "\n".join(lines) + "\n"


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Integration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="bdd-tests-")
        cls.directory = Path(cls.temp.name)
        cls.contract = (yaml.safe_load((Path(__file__).resolve().parent.parent / "openapi.yaml").read_text())
                        if yaml else None)

        def openssl(*args):
            subprocess.run([OPENSSL, *map(str, args)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

        def key_and_request(name):
            openssl("req", "-new", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                    "-nodes", "-keyout", cls.directory / f"{name}.key", "-out",
                    cls.directory / f"{name}.csr", "-subj", f"/CN={name}")

        for ca in ("server-ca", "client-ca", "other-ca"):
            openssl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                    "-nodes", "-days", "3", "-keyout", cls.directory / f"{ca}.key",
                    "-out", cls.directory / f"{ca}.pem", "-subj", f"/CN={ca}",
                    "-addext", "basicConstraints=critical,CA:TRUE",
                    "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        certificates = [
            ("server", "server-ca", "serverAuth", 3),
            ("client", "client-ca", "clientAuth", 3),
            ("untrusted", "other-ca", "clientAuth", 3),
            ("wrong-purpose", "client-ca", "serverAuth", 3),
            ("expired", "client-ca", "clientAuth", -1),
            ("expired-server", "server-ca", "serverAuth", -1),
        ]
        for index, (name, ca, purpose, days) in enumerate(certificates, 1):
            key_and_request(name)
            extensions = cls.directory / f"{name}.ext"
            extensions.write_text("basicConstraints=critical,CA:FALSE\n"
                                  "keyUsage=critical,digitalSignature\n"
                                  f"extendedKeyUsage={purpose}\nsubjectAltName=DNS:localhost\n")
            openssl("x509", "-req", "-in", cls.directory / f"{name}.csr", "-CA",
                    cls.directory / f"{ca}.pem", "-CAkey", cls.directory / f"{ca}.key",
                    "-set_serial", index, "-days", days, "-extfile", extensions,
                    "-out", cls.directory / f"{name}.pem")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.work = tempfile.TemporaryDirectory(dir=self.directory)
        self.path = Path(self.work.name)
        self.audit = self.path / "audit"
        self.audit.touch()

    def tearDown(self):
        self.work.cleanup()

    def tls_config(self):
        return {"enabled": True,
                "certificate_file": str(self.directory / "server.pem"),
                "private_key_file": str(self.directory / "server.key"),
                "client_ca_file": str(self.directory / "client-ca.pem")}

    def context(self, client="client", ca="server-ca", version=None):
        context = ssl.create_default_context(cafile=str(self.directory / f"{ca}.pem"))
        if client:
            context.load_cert_chain(self.directory / f"{client}.pem",
                                    self.directory / f"{client}.key")
        if version:
            context.minimum_version = version
            context.maximum_version = version
        return context

    @contextlib.contextmanager
    def server(self, *, tls=False, writable=None, failure=None, config=None):
        port = free_port()
        configuration = {"listen_address": "127.0.0.1", "port": port,
                         "writable_keys": writable if writable is not None else ["serial", "serial#", "new"]}
        if tls:
            configuration["tls"] = self.tls_config()
        if config:
            configuration.update(config)
        file = self.path / "bdd.conf"
        file.write_text(configuration_text(configuration))
        environment = dict(os.environ, BDD_TEST_AUDIT=str(self.audit))
        if failure:
            environment["BDD_TEST_FAIL"] = failure
        process = subprocess.Popen([SERVER, "--config", str(file)], env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            for _ in range(500):
                if process.poll() is not None:
                    self.fail(f"Server did not start: {process.stderr.read().decode()}")
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                        break
                except OSError:
                    time.sleep(0.01)
            else:
                self.fail("Server startup timed out")
            yield port
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                out, err = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                out, err = process.communicate()
                self.fail("Server did not stop on SIGTERM")
            self.assertEqual(process.returncode, 0, err.decode())
            self.assertNotIn(b"ABC123", err)
            self.assertEqual(out, b"")

    def request(self, port, method="GET", path=ROOT, body=None, context=None,
                content_type="application/json", host="localhost", chunked=False, requested_with=None):
        connection = (http.client.HTTPSConnection(host, port, context=context, timeout=3)
                      if context else http.client.HTTPConnection(host, port, timeout=3))
        headers = {"Content-Type": content_type} if content_type else {}
        if requested_with: headers["X-Requested-With"] = requested_with
        if getattr(self, "authorization", None): headers["Authorization"] = self.authorization
        try:
            try:
                connection.request(method, path, body=body, headers=headers, encode_chunked=chunked)
            except BrokenPipeError:
                # The server can reject Content-Length before the client finishes sending.
                pass
            response = connection.getresponse()
            data = response.read()
            result = (response.status, json.loads(data) if data else None, dict(response.getheaders()))
            if response.status != 204:
                self.assertEqual(response.getheader("Content-Type"), "application/json")
            self.assertEqual(response.getheader("Cache-Control"), "no-store")
            if self.contract:
                # Resolve the local references before validating real API responses.
                def resolve(value):
                    if isinstance(value, dict):
                        if "$ref" in value:
                            target = self.contract
                            for part in value["$ref"].removeprefix("#/").split("/"):
                                target = target[part]
                            return resolve(target)
                        return {key: resolve(item) for key, item in value.items()}
                    if isinstance(value, list):
                        return [resolve(item) for item in value]
                    return value

                route = path.split("?", 1)[0]
                if route not in self.contract["paths"]: route = ROOT + "/{key}"
                operation = self.contract["paths"][route].get(method.lower())
                if operation and str(response.status) in operation["responses"]:
                    schema = resolve(operation["responses"][str(response.status)])
                    if data:
                        Draft4Validator(schema["content"]["application/json"]["schema"]).validate(result[1])
                    else:
                        self.assertNotIn("content", schema)
            return result
        finally:
            connection.close()

    def operations(self):
        return self.audit.read_text().splitlines()

    def test_crud_and_listing(self):
        with self.server() as port:
            self.assertEqual(self.request(port)[1],
                             {"serial": "ABC123", "serial#": "BOARD42", "model": "Test Device"})
            status, data, headers = self.request(port, "PUT", ROOT + "/new", '{"value":"created"}')
            self.assertEqual((status, data), (201, {"value": "created"}))
            self.assertEqual(headers["Location"], ROOT + "/new")
            self.assertEqual(self.request(port, path=ROOT + "/new")[:2], (200, {"value": "created"}))
            self.assertEqual(self.request(port, "PUT", ROOT + "/new", '{"value":""}')[:2],
                             (200, {"value": ""}))
            self.assertEqual(self.request(port, path=ROOT + "/new")[1], {"value": ""})
            self.assertEqual(self.request(port, "DELETE", ROOT + "/new")[:2], (204, None))
            self.assertEqual(self.request(port, path=ROOT + "/new")[0], 404)
            self.assertEqual(self.request(port, "DELETE", ROOT + "/new")[0], 404)
            self.assertEqual(self.request(port, "PUT", ROOT + "/serial", '{"value":"changed"}')[0], 200)
            self.assertEqual(self.request(port, path=ROOT + "/serial")[1], {"value": "changed"})
        operations = self.operations()
        self.assertEqual(operations.count("open"), operations.count("close"))
        self.assertEqual(operations.count("set"), 4)
        self.assertEqual(operations.count("store"), 4)

    def test_whitelist_denies_before_storage(self):
        for writable in ([], ["Serial"], ["serial*"]):
            with self.server(writable=writable) as port:
                for method in ("PUT", "DELETE"):
                    self.assertEqual(self.request(port, method, ROOT + "/serial", '{"value":"x"}')[0], 403)
                self.assertEqual(self.operations(), [])
                self.assertEqual(self.request(port, path=ROOT + "/serial")[0], 200)
            self.audit.write_text("")

    def test_encoded_keys_and_single_decoding(self):
        keys = ["serial#", "serial%23", "a/b", "a+b", "Größe"]
        with self.server(writable=keys) as port:
            self.assertEqual(self.request(port, path=ROOT + "/serial%23")[1], {"value": "BOARD42"})
            for key in keys:
                path = ROOT + "/" + quote(key, safe="")
                self.assertIn(self.request(port, "PUT", path, '{"value":"ok"}')[0], (200, 201))
                self.assertEqual(self.request(port, path=path)[1], {"value": "ok"})

    def test_string_values_preserve_text_and_escapes(self):
        with self.server() as port:
            for value in ("", "Größe 🔑", "line one\nline two", r"literal \u0000", "a\\b\"c"):
                body = json.dumps({"value": value})
                self.assertEqual(self.request(port, "PUT", ROOT + "/serial", body)[:2],
                                 (200, {"value": value}))
                self.assertEqual(self.request(port, path=ROOT + "/serial")[1], {"value": value})

    def test_validation_precedes_storage(self):
        with self.server() as port:
            for body in ('{}', '{"value":null}', '{"value":42}', '{"value":"a\\u0000b"}',
                         '{"value":"x","extra":1}', '{"value":"x"}junk', '["x"]',
                         '{"value":"x",}', '{"value\\u0000extra":"x"}',
                         '', b'{"value":"\xff"}'):
                self.assertEqual(self.request(port, "PUT", ROOT + "/serial", body)[0], 400, body)
            self.assertEqual(self.request(port, "PUT", ROOT + "/serial", '{}',
                                          content_type="text/plain")[0], 415)
            for key in ("", "%00", "serial%00ignored", "a%3Db", "%01", "%7f", "%ff", "%ZZ", "%"):
                self.assertEqual(self.request(port, path=ROOT + "/" + key)[0], 400, key)
            self.assertEqual(self.request(port, path="/missing")[0], 404)
            self.assertEqual(self.request(port, "POST")[0], 405)
            self.assertEqual(self.request(port, "PATCH", ROOT + "/serial")[2]["Allow"], "GET, PUT, DELETE")
            self.assertEqual(self.request(port, "GET", ROOT, "body")[0], 400)
            self.assertEqual(self.operations(), [])

    def test_body_limit_and_chunked_put(self):
        with self.server() as port:
            self.assertEqual(self.request(port, "PUT", ROOT + "/serial", b'x' * (1024 * 1024 + 1))[0], 413)
            self.assertEqual(self.operations(), [])
            # Use an iterable so http.client actually applies chunked framing.
            self.assertEqual(self.request(port, "PUT", ROOT + "/serial",
                                          iter([b'{"value":', b'"chunked"}']), chunked=True)[0], 200)
            self.assertEqual(self.request(port, path=ROOT + "/serial")[1], {"value": "chunked"})
            before = self.operations()
            self.assertEqual(self.request(port, "PUT", ROOT + "/serial",
                                          iter([b'x' * 65536] * 17), chunked=True)[0], 413)
            self.assertEqual(self.operations(), before)

    def test_storage_errors_and_context_release(self):
        for failure in ("open", "set", "store"):
            self.audit.write_text("")
            with self.server(failure=failure) as port:
                self.assertEqual(self.request(port, "PUT", ROOT + "/serial", '{"value":"secret"}')[0], 500)
                expected = ["open"] if failure == "open" else (
                    ["open", "set", "close"] if failure == "set" else ["open", "set", "store", "close"])
                self.assertEqual(self.operations(), expected)
                if failure != "open":
                    self.assertEqual(self.request(port, path=ROOT + "/serial")[1], {"value": "ABC123"})

    def test_mtls_independent_cas_and_protocols(self):
        with self.server(tls=True) as port:
            for index, version in enumerate((ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3)):
                context = self.context(version=version)
                self.assertEqual(self.request(port, context=context)[0], 200)
                self.assertEqual(self.request(port, "PUT", ROOT + "/serial", '{"value":"secure"}',
                                              context=context)[0], 200 if index == 0 else 201)
                self.assertEqual(self.request(port, path=ROOT + "/serial", context=context)[1],
                                 {"value": "secure"})
                self.assertEqual(self.request(port, "DELETE", ROOT + "/serial", context=context)[0], 204)
            self.assertEqual(self.request(port, "PUT", ROOT + "/model", '{"value":"x"}',
                                          context=self.context())[0], 403)

    def test_mtls_rejects_invalid_clients_without_storage(self):
        with self.server(tls=True) as port:
            for client in (None, "untrusted", "wrong-purpose", "expired"):
                for method in ("GET", "PUT", "DELETE"):
                    with self.subTest(client=client, method=method):
                        try:
                            status = self.request(port, method, ROOT + "/serial",
                                                  '{"value":"x"}' if method == "PUT" else None,
                                                  context=self.context(client=client))[0]
                        except (ssl.SSLError, ConnectionError, http.client.RemoteDisconnected):
                            pass  # TLS-layer rejection is also valid.
                        else:
                            self.assertEqual(status, 403)
                        self.assertEqual(self.operations(), [])

    def test_clients_verify_server_and_plaintext_is_rejected(self):
        with self.server(tls=True) as port:
            with self.assertRaises(ssl.SSLCertVerificationError):
                self.request(port, context=self.context(ca="client-ca"))
            with self.assertRaises(ssl.SSLCertVerificationError):
                self.request(port, context=self.context(), host="127.0.0.1")
            try:
                status = self.request(port)[0]
            except (OSError, http.client.HTTPException):
                pass
            else:
                self.assertNotEqual(status, 200)
            self.assertEqual(self.operations(), [])

    def test_tls_rejects_legacy_protocols(self):
        with self.server(tls=True) as port:
            for version in (ssl.TLSVersion.TLSv1, ssl.TLSVersion.TLSv1_1):
                context = self.context()
                # Permit legacy protocols on the test client, so the server must refuse.
                context.set_ciphers("DEFAULT:@SECLEVEL=0")
                context.minimum_version = version
                context.maximum_version = version
                with self.assertRaises((ssl.SSLError, ConnectionError)):
                    self.request(port, context=context)
            self.assertEqual(self.operations(), [])

    def assert_start_fails(self, configuration):
        file = self.path / "invalid.conf"
        file.write_text(configuration if isinstance(configuration, str) else configuration_text(configuration))
        result = subprocess.run([SERVER, "--config", str(file)], capture_output=True, timeout=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(b"PRIVATE KEY", result.stderr)

    def test_metadata_and_batch(self):
        with self.server() as port:
            metadata = self.request(port, path="/api/v1/metadata")[1]
            self.assertEqual(metadata, {"keys": {"serial": True, "serial#": True, "model": False, "new": True}, "write_authentication_required": False})
            self.audit.write_text("")
            body = json.dumps({"serial": "", "serial#": None, "new": "created"})
            self.assertEqual(self.request(port, "PUT", ROOT, body)[0], 204)
            self.assertEqual(self.operations(), ["open", "set", "set", "set", "store", "close"])
            self.assertEqual(self.request(port)[1], {"serial": "", "new": "created", "model": "Test Device"})

    def test_batch_validation_and_failures(self):
        with self.server() as port:
            for body, code in (({ "serial": "changed", "model": "forbidden"}, 403),
                               ({"serial": "changed", "new": 123}, 400),
                               ({"serial": "changed", "bad=key": "bad"}, 400)):
                self.assertEqual(self.request(port, "PUT", ROOT, json.dumps(body))[0], code)
                self.assertEqual(self.operations(), [])
        for failure in ("set", "store"):
            with self.server(failure=failure) as port:
                self.assertEqual(self.request(port, "PUT", ROOT, '{"serial":"changed","new":"new"}')[0], 500)
                self.assertEqual(self.request(port)[1]["serial"], "ABC123")

    def test_password_and_assets(self):
        import base64
        with self.server(config={"password": "secret"}) as port:
            for path in (ROOT, ROOT + "/serial", "/api/v1/metadata"):
                self.assertEqual(self.request(port, path=path)[0], 200)
            self.assertTrue(self.request(port, path="/api/v1/metadata")[1]["write_authentication_required"])
            self.audit.write_text("")
            for method, path, body in (("PUT", ROOT, '{"serial":"x"}'),
                                       ("PUT", ROOT + "/serial", '{"value":"x"}'),
                                       ("DELETE", ROOT + "/serial", None)):
                status, _, headers = self.request(port, method, path, body)
                self.assertEqual(status, 401)
                self.assertIn("Basic", headers["WWW-Authenticate"])
            self.assertEqual(self.operations(), [])
            status, _, headers = self.request(port, "PUT", ROOT, '{"serial":"x"}', requested_with="XMLHttpRequest")
            self.assertEqual(status, 401)
            self.assertNotIn("WWW-Authenticate", headers)
            for path, kind in (("/", "text/html"), ("/app.js", "text/javascript"),
                               ("/app.css", "text/css"), ("/bootstrap.min.css", "text/css"), ("/openapi.yaml", "application/yaml")):
                connection = http.client.HTTPConnection("localhost", port, timeout=3)
                connection.request("GET", path)
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                self.assertTrue(response.getheader("Content-Type").startswith(kind))
                self.assertTrue(response.read())
                connection.close()
            try:
                self.authorization = "Basic " + base64.b64encode(b"admin:wrong").decode()
                self.assertEqual(self.request(port)[0], 200)
                self.assertEqual(self.request(port, "PUT", ROOT, '{"serial":"x"}')[0], 401)
                self.authorization = "Basic " + base64.b64encode(b"admin:secret").decode()
                self.assertEqual(self.request(port)[0], 200)
                self.assertEqual(self.request(port, "PUT", ROOT, '{"serial":"ok"}')[0], 204)
            finally:
                self.authorization = None

    def test_https_without_client_certificate(self):
        config = self.tls_config()
        del config['client_ca_file']
        with self.server(config={"tls": config}) as port:
            self.assertEqual(self.request(port, context=self.context(client=None))[0], 200)
        certificate = self.directory / 'selfsigned.pem'
        key = self.directory / 'selfsigned.key'
        subprocess.run([OPENSSL, 'req', '-x509', '-newkey', 'ec', '-pkeyopt',
                        'ec_paramgen_curve:P-256', '-nodes', '-days', '3', '-subj', '/CN=localhost',
                        '-addext', 'subjectAltName=DNS:localhost', '-keyout', str(key), '-out', str(certificate)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        config.update(certificate_file=str(certificate), private_key_file=str(key))
        with self.server(config={"tls": config}) as port:
            context = ssl.create_default_context(cafile=str(certificate))
            self.assertEqual(self.request(port, context=context)[0], 200)

    def test_invalid_configuration(self):
        for config in ('{', '{} extra', 'null', '[]', 'Port=8080',
                       '[bdd]\nPort=0', '[bdd]\nPort=65536', '[bdd]\nPort=true',
                       '[bdd]\nPort=8080junk', '[bdd]\nListenAddress=localhost',
                       '[bdd]\nUnknown=1', '[bdd]\nWritableKey=bad=key',
                       '[bdd]\nPassword=plaintext', '[bdd]\nPassword=$unknown$invalid',
                       '[bdd]\nCertificateFile=/missing', '[bdd]\nClientCAFile=/missing',
                       '[Other]\nPort=8080'):
            with self.subTest(config=config):
                self.assert_start_fails(config)
        result = subprocess.run([SERVER, "--config", str(self.path / "missing")], capture_output=True)
        self.assertNotEqual(result.returncode, 0)

    def test_invalid_tls_credentials_prevent_start(self):
        broken = self.path / "broken.pem"
        broken.write_text("not a certificate")
        empty = self.path / "empty.pem"
        empty.touch()
        for field, path in (
            ("private_key_file", self.directory / "client.key"),
            ("private_key_file", self.directory / "missing.key"),
            ("certificate_file", broken),
            ("client_ca_file", broken),
            ("client_ca_file", empty),
            ("client_ca_file", self.directory / "missing.pem"),
        ):
            config = self.tls_config()
            config[field] = str(path)
            self.assert_start_fails({"port": free_port(), "tls": config})
        config = self.tls_config()
        config.update(certificate_file=str(self.directory / "expired-server.pem"),
                      private_key_file=str(self.directory / "expired-server.key"))
        self.assert_start_fails({"port": free_port(), "tls": config})

    def test_disabled_tls_and_missing_fields_use_defaults(self):
        with self.server(config={"tls": {"enabled": False}}) as port:
            self.assertEqual(self.request(port)[0], 200)
        # Only override the port, leaving address and whitelist at their defaults.
        file = self.path / "defaults.conf"
        port = free_port()
        file.write_text(configuration_text({"port": port}))
        process = subprocess.Popen([SERVER, "--config", str(file)], stderr=subprocess.PIPE)
        try:
            for _ in range(100):
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                        break
                except OSError:
                    time.sleep(0.01)
            self.assertEqual(self.request(port)[0], 200)
            self.assertEqual(self.request(port, "PUT", ROOT + "/serial", '{"value":"x"}')[0], 403)
        finally:
            process.terminate()
            _, error = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 0, error)


if __name__ == "__main__":
    unittest.main(verbosity=2)

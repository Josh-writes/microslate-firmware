#!/usr/bin/env python3
"""Test-only HTTP-to-HTTPS forwarder for MicroSlate firmware test builds.

Some networks block Google and Dropbox. A firmware built with -DMS_TEST_PROXY sends those requests here as plain HTTP
on the local network (http://<this machine>:<port>/<host>/<path>); this script repeats them over HTTPS from a machine
that can reach the services and hands the answer back unchanged. Redirects are not followed.

It is not a general proxy: only the hosts below are allowed, and every request must carry the key given on the
command line. Tokens and notes cross the local network unencrypted, so use it only at home, only while testing.

    python3 dev-proxy.py --key <random text> [--port 8899]
"""
import argparse
import http.client
import http.server
import socketserver
import sys

ALLOWED = {
    "oauth2.googleapis.com",
    "www.googleapis.com",
    "api.dropboxapi.com",
    "content.dropboxapi.com",
    "api.dropbox.com",
    "microslate-link.typeslate.workers.dev",   # the Dropbox sign-in relay
}
SKIP_REQUEST_HEADERS = {"host", "connection", "accept-encoding", "x-ms-proxy-key", "content-length", "transfer-encoding"}
SKIP_RESPONSE_HEADERS = {"connection", "transfer-encoding", "content-length", "content-encoding", "keep-alive"}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    key = ""

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def reply(self, status, body=b"", headers=()):
        self.send_response(status)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)
        self.close_connection = True

    def forward(self):
        if self.headers.get("X-MS-Proxy-Key", "") != self.key:
            return self.reply(403, b"bad key")
        parts = self.path.split("/", 2)   # ["", host, rest]
        host = parts[1] if len(parts) > 1 else ""
        rest = "/" + (parts[2] if len(parts) > 2 else "")
        if host not in ALLOWED:
            return self.reply(403, b"host not allowed")
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else None
        headers = {k: v for k, v in self.headers.items() if k.lower() not in SKIP_REQUEST_HEADERS}
        headers["Accept-Encoding"] = "identity"
        try:
            conn = http.client.HTTPSConnection(host, timeout=60)
            conn.request(self.command, rest, body=body, headers=headers)
            resp = conn.getresponse()
            data = resp.read()
            out = [(k, v) for k, v in resp.getheaders() if k.lower() not in SKIP_RESPONSE_HEADERS]
            conn.close()
        except Exception as e:   # noqa: BLE001 - report any failure to the device as a gateway error
            sys.stderr.write("upstream error: %r\n" % (e,))
            return self.reply(502, b"upstream error")
        if resp.status >= 400:   # show why a service refused, never the request itself
            body = data[:400].decode("utf-8", "replace").replace(chr(10), " ")
            sys.stderr.write("  -> %d %s" % (resp.status, body) + chr(10))
        self.reply(resp.status, data, out)

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = forward


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--key", required=True)
    ap.add_argument("--port", type=int, default=8899)
    a = ap.parse_args()
    Handler.key = a.key
    print("dev proxy on port %d, hosts: %s" % (a.port, ", ".join(sorted(ALLOWED))))
    Server(("0.0.0.0", a.port), Handler).serve_forever()

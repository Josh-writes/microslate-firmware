#!/usr/bin/env python3
"""Test-only stand-in for GitHub releases, for the firmware update (Settings > Wi-Fi > Firmware update).

A debug build made with -DMS_UPDATE_REPO=\\"http://<this machine>:<port>\\" asks this server instead of GitHub. It
answers as github.com does: /releases/latest redirects to /releases/tag/<tag>, and
/releases/download/<tag>/<file> redirects to the file (404 when the folder has no such file). Plain HTTP, so only
debug builds accept it.

    python3 update-test-server.py --tag v3.0.1 --dir <folder with microslate-<device>-v3.0.1.bin> [--port 8765]
"""
import argparse
import http.server
import os
import socketserver


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--port", type=int, default=8765)
    args = ap.parse_args()
    folder = os.path.abspath(args.dir)

    class Handler(http.server.BaseHTTPRequestHandler):
        def redirect(self, path):
            self.send_response(302)
            self.send_header("Location", f"http://{self.headers.get('Host')}{path}")
            self.send_header("Content-Length", "0")
            self.end_headers()

        def do_GET(self):
            parts = self.path.strip("/").split("/")
            if parts == ["releases", "latest"]:
                return self.redirect(f"/releases/tag/{args.tag}")
            if len(parts) == 4 and parts[:2] == ["releases", "download"] and parts[2] == args.tag:
                if os.path.isfile(os.path.join(folder, parts[3])):
                    return self.redirect(f"/files/{parts[3]}")
            if len(parts) == 2 and parts[0] == "files" and "/" not in parts[1] and ".." not in parts[1]:
                path = os.path.join(folder, parts[1])
                if os.path.isfile(path):
                    size = os.path.getsize(path)
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(size))
                    self.end_headers()
                    with open(path, "rb") as f:
                        while chunk := f.read(65536):
                            self.wfile.write(chunk)
                    return
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

    socketserver.ThreadingTCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(("", args.port), Handler) as srv:
        print(f"serving {args.tag} from {folder} on port {args.port}", flush=True)
        srv.serve_forever()


if __name__ == "__main__":
    main()

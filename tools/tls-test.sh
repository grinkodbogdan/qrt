#!/usr/bin/env bash
# tls-test.sh - run tests/test_tls.c against a local TLS 1.3 server
# (Python's ssl module = OpenSSL) with a throwaway self-signed certificate.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'kill $srv 2>/dev/null || true; rm -rf "$tmp"' EXIT
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$tmp/key.pem" -out "$tmp/cert.pem" -days 1 -subj /CN=localhost 2>/dev/null
port=${TLS_TEST_PORT:-18443}
python3 - "$tmp" "$port" <<'PY' &
import ssl, sys, http.server
d, port = sys.argv[1], int(sys.argv[2])
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = (b"0123456789abcdef" * 18750) if self.path == "/big" else b"<html><body><h1>QRT TLS test</h1></body></html>"
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ssl.TLSVersion.TLSv1_3
ctx.load_cert_chain(d + "/cert.pem", d + "/key.pem")
srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), H)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
srv.serve_forever()
PY
srv=$!
sleep 1
"$here/build/test_tls" 127.0.0.1 "$port"

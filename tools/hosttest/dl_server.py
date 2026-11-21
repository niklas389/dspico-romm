import http.server, ssl, sys, re
SIZE = 9 * 1024 * 1024 + 123
def data(a, b):  # deterministic content: byte i = (i*7 + i>>11) & 255
    return bytes(((i * 7) + (i >> 11)) & 255 for i in range(a, b))
CACHE = data(0, SIZE)
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    count = 0
    def log_message(self, *a): pass
    def do_GET(self):
        H.count += 1
        if self.headers.get("Authorization") != "Bearer TESTTOKEN":
            self.send_response(401); self.send_header("Content-Length", "5"); self.end_headers(); self.wfile.write(b"nope\n"); return
        if not self.path.startswith("/api/roms/1/content/"):
            self.send_response(404); self.send_header("Content-Length", "9"); self.end_headers(); self.wfile.write(b"not found"); return
        m = re.match(r"bytes=(\d+)-(\d+)", self.headers.get("Range", ""))
        a, b = int(m.group(1)), int(m.group(2)) + 1
        body = CACHE[a:b]
        self.send_response(206)
        self.send_header("Content-Range", "bytes %d-%d/%d" % (a, b - 1, SIZE))
        self.send_header("Content-Length", str(len(body)))
        if "close" in self.path: self.send_header("Connection", "close"); self.close_connection = True
        self.end_headers()
        for i in range(0, len(body), 3000):   # odd write sizes -> odd TLS records
            self.wfile.write(body[i:i+3000])
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain("cert.pem", "key.pem")
ctx.options &= ~ssl.OP_CIPHER_SERVER_PREFERENCE  # honour the client order (ChaCha20 first)
ctx.num_tickets = 2
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 8443), H)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
srv.serve_forever()

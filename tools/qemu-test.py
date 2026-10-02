#!/usr/bin/env python3
"""Headless smoke test: boot build/qrt.img under OVMF, drive the UI with
synthetic touch (USB tablet = absolute pointer) and keyboard events over
QMP, and save screenshots to build/shots/.

usage: tools/qemu-test.py [ia32|x64] [--keep]    (exit status 0 = pass)
"""
import json, os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARCH = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "ia32"
SHOTS = os.path.join(ROOT, "build", "shots")
W, H = 1280, 800


class QMP:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        self.f = self.s.makefile("rw")
        json.loads(self.f.readline())
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                if "error" in r:
                    raise RuntimeError(r["error"])
                return r["return"]

    # --- input helpers -------------------------------------------------
    # Stock OVMF has no pointer drivers, so touches are injected through the
    # kernel's serial test channel (see inject_key() in src/kernel/hal.c).
    serial = None

    def _pkt(self, kind, x, y):
        self.serial.sendall(f"\x14{kind}{int(x)},{int(y)};".encode())

    def move(self, x, y):
        self._pkt("m", x, y)

    def tap(self, x, y, settle=1.2):
        self._pkt("d", x, y); time.sleep(0.15)
        self._pkt("u", x, y); time.sleep(settle)

    def drag(self, pts, settle=1.0):
        self._pkt("d", *pts[0]); time.sleep(0.1)
        for p in pts[1:]:
            self._pkt("m", *p); time.sleep(0.05)
        self._pkt("u", *pts[-1]); time.sleep(settle)

    # Keys go over the serial console: the firmware's terminal driver and QRT's
    # native UART driver both read it, while native mode has no USB keyboard yet.
    KEYMAP = {"ret": "\r", "esc": "\x1b", "backspace": "\x08", "spc": " ", "tab": "\t",
              "up": "\x1b[A", "down": "\x1b[B", "right": "\x1b[C", "left": "\x1b[D",
              # the tablet's buttons, as F9/F10/F11 on the serial console
              "volup": "\x1b[20~", "voldown": "\x1b[21~", "power": "\x1b[23~",
              # F12: power held for a second; F8: the Windows button
              "powerlong": "\x1b[24~", "win": "\x1b[19~"}

    def keys(self, *names, settle=0.6):
        for n in names:
            self.serial.sendall(self.KEYMAP.get(n, n).encode())
            time.sleep(0.25 if n == "esc" else 0.12)
        time.sleep(settle)

    def shot(self, name):
        os.makedirs(SHOTS, exist_ok=True)
        path = os.path.join(SHOTS, f"{ARCH}-{name}.png")
        self.cmd("screendump", filename=path, format="png")
        return path


TEST_PAGE = """<!doctype html><html><head><title>QRT browser test</title><style>p{color:red}</style>
<script>document.write('<p>scripts must not show</p>')</script></head><body>
<p><a href="/page2.html">Next page (a link)</a></p>
<h1>Hello from the host</h1>
<p>This page came over <b>%s</b> from QEMU's host. Caf&eacute; &mdash; &ldquo;quotes&rdquo; &amp; entities.</p>
<ul><li>First item</li><li>Second item with <a href="https://example.com/">a link</a></li></ul>
<form action="/search"><input name="q" value="qrt"><button>Search</button></form>
<pre>  preformatted
    text</pre><hr><p><small>small print</small></p></body></html>"""


def start_web_fixtures():
    """HTTP on 18080 and HTTPS (TLS 1.3, throwaway certificate) on 18443 for
    the browser; the guest reaches them at 10.0.2.2."""
    import http.server, ssl, tempfile, threading, urllib.parse

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            u = urllib.parse.urlparse(self.path)
            scheme = "https (TLS 1.3)" if isinstance(self.connection, ssl.SSLSocket) else "plain http"
            if u.path == "/page2.html":
                body = "<title>Page two</title><h2>Page two</h2><p>The link worked.</p>"
            elif u.path == "/search":
                q = urllib.parse.parse_qs(u.query).get("q", [""])[0]
                body = "<title>Results</title><h2>Search results</h2><p>You searched for <b>%s</b>.</p>" % q
            elif u.path == "/moved":
                self.send_response(302); self.send_header("Location", "/page2.html"); self.end_headers(); return
            else:
                body = TEST_PAGE % scheme
            data = body.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            if u.path == "/page2.html":
                self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
                for i in range(0, len(data), 20):
                    self.wfile.write(b"%x\r\n%s\r\n" % (len(data[i:i + 20]), data[i:i + 20]))
                self.wfile.write(b"0\r\n\r\n")
                return
            self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)
        def log_message(self, *a): pass

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 18080), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        d = tempfile.mkdtemp()
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", d + "/k.pem", "-out", d + "/c.pem",
                        "-days", "1", "-subj", "/CN=10.0.2.2"], check=True, capture_output=True)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        ctx.load_cert_chain(d + "/c.pem", d + "/k.pem")
        tsrv = http.server.ThreadingHTTPServer(("0.0.0.0", 18443), H)
        tsrv.socket = ctx.wrap_socket(tsrv.socket, server_side=True)
        threading.Thread(target=tsrv.serve_forever, daemon=True).start()
    except Exception as e:  # noqa
        print("https fixture unavailable:", e)


def main():
    start_web_fixtures()
    sock = os.path.join(ROOT, "build", f"qmp-{ARCH}.sock")
    serial = os.path.join(ROOT, "build", f"serial-{ARCH}.log")
    for p in (sock, serial, serial + ".sock", os.path.join(ROOT, "build", f"vars-{ARCH}.fd")):
        if os.path.exists(p):
            os.remove(p)
    qemu = subprocess.Popen([os.path.join(ROOT, "tools", "run-qemu.sh"), ARCH,
                             "-display", "none", "-qmp", f"unix:{sock},server,nowait",
                             "-chardev", f"socket,id=ser0,path={serial}.sock,server=on,wait=off",
                             "-serial", "chardev:ser0"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    ok = True
    try:
        q = QMP(sock)
        q.serial = socket.socket(socket.AF_UNIX)
        q.serial.connect(serial + ".sock")
        def pump():
            with open(serial, "wb") as out:
                while True:
                    try:
                        d = q.serial.recv(4096)
                    except OSError:
                        return
                    if not d:
                        return
                    out.write(d); out.flush()
        import threading
        threading.Thread(target=pump, daemon=True).start()
        # Wait for the kernel to reach the shell (its boot log goes to serial too).
        deadline = time.time() + 180
        log = ""
        while time.time() < deadline:
            log = open(serial, errors="replace").read() if os.path.exists(serial) else ""
            if "storage:" in log and "mounted volumes" in log:
                break
            time.sleep(0.5)
        else:
            print("FAIL: kernel never reached the shell\n" + log[-2000:])
            return 1
        # then the kernel decides on native mode (a key typed before that would count as
        # "held at boot") - with a big image, copying it into RAM takes a while under TCG
        while time.time() < deadline + 300:
            log = open(serial, errors="replace").read()
            if "shell: first frame" in log or "firmware mode" in log:
                break
            time.sleep(0.5)
        time.sleep(4)   # splash animation + first frame under TCG
        print("--- kernel log (serial) ---")
        print("\n".join(l for l in log.replace("\r", "").splitlines() if l.strip() and "\x1b" not in l)[-1500:])
        shots = [q.shot("01-home")]

        script = os.environ.get("QRT_TEST_SCRIPT")
        if script:
            exec(open(script).read(), {"q": q, "shots": shots, "time": time})
        else:
            default_script(q, shots)
        print("screenshots:\n  " + "\n  ".join(shots))
    except Exception as e:  # noqa
        ok = False
        print("FAIL:", e)
        if qemu.poll() is not None:
            print(qemu.stderr.read().decode(errors="replace")[-2000:])
    finally:
        if "--keep" not in sys.argv:
            qemu.terminate()
            try:
                qemu.wait(5)
            except subprocess.TimeoutExpired:
                qemu.kill()
    return 0 if ok else 1


def pad_key(label, s=800 / 680):
    """Centre of a key of the desk-mode controller's keyboard (letters layer), which
    spans the whole 1280x800 panel - the geometry of key_rect() in src/ui/osk.c."""
    dpv = lambda v: int(v * s + 0.5)
    rows = [[(c, 1) for c in "qwertyuiop"], [(c, 1) for c in "asdfghjkl"],
            [("shift", 1.5)] + [(c, 1) for c in "zxcvbnm"] + [("back", 1.5)],
            [("?123", 1.5), ("-", 1), ("/", 1), ("space", 4), (".", 1), ("enter", 1.5), ("hide", 1)]]
    pad, gap, kh = dpv(8), dpv(6), dpv(48)
    top = H - dpv(4 * 54 + 16)
    maxw = min(W - 2 * pad, dpv(820))
    x0 = (W - maxw) // 2
    unit = (maxw - gap * 10) / max(sum(w for _, w in r) for r in rows)
    for ri, r in enumerate(rows):
        for ki, (l, w) in enumerate(r):
            if l == label:
                row_w = sum(ww for _, ww in r) * unit + gap * (len(r) - 1)
                x = x0 + (maxw - row_w) / 2 + sum(ww * unit + gap for _, ww in r[:ki])
                return int(x + w * unit / 2), top + pad + ri * (kh + gap) + kh // 2
    raise KeyError(label)


def native_test(q, shots):
    """Native QRT programs (built with the SDK: musl on QRT's own system calls): the
    C test, the Rust one, and a window - tapped, typed into, drawn on, then closed."""
    serial = os.path.join(ROOT, "build", f"serial-{ARCH}.log")
    slog = lambda: open(serial, errors="replace").read()
    progs = ["native-test", "cxx-test"] + (["rust-hello"] if os.path.exists(os.path.join(ROOT, "build", "rootfs", "bin", "rust-hello")) else [])
    for cmd in progs:
        q.keys(*cmd, settle=0.2)
        q.keys("ret", settle=8)
    for cmd in progs:
        if not any(f"proc: {cmd} (pid" in l and "exited with 0," in l for l in slog().splitlines()):
            raise RuntimeError(f"native: {cmd} did not exit cleanly\n  " + "\n  ".join(l for l in slog().splitlines() if "native" in l or "rust" in l)[-1200:])
    if os.path.exists(os.path.join(ROOT, "build", "rootfs", "bin", "js")):
        # Ladybird's JavaScript engine (ports/ladybird/build.sh), its output into the kernel log
        q.keys(*"js /share/tests/js-test.js >/dev/kmsg 2>&1; echo js-exit=$? >/dev/kmsg", settle=0.2)
        q.keys("ret", settle=1)
        for _ in range(240):
            if "js-exit=" in slog():
                break
            time.sleep(1)
        if "js: \"js: ok\"" not in slog() or "js-exit=0" not in slog():
            raise RuntimeError("native: Ladybird's js failed\n  " + "\n  ".join(l for l in slog().splitlines() if l.startswith("js") or "proc: js" in l)[-1500:])
        progs.append("Ladybird's js (%d checks)" % (slog().count(": ok\"") - 1))
    if os.path.exists(os.path.join(ROOT, "build", "rootfs", "bin", "ladybird")):
        # Ladybird (L4): tests/ladybird/page.html rendered headless by the browser and its
        # helper processes; the PNG comes back through the kernel log (tools/kmsg-png.py)
        q.keys(*"sh /share/tests/render.sh", settle=0.2)
        q.keys("ret", settle=1)
        for _ in range(600):
            if "lb-done" in slog():
                break
            time.sleep(1)
        png = os.path.join(SHOTS, f"{ARCH}-06-ladybird-page.png")
        os.makedirs(SHOTS, exist_ok=True)
        ok = "lb-exit=0" in slog() and subprocess.run([sys.executable, os.path.join(ROOT, "tools", "kmsg-png.py"), serial, png]).returncode == 0
        if ok:
            from PIL import Image
            im = Image.open(png).convert("RGB")
            ok = im.size == (800, 600) and len(im.resize((40, 30)).getcolors(1200) or []) > 20   # not a blank page
        if not ok:
            raise RuntimeError("native: Ladybird did not render the test page\n  " + "\n  ".join(l for l in slog().splitlines() if "ladybird" in l)[-1500:])
        shots.append(png)
        progs.append("Ladybird (a page rendered by its processes)")
        # Ladybird as the shell's browser (L6): the dock's Browser opens its window; a URL
        # typed into its address bar loads and paints; the title reaches the shell
        q.tap(1226, 246, settle=2)
        for _ in range(120):
            if "ladybird (pid" in slog() and "opened window" in slog().split("browser: started Ladybird")[-1]:
                break
            time.sleep(1)
        else:
            raise RuntimeError("native: Ladybird's window did not open\n  " + "\n  ".join(l for l in slog().splitlines() if "ladybird" in l or "browser:" in l)[-1200:])
        time.sleep(20)
        q.tap(700, 120, settle=1)                    # its address bar
        q.keys(*"file:///share/tests/page.html", settle=0.3)
        q.keys("ret", settle=40)
        shot = q.shot("07-ladybird-window")
        shots.append(shot)
        from PIL import Image
        im = Image.open(shot).convert("RGB")
        header = im.getpixel((700, 170))             # the test page's blue-purple header band
        if not (header[2] > 150 and header[0] < 160):
            raise RuntimeError("native: Ladybird's window did not show the page (pixel %s)" % (header,))
        q.tap(1141, 66, settle=4)                    # close it: QRT_EV_CLOSE, Ladybird exits
        q.tap(1226, 170, settle=2)                   # back to the Terminal
        progs.append("Ladybird in the shell (dock, address bar, a page)")
    q.keys(*"hello-window", settle=0.2)
    q.keys("ret", settle=1)
    for _ in range(40):
        if "opened window" in slog():
            break
        time.sleep(0.5)
    time.sleep(1.5)
    q.tap(122, 368, settle=0.6)                    # the program's "Tap me" button
    q.keys("h", "i", settle=0.8)
    q.drag([(200 + i * 40, 600 + (i % 4) * 25) for i in range(15)], settle=1)
    shots.append(q.shot("05-native-window"))
    q.tap(1141, 66, settle=2.5)                    # close: the program gets QRT_EV_CLOSE and exits
    if not any("proc: hello-window (pid" in l and "exited with 0," in l for l in slog().splitlines()):
        raise RuntimeError("native: hello-window did not close cleanly")
    print("native: %s and a window (tap, keys, drawing, close) work" % " and ".join(progs))
    q.tap(1226, 170, settle=1.5)                   # back to the Terminal


def desk_test(q, shots, dock):
    """A USB mouse (QEMU's usb-tablet, an absolute pointer) clicks a dock icon; then a
    virtual monitor is plugged in (QEMU has no DisplayPort): the shell moves to it and
    the panel becomes the controller.  Its Apps button and keyboard open Clock on the
    monitor; the power menu brings the shell back - Clock must then be on the panel."""
    def mouse_to(x, y):
        q.cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
                                          {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}}])
    def mouse_button(down):
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])
    for i in range(10):
        mouse_to(300 + i * 92, 600 - i * 50); time.sleep(0.05)
    mouse_to(1226, dock["files"]); time.sleep(0.5)
    mouse_button(True); time.sleep(0.15); mouse_button(False); time.sleep(1.5)
    shots.append(q.shot("21-mouse-files"))
    q.keys("esc", settle=1.0)
    # plug a 1280x720 monitor: the panel shows the touchpad and keyboard
    q.serial.sendall(b"\x14v1280,720;"); time.sleep(2.0)
    shots.append(q.shot("22-desk-controller"))
    q.tap(1058, 483, settle=1.0)                    # Apps: the launcher, on the monitor
    for ch in "clock":
        q.tap(*pad_key(ch), settle=0.3)
    q.tap(*pad_key("enter"), settle=1.5)
    q.drag([(400, 250)] + [(400 + i * 30, 250 + i * 8) for i in range(1, 10)], settle=0.5)   # the touchpad
    q.tap(600, 300, settle=0.8)                     # tap: a click on the monitor
    q.keys("powerlong", settle=1.0)
    shots.append(q.shot("22-desk-power"))
    q.tap(640, 261, settle=2.0)                     # "Mirror to the external screen"
    shots.append(q.shot("22-desk-mirrored"))
    q.serial.sendall(b"\x14v0,0;"); time.sleep(1.0)   # unplug
    log = open(os.path.join(ROOT, "build", f"serial-{ARCH}.log"), errors="replace").read()
    for want in ("absolute pointer ready", "shell: controlling the external screen", "shell: back on the tablet, mirrored"):
        if want not in log:
            raise RuntimeError(f"desk test: no '{want}' in the log")
    print("desk: mouse, virtual monitor, controller and power menu work")
    q.keys("esc", settle=1.0)


def default_script(q, shots):
    """Walk the shell and every app with touch, the on-screen keyboard and the
    serial keyboard.  Coordinates are for the 1280x800 landscape layout the
    QEMU VGA device reports (UI scale 1.18: the dock floats at x 1181-1271,
    hanging from y 47; the keyboard is centred in the 1172 px content area;
    app windows have a 57 px header bar from y 38, the app area from y 95)."""
    # pinned apps; the launcher button follows them while only they are in the dock
    dock = {"files": 94, "terminal": 170, "browser": 246, "wifi": 322, "sketch": 398, "settings": 474, "launcher": 564}

    def dock_tap(name, settle=1.2):
        q.tap(1226, dock[name], settle=settle)

    def launch(name, settle=1.5):
        """Type on the home screen (opens the launcher search), Enter opens the first hit."""
        q.keys("esc", settle=0.8)
        q.keys(*list(name), settle=0.3)
        q.keys("ret", settle=settle)

    # Launcher from the dock, then search with the on-screen keyboard
    dock_tap("launcher")
    shots.append(q.shot("02-launcher"))
    q.tap(631, 90)                         # search field -> keyboard
    for x, y in [(541, 563), (364, 563), (453, 563)]:   # t, e, r
        q.tap(x, y, settle=0.3)
    shots.append(q.shot("03-launcher-search"))
    q.tap(904, 752, settle=1.5)            # Enter -> Terminal
    # Terminal: type "ls /bin" on the on-screen keyboard, then run programs over serial
    q.tap(500, 400)                        # the console -> keyboard
    for x, y in [(939, 627), (320, 627), (585, 752), (375, 752), (674, 690), (806, 563), (761, 690)]:
        q.tap(x, y, settle=0.3)            # l s space / b i n
    q.tap(904, 752, settle=5)              # Enter
    shots.append(q.shot("04-terminal-osk"))
    q.tap(1011, 752, settle=1)             # hide the keyboard
    for cmd in ["hello", "uname -a"]:
        q.keys(*[c if c != " " else "spc" for c in cmd], settle=0.2)
        q.keys("ret", settle=6.0)
    shots.append(q.shot("05-terminal"))
    # networking (x64: QEMU's e1000e + user-mode network; DHCP ran at boot)
    for cmd, wait in [("clear", 1), ("ifconfig", 2), ("ping -c 2 10.0.2.2", 5), ("nslookup localhost 10.0.2.3", 5)]:
        q.keys(*[c if c != " " else "spc" for c in cmd], settle=0.2)
        q.keys("ret", settle=wait)
    shots.append(q.shot("05-terminal-net"))
    # dynamically linked glibc programs: ld.so + libc/libm/libstdc++, threads (clone + futex)
    if ARCH == "x64":
        for cmd, wait in [("clear", 1), ("dynhello", 6), ("threads", 15), ("cxx", 10), ("procs", 15), ("signals", 15), ("memory", 15), ("events", 15), ("ls /bin | wc -l", 6)]:
            q.keys(*[c if c != " " else "spc" for c in cmd], settle=0.2)
            q.keys("ret", settle=wait)
        shots.append(q.shot("05-terminal-dynamic"))
        slog = open(os.path.join(ROOT, "build", f"serial-{ARCH}.log"), errors="replace").read()
        for prog in ["dynhello", "threads", "cxx", "procs", "signals", "memory", "events"]:
            if f"proc: {prog} (pid" not in slog or not any(f"proc: {prog} (pid" in l and "exited with 0," in l for l in slog.splitlines()):
                lines = [l for l in slog.splitlines() if prog in l or "linux:" in l][-14:]
                print(f"FAIL: {prog} did not exit cleanly\n  " + "\n  ".join(lines))
                sys.exit(1)
        print("linux: dynhello, threads, cxx, procs, signals, memory and events ran and exited with 0")
        native_test(q, shots)
        # the USB keyboard through QRT's own xHCI driver: type "hello" + Enter
        for k in ["h", "e", "l", "l", "o", "ret"]:
            q.cmd("send-key", keys=[{"type": "qcode", "data": k}])
            time.sleep(0.15)
        time.sleep(4)
        slog = open(os.path.join(ROOT, "build", f"serial-{ARCH}.log"), errors="replace").read()
        if slog.count("proc: hello (pid") < 2:
            print("FAIL: typing on the USB keyboard did not run hello\n  " + "\n  ".join(l for l in slog.splitlines() if "usb:" in l))
            sys.exit(1)
        shots.append(q.shot("05-terminal-usbkbd"))
        print("usb: the USB keyboard typed a command")
    # Sketch from the dock: draw a stroke, change ink, draw another
    dock_tap("sketch")
    q.drag([(200 + i * 40, 400 + (i % 5) * 30) for i in range(20)])
    q.tap(98, 109)                         # second ink swatch
    q.drag([(300 + i * 30, 600 - i * 12) for i in range(25)])
    shots.append(q.shot("06-sketch"))
    # Files from the dock: the volume, the qrt folder, welcome.txt
    dock_tap("files")
    row = lambda i: 151 + i * 61 + 30      # list rows: 61 px from y 151
    q.tap(400, row(0))                     # the QRT volume
    # firmware volume: "..", bin, EFI, lib, lib64, qrt; native RAM root: "..", bin, dev, EFI, etc, lib, lib64, proc, qrt, tmp
    q.tap(400, row(8) if ARCH == "x64" else row(5))
    shots.append(q.shot("07-files-dir"))
    q.tap(400, row(2))                     # rows: "..", hwdump, welcome.txt
    shots.append(q.shot("07-files-text"))
    q.keys("backspace")
    # Browser: start page, then the host's test pages over http and https (x64: native network)
    dock_tap("browser", settle=1.5)
    shots.append(q.shot("19-browser-home"))
    if ARCH == "x64":
        def open_url(u, wait=6):
            q.tap(600, 123, settle=0.8)                  # address bar -> keyboard
            q.keys(*[c for c in u], settle=0.2)
            q.keys("ret", settle=wait)
        open_url("10.0.2.2:18080/")
        shots.append(q.shot("19-browser-http"))
        q.tap(80, 214, settle=5)                        # "Next page (a link)": chunked reply
        shots.append(q.shot("19-browser-link"))
        open_url("https://10.0.2.2:18443/", wait=10)
        shots.append(q.shot("19-browser-https"))
        open_url("http://10.0.2.2:18080/moved")         # a redirect
        open_url("10.0.2.2:18080/search?q=from+the+bar")
        shots.append(q.shot("19-browser-form"))
        q.keys("backspace", settle=5)                   # back
    # Wi-Fi (no Intel 8260 in QEMU: the app must say so cleanly)
    dock_tap("wifi")
    shots.append(q.shot("08-wifi"))
    # System Monitor (and its Hardware tab), Clock through the launcher search
    launch("monitor")
    shots.append(q.shot("09-system"))
    q.tap(586, 125, settle=1.5)            # Hardware tab
    shots.append(q.shot("09-system-hw"))
    launch("clock")
    shots.append(q.shot("10-clock"))
    launch("button")
    shots.append(q.shot("10-button-test"))
    # open apps: swipe up on the home screen, close one with its x, one by swiping it up
    q.keys("esc", settle=1.0)
    q.drag([(500, 650)] + [(500, 650 - i * 30) for i in range(1, 10)], settle=1.5)
    shots.append(q.shot("11-overview"))
    q.tap(268, 133, settle=1.0)            # x on the first window (Files; 7 windows -> 4 columns)
    q.drag([(205, 300)] + [(205, 300 - i * 25) for i in range(1, 10)], settle=1.2)   # swipe the new first window (Terminal) up
    shots.append(q.shot("12-overview-closed"))
    q.tap(600, 760, settle=1.0)            # empty space: back home
    # hardware buttons: volume = mock audio indicator, Windows = launcher,
    # power = lock (again: sleep, again: wake), power held = power menu
    q.keys("esc", settle=0.8)
    q.keys("volup", "volup", "volup", settle=0.4)
    shots.append(q.shot("13-button-volume"))
    q.keys("win", settle=1.0)
    shots.append(q.shot("13-button-launcher"))
    q.keys("win", settle=1.0)
    q.keys("power", settle=1.0)
    shots.append(q.shot("13-lock"))
    q.keys("power", settle=1.0)
    shots.append(q.shot("13-asleep"))
    q.keys("power", settle=1.0)            # wakes to the lock screen
    q.keys("powerlong", settle=1.0)
    shots.append(q.shot("13-button-power"))
    q.keys("esc", settle=0.8)
    q.drag([(640, 700)] + [(640, 700 - i * 30) for i in range(1, 10)], settle=1.2)   # swipe up to unlock
    shots.append(q.shot("13-home"))
    # the dock: drag it to the bottom edge (screenshot mid-drag), then left, then back right
    q._pkt("d", 1226, 300); time.sleep(0.1)
    for i in range(1, 11):
        q.move(1226 - i * 60, 300 + i * 45); time.sleep(0.05)
    time.sleep(1.0)
    shots.append(q.shot("18-dock-dragging"))
    q._pkt("u", 626, 750); time.sleep(1.2)
    shots.append(q.shot("18-dock-bottom"))
    q.drag([(320, 746)] + [(320 - i * 30, 746 - i * 35) for i in range(1, 10)], settle=1.2)
    shots.append(q.shot("18-dock-left"))
    q.drag([(45, 300)] + [(45 + i * 125, 300 + i * 5) for i in range(1, 10)], settle=1.2)
    shots.append(q.shot("18-dock-right"))
    if ARCH == "x64":
        desk_test(q, shots, dock)
    # Settings: rotate to portrait
    dock_tap("settings")
    shots.append(q.shot("14-settings"))
    q.drag([(600, 600)] + [(600, 600 - i * 40) for i in range(1, 10)], settle=2.5)   # the flick coasts to a stop
    shots.append(q.shot("14-settings-more"))
    # scrolling shifts the pixels already on screen and draws only the new strip:
    # a full redraw of the same state (power menu opened and closed) must match it.
    # Native kernel only: the firmware's serial terminal splits the F12 sequence.
    if ARCH == "x64":
        q.keys("powerlong", settle=1.0)
        q.keys("esc", settle=1.0)
        redrawn = q.shot("14-settings-redrawn")
        try:
            from PIL import Image, ImageChops
            a_img = Image.open(shots[-1]).convert("RGB").crop((0, 40, 1170, 800))     # below the clock
            b_img = Image.open(redrawn).convert("RGB").crop((0, 40, 1170, 800))
            box = ImageChops.difference(a_img, b_img).getbbox()
            if box:
                print(f"FAIL: scrolled picture differs from a full redraw in {box}")
                sys.exit(1)
            print("scroll: shifted picture matches a full redraw")
        except ImportError:
            pass
    q.drag([(600, 200)] + [(600, 200 + i * 60) for i in range(1, 10)], settle=1.0)
    q.tap(744, 257)                        # rotation 90 deg -> 800x1280 portrait canvas
    time.sleep(2)
    shots.append(q.shot("15-settings-portrait"))
    q.keys("esc", settle=2)
    shots.append(q.shot("16-home-portrait"))
    q.keys("c", "l", "o", "c", "k", "ret", settle=2)
    shots.append(q.shot("17-clock-portrait"))
    q.keys("esc", settle=2)
    q.keys(*"bluetooth", "ret", settle=2)          # QEMU has no Bluetooth controller: the panel says so
    shots.append(q.shot("20-bluetooth"))


if __name__ == "__main__":
    sys.exit(main())

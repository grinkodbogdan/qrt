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
              "up": "\x1b[A", "down": "\x1b[B", "right": "\x1b[C", "left": "\x1b[D"}

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


def main():
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


def default_script(q, shots):
    """Walk every app with touch + keyboard.  Coordinates are for the
    1280x800 landscape layout the QEMU VGA device reports."""
    back = (38, 64)
    # Sketch: open, draw a stroke, change ink, draw another
    q.tap(1100, 135)
    q.drag([(200 + i * 40, 400 + (i % 5) * 30) for i in range(20)])
    q.tap(90, 124)                       # second ink swatch
    q.drag([(300 + i * 30, 600 - i * 12) for i in range(25)])
    shots.append(q.shot("02-sketch"))
    q.tap(*back)
    # Files: open the volume, then the qrt folder, then welcome.txt
    q.tap(780, 290)
    q.tap(400, 200)                       # row 0: the QRT volume
    q.tap(400, 350)                       # rows: "..", EFI, qrt -> open qrt
    shots.append(q.shot("03-files-dir"))
    q.tap(400, 350)                       # rows: "..", hwdump, welcome.txt
    shots.append(q.shot("03-files-text"))
    q.keys("backspace")
    # System info
    q.tap(*back)
    q.tap(1100, 290)
    shots.append(q.shot("04-system"))
    q.tap(*back)
    # Clock
    q.tap(780, 135)
    shots.append(q.shot("05-clock"))
    q.keys("esc")
    # Ask bar with the keyboard: type "li", Enter -> Life
    q.keys("l", "i", settle=1.0)
    shots.append(q.shot("06-ask"))
    q.keys("ret", settle=3.0)
    shots.append(q.shot("07-life"))
    q.keys("esc")
    # Touch Lab: probe + "Go native" (no LPSS I2C in QEMU, so it must refuse cleanly)
    q.tap(780, 600)
    q.tap(640, 141, settle=2)
    shots.append(q.shot("12-touchlab"))
    q.keys("esc")
    # Settings: rotate to portrait, pick an accent
    q.tap(780, 445)
    shots.append(q.shot("08-settings"))
    q.tap(488, 305)                       # rotation 90 deg -> 800x1280 portrait canvas
    time.sleep(2)
    shots.append(q.shot("09-settings-portrait"))
    q.keys("esc", settle=2)
    shots.append(q.shot("10-home-portrait"))
    q.keys("l", "i", "f", "e", "ret", settle=2)
    shots.append(q.shot("11-life-portrait"))


if __name__ == "__main__":
    sys.exit(main())

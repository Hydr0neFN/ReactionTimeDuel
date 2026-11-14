#!/usr/bin/env python3
"""Reaction Time Duel build / flash / test wrapper.

  python tools/rtd.py test                  run the native core tests
  python tools/rtd.py build [target ...]    build firmware (default: all)
  python tools/rtd.py flash <target> [--port COM5]
  python tools/rtd.py audio [--port COM5]   upload the host MP3 file system
  python tools/rtd.py dist                  build all and copy the OTA images to dist/
  python tools/rtd.py debug [--watch SEC]   read the host debug page data (PC on Wi-Fi RTD-Host)
  python tools/rtd.py monitor [--port COM5] [--seconds N]   record the host serial port
  python tools/rtd.py ports                 list the serial ports

Targets: host, display, stick (the OTA image), stick1 .. stick4 (USB images
that also write the stick ID).

Standard library only. PlatformIO (`pio`) must be on PATH.
`debug` and `monitor` write their output to logs/ as well (see DESIGN.md 11).
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# target -> (project directory, PlatformIO environment or None for the default)
TARGETS = {
    "host": ("firmware/host", None),
    "display": ("firmware/display", None),
    "stick": ("firmware/joystick", "stick"),
    "stick1": ("firmware/joystick", "stick1"),
    "stick2": ("firmware/joystick", "stick2"),
    "stick3": ("firmware/joystick", "stick3"),
    "stick4": ("firmware/joystick", "stick4"),
}
ALL = ["host", "display", "stick"]

# OTA image name in dist/ -> target
DIST = {"host.bin": "host", "display.bin": "display", "joystick.bin": "stick"}


def pio_env():
    """PlatformIO's ESP32 tool setup refuses to run under MSYS / Git Bash."""
    env = dict(os.environ)
    env.pop("MSYSTEM", None)
    return env


def pio(args):
    if shutil.which("pio") is None:
        sys.exit("pio is not on PATH. Install PlatformIO Core first.")
    print("> pio " + " ".join(args), flush=True)
    return subprocess.call(["pio"] + args, cwd=ROOT, env=pio_env())


def run_args(target, extra=()):
    if target not in TARGETS:
        sys.exit("unknown target '%s' (use: %s)" % (target, ", ".join(TARGETS)))
    directory, env = TARGETS[target]
    args = ["run", "-d", directory]
    if env:
        args += ["-e", env]
    return args + list(extra)


def cmd_test(_):
    return subprocess.call([sys.executable, os.path.join(ROOT, "tools", "run_tests.py")])


def cmd_build(opts):
    for target in opts.targets or ALL:
        rc = pio(run_args(target))
        if rc:
            return rc
    return 0


def cmd_flash(opts):
    extra = ["-t", "upload"]
    if opts.port:
        extra += ["--upload-port", opts.port]
    return pio(run_args(opts.target, extra))


def cmd_audio(opts):
    extra = ["-t", "uploadfs"]
    if opts.port:
        extra += ["--upload-port", opts.port]
    return pio(run_args("host", extra))


def find_firmware(target):
    """Return the firmware.bin PlatformIO built for a target, or None."""
    directory, env = TARGETS[target]
    out = subprocess.run(
        ["pio", "project", "config", "-d", directory, "--json-output"],
        cwd=ROOT, env=pio_env(), capture_output=True, text=True).stdout
    build_dir = os.path.join(ROOT, directory, ".pio", "build")
    try:
        import json
        for section, items in json.loads(out):
            if section == "platformio":
                for key, value in items:
                    if key == "build_dir":
                        build_dir = os.path.expanduser(value)
    except ValueError:
        pass
    if not os.path.isabs(build_dir):
        build_dir = os.path.join(ROOT, directory, build_dir)
    roots = [build_dir]
    # A project without a [platformio] section does not report PLATFORMIO_BUILD_DIR. PlatformIO then
    # builds in "<parent>/<project folder>-<hash>": search those folders too.
    shared = os.environ.get("PLATFORMIO_BUILD_DIR", "")
    if "$PROJECT_HASH" in shared:
        parent = os.path.dirname(shared)
        prefix = os.path.basename(directory) + "-"
        if os.path.isdir(parent):
            roots += [os.path.join(parent, d) for d in os.listdir(parent) if d.startswith(prefix)]
    best = None
    for root in roots:
        for base, _dirs, files in os.walk(root):
            if "firmware.bin" in files and (env is None or os.path.basename(base) == env):
                path = os.path.join(base, "firmware.bin")
                if best is None or os.path.getmtime(path) > os.path.getmtime(best):
                    best = path
    return best


def cmd_dist(_):
    rc = cmd_build(argparse.Namespace(targets=ALL))
    if rc:
        return rc
    dist = os.path.join(ROOT, "dist")
    os.makedirs(dist, exist_ok=True)
    for name, target in DIST.items():
        src = find_firmware(target)
        if src is None:
            print("no firmware.bin found for " + target)
            return 1
        shutil.copyfile(src, os.path.join(dist, name))
        print("dist/%s  <- %s (%d bytes)" % (name, src, os.path.getsize(src)))
    return 0

HOST_URL = "http://192.168.4.1"


def http_post(url, body, content_type, timeout=180.0):
    request = urllib.request.Request(url, data=body, headers={"Content-Type": content_type}, method="POST")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read().decode("utf-8", "replace")


def multipart(path):
    """Body and content type of a form upload with the one field 'firmware'."""
    boundary = "----rtd%d" % int(time.time() * 1000)
    with open(path, "rb") as source:
        data = source.read()
    head = ("--%s\r\nContent-Disposition: form-data; name=\"firmware\"; filename=\"%s\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n" % (boundary, os.path.basename(path)))
    tail = "\r\n--%s--\r\n" % boundary
    return head.encode("ascii") + data + tail.encode("ascii"), "multipart/form-data; boundary=" + boundary


OTA_FILE = {"display": "display.bin", "host": "host.bin", "sticks": "joystick.bin"}


def cmd_ota(opts):
    """Send an image over the air. The PC must be on the Wi-Fi RTD-Host and the host in update mode."""
    import json
    path = opts.file or os.path.join(ROOT, "dist", OTA_FILE[opts.target])
    if not opts.file:
        # A build that is newer than the copy in dist/ wins: dist/ is only refreshed by `rtd.py dist`.
        built = find_firmware("stick" if opts.target == "sticks" else opts.target)
        if built and (not os.path.isfile(path) or os.path.getmtime(built) > os.path.getmtime(path)):
            print("using the newer build %s" % built)
            path = built
    if not os.path.isfile(path):
        print("no image at %s. Run `python tools/rtd.py dist` first." % path)
        return 1
    try:
        status = json.loads(http_get("/api/status", timeout=5.0))
        if status["host"]["phase"] != 9:
            print("The host is not in update mode. Press START UPDATE MODE in the service menu of the")
            print("display (settings, STATUS tab, SERVICE MENU), or hold the host BOOT button for 3 s.")
            return 1
        size = os.path.getsize(path)
        if opts.target == "display":
            address = status["display"]["ip"]
            if not status["display"]["online"] or "." not in address:
                print("The display has no address yet (it joins the access point in update mode). Try again.")
                return 1
            print("display at %s: sending %s (%d bytes)..." % (address, path, size), flush=True)
            with open(path, "rb") as source:
                print(http_post("http://%s/update" % address, source.read(), "application/octet-stream"))
            print("The display restarts now.")
        elif opts.target == "host":
            print("host: sending %s (%d bytes)..." % (path, size), flush=True)
            body, kind = multipart(path)
            print(http_post(HOST_URL + "/upload/host", body, kind))
            print("The host restarts now and enters update mode again.")
        else:
            print("host: storing the joystick image %s (%d bytes)..." % (path, size), flush=True)
            body, kind = multipart(path)
            answer = http_post(HOST_URL + "/upload/joystick", body, kind)
            print(answer)
            if '"ok":true' not in answer:
                print("The host did not accept the joystick image. No stick update was started.")
                return 1
            if opts.store_only:
                print("Stored. Press UPDATE JOYSTICKS on the display or on the page to start.")
            else:
                print(http_post(HOST_URL + "/ota/sticks", b"", "text/plain", timeout=10.0))
                print("The host updates each online stick now. Watch the display or " + HOST_URL)
    except (urllib.error.URLError, OSError, ValueError, KeyError) as error:
        print("OTA failed (%s)." % error)
        print("Join the Wi-Fi network RTD-Host first (the PC gets its address by itself).")
        return 1
    return 0


def log_path(prefix, out):
    if out:
        return out
    logs = os.path.join(ROOT, "logs")
    os.makedirs(logs, exist_ok=True)
    return os.path.join(logs, "%s-%s.log" % (prefix, time.strftime("%Y%m%d-%H%M%S")))


def http_get(path, timeout=3.0):
    with urllib.request.urlopen(HOST_URL + path, timeout=timeout) as response:
        return response.read().decode("utf-8", "replace")


def cmd_debug(opts):
    """Read /api/debug once, then follow /api/log for --watch seconds."""
    path = log_path("debug", opts.out)
    since = 0
    end = time.time() + opts.watch
    try:
        with open(path, "w", encoding="utf-8") as out:
            status = http_get("/api/debug")
            print(status)
            out.write("# /api/debug\n" + status + "\n# /api/log\n")
            while True:
                text = http_get("/api/log?since=%d" % since)
                for line in text.splitlines():
                    print(line)
                    out.write(line + "\n")
                    head = line.split(None, 1)[0] if line.strip() else ""
                    if head.isdigit():
                        since = max(since, int(head))
                out.flush()
                if time.time() >= end:
                    break
                time.sleep(1.0)
            if opts.watch:
                status = http_get("/api/debug")
                out.write("# /api/debug (end)\n" + status + "\n")
    except (urllib.error.URLError, OSError) as error:
        print("cannot reach %s (%s)." % (HOST_URL, error))
        print("Join the Wi-Fi network RTD-Host first. The host needs debug Wi-Fi on,")
        print("or maintenance mode (BOOT button held 3 s).")
        return 1
    print("saved: " + path)
    return 0


def cmd_monitor(opts):
    """Record a serial port to logs/. Needs pyserial, else runs `pio device monitor`."""
    path = log_path("serial", opts.out)
    try:
        import serial  # pyserial, optional
        from serial.tools import list_ports
    except ImportError:
        args = ["device", "monitor", "-b", "115200", "--quiet"]
        if opts.port:
            args += ["-p", opts.port]
        print("pyserial not found: `pio device monitor` runs now (Ctrl+C ends it; no file is written).")
        return pio(args)
    port = opts.port
    if not port:
        found = [p.device for p in list_ports.comports()]
        if len(found) != 1:
            sys.exit("give --port (found: %s)" % (", ".join(found) or "none"))
        port = found[0]
    end = time.time() + opts.seconds if opts.seconds else None
    with serial.Serial(port, 115200, timeout=0.2) as link, open(path, "wb") as out:
        if opts.send:
            link.write(opts.send.encode("ascii"))
        print("recording %s -> %s (Ctrl+C ends it)" % (port, path), flush=True)
        try:
            while end is None or time.time() < end:
                data = link.read(4096)
                if data:
                    out.write(data)
                    out.flush()
                    sys.stdout.write(data.decode("utf-8", "replace"))
                    sys.stdout.flush()
        except KeyboardInterrupt:
            pass
    print("\nsaved: " + path)
    return 0


def cmd_ports(_):
    return pio(["device", "list"])


def main():
    parser = argparse.ArgumentParser(description="Reaction Time Duel helper")
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("test").set_defaults(fn=cmd_test)
    build = sub.add_parser("build")
    build.add_argument("targets", nargs="*")
    build.set_defaults(fn=cmd_build)
    flash = sub.add_parser("flash")
    flash.add_argument("target")
    flash.add_argument("--port")
    flash.set_defaults(fn=cmd_flash)
    audio = sub.add_parser("audio")
    audio.add_argument("--port")
    audio.set_defaults(fn=cmd_audio)
    sub.add_parser("dist").set_defaults(fn=cmd_dist)
    ota = sub.add_parser("ota", help="send an image over the air (Wi-Fi RTD-Host, update mode)")
    ota.add_argument("target", choices=sorted(OTA_FILE))
    ota.add_argument("--file", help="image file (default: dist/<name>.bin)")
    ota.add_argument("--store-only", action="store_true", help="sticks: store the image, do not start the update")
    ota.set_defaults(fn=cmd_ota)
    debug = sub.add_parser("debug")
    debug.add_argument("--watch", type=float, default=0, help="follow the event log for SEC seconds")
    debug.add_argument("--out", help="output file (default: logs/debug-<time>.log)")
    debug.set_defaults(fn=cmd_debug)
    monitor = sub.add_parser("monitor")
    monitor.add_argument("--port")
    monitor.add_argument("--seconds", type=float, default=0, help="stop after N seconds (default: until Ctrl+C)")
    monitor.add_argument("--send", help="characters to send first, for example s or l")
    monitor.add_argument("--out", help="output file (default: logs/serial-<time>.log)")
    monitor.set_defaults(fn=cmd_monitor)
    sub.add_parser("ports").set_defaults(fn=cmd_ports)
    opts = parser.parse_args()
    sys.exit(opts.fn(opts))


if __name__ == "__main__":
    main()

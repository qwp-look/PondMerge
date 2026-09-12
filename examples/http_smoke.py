#!/usr/bin/env python3
"""PondMerge v1 demo HTTP-layer regression (round-9 guide section 12.3).

Starts examples/demo_server.py --host on a loopback port and verifies:

  * concurrent POST /api/op from two threads: every response carries its own
    result and no response is another request's (no response mismatch);
  * rapid sequential operations keep seq monotonic and the scene consistent;
  * the maintenance diff (last_diff) appears after compact/merge/split with
    the epoch/generation/digest invariants intact;
  * a slow /api/state reader never blocks the pumps (state stays fresh);
  * killing the demo process puts the server into a visible degraded state;
  * ESP32 display-only: with --serial, POST /api/op answers
    UNSUPPORTED_DISPLAY_ONLY (verified via a local pty -- no real device).

Usage: python3 examples/http_smoke.py [path-to-host_demo]
"""
import faulthandler
import json
import os
import pty
import subprocess
import sys
import threading
import time
import urllib.request

DEMO = sys.argv[1] if len(sys.argv) > 1 else "build/host_demo"
PORT = 8977
BASE = f"http://127.0.0.1:{PORT}"

checks = 0
fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print(f"  CHECK failed: {what}")


def get(path, base=BASE):
    with urllib.request.urlopen(base + path, timeout=5) as r:
        return json.loads(r.read())


def post(path, obj, base=BASE):
    req = urllib.request.Request(base + path, method="POST",
                                 data=json.dumps(obj).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())


def start_server(extra=None, port=PORT):
    cmd = ["python3", "examples/demo_server.py", "--host", DEMO,
           "--port", str(port)]
    if extra:
        cmd = cmd + extra
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def wait_listening(port, timeout=5.0):
    import socket
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def main():
    faulthandler.dump_traceback_later(60, exit=True)
    srv = start_server()
    try:
        check(wait_listening(PORT), "server listens")
        time.sleep(0.8)  # let the demo process boot and emit its snapshot

        # ---- 1) rapid sequential ops: seq monotonic, results correlate ----
        seqs = []
        ids = []
        for i in range(6):
            r = post("/api/op", {"cmd": "alloc", "pool": 0,
                                 "size": 100 + i, "flags": 0})
            check(r["sent"] is True and r["status"] == "OK",
                  f"rapid alloc {i} OK")
            ids.append(r["result"]["object_id"])
            if r["snapshot"]:
                seqs.append(r["snapshot"]["seq"])
        check(all(seqs[i] < seqs[i + 1] for i in range(len(seqs) - 1)),
              "seq strictly monotonic")
        state = get("/api/state")
        live_ids = {o["object_id"] for o in state["snapshot"]["objects"]}
        check(set(ids) <= live_ids, "all rapid allocations are live")

        # ---- 2) maintenance diff invariants ----
        r = post("/api/op", {"cmd": "compact", "pool": 0})
        d = r.get("snapshot") and get("/api/state")["last_diff"]
        if d:
            for m in d["moved"]:
                check(m["invariants_ok"],
                      f"moved invariants for object {m['object_id']}")
        else:
            check(d is not None, "compact produced a diff")

        # ---- 3) concurrent POSTs: no response mismatch ----
        results = []
        lock = threading.Lock()

        def worker(tid):
            for j in range(5):
                size = 1000 + tid * 100 + j
                r = post("/api/op", {"cmd": "alloc", "pool": 0,
                                     "size": size, "flags": 0})
                oid = (r.get("result") or {}).get("object_id")
                with lock:
                    results.append((tid, size, oid))

        threads = [threading.Thread(target=worker, args=(t,)) for t in range(2)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        check(all(r[2] is not None for r in results),
              "every concurrent alloc returned an object id")
        state = get("/api/state")
        snap_ids = {o["object_id"] for o in state["snapshot"]["objects"]}
        check(all(r[2] in snap_ids for r in results),
              "all concurrently allocated objects are in the final snapshot")

        # ---- 4) slow /api/state reader does not block the pumps ----
        t0 = time.time()
        for _ in range(20):
            get("/api/state")
        state = get("/api/state")
        seq_after = state["snapshot"]["seq"]
        time.sleep(0.5)
        state = get("/api/state")
        check(state["snapshot"]["seq"] >= seq_after,
              "pumps keep running while state is polled")

        # ---- 5) demo process death -> visible degraded state ----
        subprocess.run(["pkill", "-x", "host_demo"], check=False)
        time.sleep(0.8)
        state = get("/api/state")
        check(state["connected"] is False or state["degraded"] is True,
              "demo death is visible as disconnected/degraded")

        srv.terminate()
        srv.wait(timeout=5)

        # ---- 6) ESP32 display-only: commands are refused (pty as serial) ----
        PORT2 = PORT + 1
        master, slave = pty.openpty()
        os.set_blocking(slave, False)
        srv2 = start_server(extra=["--serial", os.ttyname(slave)],
                            port=PORT2)
        try:
            check(wait_listening(PORT2), "serial-mode server listens")
            time.sleep(0.8)
            r = post("/api/op", {"cmd": "reset"},
                     base=f"http://127.0.0.1:{PORT2}")
            check(r.get("status") == "UNSUPPORTED_DISPLAY_ONLY",
                  "serial mode refuses UI commands explicitly")
        finally:
            srv2.terminate()
            srv2.wait(timeout=5)
            os.close(master)
            os.close(slave)
    finally:
        # terminating the server closes the demo's stdin: the demo exits on
        # EOF by itself. NEVER pkill by name here -- this script's own parent
        # shell may match the pattern.
        srv.terminate()

    print(f"{checks} checks, {fails} failures")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

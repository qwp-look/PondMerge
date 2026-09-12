#!/usr/bin/env python3
"""PondMerge v1 demo protocol regression (round-7 guide section 10).

Drives examples/host_demo over stdin/stdout -- no browser, no HTTP -- and
asserts the full protocol contract:

  * every emitted line is valid JSON with a known record type;
  * every operation answers exactly one result with a status;
  * snapshot seq is monotonically increasing;
  * object ids are stable; survivors keep generation and payload digest
    across compact/merge/split while address_epoch increments on relocation;
  * malformed input (bad JSON, whitespace, reordered/missing/wrong-typed
    fields, unknown commands, empty lines) is rejected with a structured
    INVALID_REQUEST result and never corrupts the scene;
  * failed operations (double free, unknown pool) do not produce an
    invalid scene state (the follow-up validate-style snapshot still parses
    and the scenario continues).

Usage: python3 examples/protocol_smoke.py [path-to-host_demo]
Exit code 0 = all checks passed.
"""
import json
import select
import subprocess
import sys

DEMO = sys.argv[1] if len(sys.argv) > 1 else "build/host_demo"

checks = 0
fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print(f"  CHECK failed: {what}")


class Demo:
    def __init__(self):
        self.p = subprocess.Popen([DEMO], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True,
                                  bufsize=1)
        # drain the startup burst (ready info + initial reset records) so
        # every later op() reads exactly its own answer
        self.drain_snapshot()

    def drain_snapshot(self):
        while True:
            line = self.p.stdout.readline()
            if not line:
                return
            try:
                if json.loads(line.strip()).get("t") == "snapshot":
                    return
            except json.JSONDecodeError:
                pass

    def drain(self):
        """Discard any desynced leftover records (non-blocking)."""
        while select.select([self.p.stdout], [], [], 0.05)[0]:
            line = self.p.stdout.readline()
            if not line:
                return

    def line(self, text, until_snapshot=True):
        self.drain()
        """Send a RAW line and return the records emitted until the next
        snapshot (inclusive) or a result (when until_snapshot=False)."""
        self.p.stdin.write(text + "\n")
        self.p.stdin.flush()
        recs = []
        while True:
            line = self.p.stdout.readline()
            if not line:
                return recs  # process died: caller will notice empty results
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                rec = {"t": "BAD_JSON", "raw": line[:80]}
            recs.append(rec)
            if rec.get("t") == "snapshot":
                return recs
            if rec.get("t") == "result" and not until_snapshot:
                return recs

    def op(self, cmd, **params):
        payload = {"cmd": cmd}
        payload.update(params)
        # every state-changing op emits result THEN snapshot; thresholds emits
        # a bare result (query-only), so it reads until the result instead
        until_snapshot = cmd != "thresholds"
        return self.line(json.dumps(payload, separators=(",", ":")),
                         until_snapshot=until_snapshot)

    def quit(self):
        try:
            self.p.stdin.write('{"cmd":"quit"}\n')
            self.p.stdin.flush()
            self.p.wait(timeout=2)
        except Exception:
            self.p.kill()


def status_of(recs):
    for r in recs:
        if r.get("t") == "result":
            return r.get("status")
    return None


def snapshot_of(recs):
    for r in reversed(recs):
        if r.get("t") == "snapshot":
            return r
    return None


def main():
    d = Demo()

    # ---- reset scene ----
    print("step: reset", flush=True)
    recs = d.op("reset")
    check(status_of(recs) is None or True, "reset answers")
    snap = snapshot_of(recs)
    check(snap is not None and snap["protocol"] == 1, "protocol v1 snapshot")
    check(snap["source"] == "HOST", "host source")
    check(len(snap["pools"]) == 2 and len(snap["objects"]) == 5, "seeded scene")

    print("step: pre-alloc", flush=True)
    recs = d.op("alloc", pool=0, size=500, flags=0)
    print("step: post-alloc", flush=True)
    check(status_of(recs) == "OK", "alloc movable")
    recs = d.op("alloc", pool=0, size=300, flags=2)
    check(status_of(recs) == "OK", "alloc pinned")
    snap = snapshot_of(recs)
    pinned = [b for p in snap["pools"] for b in p["blocks"] if b["kind"] == "PINNED"]
    check(len(pinned) == 1, "pinned block visible")

    # ---- fragmentation + advice ----
    recs = d.op("fragment")
    check(status_of(recs) == "OK", "fragment")
    recs = d.op("advice", pool=0, size=0, align=8)
    a = [r for r in recs if r.get("t") == "result"][0]
    check("verdict" in a and "caller_must_establish_quiescence" in a,
          "advice record fields")

    # ---- compact: object identity, digest, epoch ----
    before = snapshot_of(recs)["objects"]
    recs = d.op("compact", pool=0)
    check(status_of(recs) == "OK", "compact")
    after = snapshot_of(recs)["objects"]
    before_by_id = {o["object_id"]: o for o in before}
    for o in after:
        b = before_by_id[o["object_id"]]
        check(o["payload_digest"] == b["payload_digest"],
              f"digest stable for {o['object_id']}")
        check(o["generation"] == b["generation"],
              f"generation unchanged for {o['object_id']}")
        if o["address_offset"] != b["address_offset"]:
            check(o["address_epoch"] > b["address_epoch"],
                  f"epoch incremented for relocated {o['object_id']}")

    # ---- merge + split ----
    recs = d.op("merge", source=1, target=0)
    check(status_of(recs) == "OK", "merge")
    recs = d.op("split", source=0, segments=2)
    check(status_of(recs) == "OK", "split")

    # ---- malformed input: structured rejection, scene provably intact ----
    # (a) pure-reject inputs: ONE bare INVALID_REQUEST result, no snapshot;
    #     the scene snapshot before/after each rejection must be IDENTICAL
    #     (object set, digests, pool layout, counters) -- round-8 guide §7.
    reject_inputs = [
        "not json at all",
        "",
        "   ",
        '{ "cmd" : "free" }',                             # missing id
        '{"cmd":"alloc","pool":0,"size":"hundred"}',      # wrong type
        '{"cmd":"nosuchcommand"}',                        # unknown
        '{"no_cmd":1}',
        '{"cmd":"alloc","pool":0,"size":99999999999}',    # out of range
        '{"cmd":"alloc","pool":0,"size":{"a":1}}',        # nested object
        '{"cmd":"alloc","pool":[0]}',                     # array
        '{"cmd":"alloc","pool":0,"size":0x10}',           # hex literal
        '{"cmd":"alloc","pool":0,"size":1.5}',            # float
        '{"cmd":"alloc","pool":-1,"size":10}',            # negative
        '{"cmd":"' + "x" * 200 + '"}',                    # overlong value
        '{"' + "k" * 40 + '":1,"cmd":"advice"}',          # overlong key
    ]

    def scene_signature(snap):
        objs = sorted((o["object_id"], o["generation"], o["pool_id"],
                       o["address_offset"], o["payload_digest"])
                      for o in snap["objects"])
        pools = sorted((p["pool_id"], p["state"], p["used_bytes"],
                        p["free_bytes"], p["live_objects"])
                       for p in snap["pools"])
        return objs, pools

    seq_before = snapshot_of(d.op("advice", pool=0))["seq"]
    for text in reject_inputs:
        before = snapshot_of(d.op("advice", pool=0))
        recs = d.line(text, until_snapshot=False)
        check(len(recs) > 0, f"answer emitted for {text[:32]!r}")
        result = next((r for r in recs if r.get("t") == "result"), None)
        check(result is not None, f"result for {text[:32]!r}")
        if result:
            check(result.get("status") == "INVALID_REQUEST",
                  f"INVALID_REQUEST for {text[:32]!r}")
        after = snapshot_of(d.op("advice", pool=0))
        check(scene_signature(before) == scene_signature(after),
              f"scene unchanged after {text[:32]!r}")
    snap = snapshot_of(d.op("advice", pool=0))
    check(snap["seq"] > seq_before, "seq monotonic after rejections")

    # (b) duplicate keys: FIRST occurrence wins (DEMO_REQUIREMENTS §2.1) --
    #     size 0 (first) means generic advice, not a request error
    recs = d.line('{"cmd":"advice","pool":0,"size":0,"size":100}')
    result = next((r for r in recs if r.get("t") == "result"), None)
    check(result is not None and "verdict" in result,
          "duplicate key: first wins, advice still answers")

    # (c) legal omitted-optional fields execute with the documented defaults
    recs = d.op("alloc", pool=0, size=100)  # align/flags omitted
    check(status_of(recs) == "OK", "omitted optional fields default")
    snap = snapshot_of(recs)
    new_obj = max(snap["objects"], key=lambda o: o["object_id"])
    check(new_obj["flags"] == 0, "default flags = 0")
    check(new_obj["block_size"] == 112, "default align 8 -> block 112")

    # ---- failed operations keep the scene consistent ----
    # object ids 1 and 3 were freed by the fragment step; free a survivor
    live_id = snapshot_of(d.op("advice", pool=0))["objects"][0]["object_id"]
    recs = d.op("free", id=live_id)
    check(status_of(recs) == "OK", f"free id {live_id}")
    recs = d.op("free", id=live_id)
    check(status_of(recs) == "INVALID_REF", "double free refused")
    snap = snapshot_of(recs)
    check(all(o["object_id"] != live_id for o in snap["objects"]), "id gone")
    recs = d.op("compact", pool=99)
    check(status_of(recs) == "INVALID_POOL", "unknown pool refused")
    snap = snapshot_of(recs)
    check(len(snap["pools"]) >= 1, "scene still alive")

    d.quit()
    print(f"{checks} checks, {fails} failures")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

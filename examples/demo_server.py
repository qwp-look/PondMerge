#!/usr/bin/env python3
"""PondMerge v1 demo server (round-6 requirements doc section 7).

Two sources, one UI:
  --host  [path]   spawn the C++ demo process (examples/host_demo) and talk
                   to it over stdin/stdout (default: build/host_demo)
  --serial PORT    attach an ESP32-S3 running the demo firmware and stream
                   its JSON Lines over USB-Serial-JTAG (display-only)

HTTP endpoints (stdlib only, no third-party packages):
  GET  /           single-page UI: pool lanes, object table, advice panel,
                   operation log, before/after compaction comparison
  GET  /api/state  latest snapshot + log tail + last result (JSON)
  POST /api/op     {"cmd":"alloc","pool":0,"size":120,...} -> forward to the
                   source, return the records it produced

Security/robustness notes: binds to 127.0.0.1 only, tolerates a dying demo
process, and never mutates allocator state itself -- every mutation goes
through the PondMerge public API in the source process.
"""
import argparse
import json
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_COMMAND_BYTES = 1024
RESPONSE_TIMEOUT = 3.0

STATE = {
    "snapshot": None,
    "last_result": None,
    "log": [],
    "source": "HOST",
    "connected": False,
    "degraded": False,
    "build_id": "",
    "result_seq": 0,
    "last_seq": None,           # last ACCEPTED snapshot seq (state machine)
    "protocol_errors": 0,
    "last_protocol_error": "",
    "prev_snapshot": None,      # for the maintenance before/after diff
    "pending_diff_op": None,
    "last_diff": None,
}
STATE_LOCK = threading.Lock()   # guards STATE only -- never held during I/O
COMMAND_LOCK = threading.Lock() # serializes send -> wait-for-result
SERIAL = None
PROC = None


def log_protocol_error(what):
    with STATE_LOCK:
        STATE["protocol_errors"] += 1
        STATE["last_protocol_error"] = what
        STATE["log"].append({"t": "info", "event": "protocol-error",
                             "detail": what})
        del STATE["log"][:-200]


def push(rec, expected_source):
    """Receiver state machine (round-9 guide section 6). Returns True when the
    record was accepted as current state."""
    if not isinstance(rec, dict) or rec.get("t") not in ("info", "result",
                                                         "snapshot"):
        log_protocol_error("unknown_record")
        return False
    if rec.get("protocol") != 1:
        log_protocol_error("unsupported_protocol")
        return False
    t = rec.get("t")
    if t == "info" and rec.get("event") == "ready":
        # an explicit ready opens a NEW session: seq may restart
        with STATE_LOCK:
            STATE["last_seq"] = None
            STATE["connected"] = True
            STATE["degraded"] = False
            if rec.get("commit"):
                STATE["build_id"] = rec["commit"]
        return True
    if t == "snapshot":
        if rec.get("source") != expected_source:
            log_protocol_error("source_mismatch")
            return False
        seq = rec.get("seq")
        with STATE_LOCK:
            if STATE["last_seq"] is not None and seq <= STATE["last_seq"]:
                log_protocol_error("stale_snapshot")
                return False
            STATE["last_seq"] = seq
            prev = STATE["prev_snapshot"]
            STATE["prev_snapshot"] = rec
            STATE["snapshot"] = rec
            STATE["connected"] = True
            STATE["degraded"] = False
            pending = STATE.pop("pending_diff_op", None)
            STATE["last_diff"] = (snapshot_diff(prev, rec, pending)
                                  if (pending and prev is not None) else None)
        return True
    if t == "result":
        with STATE_LOCK:
            STATE["last_result"] = rec
            STATE["result_seq"] += 1
            STATE["log"].append(rec)
            del STATE["log"][:-200]
            # a maintenance op announces itself: the NEXT accepted snapshot
            # is diffed against the previous one (object-level MOVED/
            # CREATED/DELETED with epoch/generation/digest assertions)
            op = rec.get("op")
            STATE["pending_diff_op"] = op if op in ("compact", "merge",
                                                     "split") else None
        return True
    return False


def snapshot_diff(prev, nxt, op):
    """Object-level diff between two ACCEPTED snapshots. Asserts the
    protocol invariants for moved objects: generation and payload digest
    unchanged, address_epoch strictly increased. Pool lifecycle changes are
    reported as well."""
    before = {o["object_id"]: o for o in prev.get("objects", [])}
    after = {o["object_id"]: o for o in nxt.get("objects", [])}
    diff = {"op": op, "moved": [], "created": [], "deleted": [],
            "pools_before": sorted(p["pool_id"] for p in prev.get("pools", [])),
            "pools_after": sorted(p["pool_id"] for p in nxt.get("pools", []))}
    for oid in sorted(set(before) | set(after)):
        b = before.get(oid)
        a = after.get(oid)
        if a is None:
            diff["deleted"].append(oid)
        elif b is None:
            diff["created"].append(oid)
        elif b["pool_id"] != a["pool_id"] or \
                b["address_offset"] != a["address_offset"]:
            moved_ok = (a["address_epoch"] > b["address_epoch"] and
                        a["generation"] == b["generation"] and
                        a["payload_digest"] == b["payload_digest"])
            diff["moved"].append({"object_id": oid,
                                  "old_pool": b["pool_id"],
                                  "new_pool": a["pool_id"],
                                  "old_offset": b["address_offset"],
                                  "new_offset": a["address_offset"],
                                  "invariants_ok": moved_ok})
            if not moved_ok:
                log_protocol_error("moved_invariant_violation")
    return diff


def pump_host(proc):
    def run():
        for line in proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                push(json.loads(line), "HOST")
            except json.JSONDecodeError:
                log_protocol_error("bad_json:" + line[:80])
        with STATE_LOCK:  # the demo process exited: visible degraded state
            STATE["connected"] = False
            STATE["degraded"] = True
            STATE["log"].append({"t": "info", "event": "process-exited"})
    threading.Thread(target=run, daemon=True).start()


def pump_serial():
    while True:
        try:
            line = SERIAL.readline().decode("utf-8", "replace").strip()
            if not line:
                continue
            if line.startswith("{"):
                try:
                    push(json.loads(line), "ESP32")
                except json.JSONDecodeError:
                    log_protocol_error("bad_json")
        except Exception:
            time.sleep(0.5)
            with STATE_LOCK:
                STATE["connected"] = False
                STATE["degraded"] = True


def send(cmd):
    line = json.dumps(cmd, separators=(",", ":")) + "\n"
    if SERIAL is not None:
        SERIAL.write(line.encode())
        return True
    try:
        PROC.stdin.write(line)  # text=True: str, not bytes
        PROC.stdin.flush()
        return True
    except Exception:
        with STATE_LOCK:
            STATE["connected"] = False
            STATE["degraded"] = True
        return False


PAGE = """<!doctype html><html><head><meta charset="utf-8">
<title>PondMerge v1 demo</title><style>
body{font-family:system-ui,sans-serif;margin:16px;background:#111;color:#ddd}
h2{margin:.4em 0}
.lane{display:flex;height:44px;border:2px solid #555;border-radius:4px;
      overflow:hidden;margin:6px 0;position:relative}
.blk{height:100%;box-sizing:border-box;border-right:1px solid #111;
     overflow:hidden;font-size:10px;white-space:nowrap}
.MOVABLE{background:#3b6fd4}.PINNED{background:#c0392b}
.FREE{background:#4a4f55}.SLACK{background:#8a7a1f}
.sel{outline:3px solid #7dff9a;outline-offset:-3px}
.warn{background:#4a1d1d;border:1px solid #a55;padding:8px;margin:10px 0}
table{border-collapse:collapse;font-size:12px}
td,th{border:1px solid #444;padding:3px 8px}
button{margin:2px;padding:4px 10px}
input{width:70px}
#log{height:130px;overflow-y:scroll;background:#0a0a0a;font-size:11px}
</style></head><body>
<h1>PondMerge v1 demo</h1>
<div class="warn"><b>Remember:</b> a raw <code>T*</code> does not follow
relocation &middot; compaction during an active borrow returns Busy &middot;
DMA/ISR/external users must be stopped by <i>you</i> &middot; pinned objects
can block compaction &middot; <code>RawRef</code> is a forgeable low-level
handle &middot; <code>resolve/peek</code> pointers are not RAII borrows &middot;
advice is not a compaction guarantee &middot; the quiescence button only
simulates what real drivers must do themselves.</div>
<div>source: <b id="src">-</b> build: <b id="build">-</b>
     connected: <b id="conn">-</b></div>
<div>
 <button data-mutating onclick="op('reset')">reset scene</button>
 alloc pool <input id="apool" value="0" size="2"> size <input id="asize" value="300">
 flags <select id="aflags"><option value="0">movable</option>
   <option value="2">pinned</option><option value="6">dma</option></select>
 <button data-mutating onclick="op('alloc',{pool:+apool.value,size:+asize.value,flags:+aflags.value})">alloc</button>
 free id <input id="fid" value="1" size="3"> <button data-mutating onclick="op('free',{id:+fid.value})">free</button>
 <button data-mutating onclick="op('fragment')">make fragmentation</button>
 <button data-mutating onclick="op('compact',{pool:0})">compact pool 0</button>
 <button data-mutating onclick="op('compact',{pool:1})">compact pool 1</button>
 merge src <input id="msrc" value="0" size="2"> tgt <input id="mtgt" value="1" size="2">
 <button data-mutating onclick="op('merge',{source:+msrc.value,target:+mtgt.value})">merge</button>
 split src <input id="ssrc" value="0" size="2"> segs <input id="sseg" value="2" size="2">
 <button data-mutating onclick="op('split',{source:+ssrc.value,segments:+sseg.value})">split</button>
 advice pool <input id="apol" value="0" size="2"> size <input id="asize2" value="0" size="5">
 <button onclick="op('advice',{pool:+apol.value,size:+asize2.value,align:8})">analyze</button>
 <button onclick="sendQuit()">quit process</button>
</div>
<div id="lanes"></div>
<div id="advice"></div>
<h2>objects</h2><div id="objs"></div>
<h2>last result</h2><pre id="res">-</pre>
<h2>log</h2><div id="log"></div>
<script>
let last = null;
// All dynamic text goes through textContent (round-7 guide section 9): no
// protocol field, build id or error detail is ever interpolated into HTML.
function el(tag, cls, text){
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
}
function op(cmd, params){ fetch('/api/op',{method:'POST',
  headers:{'Content-Type':'application/json'},
  body:JSON.stringify(Object.assign({cmd:cmd}, params||{}))}).then(refresh); }
function sendQuit(){ op('quit'); }
function refresh(){
  fetch('/api/state').then(r=>r.json()).then(s=>{
    document.getElementById('src').textContent = s.source;
    document.getElementById('build').textContent = s.build_id || '-';
    document.getElementById('conn').textContent = s.connected ? 'yes' : 'NO';
    const sn = s.snapshot;
    if (!sn) return;
    const lanes = document.getElementById('lanes');
    lanes.textContent = '';
    for (const p of sn.pools){
      const cap = p.capacity || 1;
      lanes.appendChild(el('h2', null,
        `pool ${p.pool_id} [segs ${p.segment_first}..${p.segment_first+p.segment_count-1}] ` +
        `state ${p.state} · used ${p.used_bytes} / free ${p.free_bytes} · ` +
        `largest free ${p.largest_free_block} · fragment ${p.fragment_bytes} · ` +
        `objects ${p.live_objects} · borrows ${p.borrow_count} · epoch ${p.structure_epoch}`));
      const lane = el('div', 'lane');
      for (const b of p.blocks){
        const d = el('div', 'blk ' + b.kind,
          (b.kind === 'MOVABLE' || b.kind === 'PINNED') ? '#' + b.object_id : '');
        d.style.width = (100*b.size/cap).toFixed(2) + '%';
        d.title = 'off ' + b.offset + ' size ' + b.size +
          (b.object_id !== undefined
            ? ' id ' + b.object_id + ' gen ' + b.generation +
              ' epoch ' + b.address_epoch + ' flags ' + b.flags
            : '');
        lane.appendChild(d);
      }
      lanes.appendChild(lane);
    }
    const adv = document.getElementById('advice');
    adv.textContent = '';
    adv.appendChild(el('h2', null, 'compaction advice'));
    const at = el('table');
    const ah = el('tr');
    for (const h of ['pool','verdict','fragment ‰','borrows','pinned','caller must establish quiescence'])
      ah.appendChild(el('th', null, h));
    at.appendChild(ah);
    const names = ['NO_ACTION','COMPACT_RECOMMENDED','COMPACT_BLOCKED',
                   'COMPACT_UNLIKELY_TO_HELP','INVALID_METADATA','INVALID_REQUEST'];
    for (const a of sn.advice){
      const row = el('tr');
      row.appendChild(el('td', null, String(a.pool_id)));
      row.appendChild(el('td')).appendChild(el('b', null, names[a.verdict] || a.verdict));
      row.appendChild(el('td', null, String(a.fragment_ratio_permille)));
      row.appendChild(el('td', null, String(a.borrow_count)));
      row.appendChild(el('td', null, String(a.has_pinned_objects)));
      row.appendChild(el('td', null, String(a.caller_must_establish_quiescence)));
      at.appendChild(row);
    }
    adv.appendChild(at);
    const objs = document.getElementById('objs');
    objs.textContent = '';
    const ot = el('table');
    const oh = el('tr');
    for (const h of ['id','gen','pool','offset','size','block','epoch','flags','digest'])
      oh.appendChild(el('th', null, h));
    ot.appendChild(oh);
    for (const o of sn.objects){
      const row = el('tr');
      for (const v of [o.object_id, o.generation, o.pool_id, o.address_offset,
                       o.size, o.block_size, o.address_epoch, o.flags,
                       o.payload_digest])
        row.appendChild(el('td', null, String(v)));
      ot.appendChild(row);
    }
    objs.appendChild(ot);
    document.getElementById('res').textContent =
      JSON.stringify(s.last_result, null, 1);
    const log = document.getElementById('log');
    log.textContent = s.log.slice(-30).map(r=>JSON.stringify(r)).reverse().join('\n');
  });
}
refresh(); setInterval(refresh, 400);
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/":
            body = PAGE.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/api/state":
            # lock-copy, then send OUTSIDE the lock (round-9 guide section 4):
            # a slow client must never block the pumps
            with STATE_LOCK:
                response = {"source": STATE["source"],
                            "build_id": STATE["build_id"],
                            "connected": STATE["connected"],
                            "degraded": STATE["degraded"],
                            "protocol_errors": STATE["protocol_errors"],
                            "last_protocol_error": STATE["last_protocol_error"],
                            "last_diff": STATE["last_diff"],
                            "snapshot": STATE["snapshot"],
                            "last_result": STATE["last_result"],
                            "log": list(STATE["log"])}
            self._json(response)
        else:
            self._json({"error": "not found"}, 404)

    def do_POST(self):
        if self.path != "/api/op":
            self._json({"error": "not found"}, 404)
            return
        n = int(self.headers.get("Content-Length", 0))
        if n > MAX_COMMAND_BYTES:
            self._json({"sent": False, "status": "INVALID_REQUEST",
                        "why": "command too large"}, 400)
            return
        try:
            cmd = json.loads(self.rfile.read(n) or b"{}")
        except json.JSONDecodeError:
            self._json({"sent": False, "status": "INVALID_REQUEST",
                        "why": "bad json"}, 400)
            return
        if not isinstance(cmd, dict) or "cmd" not in cmd:
            self._json({"sent": False, "status": "INVALID_REQUEST",
                        "why": "missing cmd"}, 400)
            return
        # ESP32 demo is display-only (round-9 guide section 8): commands are
        # never forwarded to the device
        if SERIAL is not None:
            self._json({"sent": False, "status": "UNSUPPORTED_DISPLAY_ONLY"})
            return
        if PROC is not None and PROC.poll() is not None:
            with STATE_LOCK:
                STATE["connected"] = False
                STATE["degraded"] = True
            self._json({"sent": False, "status": "DISCONNECTED"})
            return
        # one command in flight: send + correlate under the COMMAND lock;
        # the STATE lock is only taken for short copies (never during I/O)
        with COMMAND_LOCK:
            with STATE_LOCK:
                before = STATE["result_seq"]
            if not send(cmd):
                self._json({"sent": False, "status": "DISCONNECTED"})
                return
            deadline = time.time() + RESPONSE_TIMEOUT
            while time.time() < deadline:
                with STATE_LOCK:
                    if STATE["result_seq"] != before:
                        out = {"sent": True, "status": "OK",
                               "result": dict(STATE["last_result"]),
                               "snapshot": STATE["snapshot"]}
                        break
                time.sleep(0.02)
            else:
                out = {"sent": True, "status": "TIMEOUT",
                       "result": None, "snapshot": None}
        self._json(out)

    def log_message(self, *a):  # keep the console quiet
        pass


def main():
    global STATE
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", nargs="?", const="build/host_demo",
                    default=None)
    ap.add_argument("--serial", default=None)
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()

    if args.serial:
        import serial  # pyserial; shipped inside the ESP-IDF python env
        global SERIAL
        SERIAL = serial.Serial(args.serial, 115200, timeout=0.2)
        STATE["source"] = "ESP32"
        threading.Thread(target=pump_serial, daemon=True).start()
    else:
        global PROC
        PROC = subprocess.Popen([args.host], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, text=True, bufsize=1)
        STATE["source"] = "HOST"
        pump_host(PROC)

    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"demo UI: http://127.0.0.1:{args.port}/  (Ctrl-C to stop)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        if PROC is not None:
            try:
                PROC.stdin.write('{"cmd":"quit"}\n')
                PROC.stdin.flush()
            except Exception:
                pass


if __name__ == "__main__":
    main()

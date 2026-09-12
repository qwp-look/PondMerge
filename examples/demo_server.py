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

STATE = {
    "snapshot": None,
    "last_result": None,
    "log": [],
    "source": "HOST",
    "connected": False,
    "build_id": "",
    "result_seq": 0,
}
LOCK = threading.Lock()
SERIAL = None
PROC = None


def push(rec):
    with LOCK:
        t = rec.get("t")
        if t == "snapshot":
            STATE["snapshot"] = rec
            STATE["connected"] = True
        elif t == "result":
            STATE["last_result"] = rec
            STATE["result_seq"] += 1
            STATE["log"].append(rec)
            del STATE["log"][:-200]
        elif t == "info":
            if rec.get("event") == "error":
                STATE["log"].append(rec)
                del STATE["log"][:-200]
            if rec.get("commit"):
                STATE["build_id"] = rec["commit"]


def pump_host(proc):
    def run():
        for line in proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                push(json.loads(line))
            except json.JSONDecodeError:
                with LOCK:
                    STATE["log"].append({"t": "info", "event": "protocol-error",
                                         "detail": line[:120]})
    threading.Thread(target=run, daemon=True).start()


def pump_serial():
    while True:
        try:
            line = SERIAL.readline().decode("utf-8", "replace").strip()
            if not line:
                continue
            if line.startswith("{"):
                try:
                    push(json.loads(line))
                except json.JSONDecodeError:
                    pass
        except Exception:
            time.sleep(0.5)
            with LOCK:
                STATE["connected"] = False


def send(cmd):
    # compact separators: the C++ demo parses with sscanf against the exact
    # '{"cmd":"...","key":value}' layout
    line = json.dumps(cmd, separators=(",", ":")) + "\n"
    if SERIAL is not None:
        SERIAL.write(line.encode())
        return {"sent": True}
    try:
        PROC.stdin.write(line)  # text=True: str, not bytes
        PROC.stdin.flush()
        return {"sent": True}
    except Exception as exc:
        return {"sent": False, "error": str(exc)}


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
 <button onclick="op('reset')">reset scene</button>
 alloc pool <input id="apool" value="0" size="2"> size <input id="asize" value="300">
 flags <select id="aflags"><option value="0">movable</option>
   <option value="2">pinned</option><option value="6">dma</option></select>
 <button onclick="op('alloc',{pool:+apool.value,size:+asize.value,flags:+aflags.value})">alloc</button>
 free id <input id="fid" value="1" size="3"> <button onclick="op('free',{id:+fid.value})">free</button>
 <button onclick="op('fragment')">make fragmentation</button>
 <button onclick="op('compact',{pool:0})">compact pool 0</button>
 <button onclick="op('compact',{pool:1})">compact pool 1</button>
 merge src <input id="msrc" value="0" size="2"> tgt <input id="mtgt" value="1" size="2">
 <button onclick="op('merge',{source:+msrc.value,target:+mtgt.value})">merge</button>
 split src <input id="ssrc" value="0" size="2"> segs <input id="sseg" value="2" size="2">
 <button onclick="op('split',{source:+ssrc.value,segments:+sseg.value})">split</button>
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
            with LOCK:
                self._json({"source": STATE["source"],
                            "build_id": STATE["build_id"],
                            "connected": STATE["connected"],
                            "snapshot": STATE["snapshot"],
                            "last_result": STATE["last_result"],
                            "log": STATE["log"]})
        else:
            self._json({"error": "not found"}, 404)

    def do_POST(self):
        if self.path != "/api/op":
            self._json({"error": "not found"}, 404)
            return
        n = int(self.headers.get("Content-Length", 0))
        try:
            cmd = json.loads(self.rfile.read(n) or b"{}")
        except json.JSONDecodeError:
            self._json({"error": "bad json"}, 400)
            return
        out = send(cmd)
        # Wait (bounded) until the source answers with a result record.
        with LOCK:
            start_seq = STATE["result_seq"]
        deadline = time.time() + 1.5
        while time.time() < deadline:
            with LOCK:
                if STATE["result_seq"] != start_seq:
                    break
            time.sleep(0.02)
        with LOCK:
            self._json({"sent": out, "last_result": STATE["last_result"],
                        "snapshot": STATE["snapshot"]})

    def log_message(self, *a):  # keep the console quiet
        pass


def main():
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

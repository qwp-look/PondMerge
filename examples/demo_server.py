#!/usr/bin/env python3
"""PondMerge v1 demo server (round-6 requirements doc section 7).

Two sources, one UI:
  --host  [path]   spawn the C++ demo process (examples/host_demo) and talk
                   to it over stdin/stdout (default: build/host_demo,
                   resolved from this script's location, not the CWD)
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
import os
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_COMMAND_BYTES = 1024
RESPONSE_TIMEOUT = 3.0

# demo process auto-restart (HOST mode): first retry 1 s after the death is
# seen, doubling per consecutive death that never produced a new session
RESTART_BACKOFF_S = 1.0
RESTART_BACKOFF_MAX_S = 30.0

# repo root derived from this script's location: the --host default must not
# depend on the directory the server happens to be started from
DEFAULT_HOST = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "build", "host_demo")

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
RESTART = {"fails": 0}          # consecutive demo deaths without a new session


def _log_protocol_error_locked(what):
    """Caller holds STATE_LOCK (push() and snapshot_diff() run under it)."""
    STATE["protocol_errors"] += 1
    STATE["last_protocol_error"] = what
    STATE["log"].append({"t": "info", "event": "protocol-error",
                         "detail": what})
    del STATE["log"][:-200]


def log_protocol_error(what):
    with STATE_LOCK:
        _log_protocol_error_locked(what)


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
            RESTART["fails"] = 0    # a live session: the demo is healthy again
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
                _log_protocol_error_locked("stale_snapshot")
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
                _log_protocol_error_locked("moved_invariant_violation")
    return diff


def spawn_host(host_path):
    """Spawn the C++ demo process and start its pump thread. Returns the new
    Popen (the initial PROC in main, or the auto-restart chain)."""
    proc = subprocess.Popen([host_path], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, text=True, bufsize=1)
    pump_host(proc, host_path)
    return proc


def restart_host(host_path):
    """Bring the demo process back after the pump saw it die. The delay
    doubles per consecutive death that never produced a new session, so a
    crashing or missing binary rate-limits itself instead of spin-restarting;
    every attempt is logged so the UI shows why the source flapped."""
    while True:
        with STATE_LOCK:
            fails = RESTART["fails"]
            # cap the shift as well as the delay: fails is unbounded
            delay = min(RESTART_BACKOFF_S * (2 ** min(fails, 16)),
                        RESTART_BACKOFF_MAX_S)
            RESTART["fails"] = fails + 1
            STATE["log"].append({"t": "info", "event": "restart-scheduled",
                                 "detail": "%s in %ds" % (host_path, delay)})
            del STATE["log"][:-200]
        time.sleep(delay)   # daemon pump thread; no lock held while waiting
        try:
            spawn_host(host_path)
            return
        except Exception as e:
            with STATE_LOCK:
                STATE["log"].append({"t": "info", "event": "restart-failed",
                                     "detail": repr(e)})
                del STATE["log"][:-200]


def pump_host(proc, host_path):
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
        try:  # reap the child so no zombie lingers while the UI keeps running
            proc.wait(timeout=2)
        except Exception:
            try:
                proc.kill()
                proc.wait(timeout=1)
            except Exception:
                pass
        if host_path:   # the UI outlives the demo: schedule a restart
            restart_host(host_path)
    threading.Thread(target=run, daemon=True).start()


def pump_serial():
    last_err = None
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
        except Exception as e:
            # a failing port must not disappear into this except: surface
            # each distinct failure in the UI log (deduped -- the port error
            # repeats identically, and the log is a 200-record ring)
            if repr(e) != last_err:
                last_err = repr(e)
                with STATE_LOCK:
                    STATE["log"].append({"t": "info", "event": "serial-error",
                                         "detail": last_err})
                    del STATE["log"][:-200]
            time.sleep(0.5)
            with STATE_LOCK:
                STATE["connected"] = False
                STATE["degraded"] = True


def send(cmd):
    line = json.dumps(cmd, separators=(",", ":")) + "\n"
    if SERIAL is not None:
        payload = line.encode()
        try:
            n = SERIAL.write(payload)
            SERIAL.flush()  # wait until the bytes really left for the device
        except Exception:
            with STATE_LOCK:
                STATE["connected"] = False
                STATE["degraded"] = True
            return False
        # pyserial returns the byte count (None on ports that cannot report
        # one): anything short of the whole line is a failed send, not success
        ok = n is None or n == len(payload)
        if not ok:
            with STATE_LOCK:
                STATE["connected"] = False
                STATE["degraded"] = True
        return ok
    try:
        PROC.stdin.write(line)  # text=True: str, not bytes
        PROC.stdin.flush()
        return True
    except Exception:
        with STATE_LOCK:
            STATE["connected"] = False
            STATE["degraded"] = True
        return False


PAGE = """<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PondMerge 演示</title><style>
:root{--bg:#0b0e14;--panel:#11161f;--panel2:#161c26;--border:#242c39;
      --text:#dbe2ea;--muted:#8b96a5;--accent:#4f8cff;--ok:#3fb950;
      --warn:#d29922;--err:#f85149;--mono:ui-monospace,SFMono-Regular,Consolas,monospace}
*{box-sizing:border-box}
body{font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;
     margin:0;background:var(--bg);color:var(--text);font-size:14px;line-height:1.5}
.wrap{max-width:1180px;margin:0 auto;padding:20px 24px 40px}
header{display:flex;align-items:baseline;gap:14px;flex-wrap:wrap;margin-bottom:14px}
h1{font-size:22px;margin:0;letter-spacing:.5px}
h1 small{color:var(--muted);font-weight:400;font-size:13px;margin-left:8px}
.chips{display:flex;gap:8px;margin-left:auto;flex-wrap:wrap}
.chip{background:var(--panel2);border:1px solid var(--border);border-radius:999px;
      padding:2px 12px;font-size:12px;color:var(--muted)}
.chip b{color:var(--text);font-weight:600}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:5px;
     background:var(--muted);vertical-align:1px}
.dot.on{background:var(--ok);box-shadow:0 0 6px var(--ok)}
.dot.off{background:var(--err)}
.banner{border:1px solid;border-radius:8px;padding:10px 14px;font-size:12.5px;
        margin:0 0 14px;color:var(--muted)}
.banner b{color:var(--text)}
.banner.warnbox{border-color:#5a4a1a;background:#191509}
.banner.ro{border-color:#5a2a2a;background:#190d0d;display:none}
.grid{display:grid;grid-template-columns:330px 1fr;gap:16px;align-items:start}
@media(max-width:860px){.grid{grid-template-columns:1fr}}
.card{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:14px 16px;margin-bottom:16px}
.card h2{font-size:13px;margin:0 0 10px;color:var(--muted);font-weight:600;
         letter-spacing:.8px;text-transform:uppercase}
.group{border-top:1px dashed var(--border);padding:10px 0}
.group:first-of-type{border-top:none;padding-top:2px}
.group .gt{font-size:12px;color:var(--accent);margin-bottom:6px;font-weight:600}
button{margin:2px 4px 2px 0;padding:5px 12px;border-radius:6px;font-size:13px;
       border:1px solid var(--border);background:var(--panel2);color:var(--text);cursor:pointer}
button:hover:not(:disabled){border-color:var(--accent);color:#fff}
button:disabled{opacity:.4;cursor:not-allowed}
button.primary{background:#1a2d52;border-color:#2c4a86}
button.danger{background:#3a1414;border-color:#6e2a2a}
button.danger:hover:not(:disabled){border-color:var(--err)}
button.ghost{background:transparent}
input,select{background:#0d1117;border:1px solid var(--border);border-radius:6px;
       color:var(--text);padding:4px 8px;font-size:13px}
input{width:76px}input.wide{width:100px}
label.inline{font-size:12.5px;color:var(--muted);margin-right:4px}
.qline{display:flex;align-items:center;gap:6px;font-size:12.5px;color:var(--muted);margin-top:6px}
.qline input{width:auto}
.legend{display:flex;gap:14px;flex-wrap:wrap;font-size:12px;color:var(--muted);margin-bottom:10px}
.legend .sw{display:inline-block;width:14px;height:12px;border-radius:3px;margin-right:5px;vertical-align:-1px}
.poolhead{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin:14px 0 6px;font-size:13px}
.poolhead .pid{font-weight:700;font-size:14px}
.stat{background:var(--panel2);border:1px solid var(--border);border-radius:6px;
      padding:1px 9px;font-size:11.5px;color:var(--muted)}
.stat b{color:var(--text);font-weight:600}
.stat.hot b{color:var(--warn)}
.lane{display:flex;height:46px;border:1px solid var(--border);border-radius:8px;
      overflow:hidden;margin:4px 0 4px;position:relative;background:#0d1117}
.blk{height:100%;box-sizing:border-box;border-right:1px solid var(--bg);
     overflow:hidden;font-size:10.5px;font-family:var(--mono);white-space:nowrap;
     display:flex;align-items:center;justify-content:center;color:#fff;min-width:2px}
.MOVABLE{background:linear-gradient(180deg,#3a76d6,#2b5cb0)}
.PINNED{background:linear-gradient(180deg,#d43a3a,#a32626)}
.FREE{background:repeating-linear-gradient(45deg,#2a303b,#2a303b 6px,#232833 6px,#232833 12px);color:var(--muted)}
.SLACK{background:#8a7a1f}
.diff{border:1px solid #2c4a86;background:#101a2c;border-radius:8px;padding:8px 12px;
      margin:10px 0;font-size:12.5px;display:none}
table{border-collapse:collapse;font-size:12.5px;width:100%}
td,th{border:1px solid var(--border);padding:4px 10px;text-align:left}
th{color:var(--muted);font-weight:600;background:var(--panel2)}
tr:hover td{background:#141a24}
.v0{color:var(--muted)}.v1{color:var(--ok);font-weight:700}.v2{color:var(--warn);font-weight:700}
.v3{color:var(--muted)}.v4,.v5{color:var(--err);font-weight:700}
#res{font-family:var(--mono);font-size:12px;background:#0d1117;border:1px solid var(--border);
     border-radius:8px;padding:10px;overflow-x:auto;white-space:pre-wrap;margin:0}
.resnote{font-size:12.5px;border-radius:6px;padding:6px 10px;margin:8px 0;display:none}
.resnote.qwarn{border:1px solid #5a4a1a;background:#191509;color:var(--warn)}
.resnote.qok{border:1px solid #1d4429;background:#0c1a10;color:var(--ok)}
.statusline{font-size:13px;margin-bottom:8px}
.badge{display:inline-block;padding:1px 10px;border-radius:999px;font-size:12px;font-weight:700}
.badge.ok{background:#0c1a10;border:1px solid #1d4429;color:var(--ok)}
.badge.err{background:#1a0c0c;border:1px solid #5a2a2a;color:var(--err)}
#log{height:150px;overflow-y:scroll;background:#0d1117;border:1px solid var(--border);
     border-radius:8px;padding:8px 10px;font-family:var(--mono);font-size:11.5px}
#log .ev{color:var(--accent)}#log .dt{color:var(--muted)}
.mono{font-family:var(--mono)}
</style></head><body><div class="wrap">
<header>
 <h1>PondMerge 演示<small>v1.2.0 · 托管内存：池 · 逻辑引用 · 显式整理</small></h1>
 <div class="chips">
  <span class="chip">来源 <b id="src">-</b></span>
  <span class="chip">构建 <b id="build">-</b></span>
  <span class="chip"><span class="dot" id="condot"></span>连接 <b id="conn">-</b></span>
  <span class="chip">协议错误 <b id="perr">0</b></span>
 </div>
</header>
<div class="banner warnbox"><b>安全与生命周期提醒：</b>
 裸 <code>T*</code> 不会跟随搬迁 · 借用期间整理返回 Busy · DMA/ISR/外部持有者必须由
 <i>你</i> 停止 · pinned 对象可能挡住整理 · <code>RawRef</code> 是可伪造的低层句柄 ·
 <code>resolve/peek</code> 返回的指针不是 RAII 借用 · 建议只是分析、不是整理保证 ·
 静默期复选框只是声明——库看不见 DMA、ISR 或任何外部持有者。</div>
<div id="ro-banner" class="banner ro"><b>仅显示模式：</b>
 ESP32 演示固件按自己的脚本推进场景，浏览器命令不会转发到设备——
 下方所有变更类按钮已禁用。</div>
<div class="grid">
<div>
 <div class="card">
  <h2>控制台</h2>
  <div class="group"><div class="gt">对象操作</div>
   <label class="inline">池</label><input id="apool" value="0" size="2">
   <label class="inline">大小</label><input id="asize" value="300" class="wide">
   <label class="inline">对齐</label><input id="aalign" value="8" size="2">
   <label class="inline">类型</label>
   <select id="aflags"><option value="0">可搬移</option>
     <option value="2">钉住 pinned</option><option value="6">DMA（钉住）</option></select>
   <button class="primary" data-mutating onclick="op('alloc',{pool:+apool.value,size:+asize.value,align:+aalign.value,flags:+aflags.value})">分配</button>
  </div>
  <div class="group"><div class="gt">释放</div>
   <label class="inline">对象 id</label><input id="fid" value="1" size="4">
   <button data-mutating onclick="op('free',{id:+fid.value})">释放</button>
  </div>
  <div class="group"><div class="gt">场景与维护</div>
   <button data-mutating onclick="op('fragment')">制造碎片</button>
   <button data-mutating onclick="op('compact',{pool:0})">整理池 0</button>
   <button data-mutating onclick="op('compact',{pool:1})">整理池 1</button>
   <button class="ghost" data-mutating onclick="op('reset')">重置场景</button>
  </div>
  <div class="group"><div class="gt">池结构</div>
   <label class="inline">合并 源</label><input id="msrc" value="0" size="2">
   <label class="inline">→ 目标</label><input id="mtgt" value="1" size="2">
   <button data-mutating onclick="op('merge',{source:+msrc.value,target:+mtgt.value})">合并</button>
   <br><label class="inline">切分 源</label><input id="ssrc" value="0" size="2">
   <label class="inline">段数</label><input id="sseg" value="2" size="2">
   <button data-mutating onclick="op('split',{source:+ssrc.value,segments:+sseg.value})">切分</button>
  </div>
  <div class="group"><div class="gt">整理建议（只读，绝不改动池）</div>
   <label class="inline">池</label><input id="apol" value="0" size="2">
   <label class="inline">期望大小</label><input id="asize2" value="0" class="wide">
   <button class="ghost" onclick="op('advice',{pool:+apol.value,size:+asize2.value,align:8})">分析</button>
  </div>
  <div class="qline"><input type="checkbox" id="qchk">
   我已停止 DMA / ISR / 外部持有者（<b>纯标注</b>：随请求发不加字段，库无法看见外部访问者）</div>
  <div class="group"><div class="gt">危险区</div>
   <button class="danger" onclick="sendQuit()">结束协议进程</button>
  </div>
 </div>
</div>
<div>
 <div class="card">
  <h2>内存布局（Auto Zone，按比例）</h2>
  <div class="legend">
   <span><span class="sw MOVABLE"></span>可搬移</span>
   <span><span class="sw PINNED"></span>钉住 / DMA / 外部</span>
   <span><span class="sw FREE"></span>空闲</span>
   <span><span class="sw SLACK"></span>松弛（不可用）</span>
   <span>悬停块可看 offset / id / generation / epoch</span>
  </div>
  <div id="lanes"></div>
  <div id="diff" class="diff"></div>
 </div>
 <div class="card">
  <h2>整理建议</h2>
  <div id="advice"></div>
 </div>
 <div class="card">
  <h2>对象</h2>
  <div id="objs"></div>
 </div>
</div>
</div>
<div class="card">
 <h2>最近结果</h2>
 <div class="statusline" id="resline" style="display:none">
   <span class="badge" id="resbadge"></span> <span id="resop" class="mono"></span></div>
 <div id="resnote" class="resnote"></div>
 <pre id="res">-</pre>
</div>
<div class="card">
 <h2>操作日志</h2>
 <div id="log"></div>
</div>
</div>
<script>
let last = null, prev = null, lastOp = null, lastQ = null, diffData = null;
// All dynamic text goes through textContent (round-7 guide section 9): no
// protocol field, build id or error detail is ever interpolated into HTML.
const FMT = n => Number(n).toLocaleString('en-US');
const POOL_STATE = ['空','运行中','已暂停','整理中','合并中','切分中'];
const VERDICTS = ['无需整理','建议整理','被阻塞','难有收益','元数据损坏','请求无效'];
const FLAGS = [[1,'可搬移'],[2,'钉住'],[4,'DMA'],[8,'外部'],[16,'零初始化']];
const EVENTS = {ready:'就绪','process-exited':'进程退出','restart-scheduled':'计划重启',
                'restart-failed':'重启失败','serial-error':'串口错误'};
function el(tag, cls, text){
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
}
function flagsText(f){
  const parts = [];
  for (const [bit, name] of FLAGS) if (f & bit) parts.push(name);
  return parts.length ? parts.join('+') : '-';
}
function op(cmd, params){
  lastOp = cmd;
  lastQ = document.getElementById('qchk').checked;
  diffData = null; // the next maintenance op earns a fresh before/after diff
  return fetch('/api/op',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify(Object.assign({cmd:cmd}, params||{}))})
  .then(r=>r.json()).then(j=>{
    // surface refusals the user would otherwise never see (display-only
    // refusals, oversized commands, server-side validation)
    const res = document.getElementById('res');
    if (j.status && j.status !== 'OK'){
      res.textContent = j.status + (j.why ? (': ' + j.why) : '');
    }
    showQuiescenceNote();
    return j;
  }).then(refresh);
}
function showQuiescenceNote(){
  const note = document.getElementById('resnote');
  const maint = (lastOp === 'compact' || lastOp === 'merge' || lastOp === 'split');
  note.className = 'resnote';
  if (!maint){ note.style.display = 'none'; return; }
  note.style.display = 'block';
  if (lastQ){
    note.classList.add('qok');
    note.textContent = '已声明静默期（标注）。记住：声明本身什么也不做——真实驱动必须真的排空 DMA/ISR/外部持有者。';
  } else {
    note.classList.add('qwarn');
    note.textContent = '⚠ 未声明静默期。真实驱动必须先停止 DMA/ISR/其他任务/外部裸指针，再执行维护操作——库无法替你发现它们。';
  }
}
function sendQuit(){ op('quit'); }
function refresh(){
  fetch('/api/state').then(r=>r.json()).then(s=>{
    document.getElementById('src').textContent = s.source;
    document.getElementById('build').textContent = s.build_id || '-';
    document.getElementById('conn').textContent = s.connected ? '已连接' : '未连接';
    document.getElementById('condot').className = 'dot ' + (s.connected ? 'on' : 'off');
    document.getElementById('perr').textContent = String(s.protocol_errors || 0);
    // display-only (ESP32 serial): mutating controls disabled + visible
    // banner (DEMO_REQUIREMENTS section 6)
    const ro = s.source === 'ESP32';
    document.querySelectorAll('button[data-mutating]').forEach(b=>{b.disabled = ro;});
    const banner = document.getElementById('ro-banner');
    banner.style.display = ro ? 'block' : 'none';
    const sn = s.snapshot;
    if (!sn) return;
    renderLanes(sn);
    renderAdvice(sn);
    renderObjects(sn);
    document.getElementById('res').textContent =
      JSON.stringify(s.last_result, null, 1);
    const lr = s.last_result || {};
    const line = document.getElementById('resline');
    if (lr.status){
      line.style.display = 'block';
      const badge = document.getElementById('resbadge');
      badge.textContent = lr.status;
      badge.className = 'badge ' + (lr.status === 'OK' ? 'ok' : 'err');
      document.getElementById('resop').textContent = 'op: ' + (lr.op || '-');
    }
    renderLog(s.log);
    // The before/after diff is computed ONCE, from the pre-op snapshot, and
    // then stays visible until the next op() clears it -- a 400 ms flash
    // would be unreadable.
    const isMaint = lastOp === 'compact' || lastOp === 'merge' || lastOp === 'split';
    if (isMaint && prev){
      diffData = buildDiffText(sn, prev);
      lastOp = null;
    }
    prev = sn;
    const box = document.getElementById('diff');
    if (diffData){
      box.style.display = 'block';
      box.textContent = '整理前后对比 — ' + diffData;
    } else {
      box.style.display = 'none';
    }
  }).catch(()=>{
    document.getElementById('conn').textContent = '未连接';
    document.getElementById('condot').className = 'dot off';
  });
}
function renderLanes(sn){
  const lanes = document.getElementById('lanes');
  lanes.textContent = '';
  for (const p of sn.pools){
    const stranded = Math.max(0, (p.free_bytes||0) - (p.largest_free_block||0));
    const head = el('div', 'poolhead');
    head.appendChild(el('span', 'pid', '池 ' + p.pool_id));
    head.appendChild(el('span', 'stat', '段 ' + p.segment_first + '–' +
                       (p.segment_first + p.segment_count - 1)));
    head.appendChild(el('span', 'stat', POOL_STATE[p.state] || ('状态 ' + p.state)));
    head.appendChild(el('span', 'stat', '已用 ' + FMT(p.used_bytes) + ' B'));
    head.appendChild(el('span', 'stat', '空闲 ' + FMT(p.free_bytes) + ' B'));
    head.appendChild(el('span', 'stat', '最大空闲块 ' + FMT(p.largest_free_block) + ' B'));
    if (stranded > 0)
      head.appendChild(el('span', 'stat hot', '搁浅 ' + FMT(stranded) + ' B'));
    if (p.fragment_bytes > 0)
      head.appendChild(el('span', 'stat hot', '碎片 ' + FMT(p.fragment_bytes) + ' B'));
    head.appendChild(el('span', 'stat', '对象 ' + p.live_objects));
    if (p.borrow_count > 0)
      head.appendChild(el('span', 'stat hot', '借用中 ' + p.borrow_count));
    head.appendChild(el('span', 'stat', '纪元 ' + p.structure_epoch));
    lanes.appendChild(head);
    const cap = p.capacity || 1;
    const lane = el('div', 'lane');
    for (const b of p.blocks){
      const live = (b.kind === 'MOVABLE' || b.kind === 'PINNED');
      const d = el('div', 'blk ' + b.kind);
      d.style.width = Math.max(0.4, 100*b.size/cap).toFixed(2) + '%';
      if (100*b.size/cap > 7){
        d.textContent = live ? ('#' + b.object_id + ' · ' + FMT(b.size) + 'B')
                             : (b.kind === 'FREE' ? '空闲 ' + FMT(b.size) + 'B' : b.kind);
      } else if (live){
        d.textContent = '#' + b.object_id;
      }
      d.title = 'offset ' + b.offset + ' · ' + FMT(b.size) + ' B · ' + b.kind +
        (b.object_id !== undefined
          ? ' · id ' + b.object_id + ' · gen ' + b.generation +
            ' · epoch ' + b.address_epoch + ' · flags ' + flagsText(b.flags)
          : '');
      lane.appendChild(d);
    }
    lanes.appendChild(lane);
  }

}
function buildDiffText(sn, before){
  const rows = [];
  for (const p of sn.pools){
    const q = before.pools.find(x => x.pool_id === p.pool_id);
    if (!q) continue;
    const bStrand = Math.max(0, (q.free_bytes||0) - (q.largest_free_block||0));
    const aStrand = Math.max(0, (p.free_bytes||0) - (p.largest_free_block||0));
    if (q.largest_free_block === p.largest_free_block && bStrand === aStrand &&
        q.structure_epoch === p.structure_epoch) continue;
    rows.push('池 ' + p.pool_id + '：最大空闲块 ' + FMT(q.largest_free_block) +
              ' → ' + FMT(p.largest_free_block) + ' B · 搁浅 ' + FMT(bStrand) +
              ' → ' + FMT(aStrand) + ' B · 纪元 ' + q.structure_epoch +
              ' → ' + p.structure_epoch);
  }
  return rows.join('　|　');
}
function renderAdvice(sn){
  const adv = document.getElementById('advice');
  adv.textContent = '';
  const at = el('table');
  const ah = el('tr');
  for (const h of ['池','判定','碎片率 ‰','借用','钉住','需自建静默期','预计搬移'])
    ah.appendChild(el('th', null, h));
  at.appendChild(ah);
  for (const a of sn.advice){
    const row = el('tr');
    row.appendChild(el('td', null, String(a.pool_id)));
    const vd = el('td');
    vd.appendChild(el('b', VERDICTS[a.verdict] ? ('v' + a.verdict) : null,
                     VERDICTS[a.verdict] || ('verdict ' + a.verdict)));
    row.appendChild(vd);
    row.appendChild(el('td', null, String(a.fragment_ratio_permille)));
    row.appendChild(el('td', null, String(a.borrow_count)));
    row.appendChild(el('td', null, String(a.has_pinned_objects)));
    row.appendChild(el('td', null, a.caller_must_establish_quiescence ? '是' : '否'));
    row.appendChild(el('td', null,
      a.estimated_moved_bytes > 0 ? FMT(a.estimated_moved_bytes) + ' B' : '0'));
    at.appendChild(row);
  }
  adv.appendChild(at);
}
function renderObjects(sn){
  const objs = document.getElementById('objs');
  objs.textContent = '';
  const ot = el('table');
  const oh = el('tr');
  for (const h of ['id','代','池','偏移','大小','块','纪元','标志','摘要'])
    oh.appendChild(el('th', null, h));
  ot.appendChild(oh);
  for (const o of sn.objects){
    const row = el('tr');
    const vals = [o.object_id, o.generation, o.pool_id, o.address_offset,
                  FMT(o.size) + ' B', FMT(o.block_size) + ' B', o.address_epoch,
                  flagsText(o.flags), o.payload_digest];
    for (const v of vals) row.appendChild(el('td', null, String(v)));
    ot.appendChild(row);
  }
  objs.appendChild(ot);
}
function renderLog(log){
  const box = document.getElementById('log');
  box.textContent = '';
  for (const r of log.slice(-40).reverse()){
    const line = el('div');
    const ev = EVENTS[r.event] || r.event || JSON.stringify(r);
    line.appendChild(el('span', 'ev', '[' + ev + '] '));
    if (r.detail) line.appendChild(el('span', 'dt', r.detail));
    if (r.why) line.appendChild(el('span', 'dt', r.why));
    box.appendChild(line);
  }
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

    def _host_ok(self):
        # DNS-rebinding guard: the server is loopback-only, so a Host header
        # naming anything else is not this server's page's origin.
        host = (self.headers.get("Host") or "").split(":")[0].strip().lower()
        return host in ("127.0.0.1", "localhost")

    def do_GET(self):
        if not self._host_ok():
            self._json({"error": "forbidden"}, 403)
            return
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
        if not self._host_ok():
            self._json({"error": "forbidden"}, 403)
            return
        if self.path != "/api/op":
            self._json({"error": "not found"}, 404)
            return
        # CSRF guard: browsers send text/plain (and friends) cross-site
        # WITHOUT a preflight; application/json is not CORS-safelisted, so
        # requiring it here makes every forged cross-site POST fail its
        # preflight. The UI and the smokes always send this header.
        ctype = (self.headers.get("Content-Type") or
                 "").split(";")[0].strip().lower()
        if ctype != "application/json":
            self._json({"sent": False, "status": "INVALID_REQUEST",
                        "why": "content-type must be application/json"}, 415)
            return
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._json({"sent": False, "status": "INVALID_REQUEST",
                        "why": "bad content-length"}, 400)
            return
        if n <= 0 or n > MAX_COMMAND_BYTES:
            self._json({"sent": False, "status": "INVALID_REQUEST",
                        "why": "bad content-length"}, 400)
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
        # the STATE lock is only taken for short copies (never during I/O).
        # Completion is ANY observed progress belonging to this command --
        # result_seq for answering commands, last_seq for reset (which
        # produces a new session's snapshot, not a result record) and
        # connected=False for quit (process exit). Without that, reset/quit
        # always surfaced as a 3 s TIMEOUT although they had succeeded.
        with COMMAND_LOCK:
            with STATE_LOCK:
                before = (STATE["result_seq"], STATE["last_seq"])
            if not send(cmd):
                self._json({"sent": False, "status": "DISCONNECTED"})
                return
            is_quit = cmd["cmd"] == "quit"
            deadline = time.time() + RESPONSE_TIMEOUT
            while time.time() < deadline:
                with STATE_LOCK:
                    progressed = (STATE["result_seq"],
                                  STATE["last_seq"]) != before
                    if is_quit and not STATE["connected"]:
                        out = {"sent": True, "status": "OK",
                               "result": None,
                               "snapshot": STATE["snapshot"]}
                        break
                    if progressed:
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
    ap.add_argument("--host", nargs="?", const=DEFAULT_HOST, default=DEFAULT_HOST,
                    help="path to the host_demo process (default: "
                         "build/host_demo under the repository root)")
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
        STATE["source"] = "HOST"
        PROC = spawn_host(args.host)

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
            try:  # do not leave an orphan behind on Ctrl-C
                PROC.wait(timeout=2)
            except Exception:
                PROC.kill()
                try:
                    PROC.wait(timeout=1)
                except Exception:
                    pass


if __name__ == "__main__":
    main()

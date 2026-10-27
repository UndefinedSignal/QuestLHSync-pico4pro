'use strict';
// lhsight for the PICO 4 Pro: stream bright spots from the tracking cameras (see magisk/src/lhsight.c). Runs inside
// pvrtrackingservice; reads only. Lines go to stdout via console.log:  F ... (frames)  T ... (heartbeat)
// QuestLHSync: lhsyncd runs this with frida-inject while a PC listens, and stops it after (MAX_MS 0: no limit)
var MAX_MS = parseInt("@DUR@");
// The PICO 4 Pro's four OV9282 tracking cameras deliver 640x480 frames of 16-bit pixels (10 bits used) into dmabuf
// rings that pvrtrackingservice maps: 32 buffers of 618496 bytes (151 pages: a 1280-byte embedded-data row with the
// frame counter, then the 480 image rows), 8 per camera, allocated camera by camera. CAMMAP gives the camera id of each
// ring in buffer order ("g0,g1,g2,g3" -> the ids in the camera calibration); lhsyncd fills it in from cammap.txt.
// On a PICO 4 Pro (5.13.3, checked by covering each camera) the rings come in reverse: bottom-left (cam3),
// bottom-right (cam2), top-right (cam1), top-left (cam0); the calibration's IMU frame has x up, y left, z back.
var CAMMAP_SPEC = "@CAMMAP@";
var CAMMAP = (CAMMAP_SPEC.charAt(0) === "@" ? "3,2,1,0" : CAMMAP_SPEC).split(",").map(function (v) { return parseInt(v, 10); });
var BUF = 618496, W = 640, H = 480, HDR = 1280, NCAM = 4;
var l = Process.getModuleByName("libc.so"), g = function (n) { return l.getExportByName(n); };
var fopen = new NativeFunction(g("fopen"), "pointer", ["pointer", "pointer"]);
var fgets = new NativeFunction(g("fgets"), "pointer", ["pointer", "int", "pointer"]);
var fclose = new NativeFunction(g("fclose"), "int", ["pointer"]);
var usleep = new NativeFunction(g("usleep"), "int", ["uint"]);
var clock_gettime = new NativeFunction(g("clock_gettime"), "int", ["int", "pointer"]);
// Keep every Memory.alloc in a global: JS GC frees unreferenced allocations under the C code.
var stateBuf = Memory.alloc(16384);
var cm = new CModule(`@CSRC@`, { clock_gettime: g("clock_gettime"), memcpy: g("memcpy"), snprintf: g("snprintf"), S: stateBuf });
var F = function (n, r, a) { return new NativeFunction(cm[n], r, a); };
var add_slot16 = F("add_slot16", "void", ["pointer", "int", "int", "int"]);
var set_bufs = F("set_bufs", "void", ["pointer", "pointer", "pointer", "pointer", "int"]);
var prime = F("prime", "void", []), poll = F("poll", "int", []), reset_slots = F("reset_slots", "void", []);
var drain = F("drain", "int", ["pointer", "int"]);
var n_drop = F("n_drop", "uint", []), scan_ns = F("scan_ns", "uint64", []), n_scan = F("n_scan", "uint", []);
var n_poll = F("n_poll", "uint", []), idle_us = F("idle_us", "int", []);
var scratchBuf = Memory.alloc(W * H), cellBuf = Memory.alloc((W >> 4) * (H >> 4) * 2);
var labBuf = Memory.alloc((W >> 4) * (H >> 4) * 4);
var outBuf = Memory.alloc(1 << 20), drainBuf = Memory.alloc(1 << 16), tsBuf = Memory.alloc(16);
set_bufs(scratchBuf, cellBuf, labBuf, outBuf, 64);
function discover() {
  reset_slots();
  var fp = fopen(Memory.allocUtf8String("/proc/self/maps"), Memory.allocUtf8String("r")), buf = Memory.alloc(512), all = [];
  while (!fgets(buf, 512, fp).isNull()) {
    var m = /^([0-9a-f]+)-([0-9a-f]+)\s+r\S+\s.*?dmabuf(?::dmabuf(\d+))?/.exec(buf.readCString());
    if (!m) continue;
    var a = parseInt(m[1], 16), b = parseInt(m[2], 16);
    all.push({ a: a, size: b - a, id: m[3] === undefined ? -1 : parseInt(m[3], 10) });
  }
  fclose(fp);
  var regs = all.filter(function (r) { return r.size === BUF; });
  if (!regs.length || regs.length % NCAM) {
    var hist = {};
    all.forEach(function (r) { hist[r.size] = (hist[r.size] || 0) + 1; });
    console.log("W " + regs.length + " camera buffers of " + BUF + " bytes, not " + NCAM + " equal rings; dmabuf sizes here: " +
                (Object.keys(hist).map(function (s) { return "0x" + (+s).toString(16) + "x" + hist[s]; }).join(" ") || "none"));
    if (!regs.length) return 0;
  }
  // the rings are allocated camera by camera: by dmabuf name when there is one, else by address (seen ascending)
  if (regs.every(function (r) { return r.id >= 0; })) regs.sort(function (x, y) { return x.id - y.id; });
  else regs.sort(function (x, y) { return x.a - y.a; });
  var per = Math.max(1, Math.floor(regs.length / NCAM)), n = 0, got = {};
  regs.forEach(function (r, k) {
    var ring = Math.min(NCAM - 1, Math.floor(k / per)), cam = CAMMAP[ring];
    if (!(cam >= 0)) return;  // a ring mapped to -1: not scanned
    add_slot16(ptr(r.a).add(HDR), W, H, cam);
    got[cam] = (got[cam] || 0) + 1;
    n++;
  });
  console.log("I cameras " + Object.keys(got).map(function (c) { return c + ":" + got[c]; }).join(" ") + " (rings " + CAMMAP.join(",") + ")");
  prime();
  return n;
}
function mono_us() { clock_gettime(1, tsBuf); return tsBuf.readS64().toNumber() * 1e6 + Math.floor(tsBuf.add(8).readS64().toNumber() / 1000); }
var pending = "";
function flush() {
  for (;;) {
    var n = drain(drainBuf, 65536);
    if (n <= 0) break;
    pending += drainBuf.readUtf8String(n);
  }
  var cut = pending.lastIndexOf("\n");
  if (cut >= 0) { console.log(pending.slice(0, cut)); pending = pending.slice(cut + 1); }
}
console.log("I slots " + discover() + " max_ms " + MAX_MS);
var t0 = Date.now(), lastBeat = 0, faults = 0;
function step() {
  var budget = Date.now() + 20;
  while (Date.now() < budget) {
    try { poll(); } catch (e) { faults++; usleep(200000); console.log("W fault " + e.message + " -> " + discover() + " slots"); }
    usleep(Math.max(1000, idle_us()));
  }
  flush();
  if (Date.now() - lastBeat >= 1000) {
    lastBeat = Date.now();
    var ns = n_scan();
    console.log("T " + mono_us() + " scans " + ns + " scan_us " + (scan_ns().valueOf() / 1e3 / Math.max(1, ns)).toFixed(0) + " drops " + n_drop() + " faults " + faults + " polls " + n_poll());
  }
  if (!MAX_MS || Date.now() - t0 < MAX_MS) setImmediate(step);
  else console.log("I done");
}
setImmediate(step);

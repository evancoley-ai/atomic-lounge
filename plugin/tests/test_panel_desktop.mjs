// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk VST3: the tablet panel with the desktop bridge, in headless Chrome (the plugin's WebView2
// is Chromium too). Real browser mouse input (CDP Input.dispatchMouseEvent), no touch:
//   * plugin/web/desktop_shim.js: window.AndroidSynth over a fake JUCE backend; boot shows the host's
//     sound (MFHostInit) instead of F01; edits go out as 'patch' events with the host serial; the preset
//     entry goes out as 'ui'; host pushes (MFHost.patch / restore) land on the panel without an echo;
//     computer-keyboard notes; save / delete keep the local list.
//   * mouse: drag, ctrl-drag (fine), wheel, shift-wheel (fine), double-click = default, the keyboard.
//   node plugin/tests/test_panel_desktop.mjs
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath, pathToFileURL } from 'node:url';

const require = createRequire(import.meta.url);
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const WWW = fs.existsSync(path.join(ROOT, 'panel/index.html')) ? path.join(ROOT, 'panel') : path.join(ROOT, 'panel');
const Core = require(path.join(WWW, 'js/mf_core.js'));
const M = require(path.join(WWW, 'js/panel_model.js'));
const F = require(path.join(WWW, 'js/factory.js'));
let pass = 0, fail = 0;
function check(ok, what) { if (ok) pass++; else fail++; console.log(`${ok ? '  ok  ' : '  FAIL'} ${what}`); }

const CHROME = ['C:/Program Files/Google/Chrome/Application/chrome.exe', 'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'].find((c) => fs.existsSync(c));
if (!CHROME) { console.log('no Chrome / Edge: skipped'); process.exit(0); }

// the host's state the plugin would inject: factory 6 with an edited cutoff, marked edited
const bank = F.build(Core, M);
const hostPatch = M.clone(bank[6]);
hostPatch.filt[0].cutoff = 12345;
const init = {
  state: { b64: Core.b64encode(Core.mfpWrite(hostPatch)), ui: { kind: 'F', name: bank[6].name, dirty: true }, serial: 7 },
  presets: [{ name: 'MY PAD', b64: Core.b64encode(Core.mfpWrite(bank[3])) }],
  status: { voices: 3, midi: 5, sampleRate: 48000, latencyMs: 2.17, midiDevices: ['HOST MIDI'] },
};
const fakeJuce = `window.__JUCE__ = { backend: { emitEvent(id, o) { (window.__sent = window.__sent || []).push({ id, o: JSON.parse(JSON.stringify(o)) }); } } };`;
const shim = fs.readFileSync(path.join(ROOT, 'plugin/web/desktop_shim.js'), 'utf8');
const userScript = `${fakeJuce}\nwindow.MFHostInit = ${JSON.stringify(init)};\n${shim}`;

const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'mfm1-desk-'));
const proc = spawn(CHROME, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${prof}`, '--no-first-run',
  '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1', 'about:blank'], { stdio: 'ignore' });
try {
  let port = null;
  for (let i = 0; i < 100 && !port; i++) {
    await new Promise((r) => setTimeout(r, 100));
    try { port = fs.readFileSync(path.join(prof, 'DevToolsActivePort'), 'utf8').split('\n')[0].trim(); } catch (e) { /* not yet */ }
  }
  if (!port) throw new Error('Chrome did not start');
  const targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
  const ws = new WebSocket(targets.find((t) => t.type === 'page').webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  let id = 0; const pending = new Map(); const errors = [];
  ws.onmessage = (m) => {
    const msg = JSON.parse(m.data);
    if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); }
    if (msg.method === 'Runtime.exceptionThrown') errors.push(msg.params.exceptionDetails.exception?.description || msg.params.exceptionDetails.text);
  };
  const send = (method, params) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params: params || {} })); });
  const ev = async (expr) => { const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true }); return r.result.result.value; };
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  await send('Runtime.enable'); await send('Page.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 1440, height: 900, deviceScaleFactor: 1, mobile: false });
  await send('Page.addScriptToEvaluateOnNewDocument', { source: userScript });
  await send('Page.navigate', { url: pathToFileURL(path.join(WWW, 'index.html')).href });
  await sleep(1200);

  // ---- boot from the host's state
  const boot = JSON.parse(await ev(`JSON.stringify({ name: MFUI.patch.name, cut: MFUI.patch.filt[0].cutoff, led: document.getElementById('led-name').textContent,
    flag: document.getElementById('led-flag').textContent, num: document.getElementById('led-num').textContent, info: document.querySelector('#strip .status .row:nth-child(2) span').textContent,
    users: MFUI.entries().filter((e) => e.kind === 'U').map((e) => e.name), sent: (window.__sent || []).map((s) => s.o.op) })`));
  check(boot.name === bank[6].name && boot.cut === 12345, `boots with the host's sound, not F01 (${boot.name}, cutoff ${boot.cut})`);
  check(boot.num === 'F07' && boot.flag === 'EDITED', `shows the host's preset entry and edited flag (${boot.num} ${boot.flag})`);
  check(!boot.sent.includes('patch'), `boot sends no patch back (would overwrite the host's sound): ${boot.sent.join(',')}`);
  check(boot.sent.includes('hello') && boot.sent.includes('ui'), `boot says hello and reports its preset entry (${boot.sent.join(',')})`);
  check(boot.users.join() === 'MY PAD', `user presets from the plugin's folder (${boot.users.join()})`);
  check(/48\.0 kHz/.test(boot.info) && /HOST MIDI/.test(boot.info), `status line from the plugin (${boot.info})`);

  // ---- mouse, through real browser input
  const box = async (sel) => JSON.parse(await ev(`(() => { const r = document.querySelector('${sel}').getBoundingClientRect(); return JSON.stringify([r.left + r.width / 2, r.top + r.height / 2]); })()`));
  const mouse = (type, x, y, o = {}) => send('Input.dispatchMouseEvent', { type, x, y, button: o.button || (type === 'mouseMoved' ? 'none' : 'left'), buttons: o.buttons ?? (type === 'mousePressed' || (type === 'mouseMoved' && o.down) ? 1 : 0), clickCount: o.clickCount || 1, modifiers: o.modifiers || 0, deltaX: o.deltaX || 0, deltaY: o.deltaY || 0 });
  const res = () => ev('MFUI.patch.filt[0].res');
  const RES = '.ctl[data-dst="17"]';
  const [rx, ry] = await box(RES);
  await ev('window.__sent = []');
  const r0 = await res();
  await mouse('mousePressed', rx, ry); for (let i = 1; i <= 6; i++) await mouse('mouseMoved', rx, ry - i * 10, { down: true }); await mouse('mouseReleased', rx, ry - 60);
  const r1 = await res();
  await mouse('mousePressed', rx, ry, { modifiers: 2 }); for (let i = 1; i <= 6; i++) await mouse('mouseMoved', rx, ry - i * 10, { down: true, modifiers: 2 }); await mouse('mouseReleased', rx, ry - 60, { modifiers: 2 });
  const r2 = await res();
  check(r1 > r0 + 1000, `mouse drag up turns the knob (${r0} -> ${r1})`);
  check(r2 > r1 && (r2 - r1) < 0.25 * (r1 - r0), `ctrl-drag is fine (${r1 - r0} vs ${r2 - r1})`);
  await sleep(400);
  const w0 = await res();
  await mouse('mouseWheel', rx, ry, { deltaY: -100 });
  await sleep(50);
  const w1 = await res();
  await mouse('mouseWheel', rx, ry, { deltaY: -100, modifiers: 8 });
  await sleep(50);
  const w2 = await res();
  await mouse('mouseWheel', rx, ry, { deltaY: 100 });
  await sleep(50);
  const w3 = await res();
  check(w1 > w0, `wheel up turns the knob up (${w0} -> ${w1})`);
  check(w2 > w1 && (w2 - w1) < 0.5 * (w1 - w0), `shift-wheel is fine (${w1 - w0} vs ${w2 - w1})`);
  check(w3 < w2, `wheel down turns it down (${w2} -> ${w3})`);
  await sleep(400);
  await mouse('mousePressed', rx, ry, { clickCount: 1 }); await mouse('mouseReleased', rx, ry, { clickCount: 1 });
  await mouse('mousePressed', rx, ry, { clickCount: 2 }); await mouse('mouseReleased', rx, ry, { clickCount: 2 });
  const rd = await res();
  check(rd === 0, `double-click sets the default (emphasis ${rd})`);
  const [kx, ky] = JSON.parse(await ev(`(() => { const r = document.querySelectorAll('.key')[12].getBoundingClientRect(); return JSON.stringify([r.left + r.width / 2, r.top + r.height * 0.8]); })()`));
  await mouse('mousePressed', kx, ky);
  const down = await ev(`document.querySelectorAll('.key')[12].classList.contains('down')`);
  await mouse('mouseReleased', kx, ky);
  const zoom = await ev(`document.getElementById('stage').style.transform`);
  await mouse('mouseWheel', rx, ry, { deltaY: -100, modifiers: 2 });   // ctrl+wheel: fine on the knob, never page zoom
  const zoom2 = await ev(`document.getElementById('stage').style.transform`);
  const sent = JSON.parse(await ev('JSON.stringify(window.__sent)'));
  const midi = sent.filter((s) => s.o.op === 'midi').map((s) => s.o.a.join(' '));
  check(down === true && midi.includes('144 60 ' + midi.find((m) => m.startsWith('144 60'))?.split(' ')[2]) && midi.includes('128 60 0'), `the mouse plays the keyboard: ${midi.join(' / ')}`);
  check(zoom === zoom2, 'ctrl+wheel does not zoom the panel');
  const patches = sent.filter((s) => s.o.op === 'patch');
  check(patches.length > 3 && patches.every((s) => s.o.a[1] === 7), `edits go out as patches with the host serial (${patches.length}, serial ${patches[0]?.o.a[1]})`);
  const last = Core.mfpRead(Core.b64decode(patches[patches.length - 1].o.a[0]));
  check(last.filt[0].res === (await res()), 'the last patch sent carries the knob value');

  // ---- host pushes
  const auto = M.clone(last); auto.filt[0].cutoff = 777; auto.macro[2] = 30000;
  await ev(`window.__sent = []; MFHost.patch('${Core.b64encode(Core.mfpWrite(auto))}', 9)`);
  await sleep(100);
  const ap = JSON.parse(await ev(`JSON.stringify({ cut: MFUI.patch.filt[0].cutoff, m3: MFUI.patch.macro[2], sent: window.__sent.map((s) => s.o.op) })`));
  check(ap.cut === 777 && ap.m3 === 30000 && !ap.sent.includes('patch'), `host automation lands on the panel, no echo (${JSON.stringify(ap)})`);
  const [cx, cy] = await box('.ctl[data-dst="15"]');
  await mouse('mouseWheel', cx, cy, { deltaY: -100 });
  await sleep(50);
  const after = JSON.parse(await ev('JSON.stringify(window.__sent.filter((s) => s.o.op === "patch").map((s) => s.o.a[1]))'));
  check(after.length >= 1 && after.every((s) => s === 9), `edits after a push carry the new serial (${after})`);
  await ev(`window.__sent = []; MFHost.restore({ b64: '${Core.b64encode(Core.mfpWrite(bank[2]))}', ui: { kind: 'F', name: '${bank[2].name}', dirty: false }, serial: 11 })`);
  await sleep(100);
  const rs = JSON.parse(await ev(`JSON.stringify({ name: MFUI.patch.name, num: document.getElementById('led-num').textContent, flag: document.getElementById('led-flag').textContent, sent: window.__sent.map((s) => s.o.op + ':' + (s.o.a[0] || '')) })`));
  check(rs.name === bank[2].name && rs.num === 'F03' && rs.flag === '' && !rs.sent.some((s) => s.startsWith('patch')), `host restore (program change / project load): ${JSON.stringify(rs)}`);
  check(rs.sent.some((s) => s.startsWith('ui:') && s.includes(bank[2].name)), 'the panel confirms the restored entry');

  // ---- bridge calls the panel makes synchronously
  const br = JSON.parse(await ev(`(() => {
    window.__sent = [];
    const a = AndroidSynth.savePreset('Fat/Bass*1', 'AAAA'), l1 = JSON.parse(AndroidSynth.listPresets()).map((p) => p.name);
    const d = AndroidSynth.deletePreset('MY PAD'), l2 = JSON.parse(AndroidSynth.listPresets()).map((p) => p.name);
    const st = JSON.parse(AndroidSynth.getStatus());
    AndroidSynth.pickNmsv(); AndroidSynth.openSettings();
    return JSON.stringify({ a, l1, d, l2, st: st.sampleRate, ops: window.__sent.map((s) => s.o.op + ':' + (s.o.a[0] || '')) });
  })()`));
  check(br.a === 'ok' && br.l1.includes('Fat_Bass_1') && br.d === 'ok' && !br.l2.includes('MY PAD'), `save / delete with the tablet's file-name rule: ${JSON.stringify(br)}`);
  check(br.ops.includes('save:Fat_Bass_1') && br.ops.includes('delete:MY PAD') && br.ops.includes('pick:'), `save / delete / pick reach the plugin (${br.ops})`);

  // ---- computer keyboard
  await ev('window.__sent = []');
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'a', code: 'KeyA', text: 'a' });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'a', code: 'KeyA' });
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'x', code: 'KeyX', text: 'x' });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'x', code: 'KeyX' });
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'j', code: 'KeyJ', text: 'j' });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'j', code: 'KeyJ' });
  const kb = JSON.parse(await ev('JSON.stringify(window.__sent.filter((s) => s.o.op === "midi").map((s) => s.o.a.join(" ")))'));
  check(kb.join('|') === '144 60 100|128 60 0|144 83 100|128 83 0', `computer keyboard: A = C4, X = octave up, J = B5 (${kb.join(' | ')})`);

  const shot = await send('Page.captureScreenshot', { format: 'png' });
  const out = path.join(ROOT, 'plugin/build/test_out');
  fs.mkdirSync(out, { recursive: true });
  fs.writeFileSync(path.join(out, 'panel_desktop_chrome.png'), Buffer.from(shot.result.data, 'base64'));
  check(errors.length === 0, `no page errors (${errors.slice(0, 3).join(' / ')})`);
  ws.close();
} catch (e) {
  check(false, `desktop panel test: ${e.message}`);
} finally { proc.kill(); }
console.log(`desktop panel: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);

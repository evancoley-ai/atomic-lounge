// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk VST3 (Windows): the tablet panel's native bridge, for the plugin's WebView2.
// Injected before the page's scripts (JUCE user script, after window.MFHostInit = {...}), so bridge.js
// finds window.AndroidSynth exactly as on the tablet. The tablet's calls are synchronous; JUCE's
// messages to native code are not, so calls are sent as events ('mfm1' {op, a}) and the answers the
// panel needs at once (status, preset list) come from local copies the plugin keeps current
// (window.MFHost.*). Contract: android/README.md "Bridge"; plugin side: plugin/src/PluginEditor.cpp.
'use strict';
(function () {
  const init = window.MFHostInit || {};
  function send(op, a) {
    const j = window.__JUCE__;
    try { if (j && j.backend) j.backend.emitEvent('mfm1', { op, a }); } catch (e) { /* page closing */ }
  }
  let presets = Array.isArray(init.presets) ? init.presets : [];
  let status = init.status || { voices: 0, midi: 0, sampleRate: 44100, latencyMs: 0, midiDevices: ['HOST MIDI'] };
  let serial = init.state ? init.state.serial | 0 : 0;   // last host automation push the panel shows
  let lastUi = '';
  // same file-name rule as the tablet (MainActivity.safe) and the plugin (PresetStore::safeName)
  function safe(n) {
    let s = String(n).replace(/[^A-Za-z0-9 _.-]/g, '_').trim();
    if (!s || s === '.' || s === '..') s = 'UNTITLED';
    return s.slice(0, 64);
  }
  function pushUi() {
    const U = window.MFUI;
    if (!U || typeof U.hostState !== 'function') return;
    let t;
    try { t = JSON.stringify(U.hostState()); } catch (e) { return; }
    if (t !== lastUi) { lastUi = t; send('ui', [t]); }
  }

  window.AndroidSynth = {
    loadPreset(b64) { send('patch', [String(b64), serial]); setTimeout(pushUi, 0); return 'ok'; },
    noteOn(n, v) { send('midi', [0x90, n & 127, v & 127]); },
    noteOff(n) { send('midi', [0x80, n & 127, 0]); },
    pitchBend(v) { send('midi', [0xE0, v & 127, (v >> 7) & 127]); },
    controlChange(cc, v) { send('midi', [0xB0, cc & 127, v & 127]); },
    getStatus() { pushUi(); return JSON.stringify(status); },
    listPresets() { return JSON.stringify(presets); },
    savePreset(name, b64) {
      const n = safe(name);
      presets = presets.filter((p) => p.name !== n);
      presets.push({ name: n, b64: String(b64) });
      presets.sort((a, b) => (a.name < b.name ? -1 : 1));
      send('save', [n, String(b64)]);
      setTimeout(pushUi, 0);
      return 'ok';
    },
    deletePreset(name) {
      const n = safe(name), had = presets.some((p) => p.name === n);
      presets = presets.filter((p) => p.name !== n);
      send('delete', [n]);
      return had ? 'ok' : 'not found';
    },
    pickNmsv() { send('pick', []); },
    openSettings() { /* tablet only (Android settings): nothing to open in a plugin */ },
  };

  // called by the plugin (PluginEditor.cpp)
  window.MFHost = {
    isPlugin: true,
    initialState() { return init.state || null; },
    status(s) { if (s && typeof s === 'object') status = s; },
    presets(l) { if (Array.isArray(l)) presets = l; },
    patch(b64, ser) {                       // host automation moved parameters
      serial = ser | 0;
      const U = window.MFUI; if (U && U.hostPatch) U.hostPatch(String(b64));
    },
    restore(st) {                           // project reopened / host program change
      if (!st || typeof st !== 'object') return;
      serial = st.serial | 0;
      const U = window.MFUI; if (U && U.hostRestore) U.hostRestore(st);
      lastUi = ''; pushUi();
    },
    toast(t) { const U = window.MFUI; if (U && U.toast) U.toast(String(t)); },
  };
  // a page reload (or a late start) asks for the current state again
  window.addEventListener('DOMContentLoaded', () => send('hello', []));

  // ---- desktop extras ----------------------------------------------------------------------------
  // Ctrl+wheel is fine-adjust on a knob, never page zoom; browser keys (reload, find, print) are off
  window.addEventListener('wheel', (e) => { if (e.ctrlKey) e.preventDefault(); }, { passive: false });
  const BLOCK = new Set(['F3', 'F5', 'F7', 'F11', 'F12', 'BrowserBack', 'BrowserForward', 'BrowserRefresh']);
  // computer keyboard: A W S E D F T G Y H U J K O L P = C4 .. D#5, Z / X = octave down / up
  const KEYS = 'awsedftgyhujkolp';
  let kbOct = 0;
  const kbHeld = new Map();
  window.addEventListener('keydown', (e) => {
    if (BLOCK.has(e.key) || (e.ctrlKey && /^[rfpgjus+=\-0]$/i.test(e.key))) { e.preventDefault(); return; }
    const t = e.target;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)) return;
    if (e.ctrlKey || e.altKey || e.metaKey) return;
    const k = e.key.toLowerCase();
    if (k === 'z' || k === 'x') { if (!e.repeat) kbOct = Math.max(-3, Math.min(3, kbOct + (k === 'x' ? 1 : -1))); e.preventDefault(); return; }
    const i = KEYS.indexOf(k);
    if (i < 0) return;
    e.preventDefault();
    if (e.repeat || kbHeld.has(k)) return;
    const note = Math.max(0, Math.min(127, 60 + kbOct * 12 + i));
    kbHeld.set(k, note);
    window.AndroidSynth.noteOn(note, 100);
  });
  window.addEventListener('keyup', (e) => {
    const k = e.key.toLowerCase(), n = kbHeld.get(k);
    if (n === undefined) return;
    kbHeld.delete(k);
    window.AndroidSynth.noteOff(n);
  });
  window.addEventListener('blur', () => { for (const n of kbHeld.values()) window.AndroidSynth.noteOff(n); kbHeld.clear(); });
})();

'use strict';
var TVLink = {
  command: 0,
  keys: [],
  logs: [],
  push: function (line) {
    if (line.indexOf('Média: Videó:') === 0) TVLink.nativePerformance = line;
    // Consecutive duplicates (retransmissions and repeats) would otherwise push the interesting
    // lines out of the on-screen window.
    if (TVLink.logs[TVLink.logs.length - 1] !== line) TVLink.logs.push(line);
    if (TVLink.logs.length > 200) TVLink.logs.shift();
    // Writing the trace line and the console on every single message is expensive enough to be felt
    // as stutter while streaming, so the DOM is updated a few times per second at most.
    var now = Date.now();
    if (TVLink.pushAt && now - TVLink.pushAt <= 250) return;
    TVLink.pushAt = now;
    console.log(line);
    try {
      // The newest session events are mirrored on screen, so a photo of the TV shows where the
      // handshake stops even without an inspector or a readable device log. The handshake is a
      // sequence of several events, so keep a window wide enough to hold the whole exchange.
      document.getElementById('trace').textContent = TVLink.logs.slice(-8).join('  |  ');
      // Keep the phase suffix on the status line current: it is set once when the text is written,
      // and the phase keeps moving afterwards.
      if (TVLink.lastStatus) document.getElementById('status').textContent = TVLink.statusText(TVLink.lastStatus);
    } catch (error) { /* the on-screen diagnostics are optional */ }
  },
  log: function (tag, message) { TVLink.push(tag + ': ' + message); },
  streaming: false,
  stopping: false,
  starting: false,
  autoStarted: false,
  hasHost: false,
  frameGeneration: 0,
  gamepadSlots: [null, null],
  gamepadGeneration: 0,
  // Keep the initial presets within 1080p until higher resolutions are calibrated on the TV.
  presets: [
    { width: 1024, height: 576 },
    { width: 1280, height: 720 },
    { width: 1600, height: 900 },
    { width: 1920, height: 1080 }
  ],
  frameRates: [30, 60],
  presetIndex: 3,
  frameRate: 60,
  // Bitrate choices in kbit/s. 0 = automatic (computed from resolution and frame rate).
  bitrates: [0, 10000, 20000, 30000, 50000, 80000],
  bitrateIndex: 0,
  audioOn: true,
  // 0 = automatic (H.264 up to 1440p, HEVC for 4K), 1 = H.264 only, 2 = HEVC only. Above 1080p we
  // used to switch to HEVC automatically, which is the most likely reason 2K/4K did not work on
  // this TV: H.264 is what has been proven here, so it is the default and HEVC is opt-in.
  codecs: [0, 1, 2],
  codecNames: ['Automatikus', 'H.264', 'HEVC'],
  codecIndex: 0,
  /* Latency-mode override for A/B measurements (temporary test switch, see
   * outputs/steam-tizen-measurements.md): 0 = automatic (the TV's own capability decides), 1 = normal,
   * 2 = low, 3 = ultra-low. It only changes which mode the source is created in, is stored with the
   * other settings, and defaults to automatic. */
  latencyModes: [0, 1, 2, 3],
  latencyNames: ['Automatikus', 'Normál', 'Alacsony', 'Ultra-alacsony'],
  latencyIndex: 0,
  settings: function (pointer) {
    // Seven values in, seven out: everything the user chose on the TV is read back by the C side
    // before the stream request goes out.
    var preset = TVLink.presets[TVLink.presetIndex], view = new DataView(Module.HEAPU8.buffer);
    view.setUint32(pointer, preset.width, true);
    view.setUint32(pointer + 4, preset.height, true);
    view.setUint32(pointer + 8, TVLink.frameRate, true);
    view.setUint32(pointer + 12, TVLink.bitrates[TVLink.bitrateIndex], true);
    view.setUint32(pointer + 16, TVLink.audioOn ? 1 : 0, true);
    view.setUint32(pointer + 20, TVLink.codecs[TVLink.codecIndex], true);
    view.setUint32(pointer + 24, TVLink.latencyModes[TVLink.latencyIndex], true);
    return 1;
  },
  bitrateText: function () {
    var kbps = TVLink.bitrates[TVLink.bitrateIndex];
    return kbps ? (kbps / 1000) + ' Mbit/s' : 'Automatikus';
  },
  renderSettings: function () {
    var preset = TVLink.presets[TVLink.presetIndex];
    document.getElementById('fps').textContent = 'Képfrissítés: ' + TVLink.frameRate + ' FPS';
    document.getElementById('resolution').textContent = 'Felbontás: ' + preset.width + '×' + preset.height;
    document.getElementById('bitrate').textContent = 'Bitráta: ' + TVLink.bitrateText();
    document.getElementById('audio').textContent = 'Hang: ' + (TVLink.audioOn ? 'be' : 'ki');
    document.getElementById('codec').textContent = 'Kodek: ' + TVLink.codecNames[TVLink.codecIndex];
    var latency = document.getElementById('latency');
    if (latency) latency.textContent = 'Késleltetés (teszt): ' + TVLink.latencyNames[TVLink.latencyIndex];
    TVLink.showVersion();
  },
  /* The build version comes from the C side (TV_BUILD_VERSION in probe.c) so there is exactly one
   * place to bump, and the log's RUN/MERES lines carry the same string. Read byte by byte: app.js is
   * loaded before the Emscripten module, so UTF8ToString is not guaranteed to exist here. */
  showVersion: function () {
    var element = document.getElementById('version');
    if (!element || typeof Module === 'undefined' || !Module._TVVersion) return;
    try {
      var pointer = Module._TVVersion(), text = '';
      for (var index = 0; index < 32; index++) {
        var code = Module.HEAPU8[pointer + index];
        if (!code) break;
        text += String.fromCharCode(code);
      }
      TVLink.version = text;
      element.textContent = 'Steam TV v' + text;
    } catch (error) { /* optional */ }
  },
  version: '',
  loadSettings: function () {
    try {
      var storedFps = parseInt(localStorage.getItem('steam-tv-fps'), 10);
      if (TVLink.frameRates.indexOf(storedFps) >= 0) TVLink.frameRate = storedFps;
      var storedPreset = parseInt(localStorage.getItem('steam-tv-resolution'), 10);
      if (storedPreset >= 0 && storedPreset < TVLink.presets.length) TVLink.presetIndex = storedPreset;
      var storedBitrate = parseInt(localStorage.getItem('steam-tv-bitrate'), 10);
      if (storedBitrate >= 0 && storedBitrate < TVLink.bitrates.length) TVLink.bitrateIndex = storedBitrate;
      if (localStorage.getItem('steam-tv-audio') === '0') TVLink.audioOn = false;
      var storedCodec = parseInt(localStorage.getItem('steam-tv-codec'), 10);
      if (storedCodec >= 0 && storedCodec < TVLink.codecs.length) TVLink.codecIndex = storedCodec;
      var storedLatency = parseInt(localStorage.getItem('steam-tv-latency'), 10);
      if (storedLatency >= 0 && storedLatency < TVLink.latencyModes.length) TVLink.latencyIndex = storedLatency;
    } catch (error) { TVLink.log('Beállítás', error.message); }
    TVLink.renderSettings();
  },
  saveSettings: function () {
    try {
      localStorage.setItem('steam-tv-fps', String(TVLink.frameRate));
      localStorage.setItem('steam-tv-resolution', String(TVLink.presetIndex));
      localStorage.setItem('steam-tv-bitrate', String(TVLink.bitrateIndex));
      localStorage.setItem('steam-tv-audio', TVLink.audioOn ? '1' : '0');
      localStorage.setItem('steam-tv-codec', String(TVLink.codecIndex));
      localStorage.setItem('steam-tv-latency', String(TVLink.latencyIndex));
    } catch (error) { TVLink.status(error.message); }
  },
  editable: function () { return !TVLink.streaming && !TVLink.starting; },
  /* Spatial navigation: the buttons are laid out in rows, so "left" must move to the button on the
   * left, not to the previous item of a flat list. A linear ring made the cursor jump across the
   * layout in both directions. Falls back to the list when geometry is unavailable. */
  moveFocus: function (keyCode) {
    var items = TVLink.focusables();
    if (!items.length) return;
    var step = (keyCode === 39 || keyCode === 40) ? 1 : -1;
    var current = document.activeElement;
    var currentRect = current && current.getBoundingClientRect ? current.getBoundingClientRect() : null;
    var dx = keyCode === 39 ? 1 : keyCode === 37 ? -1 : 0;
    var dy = keyCode === 40 ? 1 : keyCode === 38 ? -1 : 0;
    if (!currentRect || !currentRect.width || !currentRect.height) {
      // No layout information (kept for the test harness and for pages without geometry): fall back
      // to the flat list, but keep the direction.
      var index = items.indexOf(current);
      items[(((index < 0 ? (step > 0 ? -1 : 0) : index + step) + items.length) % items.length)].focus();
      return;
    }
    var cx = currentRect.left + currentRect.width / 2, cy = currentRect.top + currentRect.height / 2;
    var best = null, bestScore = 0;
    items.forEach(function (item) {
      if (item === current) return;
      var box = item.getBoundingClientRect ? item.getBoundingClientRect() : null;
      if (!box || !box.width || !box.height) return;
      var ix = box.left + box.width / 2, iy = box.top + box.height / 2;
      var along = (ix - cx) * dx + (iy - cy) * dy;                       // distance in the pressed direction
      if (along <= 1) return;                                             // that button is behind us
      var across = Math.abs((ix - cx) * dy) + Math.abs((iy - cy) * dx);   // off-axis distance
      var score = along + across * 2.2;                                    // prefer the same row/column
      if (best === null || score < bestScore) { best = item; bestScore = score; }
    });
    if (best) best.focus();
  },
  focusables: function () {
    /* 'latency' must be in this list: moveFocus() only ever moves the focus ring inside focusables(), so
     * while it was missing, the "Késleltetés (teszt)" button could not be reached with the TV remote or a
     * controller at all - the A/B latency test was impossible from the couch (found 2026-10-15, after
     * asking for that run several times). */
    return ['pair', 'start', 'resolution', 'fps', 'bitrate', 'audio', 'codec', 'diagnostics', 'latency'].map(function (id) { return document.getElementById(id); })
      .filter(function (item) { return item && !item.disabled && !item.hidden; });
  },
  padBuffer: 0,
  padSignature: 0,
  /* The pad is sampled here, on the browser thread, and written into the WASM heap buffer the C side
   * reads. Reading it from C through a MAIN_THREAD_EM_ASM proxy cost an unpredictable amount of time
   * (the proxy waits for this thread) and made the controller feel laggy. */
  padPolls: 0,
  padPollMaxGap: 0,
  padPollAt: 0,
  pollGamepads: function () {
    // Sampling cadence, reported in the watchdog line: this interval shares the browser thread with
    // the video pipeline's callbacks and the main loop's proxy calls, and an 8 ms sampler that slips
    // to 30 ms is felt as controller lag even though nothing in the input path is blocking.
    var now = Date.now();
    if (TVLink.padPollAt) {
      var gap = now - TVLink.padPollAt;
      if (gap > TVLink.padPollMaxGap) TVLink.padPollMaxGap = gap;
    }
    TVLink.padPollAt = now;
    TVLink.padPolls++;
    if (!TVLink.padBuffer || typeof navigator === 'undefined' || !navigator.getGamepads) return;
    var pads = Array.from(navigator.getGamepads()).filter(function (pad) {
      if (!pad || !pad.connected) return false;
      if (pad.mapping === 'standard') return true;
      return !!(pad.axes && pad.axes.length >= 4 && pad.buttons && pad.buttons.length >= 16);
    });
    var signature = 0, count = 0;
    pads.forEach(function (pad) {
      count++;
      signature = (signature * 31 + pad.index + 7) | 0;
      // 1/1000 quantisation: the wire format is 16-bit, so rounding the axes to 1% made slow, fine
      // stick movement arrive in visible steps.
      for (var axis = 0; axis < 4; axis++) signature = (signature * 31 + Math.round((pad.axes[axis] || 0) * 1000)) | 0;
      // Every field readGamepads() copies must be part of this signature, or a change in an unhashed
      // field never reaches the C side: the analog triggers were exactly that case, so pulling a
      // trigger alone left the host with the previous value. Button 17 is included because the mapping
      // table writes it.
      for (var button = 0; button < 18; button++) {
        var state = pad.buttons[button];
        signature = (signature * 31 + (state && state.pressed ? button + 1 : 0)) | 0;
        if (button === 6 || button === 7) signature = (signature * 31 + Math.round((state && state.value || 0) * 100)) | 0;
      }
    });
    signature = (signature * 31 + count) | 0;
    if (signature === TVLink.padSignature) return;
    TVLink.padSignature = signature;
    TVLink.readGamepads(TVLink.padBuffer);
  },
  videoClock: 0,
  presented: 0,
  lastPresentedTotal: null,
  nativePerformance: '',
  diagnosticOverlay: false,
  frameStats: { intervals: [], decode: [], late: [], last: null, coalesced: 0, at: null, presented: 0 },
  displayHzEstimate: 0,
  sampleDisplayClock: function () {
    if (typeof requestAnimationFrame !== 'function') return;
    var intervals = [], previous, generation = TVLink.frameGeneration;
    function sample(now) {
      if (!TVLink.streaming || generation !== TVLink.frameGeneration) return;
      if (previous !== undefined && now > previous) intervals.push(now - previous);
      previous = now;
      if (intervals.length < 120) requestAnimationFrame(sample);
      else {
        intervals.sort(function (a, b) { return a - b; });
        TVLink.displayHzEstimate = 1000 / intervals[Math.floor(intervals.length / 2)];
      }
    }
    requestAnimationFrame(sample);
  },
  observeVideoFrame: function (now, metadata) {
    var stats = TVLink.frameStats;
    metadata = metadata || {};
    var total = metadata.presentedFrames, previous = stats.last;
    if (typeof total === 'number' && total >= 0) {
      TVLink.presented += TVLink.lastPresentedTotal !== null && total >= TVLink.lastPresentedTotal ? total - TVLink.lastPresentedTotal : total;
      TVLink.lastPresentedTotal = total;
      if (previous && total > previous.total + 1) stats.coalesced += total - previous.total - 1;
      // Skipped callbacks cannot tell us the individual frame intervals or dropped frames.
      if (previous && total === previous.total + 1 && Number.isFinite(metadata.presentationTime)) {
        var interval = metadata.presentationTime - previous.time;
        if (Number.isFinite(interval) && interval > 0) stats.intervals.push(interval);
      }
      stats.last = { total: total, time: metadata.presentationTime, media: metadata.mediaTime };
    } else TVLink.presented++;
    if (Number.isFinite(metadata.processingDuration) && metadata.processingDuration >= 0)
      stats.decode.push(metadata.processingDuration * 1000);
    if (Number.isFinite(metadata.expectedDisplayTime)) stats.late.push(Math.max(0, now - metadata.expectedDisplayTime));
    [stats.intervals, stats.decode, stats.late].forEach(function (values) { if (values.length > 240) values.shift(); });
    if (stats.at === null) { stats.at = now; stats.presented = TVLink.presented; }
  },
  writeDiagnostic: function (text) {
    if (typeof Module === 'undefined' || !Module._TVLogBuffer || !Module._TVLogFlush) return;
    var pointer = Module._TVLogBuffer(), length = Math.min(text.length, 254);
    for (var index = 0; index < length; index++) Module.HEAPU8[pointer + index] = text.charCodeAt(index) & 0x7F;
    Module.HEAPU8[pointer + length] = 0;
    Module._TVLogFlush();
  },
  reportPresentation: function (now) {
    if (!TVLink.streaming) return '';
    var stats = TVLink.frameStats;
    if (stats.at === null || now - stats.at < 1800) return '';
    function summary(values) {
      if (!values.length) return 'NA';
      var sorted = values.slice().sort(function (a, b) { return a - b; });
      return [0.5, 0.95, 1].map(function (p) { return sorted[Math.max(0, Math.ceil(sorted.length * p) - 1)].toFixed(1); }).join('/');
    }
    var fps = (TVLink.presented - stats.presented) * 1000 / (now - stats.at);
    var target = 1000 / TVLink.frameRate;
    var jitter = stats.intervals.length ? Math.sqrt(stats.intervals.reduce(function (sum, value) {
      return sum + (value - target) * (value - target);
    }, 0) / stats.intervals.length).toFixed(1) : 'NA';
    var quality;
    try { quality = document.getElementById('video').getVideoPlaybackQuality(); } catch (error) { /* optional */ }
    var drops = quality && Number.isFinite(quality.droppedVideoFrames) ? quality.droppedVideoFrames : 'NA';
    var text = 'Render: fps=' + (stats.last ? fps.toFixed(1) : 'NA') + ' interval=' + summary(stats.intervals) +
      'ms decode=' + summary(stats.decode) + 'ms JS-late=' + summary(stats.late) +
      'ms coalesced=' + stats.coalesced + ' dropAPI=' + drops + ' raf~' +
      (TVLink.displayHzEstimate ? TVLink.displayHzEstimate.toFixed(1) : 'NA') + 'Hz panel=NA jitter=' + jitter +
      'ms media=' + (stats.last && Number.isFinite(stats.last.media) ? stats.last.media.toFixed(3) : 'NA');
    TVLink.writeDiagnostic(text);
    if (TVLink.diagnosticOverlay) document.getElementById('performance').textContent =
      'p50/p95/max; rAF = becslés, panel Hz ismeretlen\n' + text + '\n' + TVLink.nativePerformance;
    stats.at = now; stats.presented = TVLink.presented;
    stats.intervals.length = stats.decode.length = stats.late.length = 0;
    stats.coalesced = 0;
    return text;
  },
  /* Counts the frames the video element actually PRESENTS and mirrors its clock into shared memory,
   * so the C side can log "fed 60 frames/s" next to "presented N frames/s". A frozen picture with 60
   * frames per second of input is a presentation problem, not a transport or decoder problem. */
  startVideoClock: function () {
    if (typeof setInterval !== 'function') return;
    var video = document.getElementById('video');
    if (video && video.requestVideoFrameCallback) {
      var tick = function (now, metadata) {
        TVLink.observeVideoFrame(now, metadata);
        TVLink.reportPresentation(now);
        video.requestVideoFrameCallback(tick);
      };
      video.requestVideoFrameCallback(tick);
    }
    setInterval(function () {
      if (typeof Module === 'undefined' || !Module._TVVideoClockBuffer) return;
      if (!TVLink.videoClock) TVLink.videoClock = Module._TVVideoClockBuffer();
      try {
        var view = new DataView(Module.HEAPU8.buffer);
        view.setFloat64(TVLink.videoClock, video.currentTime || 0, true);
        view.setUint32(TVLink.videoClock + 8, video.readyState || 0, true);
        view.setUint32(TVLink.videoClock + 12, video.paused ? 1 : 0, true);
        view.setUint32(TVLink.videoClock + 16, TVLink.presented >>> 0, true);
        /* Browser-reported presented-frame counter and the media time of the last presented frame.
         * The C side logs their rates: "presentedFrames" is the compositor's own count, which is the
         * closest thing to a rendered-frame measure this platform exposes (it is NOT the panel
         * output), while the rVFC callback count above is only how often this app was called back. */
        var last = TVLink.frameStats.last;
        view.setUint32(TVLink.videoClock + 52, (Number.isFinite(TVLink.lastPresentedTotal) ? TVLink.lastPresentedTotal : 0) >>> 0, true);
        view.setFloat64(TVLink.videoClock + 72, last && Number.isFinite(last.media) ? last.media : 0, true);
        /* Gamepad sampling cadence: how many times the sampler ran since the last watchdog window and
         * its worst gap. The watchdog reads and resets these, so they are only mirrored here. */
        view.setUint32(TVLink.videoClock + 56, TVLink.padPolls >>> 0, true);
        view.setUint32(TVLink.videoClock + 60, TVLink.padPollMaxGap >>> 0, true);
      } catch (error) { /* diagnostics only */ }
    }, 250);
  },
  /* The black box. The C side's log can only be written by threads that are still running, so a frozen
   * picture usually means a silent log — exactly where the diagnosis is needed. This runs on the
   * browser thread, which keeps executing whatever the decoder threads do, and reports the counters
   * the main loop and the media layer publish into the shared buffer (see TVVideoClockState in
   * probe.c: keep the offsets in sync). It also nudges a video element that never started playing,
   * which is the state in which the decoder refuses everything and the picture stays black. */
  watchdogStalled: 0,
  watchdogTicks: -1,
  preparing: false,
  watchdog: function () {
    if (typeof Module === 'undefined' || !Module._TVLogBuffer) return;
    // A completed or stopping session must never be revived by a stale native phase.
    if (TVLink.stopping || (!TVLink.streaming && !TVLink.starting)) return;
    var phase = Module._TVStage ? Module._TVStage() : -1;
    var video = document.getElementById('video');
    if (!TVLink.videoClock && Module._TVVideoClockBuffer) TVLink.videoClock = Module._TVVideoClockBuffer();
    var ticks = 0, submitted = 0, refused = 0, audio = 0, audioErrors = 0, audioRefused = 0, sendFailures = 0, audioPts = 0, stalled = false;
    try {
      var view = new DataView(Module.HEAPU8.buffer), base = TVLink.videoClock;
      if (base) {
        ticks = view.getUint32(base + 20, true);
        submitted = view.getUint32(base + 24, true);
        refused = view.getUint32(base + 28, true);
        audio = view.getUint32(base + 32, true);
        audioErrors = view.getUint32(base + 36, true);
        audioRefused = view.getUint32(base + 40, true);
        sendFailures = view.getUint32(base + 44, true);
        audioPts = view.getUint32(base + 48, true);
      }
      stalled = ticks === TVLink.watchdogTicks;
      TVLink.watchdogTicks = ticks;
      // A prepare announced by the C side blocks the main loop on purpose (opening the media
      // pipeline), so its silence must not count as a freeze.
      if (phase >= 11 && stalled && !TVLink.preparing) TVLink.watchdogStalled++; else TVLink.watchdogStalled = 0;
      // The report is written and sent FIRST: the platform calls further down can throw, and a
      // watchdog whose report is skipped by an unrelated exception is worthless.
      // ASCII only: the text is copied byte by byte into WASM memory, so a multi-byte character
      // would be mangled (or worse, overflow the buffer) without a UTF-8 encoder.
      var text = 'fazis ' + phase + ', fociklus ' + ticks + (stalled ? ' ALL' : '') + ', kocka ' + submitted + '/' + refused +
        ', hang ' + audio + '/hiba ' + audioErrors + '/vissza ' + audioRefused + ', PTS ' + (audioPts / 1000).toFixed(1) +
        ' s, eldobott koldes ' + sendFailures + ', pad ' + TVLink.padPolls + 'db/max ' + TVLink.padPollMaxGap + 'ms, video ' +
        (video ? (video.readyState + '/' + (video.paused ? 'all' : 'megy') + '/' + video.videoWidth + 'x' + video.videoHeight) : 'nincs');
      TVLink.padPolls = 0;
      TVLink.padPollMaxGap = 0;
      TVLink.writeDiagnostic(text);
    } catch (error) { /* diagnostics only */ }
    try {
      if (video && phase >= 11 && video.paused) {
        // The element is not consuming, so the TV's decoder backs up until it refuses every frame.
        var started = video.play();
        if (started && started.catch) started.catch(function () {});
      }
      // If the C side already presented a picture but the video frame callback never fired, the app
      // still sits in its menu layout on top of the stream. Catch up from here.
      if (phase >= 12 && !TVLink.streaming) TVLink.playing();
      if (phase >= 11 && TVLink.watchdogStalled >= 3) {
        // Three silent reports (~6 s) while a stream is supposed to be running: tell the user instead
        // of leaving a black screen with an app that ignores the remote.
        document.body.classList.remove('quiet');
        TVLink.status('A kép lefagyott: a tévé dekódere nem vesz fel több kockát. Nyomd meg a Vissza gombot.');
      }
    } catch (error) { /* the recovery attempts are optional */ }
  },
  startGamepadSampling: function () {
    if (typeof setInterval !== 'function') return;
    var waiting = setInterval(function () {
      if (typeof Module === 'undefined' || !Module._TVGamepadBuffer) return;
      TVLink.padBuffer = Module._TVGamepadBuffer();
      clearInterval(waiting);
      TVLink.pollGamepads();
      // 4 ms, matching the HID poll interval in the library: two independent 8 ms clocks added up to
      // roughly 20 ms of avoidable input latency.
      setInterval(TVLink.pollGamepads, 4);
    }, 100);
  },
  readGamepads: function (pointer) {
    if (typeof navigator === 'undefined' || !navigator.getGamepads) return;
    // Tizen does not always report the standard mapping for a Bluetooth pad, so accept any pad
    // that exposes a standard-shaped layout instead of dropping it silently.
    var pads = Array.from(navigator.getGamepads()).filter(function (pad) {
      if (!pad || !pad.connected) return false;
      if (pad.mapping === 'standard') return true;
      return !!(pad.axes && pad.axes.length >= 4 && pad.buttons && pad.buttons.length >= 16);
    });
    var slots = TVLink.gamepadSlots;
    for (var slot = 0; slot < 2; slot++) {
      if (slots[slot] && !pads.some(function (pad) { return pad.index === slots[slot].index && pad.id === slots[slot].id; })) slots[slot] = null;
    }
    pads.forEach(function (pad) {
      if (slots.some(function (item) { return item && item.index === pad.index; })) return;
      var empty = slots.indexOf(null);
      if (empty >= 0) slots[empty] = { index: pad.index, id: pad.id, generation: ++TVLink.gamepadGeneration };
    });
    var snapshot = new Uint8Array(160);
    var view = new DataView(snapshot.buffer), buttons = [0, 1, 2, 3, 8, 16, 9, 10, 11, 4, 5, 12, 13, 14, 15, 17];
    function numeric(value, low, high) { return Number.isFinite(value) ? Math.max(low, Math.min(high, value)) : 0; }
    for (var i = 0; i < 2; i++) {
      if (!slots[i]) continue;
      var pad = pads.find(function (item) { return item.index === slots[i].index; });
      var base = i * 80;
      view.setUint32(base, slots[i].generation, true);
      view.setUint32(base + 4, /054c|dualsense|playstation|ps5/i.test(pad.id) ? 45 : 32, true);
      for (var axis = 0; axis < 4; axis++) {
        var value = numeric(pad.axes[axis], -1, 1);
        view.setInt16(base + 8 + axis * 2, Math.round(value * (value < 0 ? 32768 : 32767)), true);
      }
      for (var trigger = 0; trigger < 2; trigger++) view.setInt16(base + 16 + trigger * 2, Math.round(numeric(pad.buttons[6 + trigger] && pad.buttons[6 + trigger].value, 0, 1) * 32767), true);
      var bits = 0;
      for (var button = 0; button < buttons.length; button++) if (pad.buttons[buttons[button]] && pad.buttons[buttons[button]].pressed) bits |= 1 << button;
      view.setUint16(base + 24, bits, true);
      view.setUint8(base + 35, 3);
    }
    var sequence = new Uint32Array(Module.HEAPU8.buffer, pointer + 160, 1);
    Atomics.add(sequence, 0, 1);
    Module.HEAPU8.set(snapshot, pointer);
    Atomics.add(sequence, 0, 1);
  },
  beginStopping: function (text) {
    TVLink.failed(text);
    TVLink.stopping = true;
    TVLink.command = 0;
    document.getElementById('start').disabled = true;
    document.getElementById('pair').disabled = true;
  },
  endStopping: function (text, repairPairing) {
    TVLink.stopping = false;
    TVLink.failed(text, repairPairing);
  },
  failed: function (text, repairPairing) {
    if (TVLink.stopping) { TVLink.status(text); return; }
    TVLink.starting = false;
    TVLink.streaming = false;
    TVLink.frameGeneration++;
    TVLink.watchdogStalled = 0;
    TVLink.preparing = false;
    TVLink.keys.length = 0;
    document.body.classList.remove('playing');
    document.body.classList.remove('preparing');
    if (repairPairing) localStorage.removeItem('steam-tv-paired');
    document.getElementById('start').disabled = !TVLink.hasHost;
    var paired = localStorage.getItem('steam-tv-paired') === '1';
    document.getElementById('pair').hidden = paired;
    document.getElementById('pair').disabled = !TVLink.hasHost;
    document.getElementById('pin-row').hidden = paired;
    TVLink.status(text);
    document.getElementById(repairPairing ? 'pair' : 'start').focus();
  },
  awaitPicture: function () {
    if (TVLink.stopping) return;
    var video = document.getElementById('video'), generation = ++TVLink.frameGeneration;
    document.body.classList.add('preparing');
    function visible() {
      if (generation !== TVLink.frameGeneration) return;
      if (video.readyState >= 2 && video.videoWidth > 0) {
        if (Module._TVPicture) Module._TVPicture();
        TVLink.playing();
      }
    }
    if (video.requestVideoFrameCallback) video.requestVideoFrameCallback(visible);
    else {
      var previous = video.currentTime;
      video.ontimeupdate = function () { if (video.currentTime > previous) { visible(); previous = video.currentTime; } };
    }
  },
  playing: function () {
    if (TVLink.stopping) return;
    TVLink.streaming = true;
    TVLink.status('Steam Big Picture · Vissza: stream leállítása');
    document.body.classList.add('playing');
    TVLink.frameStats = { intervals: [], decode: [], late: [], last: null, coalesced: 0, at: null, presented: TVLink.presented };
    TVLink.displayHzEstimate = 0;
    TVLink.sampleDisplayClock();
    // The status bar used to sit on top of the stream for the whole session. Show it for a few
    // seconds, then get out of the way.
    TVLink.quietSoon();
  },
  statusTimer: 0,
  quietSoon: function () {
    if (!TVLink.streaming) return;
    if (typeof clearTimeout === 'function') clearTimeout(TVLink.statusTimer);
    document.body.classList.remove('quiet');
    if (typeof setTimeout !== 'function') return;
    TVLink.statusTimer = setTimeout(function () {
      if (TVLink.streaming) document.body.classList.add('quiet');
    }, 6000);
  },
  pairing: false,
  statusText: function (text) {
    // The session phase (TVStage) is appended when available: it is a compact, non-scrolling signal
    // of how far the connection got, which survives even if the trace window scrolls.
    var phase = Module && Module._TVStage ? Module._TVStage() : -1;
    return phase >= 0 ? text + ' [fázis ' + phase + ']' : text;
  },
  lastStatus: '',
  status: function (text) {
    TVLink.lastStatus = text;
    document.getElementById('status').textContent = TVLink.statusText(text);
  },
  done: function (text) {
    if (TVLink.stopping) return;
    TVLink.pairing = false;
    TVLink.status(text);
    var button = document.getElementById('pair');
    button.disabled = false;
    button.focus();
  },
  host: function (ip) {
    // Discovery repeats every few seconds. Focusing the start button on every report yanked the
    // cursor away from the settings buttons while they were being changed, so only the first
    // report may move the focus.
    var first = !TVLink.hasHost;
    TVLink.hasHost = true;
    if (TVLink.stopping) return;
    var paired = localStorage.getItem('steam-tv-paired') === '1';
    document.getElementById('host').textContent = 'Steam PC: ' + ip + (paired ? ' · Párosítva' : '');
    var button = document.getElementById('pair');
    button.hidden = paired;
    document.getElementById('pin-row').hidden = paired;
    if (button.disabled && !TVLink.pairing && !paired) { button.disabled = false; button.focus(); }
    if (!TVLink.starting) document.getElementById('start').disabled = false;
    // Deliberately no auto-start: the resolution and frame rate must be chosen before the stream
    // request goes out, so Big Picture only starts when the button is pressed.
    if (first && paired && !TVLink.starting && !TVLink.streaming) document.getElementById('start').focus();
  },
  parseIdentity: function (value) {
    return value && /^[a-f0-9]{80}$/.test(value)
      ? new Uint8Array(value.match(/../g).map(function (item) { return parseInt(item, 16); })) : null;
  },
  hexIdentity: function (bytes) {
    return Array.from(bytes).map(function (value) { return value.toString(16).padStart(2, '0'); }).join('');
  },
  identity: function (pointer) {
    try {
      var bytes = TVLink.parseIdentity(localStorage.getItem('steam-tv-identity'));
      if (!bytes) {
        // A damaged primary copy must not silently throw away a working pairing: Steam pairs the
        // device ID with its secret, so regenerating the identity would demand a fresh PIN.
        bytes = TVLink.parseIdentity(localStorage.getItem('steam-tv-identity-backup'));
        if (bytes) {
          localStorage.setItem('steam-tv-identity', TVLink.hexIdentity(bytes));
          TVLink.log('Identitás', 'Az elsődleges tároló sérült volt, a mentésből állt helyre.');
        } else {
          localStorage.removeItem('steam-tv-paired');
          bytes = new Uint8Array(40);
          crypto.getRandomValues(bytes);
          localStorage.setItem('steam-tv-identity', TVLink.hexIdentity(bytes));
          localStorage.setItem('steam-tv-identity-backup', TVLink.hexIdentity(bytes));
        }
      }
      Module.HEAPU8.set(bytes, pointer);
      return 1;
    } catch (error) { TVLink.status(error.message); return 0; }
  },
  saveSecret: function (pointer) {
    var identity = localStorage.getItem('steam-tv-identity');
    if (!TVLink.parseIdentity(identity)) throw new Error('Hibás helyi identitás.');
    var updated = identity.slice(0, 16) + TVLink.hexIdentity(Module.HEAPU8.subarray(pointer, pointer + 32));
    localStorage.setItem('steam-tv-identity', updated);
    localStorage.setItem('steam-tv-identity-backup', updated);
    localStorage.setItem('steam-tv-paired', '1');
  },
  pin: function (pointer) {
    try {
      var bytes = new Uint32Array(1);
      crypto.getRandomValues(bytes);
      var pin = (bytes[0] % 10000).toString().padStart(4, '0');
      for (var index = 0; index < 4; index++) Module.HEAPU8[pointer + index] = pin.charCodeAt(index);
      Module.HEAPU8[pointer + 4] = 0;
      document.getElementById('pin').textContent = pin;
      return 1;
    } catch (error) { TVLink.status(error.message); return 0; }
  },
  /* One key handler for the TV remote and the controller: a pad that cannot move the focus ring
   * cannot change the resolution either, which is what the on-screen settings are for. */
  uiKey: function (keyCode) {
    var scancodes = { 13: 40, 37: 80, 38: 82, 39: 79, 40: 81 };
    if (TVLink.streaming && scancodes[keyCode]) {
      if (TVLink.keys.length < 64) TVLink.keys.push(1000 + scancodes[keyCode]);
      return true;
    }
    if ([37, 38, 39, 40].includes(keyCode)) {
      TVLink.moveFocus(keyCode);
      return true;
    }
    if (keyCode === 13) {
      var selected = document.activeElement && document.activeElement.tagName === 'BUTTON'
        ? document.activeElement : document.getElementById('pair');
      if (!selected.disabled) selected.onclick();
      return true;
    }
    if (keyCode === 10009) {
      if (TVLink.stopping) {
        if (window.tizen) window.tizen.application.getCurrentApplication().exit();
        return true;
      }
      // A stalled main loop never reads TVLink.command, so the usual "stop the stream" request would
      // be read by nobody and the app would look dead while its picture is frozen. Leave instead.
      if (TVLink.streaming && TVLink.watchdogStalled >= 3 && window.tizen) {
        window.tizen.application.getCurrentApplication().exit();
        return true;
      }
      if (TVLink.streaming || TVLink.starting) {
        TVLink.command = 3; TVLink.frameGeneration++; TVLink.streaming = false;
        document.body.classList.remove('playing'); document.body.classList.remove('preparing');
      } else if (window.tizen) tizen.application.getCurrentApplication().exit();
      return true;
    }
    return false;
  },
  uiKeyUp: function (keyCode) {
    var scancodes = { 13: 40, 37: 80, 38: 82, 39: 79, 40: 81 };
    if (TVLink.streaming && scancodes[keyCode] && TVLink.keys.length < 64) TVLink.keys.push(2000 + scancodes[keyCode]);
  },
  padKey: 0,
  padKeyAt: 0,
  pollGamepadUi: function () {
    if (typeof navigator === 'undefined' || !navigator.getGamepads) return;
    // While streaming the pad belongs to the game: the C side polls it for HID reports every frame.
    if (TVLink.streaming) { TVLink.padKey = 0; return; }
    var pads = Array.from(navigator.getGamepads()).filter(function (pad) { return pad && pad.connected; });
    if (!pads.length) { TVLink.padKey = 0; return; }
    var pad = pads[0];
    function down(index) { return !!(pad.buttons[index] && pad.buttons[index].pressed); }
    var code = 0;
    if (down(12)) code = 38;
    else if (down(13)) code = 40;
    else if (down(14)) code = 37;
    else if (down(15)) code = 39;
    else if (down(0)) code = 13;
    else if (down(1)) code = 10009;
    else if (pad.axes && Math.abs(pad.axes[0]) > 0.6) code = pad.axes[0] > 0 ? 39 : 37;
    else if (pad.axes && Math.abs(pad.axes[1]) > 0.6) code = pad.axes[1] > 0 ? 40 : 38;
    var now = Date.now();
    if (!code) { TVLink.padKey = 0; return; }
    if (code === TVLink.padKey && now - TVLink.padKeyAt < 250) return;
    TVLink.padKey = code;
    TVLink.padKeyAt = now;
    TVLink.uiKey(code);
  }
};
var Module = {
  print: function (text) { console.log(text); },
  printErr: function (text) { console.error(text); },
  onAbort: function (reason) { TVLink.failed('A WASM modul leállt: ' + reason); document.getElementById('start').disabled = true; }
};
document.getElementById('pair').onclick = function () {
  if (TVLink.stopping) return;
  if (TVLink.pairing || document.getElementById('pair').disabled) return;
  TVLink.pairing = true;
  document.getElementById('pair').disabled = true;
  TVLink.command = 1;
};
document.getElementById('start').onclick = function () {
  if (TVLink.stopping) return;
  if (TVLink.starting) return;
  TVLink.starting = true;
  document.getElementById('start').disabled = true;
  TVLink.command = 2;
};
document.getElementById('fps').onclick = function () {
  if (!TVLink.editable()) return;
  TVLink.frameRate = TVLink.frameRates[(TVLink.frameRates.indexOf(TVLink.frameRate) + 1) % TVLink.frameRates.length];
  TVLink.renderSettings();
  TVLink.saveSettings();
};
document.getElementById('resolution').onclick = function () {
  if (!TVLink.editable()) return;
  TVLink.presetIndex = (TVLink.presetIndex + 1) % TVLink.presets.length;
  TVLink.renderSettings();
  TVLink.saveSettings();
};
document.getElementById('bitrate').onclick = function () {
  if (!TVLink.editable()) return;
  TVLink.bitrateIndex = (TVLink.bitrateIndex + 1) % TVLink.bitrates.length;
  TVLink.renderSettings();
  TVLink.saveSettings();
};
document.getElementById('audio').onclick = function () {
  if (!TVLink.editable()) return;
  TVLink.audioOn = !TVLink.audioOn;
  TVLink.renderSettings();
  TVLink.saveSettings();
};
document.getElementById('codec').onclick = function () {
  if (!TVLink.editable()) return;
  TVLink.codecIndex = (TVLink.codecIndex + 1) % TVLink.codecs.length;
  TVLink.renderSettings();
  TVLink.saveSettings();
};
TVLink.loadSettings();
document.getElementById('diagnostics').disabled = false;
document.getElementById('diagnostics').onclick = function () {
  TVLink.diagnosticOverlay = !TVLink.diagnosticOverlay;
  document.getElementById('diagnostics').textContent = 'Diagnosztika: ' + (TVLink.diagnosticOverlay ? 'be' : 'ki');
  document.body.classList[TVLink.diagnosticOverlay ? 'add' : 'remove']('diagnostics');
};
/* Temporary A/B switch: the mode is applied when the next stream starts, and the mode actually used
 * is logged (RUN/decoder line), so a measurement can always be tied to it. */
document.getElementById('latency').onclick = function () {
  if (!TVLink.editable()) return;
  TVLink.latencyIndex = (TVLink.latencyIndex + 1) % TVLink.latencyModes.length;
  TVLink.renderSettings();
  TVLink.saveSettings();
};
document.getElementById('pair').focus();
document.addEventListener('keydown', function (event) {
  if (TVLink.uiKey(event.keyCode)) event.preventDefault();
});
document.addEventListener('keyup', function (event) { TVLink.uiKeyUp(event.keyCode); });
if (typeof setInterval === 'function') {
  setInterval(TVLink.pollGamepadUi, 60);
  // 2 s: fast enough to catch a freeze, slow enough to stay invisible in the stream's log.
  setInterval(TVLink.watchdog, 2000);
}
TVLink.startGamepadSampling();
TVLink.startVideoClock();

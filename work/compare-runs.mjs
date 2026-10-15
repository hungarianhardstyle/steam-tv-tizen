#!/usr/bin/env node
/*
 * Compares Steam TV measurement runs from the host's Steam log.
 *
 * The client forwards its own measurement lines to the host over the stats channel, so everything is
 * already in one file: C:\Program Files (x86)\Steam\logs\streaming_log.txt (and the rotated
 * streaming_log.previous.txt). Each stream is a "session"; the client marks its own lines with
 * "CLIENT: TV: " and the measurement lines start with "MERES " (or "RUN " / "SUMMARY ").
 *
 * Usage:
 *   node work/compare-runs.mjs <logfile> [<logfile2>] [--list] [--session N] [--json] [--timeline]
 *
 *   --list        only list the sessions found (time, version, settings, effective latency mode)
 *   --session N   restrict the table to session N (1-based, in file order)
 *   --json        print the parsed per-session summaries as JSON
 *   --timeline    join the 1 Hz health line with the Render:/Idozites: reports per log second, so the
 *                 seconds where the picture stuttered can be told apart from the seconds where frames
 *                 never arrived (the per-second join is the whole point: either alone cannot decide it)
 *
 * Metric names match outputs/steam-tizen-measurements.md. A missing MERES field is printed as "-",
 * never as 0: a line that never arrived must not look like a measurement of zero.
 */
import { readFileSync } from 'node:fs';

const argv = process.argv.slice(2);
const files = argv.filter((a) => !a.startsWith('--') && !/^\d+$/.test(a));
const wantJson = argv.includes('--json');
const wantList = argv.includes('--list');
const wantTimeline = argv.includes('--timeline');
const sessionArgIndex = argv.indexOf('--session');
const onlySession = sessionArgIndex >= 0 ? parseInt(argv[sessionArgIndex + 1], 10) : 0;

if (!files.length) {
  console.error('usage: node work/compare-runs.mjs <streaming_log.txt> [streaming_log.previous.txt] [--list] [--session N] [--json]');
  process.exit(2);
}

/* The host's own pipeline events, taken from its non-CLIENT lines. They are the difference between a
 * bad second caused by the client and one caused by Steam rebuilding its capture/encode pipeline:
 * measured 2026-10-15 13:30:02, "Building new pipeline" + "Created encoder NVENC" + "Video Encoder
 * Changed" + adapter change coincided with a 10.05 s gap between presented frames on the client, while
 * the client's own appends stayed at 0.2-0.5 ms. */
const HOST_EVENT_PATTERNS = [
  [/Building new pipeline/, 'pipeline'],
  [/Video Encoder Changed/, 'encoder'],
  [/Changing adapter/, 'adapter'],
  [/Created encoder/, 'encoder-created'],
  [/Capture method set to (.+)$/, 'capture'],
  [/desktop access lost/, 'desktop-lost'],
  [/Switching video stream from (\S+) to (\S+)/, 'stream-switch'],
  [/Dropped (\d+) video frames/, 'frames-dropped'],
  [/Slow framerate: game ([\d.]+),.*?network ([\d.]+)/, 'slow-framerate'],
];
function hostEventOf(line) {
  for (const [re, kind] of HOST_EVENT_PATTERNS) {
    const m = line.match(re);
    if (m) return { kind, detail: (m[1] || '') + (m[2] ? ` -> ${m[2]}` : '') };
  }
  return null;
}

/* One parsed session: the stream header, the RUN line (build + settings) and every MERES sample. */
function parseLog(path) {
  const lines = readFileSync(path, 'utf8').split(/\r?\n/);
  const sessions = [];
  let current = null;
  let statsWindow = null;
  const timeMs = (line) => {
    const match = line.match(/\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\]/);
    return match ? Date.parse(match[1].replace(' ', 'T')) : null;
  };
  const startSession = (line) => {
    current = { file: path, header: line.trim(), startedAt: (line.match(/\[([^\]]+)\]/) || [])[1] || '', openedAtMs: timeMs(line), run: null, metrics: [], summary: null, videoStats: [], configLines: [], timeline: [], hostEvents: [], hostWindows: [] };
    sessions.push(current);
  };
  /* A session is "empty" while it has no measurements at all: the host may write the client's RUN line
   * before its own "Streaming started" line (and vice versa), so whichever marker comes second only
   * updates the header instead of opening a second, empty session for the same stream.
   * The merge is limited to one minute: without that limit, every stream that produced no client lines
   * was merged into the next one, so a "session" could span hours (measured: a 17:23-20:52 blob whose
   * host pipeline events then looked like they belonged to the 20:52 stream). */
  const isEmpty = (session) => session.metrics.length === 0 && session.videoStats.length === 0;
  const mergeable = (session, line) => {
    if (!session || !isEmpty(session)) return false;
    const at = timeMs(line);
    if (at === null || session.openedAtMs === null) return true;   // no timestamps (synthetic test log)
    return at - session.openedAtMs <= 60000;
  };
  for (const line of lines) {
    if (/Streaming started to Samsung Steam TV/.test(line)) {
      if (mergeable(current, line)) current.header = line.trim(), current.startedAt = (line.match(/\[([^\]]+)\]/) || [])[1] || current.startedAt;
      else startSession(line);
      continue;
    }
    if (!current) continue;
    /* Host-side pipeline events belong to the session they interrupt, so they are collected here. */
    const at = (line.match(/\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\]/) || [])[1] || null;
    const hostEvent = hostEventOf(line);
    if (hostEvent) current.hostEvents.push({ at, ...hostEvent });
    /* The host writes its own per-window accounting as a "SessionStats" JSON-ish block (AvgFrameMS,
     * StdDevFrameMS, SlowNetworkPercent, AvgNetworkMS, ...). It is the only place that says how many
     * frames the host actually produced in a window, which is what separates "the host sent 44 fps"
     * from "the network ate 16 of the 60". */
    if (statsWindow) {
      const field = line.match(/^\s*"([A-Za-z0-9_]+)"\s+"([^"]*)"/);
      if (field) { statsWindow.fields[field[1]] = field[2]; continue; }
      if (/^\s*}/.test(line)) { statsWindow = null; continue; }
    }
    if (/"SessionStats"/.test(line)) {
      statsWindow = { at, fields: {} };
      current.hostWindows.push(statsWindow);
      continue;
    }
    const client = line.indexOf('CLIENT: TV');
    if (client < 0) continue;
    const text = line.slice(client).replace(/^CLIENT: TV(\[type=\d+\])?:\s*/, '');
    /* Every client line together with its log second. The per-second join is what decides whether a
     * bad second was a delivery problem or a downstream (platform/display) one. */
    current.timeline.push({ at: (line.match(/\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\]/) || [])[1] || null, text });
    /* The payload keeps the client's own tag in front of the message ("Média: MERES v=...",
     * "MERES: RUN v=..."), so the markers are searched for anywhere in the remainder. */
    if (text.includes('RUN v=')) {
      /* A RUN line always begins a stream; if the current session already has data, this is a new one. */
      if (current.run !== null || !isEmpty(current)) startSession(line);
      current.run = text.slice(text.indexOf('RUN v='));
      continue;
    }
    const meresAt = text.indexOf('MERES v=');
    if (meresAt >= 0) { current.metrics.push(text.slice(meresAt)); continue; }
    const summaryAt = text.indexOf('SUMMARY v=');
    if (summaryAt >= 0) { current.summary = text.slice(summaryAt); continue; }
    /* How the run was configured (effective latency mode, platform capabilities). Kept so a latency-mode
     * A/B run can never be compared without knowing which pipeline produced it. */
    if (text.includes('Videó-dekóder mód') || text.includes('EMSS:')) { current.configLines.push(text); continue; }
    if (text.includes('Videó:')) { current.videoStats.push(text); continue; }
  }
  return sessions;
}

/* "key=value" pairs, values may themselves contain "a/b" pairs. */
function fields(text) {
  const out = {};
  for (const token of text.split(/\s+/).slice(1)) {
    const eq = token.indexOf('=');
    if (eq > 0) out[token.slice(0, eq)] = token.slice(eq + 1);
  }
  return out;
}

function numbers(sample, key) {
  const value = sample[key];
  if (value === undefined) return null;
  return value.split('/').map((v) => (Number.isFinite(parseFloat(v)) ? parseFloat(v) : null));
}

function stat(samples, key, position = 0) {
  const values = [];
  for (const sample of samples) {
    const parsed = numbers(sample, key);
    if (parsed && parsed[position] !== null) values.push(parsed[position]);
  }
  if (!values.length) return null;
  values.sort((a, b) => a - b);
  const sum = values.reduce((a, b) => a + b, 0);
  return { count: values.length, min: values[0], p50: values[Math.floor(values.length / 2)], max: values[values.length - 1], avg: sum / values.length };
}

function lastValue(samples, key) {
  for (let i = samples.length - 1; i >= 0; i--) {
    const parsed = numbers(samples[i], key);
    if (parsed && parsed[0] !== null) return parsed[0];
  }
  return null;
}

/* Median of the values that are actually present ([] -> null). The 1 Hz health line is the only
 * measurement every build has ever emitted, so it is what makes runs from different versions comparable. */
function medianOf(values) {
  const present = values.filter((value) => value !== null && value !== undefined && Number.isFinite(value));
  if (!present.length) return null;
  const sorted = present.slice().sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

/* Which pipeline the run actually used. The decoder line reports the EFFECTIVE mode; the RUN line only
 * reports what was requested (0 = automatic, and with an override 0/3 the effective mode can still be
 * normal instead of ultra-low if the TV lacks the capability), so the effective one wins. */
function latencyOf(session, run) {
  for (const line of session.configLines) {
    /* Check ultra-low first: "ultra-low időzítés" contains "low időzítés". */
    if (line.includes('ultra-low időzítés')) return 'ultra-low';
    if (line.includes('low időzítés')) return 'low';
    if (line.includes('normal időzítés')) return 'normal';
  }
  const names = ['auto', 'normal', 'low', 'ultra-low'];
  const override = run.kesleltetes;
  if (override === undefined) return null;
  return `requested=${names[Number(override)] ?? override}`;
}

function summarize(session) {
  const samples = session.metrics.map(fields);
  const first = samples[0] || {};
  const run = session.run ? fields(session.run) : {};
  const health = healthRows(session);
  return {
    file: session.file,
    startedAt: session.startedAt,
    version: run.v || first.v || '-',
    settings: session.run || '-',
    latency: latencyOf(session, run),
    configLines: session.configLines,
    samples: samples.length,
    durationS: samples.length ? Math.round(lastValue(samples, 't') || 0) : 0,
    fed: stat(samples, 'fed'),
    prez: stat(samples, 'prez'),
    rvfc: stat(samples, 'rvfc'),
    vhost: stat(samples, 'vhost', 1),   // worst host-side frame interval
    varr: stat(samples, 'varr', 1),     // worst local frame arrival interval
    vasm: stat(samples, 'vasm', 1),
    vdec: stat(samples, 'vdec', 1),
    vwait: stat(samples, 'vwait', 1),
    vint: stat(samples, 'vint', 1),
    aud: stat(samples, 'aud'),
    aarr: stat(samples, 'aarr', 1),
    aapp: stat(samples, 'aapp', 1),
    totals: {
      loss: lastValue(samples, 'loss'),
      rst: lastValue(samples, 'rst'),
      part: lastValue(samples, 'part'),
      win: lastValue(samples, 'win'),
      ref: lastValue(samples, 'ref'),
      drop: lastValue(samples, 'drop'),
      /* Why the client dropped a frame still in its own path, and why it asked for a keyframe:
       * vdrop = none/empty/big/key/pts/append, lossr = sequence/wait/decrypt/stall/mismatch/submit. */
      vdrop: lastValue(samples, 'vdrop'),
      lossr: lastValue(samples, 'lossr'),
    },
    /* The 1 Hz health line is the fallback when the MERES line is missing or truncated. */
    healthLines: session.videoStats.length,
    summary: session.summary || null,
    rows: rows(session),
    stalls: stallSummary(session),
    health,
    /* The 1 Hz health line, which is the one measurement every build has: this is what lets a run from
     * today be compared with a run from an older build at all. */
    healthFed: medianOf(health.map((r) => r.fed)),
    healthPrez: medianOf(health.map((r) => r.presented)),
    healthFedWorst: health.length ? Math.min(...health.filter((r) => r.fed !== null).map((r) => r.fed)) : null,
  };
}

/* When a run predates the MERES line (or its lines were lost), the 1 Hz health line still describes
 * every second: rank those seconds the same way so good and bad runs stay comparable. */
function printHealthEpisodes(summary) {
  /* The first seconds of a stream are not a stutter: the picture is still starting, so a 0-frame second
   * there must not be ranked as the worst one (measured: it was, and it read as "the platform append was
   * slow", which was the startup, not a fault). */
  const valid = summary.health.filter((r) => r.fed !== null && (r.clock === null || r.clock >= 5));
  if (valid.length < 5) return;
  const sorted = valid.slice().sort((a, b) => a.fed - b.fed);
  const median = sorted[Math.floor(sorted.length / 2)];
  const header = '            fed/s  presented  appendAvg/Max  audio/s  miss  ready/paused  PTS(clock)';
  const row = (label, r) => console.log(`  ${label.padEnd(9)} ${String(r.fed).padStart(5)} ${String(r.presented).padStart(9)} ` +
    `${String(r.appendAvg).padStart(8)}/${String(r.appendMax).padEnd(5)} ${String(r.audioRate).padStart(6)} ${String(r.audioMissing).padStart(5)} ` +
    `${String(r.readyState)}/${String(r.paused)}          ${String(r.videoPts)} (${String(r.clock)})`);
  console.log(`\n  --- 1 Hz health lines: worst seconds by fed (n=${valid.length}) ---`);
  console.log(header);
  sorted.slice(0, 5).forEach((r, i) => row(`worst${i + 1}`, r));
  console.log('  --- median second (baseline) ---');
  console.log(header);
  row('median', median);
  const worst = sorted[0];
  if (median.fed > 0 && worst.fed < median.fed * 0.7) {
    console.log(`\n  Episode verdict (worst second): fed ${worst.fed}/s vs median ${median.fed}/s; ` +
      `append avg/max ${worst.appendAvg}/${worst.appendMax} ms; audio ${worst.audioRate}/s (${worst.audioMissing} missing).`);
    if (worst.appendMax !== null && worst.appendMax > 20) console.log('  -> the client-side platform append was slow in that second.');
    else console.log('  -> the client side was fast (append small): the frames were not delivered to it on time (host/encoder or network).');
  }
}

/* Fallback rows from the 1 Hz health line ("Média: Videó: <fed> kocka/s be, beadás átlag <x> ms,
 * most <y> ms; kulcs ...; TV megjelenített <p> kocka/s, óra <t> s, kép-PTS <v> s, readyState <r>,
 * paused <q>; hang <f> frame/s, <e> hiba, <v> vissza, <m> hiányzó, PTS <a> s, <c> írás/s, <x> ms
 * (max <y>)."). Older builds (before v1.4.0) produced only this line, so a run can still be compared
 * field by field: "fed" here is what the client handed to the platform, not a rendered rate. */
function healthRows(session) {
  const out = [];
  for (const text of session.videoStats) {
    const at = (re) => { const m = text.match(re); return m ? parseFloat(m[1]) : null; };
    const row = {
      fed: at(/Videó:\s*([\d.]+) kocka\/s be/),
      appendAvg: at(/beadás átlag ([\d.]+) ms/),
      appendMax: at(/most ([\d.]+) ms/),
      keyFrames: at(/kulcs (\d+) db/),
      keyAvgKB: at(/kulcs \d+ db átlag (\d+) KB/),
      presented: at(/megjelenített ([\d.]+) kocka\/s/),
      clock: at(/óra ([\d.]+) s/),
      videoPts: at(/kép-PTS ([\d.]+) s/),
      readyState: at(/readyState (\d+)/),
      paused: at(/paused (\d+)/),
      audioRate: at(/hang ([\d.]+) frame\/s/),
      audioErrors: at(/hang [\d.]+ frame\/s, (\d+) hiba/),
      audioRefused: at(/hang [\d.]+ frame\/s, \d+ hiba, (\d+) vissza/),
      audioMissing: at(/hang [\d.]+ frame\/s, \d+ hiba, \d+ vissza, (\d+) hiányzó/),
      audioPts: at(/PTS ([\d.]+) s/),
      writes: at(/([\d.]+) írás\/s/),
      writeAvg: at(/írás\/s, ([\d.]+) ms/),
      writeMax: at(/írás\/s, [\d.]+ ms \(max ([\d.]+)\)/),
    };
    if (row.fed !== null || row.presented !== null) out.push(row);
  }
  return out;
}
function rows(session) {
  const parsed = session.metrics.map(fields);
  const out = [];
  let previous = null;
  for (const sample of parsed) {
    const value = (key, position = 0) => {
      const numbers_ = numbers(sample, key);
      return numbers_ && numbers_[position] !== null ? numbers_[position] : null;
    };
    const counter = (key) => value(key);
    const row = {
      t: value('t'),
      fed: value('fed'),
      prez: value('prez'),
      rvfc: value('rvfc'),
      vhostMax: value('vhost', 1),
      varrMax: value('varr', 1),
      vasmMax: value('vasm', 1),
      vdecMax: value('vdec', 1),
      vwaitMax: value('vwait', 1),
      vintMax: value('vint', 1),
      aud: value('aud'),
      aarrMax: value('aarr', 1),
      aappMax: value('aapp', 1),
      winDelta: previous && counter('win') !== null && previous.win !== null ? counter('win') - previous.win : null,
      lossDelta: previous && counter('loss') !== null && previous.loss !== null ? counter('loss') - previous.loss : null,
      dropDelta: previous && counter('drop') !== null && previous.drop !== null ? counter('drop') - previous.drop : null,
      vdropDelta: previous && counter('vdrop') !== null && previous.vdrop !== null ? counter('vdrop') - previous.vdrop : null,
      lossrDelta: previous && counter('lossr') !== null && previous.lossr !== null ? counter('lossr') - previous.lossr : null,
      win: counter('win'), loss: counter('loss'), drop: counter('drop'),
      vdrop: numbers(sample, 'vdrop'), lossr: numbers(sample, 'lossr'),
    };
    out.push(row);
    previous = row;
  }
  return out;
}

const rowWidth = (value, width, digits = 1) => (value === null || value === undefined ? '-'.padEnd(width) : Number(value).toFixed(digits).padStart(width));
function printRow(label, row) {
  const vdropText = Array.isArray(row.vdropDelta) ? row.vdropDelta.join('/') : '-';
  const lossrText = Array.isArray(row.lossrDelta) ? row.lossrDelta.join('/') : '-';
  console.log(`  ${label.padEnd(9)} ${rowWidth(row.t, 6, 0)} ${rowWidth(row.fed, 6)} ${rowWidth(row.prez, 6)} ${rowWidth(row.vhostMax, 8)} ` +
    `${rowWidth(row.varrMax, 8)} ${rowWidth(row.vasmMax, 7)} ${rowWidth(row.vdecMax, 7)} ${rowWidth(row.vwaitMax, 8)} ${rowWidth(row.vintMax, 7)} ` +
    `${rowWidth(row.aud, 6)} ${rowWidth(row.aarrMax, 7)} ${rowWidth(row.aappMax, 7)} ${rowWidth(row.winDelta, 5, 0)} ${rowWidth(row.lossDelta, 5, 0)} ` +
    `${rowWidth(row.dropDelta, 5, 0)} winΔ ${String(row.win)} lossΔ ${String(row.loss)} vdropΔ ${vdropText} lossrΔ ${lossrText}`);
}
const columnHeader = '            t     fed   prez  vhostMax varrMax  vasmMax vdecMax vwaitMax vintMax    aud  aarrMax aappMax  winΔ lossΔ dropΔ';

function printEpisodes(summary) {
  if (!summary.rows.length) return;
  const valid = summary.rows.filter((r) => r.fed !== null);
  if (!valid.length) return;
  /* A v1.4.0 run divided cumulative counters by the window, so its MERES rates are not rates at all
   * (measured: fed up to 3183/s in a 60 fps stream). Ranking windows by a broken number produced a
   * "median 1946.5/s" baseline and a nonsense verdict, so those runs are pointed at --timeline (the 1 Hz
   * health line is a correct per-second rate in every version) instead of being silently misread. */
  const configuredFps = Number((summary.settings ? fields(summary.settings).fps : 0)) || 0;
  const worstFed = Math.max(...valid.map((r) => r.fed));
  if (configuredFps > 0 && worstFed > configuredFps * 3) {
    console.log(`\n  --- MERES window rates are unusable in this run: fed up to ${format(worstFed, 1)}/s in a ` +
      `${configuredFps} fps stream (the cumulative-counter bug fixed in v1.4.1). Use --timeline: the 1 Hz ` +
      'health line carries a correct per-second rate in every version. ---');
    return;
  }
  const byFed = valid.slice().sort((a, b) => a.fed - b.fed);
  const byArrival = valid.slice().sort((a, b) => (b.varrMax || 0) - (a.varrMax || 0));
  const median = valid.slice().sort((a, b) => a.fed - b.fed)[Math.floor(valid.length / 2)];
  console.log('\n  --- worst windows by fed (received frames/s) ---');
  console.log(columnHeader);
  byFed.slice(0, 5).forEach((row, i) => printRow(`worst${i + 1}`, row));
  console.log('  --- worst windows by local arrival gap (receive side) ---');
  console.log(columnHeader);
  byArrival.slice(0, 5).forEach((row, i) => printRow(`gap${i + 1}`, row));
  console.log('  --- median window (baseline for the comparison) ---');
  console.log(columnHeader);
  printRow('median', median);
  const worst = byFed[0];
  if (worst.fed !== null && median.fed !== null && median.fed > 0 && worst.fed < median.fed * 0.7) {
    const hostOk = (worst.vhostMax || 0) <= 25;
    const arrivalBad = (worst.varrMax || 0) > 3 * (worst.vhostMax || 16.7);
    console.log(`\n  Episode verdict for the worst window (t=${worst.t}s): fed ${worst.fed.toFixed(1)}/s vs median ${median.fed.toFixed(1)}/s.`);
    if (hostOk && arrivalBad) console.log('  -> the host emitted frames on time (vhostMax small) but they arrived late: network / receive path.');
    else if (!hostOk) console.log('  -> the host itself emitted frames late (vhostMax large): PC encoder / game stall, not this client.');
    else if ((worst.winDelta || 0) > 0 || (worst.lossDelta || 0) > 0) console.log('  -> the receive window discarded packets or a keyframe was requested: client receive path.');
    else if ((worst.vdecMax || 0) > 20 || (worst.vwaitMax || 0) > 20) console.log('  -> the platform append or the media lock was slow: platform/decode, not the network.');
    else console.log('  -> no single dominant counter in this window; check the 1 Hz health lines for that second.');
  }
}
const format = (value, digits = 2) => (value === null || value === undefined ? '-' : Number(value).toFixed(digits));
const range = (s, digits = 1) => (s ? `${format(s.avg, digits)} [${format(s.min, digits)}..${format(s.max, digits)}] (n=${s.count})` : '-');

/* ------------------------------------------------------------------------------------------------
 * Per-second join of the four client reports. Neither report alone can decide why a second stuttered:
 * the 1 Hz health line says how many frames the client handed to the platform, the Render: report says
 * how many the browser actually presented, the Idozites: report says how long the media callbacks
 * took, and the MERES line adds the receive-side gaps.
 *   fed normal + Render fps low -> the frames reached the TV and were not shown (platform/display)
 *   fed low                     -> the frames never reached the client (host/network/receive path)
 * ---------------------------------------------------------------------------------------------- */
function buildTimeline(session) {
  const number = (text, re) => { const m = text.match(re); return m ? parseFloat(m[1]) : null; };
  const renderOf = (text) => ({
    fps: number(text, /fps=([\d.]+)/),
    intP95: number(text, /interval=[\d.]+\/([\d.]+)\//),
    intMax: number(text, /interval=[\d.]+\/[\d.]+\/([\d.]+)ms/),
    jsLateP95: number(text, /JS-late=[\d.]+\/([\d.]+)\//),
    coalesced: number(text, /coalesced=(\d+)/),
    dropAPI: number(text, /dropAPI=(\d+)/),
    raf: number(text, /raf~([\d.]+)Hz/),
    jitter: number(text, /jitter=([\d.]+)ms/),
  });
  const timingOf = (text) => ({
    cbP95: number(text, /cb [\d.]+\/([\d.]+)\//),
    cbMax: number(text, /cb [\d.]+\/[\d.]+\/([\d.]+)/),
    sdkP95: number(text, /READY-SDK [\d.]+\/([\d.]+)\//),
    sdkMax: number(text, /READY-SDK [\d.]+\/[\d.]+\/([\d.]+)/),
  });
  const merge = (row, values) => { for (const [key, value] of Object.entries(values)) if (value !== null && value !== undefined) row[key] = value; };
  const rows = [];
  const bySecond = new Map();
  for (const { at, text } of session.timeline) {
    const key = at || `line-${rows.length}`;
    if (!bySecond.has(key)) {
      const row = { at: key, fed: null, presented: null, appendAvg: null, appendMax: null, audioRate: null, audioMissing: null, audioErrors: null, audioRefused: null, writes: null, writeAvg: null, writeMax: null, readyState: null, fps: null, intP95: null, intMax: null, jsLateP95: null, coalesced: null, dropAPI: null, raf: null, jitter: null, cbP95: null, cbMax: null, sdkP95: null, sdkMax: null };
      bySecond.set(key, row); rows.push(row);
    }
    const row = bySecond.get(key);
    if (text.includes('Videó:')) merge(row, healthRows({ videoStats: [text] })[0] || {});
    if (text.startsWith('Render: ')) merge(row, renderOf(text));
    if (text.includes('Idozites:')) merge(row, timingOf(text));
    if (text.includes('MERES v=')) {
      /* Kept apart from the 1 Hz health values on purpose: in a v1.4.0 run the MERES rates were still
       * cumulative/2 s (the fix landed in v1.4.1), so merging them into "fed" silently inflated the
       * joined rows - measured 2026-10-15: mean fed 236.8/s in the worst Render seconds, which is the
       * MERES artefact, not the arrival rate. The health line's own rate is per-second and correct. */
      const sample = fields(text.split('MERES ')[1] || text);
      merge(row, { mFed: numbers(sample, 'fed')?.[0] ?? null, varrMax: numbers(sample, 'varr')?.[1] ?? null, vhostMax: numbers(sample, 'vhost')?.[1] ?? null, vdecMax: numbers(sample, 'vdec')?.[1] ?? null });
    }
  }
  /* The picture dropped by the browser is cumulative: a per-second delta is the only readable form. */
  let previousDrop = null;
  for (const row of rows) {
    if (row.dropAPI === null) { row.dropDelta = null; continue; }
    row.dropDelta = previousDrop === null ? null : row.dropAPI - previousDrop;
    previousDrop = row.dropAPI;
  }
  /* A 1 Hz line and a 1 s log bucket drift in and out of phase, so a second can hold the Render report
   * without the health line. Measured 2026-10-15: the rows for 13:29:21/13:29:30 printed no fed at all
   * while the neighbouring seconds show 40-43 frames/s arriving, which read as "nothing arrived" and
   * produced a wrong verdict. Borrow the nearest sample within one second and mark it (~). */
  for (let i = 0; i < rows.length; i++) {
    if (rows[i].fed !== null) continue;
    const neighbour = [rows[i - 1], rows[i + 1]].find((r) => r && r.fed !== null);
    if (neighbour) { rows[i].fed = neighbour.fed; rows[i].fedCarried = true; }
  }
  const withFed = rows.filter((r) => r.fed !== null);
  const withFps = rows.filter((r) => r.fps !== null);
  /* Older builds have the 1 Hz health line but no Render: report (it was added later), and the runs
   * before it are exactly the ones a good-vs-bad comparison needs, so one of the two is enough. */
  if (withFed.length < 5 && withFps.length < 5) return null;
  const median = (values) => { const sorted = values.slice().sort((a, b) => a - b); return sorted[Math.floor(sorted.length / 2)]; };
  const fedBaseline = withFed.length ? median(withFed.map((r) => r.fed)) : null;
  const fpsBaseline = withFps.length ? median(withFps.map((r) => r.fps)) : null;
  const worstFps = withFps.slice().sort((a, b) => a.fps - b.fps);
  const eventsBySecond = new Map();
  for (const event of session.hostEvents) {
    if (!event.at) continue;
    if (!eventsBySecond.has(event.at)) eventsBySecond.set(event.at, []);
    if (!eventsBySecond.get(event.at).includes(event.kind)) eventsBySecond.get(event.at).push(event.kind);
  }
  return { rows, withFed, withFps, fedBaseline, fpsBaseline, worstFps, eventsBySecond };
}

function printTimeline(session) {
  const joined = buildTimeline(session);
  if (!joined) {
    console.log('\n  --- per-second join: not enough joined seconds in this session (health or Render lines missing) ---');
    return;
  }
  const { rows, withFed, withFps, fedBaseline, fpsBaseline, worstFps, eventsBySecond } = joined;
  const printJoined = (label, row) => console.log(
    `  ${label.padEnd(10)} ${String(row.at).slice(-8).padEnd(9)} ${(row.fed === null ? '-' : (row.fedCarried ? `~${format(row.fed, 0)}` : format(row.fed, 0))).padStart(5)} ${rowWidth(row.presented, 5)} ${rowWidth(row.appendAvg, 6)} ` +
    `${rowWidth(row.fps, 6)} ${rowWidth(row.intP95, 5)}/${String(row.intMax ?? '-').padEnd(6)} ${rowWidth(row.jsLateP95, 6)} ${rowWidth(row.dropDelta, 5, 0)} ` +
    `${rowWidth(row.cbP95, 6)} ${rowWidth(row.sdkP95, 6)} ${rowWidth(row.audioRate, 5)} ${rowWidth(row.audioMissing, 5, 0)} ${rowWidth(row.writes, 5)} ${rowWidth(row.writeMax, 7)}` +
    `${eventsBySecond.has(row.at) ? `  HOST: ${eventsBySecond.get(row.at).join(',')}` : ''}`);
  console.log(`\n  --- per-second join: ${rows.length} log seconds, ${withFed.length} with a health line, ${withFps.length} with a Render report ---`);  /* The host's own churn is the single largest source of frame starvation in a measured run, so it is
   * reported before the worst-second tables. */
  const eventCounts = new Map();
  for (const event of session.hostEvents) eventCounts.set(event.kind, (eventCounts.get(event.kind) || 0) + 1);
  const limiterFps = session.hostEvents.filter((e) => e.kind === 'slow-framerate').map((e) => parseFloat(e.detail));
  if (eventCounts.size) {
    console.log(`  host events in this session: ${[...eventCounts].map(([kind, count]) => `${kind}=${count}`).join(' ')}` +
      (limiterFps.length ? `; its slow-framerate reports cite game=${format(Math.min(...limiterFps), 2)} (that field is NOT the` +
        ' game fps: the SessionStats table below is the host\'s own frame-rate accounting)' : ''));
    const rebuilds = session.hostEvents.filter((e) => e.kind !== 'slow-framerate');
    if (rebuilds.length) {
      console.log('  --- host pipeline events (each rebuild stops the frames for everyone) ---');
      for (const event of rebuilds) console.log(`    ${String(event.at).slice(-8)} ${event.kind.padEnd(15)} ${event.detail}`);
    }
  } else {
    console.log('  host events in this session: none parsed (older log, or the host lines were rotated out)');
  }
  /* The host's own per-window accounting, with the client's bad seconds counted inside each window: this
   * is where "the host sent 44 fps" becomes visible, which the client alone can never show. */
  if (session.hostWindows && session.hostWindows.length) {
    const badIn = (row) => row.clock !== null && row.clock < 5 ? false : ((row.fps !== null && row.fps < STALL_FPS) || (row.fed !== null && row.fed < STALL_FPS));
    console.log('  --- host SessionStats windows (its own accounting; fps = 1000/AvgFrameMS) ---');
    console.log('    until      fps   SDframe  SlowNet%  netAvg  netSD  client bad seconds in window');
    session.hostWindows.forEach((window, i) => {
      const from = i === 0 ? '' : session.hostWindows[i - 1].at;
      const f = window.fields;
      const frameMs = parseFloat(f.AvgFrameMS);
      const bad = rows.filter((row) => row.at > from && row.at <= window.at && badIn(row)).length;
      console.log(`    ${String(window.at).slice(-8)}  ${format(Number.isFinite(frameMs) && frameMs > 0 ? 1000 / frameMs : null, 1).padStart(5)}   ` +
        `${format(parseFloat(f.StdDevFrameMS), 1).padStart(6)}   ${format(parseFloat(f.SlowNetworkPercent), 1).padStart(7)}   ` +
        `${format(parseFloat(f.AvgNetworkMS), 1).padStart(6)}  ${format(parseFloat(f.StdDevNetworkMS), 1).padStart(5)}   ${bad}`);
    });
    const last = session.hostWindows[session.hostWindows.length - 1];
    console.log(`    last window also has: SlowGame=${format(parseFloat(last.fields.SlowGamePercent), 1)}% ` +
      `SlowEncode=${format(parseFloat(last.fields.SlowEncodePercent), 1)}% SlowDecode=${format(parseFloat(last.fields.SlowDecodePercent), 1)}% ` +
      `SlowDisplay=${format(parseFloat(last.fields.SlowDisplayPercent), 1)}% (all 0 means the host blamed no stage for those frames)`);
  }
  console.log('             second      fed  prez appAvg    fps intP95/intMax jslateP95 dropΔ  cbP95  SDKp95  aud/s  miss  iras/s  irasMax');
  console.log(`  --- ${Math.min(10, worstFps.length)} worst seconds by Render fps (median fps ${format(fpsBaseline, 1)}) ---`);
  worstFps.slice(0, 10).forEach((row, i) => printJoined(`fps-worst${i + 1}`, row));
  /* "Beakadás": the picture stops and then continues. That shows up as one huge gap between two
   * presented frames (`interval max`), not as a low average rate - and it is the symptom the user
   * reports, so it gets its own ranking. */
  const frozen = withFps.filter((r) => r.intMax !== null).sort((a, b) => (b.intMax || 0) - (a.intMax || 0));
  if (frozen.length) {
    console.log(`  --- ${Math.min(10, frozen.length)} worst FREEZES by presented-frame gap (interval max) ---`);
    frozen.slice(0, 10).forEach((row, i) => printJoined(`freeze${i + 1}`, row));
  }
  console.log(`  --- the same second count, ranked by fed instead (median fed ${format(fedBaseline, 1)}) ---`);
  withFed.slice().sort((a, b) => a.fed - b.fed).slice(0, 5).forEach((row, i) => printJoined(`fed-worst${i + 1}`, row));
  /* Pearson correlation: how much of the presented-rate drop is explained by the frames not arriving? */
  const pairs = rows.filter((r) => r.fed !== null && r.fps !== null);
  if (pairs.length > 4) {
    const mean = (values) => values.reduce((a, b) => a + b, 0) / values.length;
    const xs = pairs.map((r) => r.fed), ys = pairs.map((r) => r.fps);
    const mx = mean(xs), my = mean(ys);
    const cov = pairs.reduce((sum, r) => sum + (r.fed - mx) * (r.fps - my), 0);
    const sx = Math.sqrt(pairs.reduce((sum, r) => sum + (r.fed - mx) ** 2, 0));
    const sy = Math.sqrt(pairs.reduce((sum, r) => sum + (r.fps - my) ** 2, 0));
    const r = sx && sy ? cov / (sx * sy) : null;
    const slowFps = worstFps.slice(0, Math.max(1, Math.round(worstFps.length * 0.1)));
    const slowFed = slowFps.map((row) => row.fed).filter((v) => v !== null);
    const meanFed = slowFed.length ? mean(slowFed) : null;
    console.log(`\n  joined seconds with both values: ${pairs.length}; r(fed, Render fps) = ${format(r, 3)}`);
    console.log(`  in the worst 10% of Render seconds: mean fed ${format(meanFed, 1)}/s vs median ${format(fedBaseline, 1)}/s, ` +
      `mean Render fps ${format(mean(slowFps.map((row) => row.fps)), 1)} vs median ${format(fpsBaseline, 1)}`);
    if (meanFed !== null && fedBaseline > 0 && meanFed > fedBaseline * 0.8) {
      console.log('  -> the frames REACHED the TV in those seconds and were not presented: platform decode/display, not delivery.');
    } else if (meanFed !== null) {
      console.log('  -> in the worst seconds the frames did not reach the client either: host or network/receive path.');
    }
  }
}

/* The A/B verdict has to be one comparable number pair per run, so the per-second join is aggregated:
 * how many seconds were bad at all, how many of those the host's own pipeline churn explains, and how
 * strongly the presented rate followed the arrival rate. Startup seconds (client clock < 5 s) are not
 * stutter and are excluded. */
const STALL_FPS = 45;
function stallSummary(session) {
  const joined = buildTimeline(session);
  if (!joined) return null;
  const epoch = (at) => { const ms = Date.parse(String(at).replace(' ', 'T')); return Number.isFinite(ms) ? ms : null; };
  const pipelineEvents = session.hostEvents.filter((e) => e.kind !== 'slow-framerate' && epoch(e.at) !== null);
  const limiterEvents = session.hostEvents.filter((e) => e.kind === 'slow-framerate' && epoch(e.at) !== null);
  const pipelineTimes = pipelineEvents.map((e) => epoch(e.at));
  const limiterTimes = limiterEvents.map((e) => epoch(e.at));
  const near = (row, times, windowMs) => {
    const ms = epoch(row.at);
    return ms !== null && times.some((event) => Math.abs(event - ms) <= windowMs);
  };
  /* A row that has no health line at all has no "óra" either, so its clock is undefined - and an
   * undefined clock silently failed the "not a startup second" test, which dropped exactly those seconds
   * from every stall count (measured on the synthetic session: 5 starving seconds were reported as 4).
   * Only a clock that is *present* and below 5 s marks a startup second. */
  const started = joined.rows.filter((r) => r.clock === null || r.clock === undefined || r.clock >= 5);
  const bad = started.filter((r) => (r.fps !== null && r.fps < STALL_FPS) || (r.fed !== null && r.fed < STALL_FPS));
  const starved = started.filter((r) => r.fed !== null && r.fed < STALL_FPS);
  const notShown = started.filter((r) => r.fps !== null && r.fps < STALL_FPS);
  /* Two host-side buckets, because they need different follow-up: a pipeline rebuild is a hard stop for
   * everyone, while the limiter lines are the host's own report that IT is throttling (game/network).
   * Whatever is left is the part the host does not account for - that is the set the next run must
   * explain, and the only one a client-side change could plausibly address. */
  const atPipeline = bad.filter((r) => near(r, pipelineTimes, 2000));
  const atLimiter = bad.filter((r) => !near(r, pipelineTimes, 2000) && near(r, limiterTimes, 5000));
  const unexplained = bad.filter((r) => !near(r, pipelineTimes, 2000) && !near(r, limiterTimes, 5000));
  /* The unexplained seconds split into the only two things that can be done about them:
   *   arrival   - fewer than STALL_FPS frames reached the client (host pacing or network/receive path)
   *   shown     - the frames arrived and the picture still presented fewer (platform/decode/display),
   *               which is the case the latency pipeline (E1) or the display re-init (E2) could change */
  const unexplainedArrival = unexplained.filter((r) => r.fed !== null && r.fed < STALL_FPS);
  const unexplainedPresentation = unexplained.filter((r) => (r.fed === null || r.fed >= STALL_FPS) && r.fps !== null && r.fps < STALL_FPS);
  const unexplainedOther = unexplained.filter((r) => !unexplainedArrival.includes(r) && !unexplainedPresentation.includes(r));
  /* The whole run, the same way: how many bad seconds were "the frames never came" against "the frames
   * came and were not shown". That pair is what the E1 (latency pipeline) and E2 (display re-init)
   * experiments can move, so it is reported next to the bad count. */
  const arrivalBad = bad.filter((r) => r.fed !== null && r.fed < STALL_FPS);
  const presentationOnly = bad.filter((r) => (r.fed === null || r.fed >= STALL_FPS) && r.fps !== null && r.fps < STALL_FPS);
  const pairs = joined.rows.filter((r) => r.fed !== null && r.fps !== null);
  let correlation = null;
  if (pairs.length > 4) {
    const mean = (values) => values.reduce((a, b) => a + b, 0) / values.length;
    const mx = mean(pairs.map((r) => r.fed)), my = mean(pairs.map((r) => r.fps));
    const cov = pairs.reduce((sum, r) => sum + (r.fed - mx) * (r.fps - my), 0);
    const sx = Math.sqrt(pairs.reduce((sum, r) => sum + (r.fed - mx) ** 2, 0));
    const sy = Math.sqrt(pairs.reduce((sum, r) => sum + (r.fps - my) ** 2, 0));
    correlation = sx && sy ? cov / (sx * sy) : null;
  }
  const missingValues = joined.withFed.map((r) => r.audioMissing).filter((v) => v !== null);
  /* Audio append cadence. The crackle is heard when the PCM stream to the TV's sink has holes, and the
   * only place that shows them is this pair: how many audio frames arrived ("hang ... frame/s") against
   * how many times the client actually wrote to the track ("írás/s"). One write per frame means the
   * 10 ms batching never engaged; zero writes in a second with frames arriving is a hole in the sink. */
  const audioSeconds = joined.rows.filter((r) => r.writes !== null || r.audioRate !== null);  const zeroAppend = audioSeconds.filter((r) => r.writes === 0 && (r.audioRate === null || r.audioRate > 0));
  /* Freezes: a presented-frame gap of half a second or more is what a viewer calls "the picture got
   * stuck". Counted and listed so a run can be judged on the symptom the user actually reports. */
  const frozenRows = joined.rows.filter((r) => r.intMax !== null && r.intMax >= 500);
  const worstFreezeMs = joined.rows.reduce((worst, r) => Math.max(worst, r.intMax || 0), 0);
  const writeMaxWorst = audioSeconds.map((r) => r.writeMax).filter((v) => v !== null);
  /* When did audio frames go missing? The counter is cumulative, so the seconds where it *increases* are
   * the seconds with a hole. Those holes are audible as clicks because the client keeps the sample clock
   * contiguous (packet-loss concealment was removed on purpose), so the waveform jumps at the splice. */
  let previousMissing = null;
  const gapSeconds = [];
  for (const row of joined.rows) {
    if (row.audioMissing === null) continue;
    if (previousMissing !== null && row.audioMissing > previousMissing) {
      gapSeconds.push(`${String(row.at).slice(-8)}(+${row.audioMissing - previousMissing})`);
    }
    previousMissing = row.audioMissing;
  }
  /* The denominator matters: counting the seconds that merely *have a log line* makes a session with few
   * lines look mostly bad (measured: a 2-line fragment scored 50% "bad"). The client's own stream clock
   * ("óra" in the health line) is the honest length, and it exists in every build. */
  const clockValues = joined.rows.map((r) => r.clock).filter((v) => v !== null && Number.isFinite(v));
  const spanS = clockValues.length ? Math.max(...clockValues) : started.length;
  return {
    seconds: started.length,
    spanS,
    bad: bad.length,
    starved: starved.length,
    notShown: notShown.length,
    carriedSeconds: joined.rows.filter((r) => r.fedCarried).length,
    atPipeline: atPipeline.length,
    atLimiter: atLimiter.length,
    unexplained: unexplained.length,
    unexplainedArrival: unexplainedArrival.length,
    unexplainedPresentation: unexplainedPresentation.length,
    unexplainedOther: unexplainedOther.length,
    arrivalBad: arrivalBad.length,
    presentationOnly: presentationOnly.length,
    /* Without a Render: report there is no presented-rate signal at all, so "0 presentation-side bad
     * seconds" would be an artefact of missing data, not a finding. */
    hasRender: joined.withFps.length >= 5,
    frozenSeconds: frozenRows.length,
    worstFreezeMs,
    frozenList: frozenRows.map((r) => `${String(r.at).slice(-8)}(${Math.round(r.intMax)}ms${joined.eventsBySecond.has(r.at) ? ' ' + joined.eventsBySecond.get(r.at).join(',') : ''})`),
    explainedByHost: atPipeline.length + atLimiter.length,
    unexplainedSeconds: unexplained.map((r) => String(r.at).slice(-8)),
    pipelineEvents: pipelineEvents.length,
    correlation,
    medianFed: joined.fedBaseline,
    medianFps: joined.fpsBaseline,
    worstFps: joined.worstFps.length ? joined.worstFps[0].fps : null,
    worstFed: joined.withFed.length ? Math.min(...joined.withFed.map((r) => r.fed)) : null,
    audioMissing: missingValues.length ? missingValues[missingValues.length - 1] : null,
    audioMissingMax: missingValues.length ? Math.max(...missingValues) : null,
    audioRateMedian: medianOf(audioSeconds.map((r) => r.audioRate)),
    writesMedian: medianOf(audioSeconds.map((r) => r.writes)),
    zeroAppendSeconds: zeroAppend.length,
    zeroAppendList: zeroAppend.map((r) => String(r.at).slice(-8)),
    gapSeconds,
    gapTotal: gapSeconds.reduce((sum, text) => sum + Number((text.match(/\+(\d+)/) || [0, 0])[1]), 0),
    writeMaxWorst: writeMaxWorst.length ? Math.max(...writeMaxWorst) : null,
  };
}

function printSession(summary, index, session) {
  console.log(`\n=== #${index} ${summary.startedAt}  v=${summary.version}  samples=${summary.samples}  duration=${summary.durationS}s  latency=${summary.latency || '-'}`);
  console.log(`  settings: ${summary.settings}`);
  for (const line of summary.configLines) console.log(`  platform config: ${line}`);
  console.log(`  video fed/s         : ${range(summary.fed)}`);
  console.log(`  presentedFrames/s   : ${range(summary.prez)}   (browser compositor count, NOT panel output)`);
  console.log(`  rVFC callbacks/s    : ${range(summary.rvfc)}`);
  console.log(`  host frame gap ms   : ${range(summary.vhost)}   <- host/PC pacing, no network involved`);
  console.log(`  local frame gap ms  : ${range(summary.varr)}   <- receive side (network + assembly)`);
  console.log(`  assembly time ms    : ${range(summary.vasm, 2)}`);
  console.log(`  SDK append ms       : ${range(summary.vdec, 2)}`);
  console.log(`  config-lock wait ms : ${range(summary.vwait, 2)}`);
  console.log(`  callback gap ms     : ${range(summary.vint, 2)}`);
  console.log(`  audio frames/s      : ${range(summary.aud)}`);
  console.log(`  audio local gap ms  : ${range(summary.aarr, 2)}`);
  console.log(`  audio append ms     : ${range(summary.aapp, 2)}`);
  console.log(`  totals: loss=${summary.totals.loss} rst=${summary.totals.rst} part=${summary.totals.part} win=${summary.totals.win} ref=${summary.totals.ref} drop=${summary.totals.drop}`);
  /* One comparable line per run: this is what the latency-mode A/B is decided on. */
  if (summary.stalls) {
    const s = summary.stalls;
    console.log(`  stalls: ${s.bad} bad seconds of ${s.seconds} (fed<${STALL_FPS}: ${s.starved}, Render fps<${STALL_FPS}: ${s.notShown})` +
      (s.carriedSeconds ? `; ${s.carriedSeconds} seconds borrowed the neighbouring 1 Hz sample (~)` : ''));
    console.log(`          host pipeline rebuild within 2 s: ${s.atPipeline} of ${s.pipelineEvents} events; ` +
      `host limiter report within 5 s: ${s.atLimiter}; not accounted for by the host: ${s.unexplained}` +
      ` (of those: arrival below ${STALL_FPS}/s ${s.unexplainedArrival}, frames arrived but were not shown ${s.unexplainedPresentation}` +
      (s.unexplainedOther ? `, neither ${s.unexplainedOther}` : '') + ')');
    console.log(`          r(fed, Render fps) = ${format(s.correlation, 3)}; worst fed ${format(s.worstFed, 0)}/s, worst Render fps ${format(s.worstFps, 1)}; ` +
      `audio frames missing ${s.audioMissing} (max ${s.audioMissingMax})`);
    console.log(`          audio: median ${format(s.audioRateMedian, 0)} frame/s in, ${format(s.writesMedian, 0)} track writes/s ` +
      `(1 write per ~10 ms frame = no batching); seconds with frames but zero writes: ${s.zeroAppendSeconds}; worst single write ${format(s.writeMaxWorst, 1)} ms` +
      (s.zeroAppendList.length ? ` [${s.zeroAppendList.join(' ')}]` : ''));
    if (s.gapSeconds.length) console.log(`          audio gaps (cumulative missing jumped): ${s.gapSeconds.slice(0, 24).join(' ')}` +
      (s.gapSeconds.length > 24 ? ` … (+${s.gapSeconds.length - 24} more)` : ''));
    if (s.hasRender) console.log(`          freezes (presented-frame gap >= 500 ms): ${s.frozenSeconds}; worst ${format(s.worstFreezeMs, 0)} ms` +
      (s.frozenList.length ? ` [${s.frozenList.slice(0, 10).join(' ')}${s.frozenList.length > 10 ? ' …' : ''}]` : ''));
    if (s.unexplainedSeconds.length) console.log(`          unexplained seconds: ${s.unexplainedSeconds.join(' ')}`);
  }
  const describe = (value, names) => (Array.isArray(value) ? names.map((n, i) => `${n}:${value[i]}`).join(' ') : '-');
  console.log(`  drop reasons       : ${describe(summary.totals.vdrop, ['no-track', 'empty', 'too-big', 'need-key', 'pts', 'append'])}`);
  console.log(`  keyframe reasons   : ${describe(summary.totals.lossr, ['sequence', 'wait', 'decrypt', 'stall', 'mismatch', 'submit'])}`);
  console.log(`  1 Hz health lines in log: ${summary.healthLines}`);
  if (summary.summary) console.log(`  SUMMARY: ${summary.summary}`);
  printEpisodes(summary);
  printHealthEpisodes(summary);
  if (wantTimeline && session) printTimeline(session);
}

const sessions = files.flatMap(parseLog);
if (!sessions.length) {
  console.error('No "Streaming started to Samsung Steam TV" session found in the given file(s).');
  process.exit(1);
}

const summaries = sessions.map(summarize);
if (wantJson) {
  console.log(JSON.stringify(summaries, null, 2));
  process.exit(0);
}
if (wantList) {
  summaries.forEach((s, i) => console.log(`#${i + 1}  ${s.startedAt}  v=${s.version}  latency=${s.latency || '-'}  samples=${s.samples}  ${s.settings}`));
  process.exit(0);
}

if (onlySession > 0) {
  const s = summaries[onlySession - 1];
  if (!s) { console.error(`No session #${onlySession}`); process.exit(1); }
  printSession(s, onlySession, sessions[onlySession - 1]);
} else {
  summaries.forEach((s, i) => printSession(s, i + 1, sessions[i]));
}

/* Side-by-side of the decisive fields; "good" and "bad" can be picked from the list above. */
console.log('\n--- decisive comparison -------------------------------------------------');
console.log('sid  time      medH fed/prez  worst fed  bad seconds (share)  arr/pres  host  r(fed,fps)  latency');
summaries.forEach((s, i) => {
  const st = s.stalls;
  const medH = s.healthFed === null && s.healthPrez === null ? '-' : `${format(s.healthFed, 0)}/${format(s.healthPrez, 0)}`;
  /* A share is only meaningful when the stream actually ran: a fragment whose clock never advanced (all
   * health lines at "óra 0") produced "800%" before this guard, which reads like a measurement. */
  const bad = st ? `${st.bad}/${st.spanS}s${st.spanS >= 10 ? ` (${Math.round((st.bad / st.spanS) * 100)}%)` : ' (n/a)'}` : '-';
  const split = st ? `${st.arrivalBad}/${st.hasRender ? st.presentationOnly : 'n/a'}` : '-';
  console.log(
    `#${String(i + 1).padEnd(3)} ${String(s.startedAt).slice(-8).padEnd(9)} ${medH.padEnd(14)} ${format(s.healthFedWorst, 0).padStart(9)}  ${bad.padEnd(20)} ${split.padEnd(9)} ${String(st ? st.explainedByHost : '-').padStart(4)}  ${(st && st.correlation !== null ? format(st.correlation, 3) : '-').padEnd(11)} ${s.latency || '-'}`,
  );
});
console.log(`
"bad/host" = seconds below ${STALL_FPS} fps (fed or presented) / how many of them are within 2 s of a host
pipeline event. Compare that pair between runs: a latency-mode change that helps must lower "bad" while
"host" stays the same; if "bad" falls but "host" does not follow, the win is client-side.

ACROSS BUILDS only "medH fed" (arrival) is comparable. "medH prez" is not: the 2026-10-14 22:02 build
changed the measurement so a skipped JS callback no longer counts as a lost frame, and before that every
build read presented ~0.6x of fed (40/60) while later ones read ~1.0x (59/59).`);
console.log(`
Reading the comparison:
  * host gap small, local gap large  -> the frames left the host on time and the link/receive path
    delayed them (network/Wi-Fi).
  * host gap large, local gap similar -> the host/PC produced frames late (encoder or game stall).
  * both small, fed/s low            -> the host never sent more frames (content or its own limiter).
  * assembly time large               -> fragments of one frame arrived far apart.
  * append (SDK) large, wait small    -> the platform call itself is slow.
  * wait (config lock) large          -> the pipeline is busy elsewhere (prepare/teardown), not the network.
  * loss/win rising                    -> the client receive path asked for keyframes or dropped packets.`);

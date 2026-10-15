#!/usr/bin/env node
/*
 * Contract test between the measurement emitter (C, in work/steam-tv/media.cpp) and the log reader
 * (work/compare-runs.mjs).
 *
 * Why this exists: the MERES line is a plain text contract. If the C side renames or drops a field,
 * every comparison tool silently reports "-" for it, and a later analysis would draw conclusions from
 * "no data" instead of noticing the mismatch. This test therefore:
 *   1. reads the MERES format string AND its argument list out of media.cpp,
 *   2. checks that every key compare-runs.mjs looks for is present (and reports unknown keys),
 *   3. builds a synthetic log with two sessions (one smooth, one stuttering) and asserts that the
 *      aggregation produces the expected numbers.
 *
 * Run: node work/test-metrics.mjs
 */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const mediaCpp = readFileSync(join(here, 'steam-tv', 'media.cpp'), 'utf8');
const compare = readFileSync(join(here, 'compare-runs.mjs'), 'utf8');

/* ---- 1. the emitted field names ------------------------------------------------ */
const meresStart = mediaCpp.indexOf('"MERES v=%s');
assert.ok(meresStart > 0, 'MERES format string not found in media.cpp');
/* The format lives in C string-literal pieces spread over several source lines, so the literal syntax
 * is flattened first - otherwise a key at the start of a continuation line ("\n  "vhost=...") would
 * look like it does not exist. */
const meresFormat = mediaCpp.slice(meresStart, mediaCpp.indexOf(');', meresStart))
  .replace(/"/g, ' ')
  .replace(/\\?\r?\n\s*/g, ' ')
  .replace(/\s+/g, ' ');
const emittedKeys = new Set();
for (const match of meresFormat.matchAll(/(^|\s)([a-z][a-z0-9]*)=/g)) emittedKeys.add(match[2]);
assert.ok(emittedKeys.size >= 25, `expected at least 25 fields in MERES, found ${emittedKeys.size}`);

/* ---- 2. the keys the reader depends on ---------------------------------------- */
/* The direction that matters: every field compare-runs.mjs reads must exist in the emitted line. The
 * tool's own read sites are the source of truth, so they are extracted from its source instead of
 * being duplicated here - a renamed field then fails this test instead of silently printing "-". */
const readKeys = new Set();
for (const match of compare.matchAll(/(?:samples|sample),\s*'([a-z0-9]+)'/g)) readKeys.add(match[1]);
assert.ok(readKeys.size >= 20, `compare-runs.mjs should read many fields, found ${readKeys.size}`);
for (const key of readKeys) {
  assert.ok(emittedKeys.has(key), `compare-runs.mjs reads '${key}', which the MERES line does not emit`);
}
/* The core fields a comparison is built on must be read (a silent drop-out would defeat the tool). */
for (const key of ['fed', 'prez', 'rvfc', 'vhost', 'varr', 'vasm', 'vdec', 'vwait', 'vint', 'aud',
  'aarr', 'aapp', 'loss', 'win', 'drop', 'vdrop', 'lossr']) {
  assert.ok(readKeys.has(key), `compare-runs.mjs must read '${key}'`);
}
/* Fields that are emitted but deliberately only used for context are allowed (clock/media/vpts/apts/
 * loop/pad/proxy are visible in --json); they are listed here so a future removal is intentional. */
const informational = ['t', 'clock', 'media', 'vpts', 'apts', 'loop', 'pad', 'proxy'];
for (const key of informational) {
  assert.ok(emittedKeys.has(key), `informational field '${key}' disappeared from MERES`);
}
/* The reason counters must stay in a stable order: vdrop=none/empty/big/key/pts/append and
 * lossr=sequence/wait/decrypt/stall/mismatch/submit (compare-runs.mjs labels them by position). */
const token = (name) => {
  const start = meresFormat.indexOf(`${name}=`);
  assert.ok(start > 0, `${name} field missing`);
  return meresFormat.slice(start, meresFormat.indexOf(' ', start));
};
assert.equal((token('vdrop').match(/%u/g) || []).length, 6, 'vdrop must carry six counters');
assert.equal((token('lossr').match(/%u/g) || []).length, 6, 'lossr must carry six counters');

/* ---- 3. aggregation over a synthetic log -------------------------------------- */
const sample = (t, extra) => `CLIENT: TV: MERES v=1.4.0 t=${t} fed=60.0 prez=59.0 rvfc=60.0 clock=${t} media=${t} ` +
  `vpts=${t} apts=${t} vhost=16.7/16.9 varr=16.7/17.2 vasm=1.0/2.0 vdec=2.0/4.0 vwait=0.1/0.3 ` +
  `vint=16.7/17.0 aud=100.0/0/0/0 ahost=10.0/10.2 aarr=10.0/10.4 aapp=1.0/2.0(50) ` +
  `loss=0 rst=0 part=0 win=0/0 ref=0/0 drop=0 loop=250/1 pad=250/9 proxy=1000/2000 vdrop=0/0/0/0/0/0 ` +
  `lossr=0/0/0/0/0/0${extra ? ' ' + extra : ''}`;
const bad = (t) => sample(t).replace('vhost=16.7/16.9 varr=16.7/17.2', 'vhost=16.8/17.0 varr=42.0/180.0')
  .replace('vdec=2.0/4.0', 'vdec=2.0/9.0').replace('win=0/0', 'win=7/0').replace('loss=0', 'loss=3');
const log = [
  '[2026-10-15 12:00:00][1.0] Streaming started to Samsung Steam TV 1.1.0 at 192.168.1.6:1, audio channels = 2, MTU = 1468',
  'CLIENT: TV: RUN v=1.4.0 felbontas=1920x1080 fps=60 bitrate=19906 codec=0 hang=1 kesleltetes=0',
  sample(2), sample(4), sample(6),
  '[2026-10-15 12:01:00][61.0] Streaming started to Samsung Steam TV 1.1.0 at 192.168.1.6:2, audio channels = 2, MTU = 1468',
  'CLIENT: TV: RUN v=1.4.0 felbontas=1920x1080 fps=60 bitrate=19906 codec=0 hang=1 kesleltetes=1',
  bad(2), bad(4),
  'CLIENT: TV: SUMMARY v=1.4.0 frames=120 keys=2 bytes=100000 vasmMax=2.0 varrMax=180.0 vhostMax=17.0 audFrames=200 audGaps=0 audMissing=0 audReordered=0 aarrMax=10.4 loss=3 rst=0 part=0 win=7/0 ref=0/0 drop=0',
].join('\n');

const tmp = join(process.env.TEMP || '/tmp', `steam-tv-metrics-${process.pid}.log`);
const { writeFileSync, unlinkSync } = await import('node:fs');
writeFileSync(tmp, log);
let json;
try {
  json = JSON.parse(execFileSync(process.execPath, [join(here, 'compare-runs.mjs'), tmp, '--json'], { encoding: 'utf8' }));
} finally {
  unlinkSync(tmp);
}
assert.equal(json.length, 2, 'two sessions expected');
assert.equal(json[0].version, '1.4.0');
assert.equal(json[0].samples, 3);
assert.equal(json[1].samples, 2);
/* The smooth session: arrival gap stays near the frame interval. */
assert.ok(json[0].varr.max < 20, `smooth session should have a small arrival gap, got ${json[0].varr.max}`);
/* The stuttering session: host pacing unchanged, local arrival far worse -> network/receive side. */
assert.ok(json[1].vhost.max < 20 && json[1].varr.max > 100,
  'the synthetic bad session must show host=ok, arrival=bad');
assert.equal(json[1].totals.loss, 3);
assert.equal(json[1].totals.win, 7);
assert.match(json[1].settings, /kesleltetes=1/, 'the latency A/B setting must be readable from the RUN line');

/* ------------------------------------------------------------------------------------------------
 * Log-class contract (probe.c): a measurement line must never be throttled by the 30 s heartbeat
 * class. Measured defect, v1.4.0 run of 2026-10-15 13:27 (193 s): the media layer tags its lines
 * "Média: ", the exempt chain compared the full line, so only 7 of ~96 "Média: MERES …" lines reached
 * the host (30/32 s apart) and the closing "Média: SUMMARY …" was lost entirely - while the 1 Hz
 * "Média: Videó:" line arrived normally. This asserts the fix stays in place.
 * ---------------------------------------------------------------------------------------------- */
const probeC = readFileSync(join(here, 'steam-tv', 'probe.c'), 'utf8');
const forwardStart = probeC.indexOf('static void forward_to_host');
assert.ok(forwardStart > 0, 'forward_to_host not found in probe.c');
const forwardEnd = probeC.indexOf('\nstatic void flush_diagnostics', forwardStart);
const forward = probeC.slice(forwardStart, forwardEnd > 0 ? forwardEnd : probeC.length);
assert.match(forward, /const char \*body = line;/, 'forward_to_host must classify a tag-stripped body');
assert.match(forward, /kMediaTag\[\] = "Média: "/, 'the media tag must be stripped before classification');
assert.match(forward, /body \+= sizeof\(kMediaTag\) - 1/, 'the body pointer must skip the media tag');
for (const prefix of ['Render: ', 'Idozites:', 'MERES ', 'SUMMARY ', 'EMSS:', 'Videó-dekóder mód:', 'RUN v=', 'MERES: RUN']) {
  assert.ok(forward.includes(`strncmp(body, "${prefix}"`), `the budget-exempt class must match '${prefix}' on the body`);
}
for (const prefix of ['Videó', 'Beadás:', 'Él: ', 'HID: ', 'Őr: ']) {
  assert.ok(forward.includes(`strncmp(body, "${prefix}"`), `the 1 Hz measurement class must match '${prefix}' on the body`);
}
assert.ok(!/strncmp\(line, "MERES "/.test(forward), 'the MERES line must not be classified on the tagged full line again');
/* The mode/configuration lines are what make a latency-mode A/B run attributable at all. */
assert.ok(forward.includes('Videó-dekóder mód:'), 'the effective latency mode line must be exempt from the budgets');

/* ------------------------------------------------------------------------------------------------
 * Attribution contract (compare-runs.mjs): the per-second join, the host-event buckets, the audio-gap
 * list and the session split/merge rules are what decide *which* run is bad and *why*. Three of this
 * session's wrong conclusions came from bugs in exactly this code (a MERES-fed value contaminating the
 * health fed, a denominator that counted log lines instead of stream seconds, and streams hours apart
 * merged into one session), so it is exercised on a synthetic log with known answers.
 * ---------------------------------------------------------------------------------------------- */
const pad = (value) => String(value).padStart(2, '0');
const stamp = (minute, second) => `2026-10-15 14:${pad(minute)}:${pad(second)}`;
const health = (minute, second, fed, presented, missing, writes) =>
  `[${stamp(minute, second)}] CLIENT: TV: Média: Videó: ${fed} kocka/s be, beadás átlag 2.0 ms, most 1 ms; ` +
  `kulcs 0 db átlag 0 KB, delta 60 db átlag 7 KB; TV megjelenített ${presented} kocka/s, óra ${second}.0 s, ` +
  `kép-PTS ${second}.0 s, readyState 4, paused 0; hang 100.0 frame/s, 0 hiba, 0 vissza, ${missing} hiányzó, ` +
  `PTS ${second}.0 s, ${writes} írás/s, 1.0 ms (max 3.0).`;
const render = (minute, second, fps) =>
  `[${stamp(minute, second)}] CLIENT: TV: Render: fps=${fps} interval=16.7/16.7/16.7ms decode=NAms ` +
  'JS-late=0.0/0.0/0.0ms coalesced=0 dropAPI=0 raf~59.9Hz panel=NA jitter=0.0ms media=NA';
/* One frozen second: the average rate can look fine while a single gap of seconds is what a viewer
 * calls "the picture got stuck". */
const renderFrozen = (minute, second, fps, gapMs) =>
  `[${stamp(minute, second)}] CLIENT: TV: Render: fps=${fps} interval=16.7/16.7/${gapMs}ms decode=NAms ` +
  'JS-late=0.0/0.0/0.0ms coalesced=0 dropAPI=0 raf~59.9Hz panel=NA jitter=0.0ms media=NA';
const started = (minute, second) => `[${stamp(minute, second)}] Streaming started to Samsung Steam TV 1.1.0 at 192.168.1.6:48128, audio channels = 2, MTU = 1468`;

const attributionLog = [];
/* Session 1: smooth - 20 seconds at 60 fps in, 59 presented, no host events. */
attributionLog.push(started(0, 1));
for (let second = 1; second <= 20; second++) { attributionLog.push(health(0, second, 60, 59, 0, 48)); attributionLog.push(render(0, second, 60)); }
/* Session 2: five starving seconds (7..11) plus a host pipeline rebuild in the middle, one second with
 * no health line at all (only the Render report - the carry-forward case), and an audio gap of 7. */
attributionLog.push(started(5, 1));
for (let second = 1; second <= 20; second++) {
  const starving = second >= 7 && second <= 11;
  const missing = second >= 9 ? 7 : 0;
  if (second !== 10) attributionLog.push(health(5, second, starving ? 30 : 60, starving ? 30 : 59, missing, 48));
  attributionLog.push(render(5, second, starving ? 30 : 60));
  if (second === 8) {
    attributionLog.push(`[${stamp(5, 8)}] Building new pipeline`);
    attributionLog.push(`[${stamp(5, 8)}] Created encoder NVENC for codec 4`);
    attributionLog.push(`[${stamp(5, 8)}] Video Encoder Changed`);
  }
}
/* A frozen second in the same session: 4.2 s between two presented frames, host rebuild next to it. */
attributionLog.push(renderFrozen(5, 12, 12, 4200));
/* Session 3 and 4: a "Streaming started" pair ten seconds apart with no client lines must merge into
 * one session; one half an hour later must not. */
attributionLog.push(started(10, 1));
attributionLog.push(started(10, 11));
attributionLog.push(started(40, 1));

const attributionPath = join(process.env.TEMP || '/tmp', `steam-tv-attribution-${process.pid}.log`);
writeFileSync(attributionPath, attributionLog.join('\n'), 'utf8');
const attributionJson = JSON.parse(
  execFileSync(process.execPath, [join(here, 'compare-runs.mjs'), attributionPath, '--json'], { encoding: 'utf8' }),
);
unlinkSync(attributionPath);
assert.equal(attributionJson.length, 4, 'two measured sessions + one merged pair + one later session');
const [smooth, starving, merged, later] = attributionJson;
assert.equal(smooth.stalls.bad, 0, 'a clean session must not report a single bad second');
assert.equal(smooth.healthFed, 60);
assert.equal(smooth.healthPrez, 59);
assert.equal(starving.stalls.bad, 6, 'the five starving seconds plus the frozen one (its average rate is 12/s)');
assert.equal(starving.stalls.seconds, 16, 'the startup seconds are excluded, the Render-only second is not');
assert.ok(starving.stalls.atPipeline >= 4, 'the starving seconds must be attributed to the host pipeline rebuild');
assert.equal(starving.stalls.gapTotal, 7, 'the audio gap list must add up to the missing-frame jump');
assert.equal(starving.stalls.gapSeconds.length, 1, 'one jump in the cumulative counter is one gap second');
assert.ok(starving.stalls.carriedSeconds >= 1, 'a second without its own health line must borrow the neighbouring sample');
assert.equal(starving.stalls.zeroAppendSeconds, 0, 'a second with frames always had a track write here');
assert.equal(starving.stalls.frozenSeconds, 1, 'a 4.2 s presented-frame gap is one frozen second');
assert.equal(starving.stalls.worstFreezeMs, 4200, 'the freeze length must be reported, not just counted');
assert.ok(merged.samples === 0 && later.samples === 0, 'the unmeasured sessions carry no measurements');
assert.equal(merged.startedAt, `2026-10-15 14:10:11`, 'the pair ten seconds apart merges into one session');

console.log('Measurement contract: MERES fields, vdrop/lossr ordering, log aggregation, per-second attribution (host buckets, audio gaps, carry-forward, session merge) and the remote-reachability of every panel button passed.');

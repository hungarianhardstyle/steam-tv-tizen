# Steam TV — Steam Remote Play client for Samsung Tizen TVs

A Steam Link–style client that runs **on the TV itself** as a Tizen web application: it speaks Steam's
in-home streaming protocol over UDP and plays the stream through **Samsung's Tizen WASM Player (EMSS)**
— `ElementaryMediaStreamSource` feeding an HTML `<video>` element, video decoded by the TV's hardware
decoder and PCM audio written straight to the media track.

Built and measured on a **Tizen 9.0** Samsung TV at 1920×1080@60. No PC-side agent is needed beyond
Steam's own Remote Play.

> ⚠️ Not affiliated with Valve or Samsung. "Steam" is a trademark of Valve Corporation. The Samsung
> Emscripten SDK is **not** included here (see *Building*), and the bundled third-party libraries keep
> their own licenses (see *Licensing*).

---

## 1. Install (TV, no toolchain needed)

Grab `outputs/SteamTV.wgt` and install it with any Tizen sideloader. The easiest is
[Apps2Samsung](https://github.com/Apps2Samsung/Apps2Samsung/releases/latest) (Windows/macOS/Linux/Android),
which has a **Custom WGT File** option and handles the signing that Tizen 7+ requires.

1. **Developer mode on the TV:** `Apps → App Settings →` press `1 2 3 4 5` on the remote (use the `123`
   key if there is no number pad) `→ Developer mode ON →` enter your **PC's LAN IP** in *Host PC IP*
   `→ OK`. Then power-cycle the TV (hold power ≥ 5 s) — the `DEVELOP MODE` tag appears in the Apps panel.
2. In Apps2Samsung: `Settings → Certificate: Jelly2Sams`, `Local IP: your adapter`, then
   `Release → Custom WGT File → outputs/SteamTV.wgt → Download & Install`.
   (On Tizen 7+ you will be asked to sign in with a Samsung account to sign the package.)
3. On the TV: launch **Steam TV**, press *Párosítás a Steammel* (Pair with Steam) and enter the PIN
   shown on the PC, then *Big Picture indítása* (Start Big Picture).

Alternative: import the `.wgt` into Tizen Studio, create an author + distributor certificate profile
(TV DUID), and use *Run As → Tizen Web Application*.

## 2. Using it

* On-screen settings (D-pad to move, `A`/OK to press): resolution, frame rate, bitrate, codec, audio,
  diagnostics overlay, and a **latency-mode test switch** (`Automatikus → Normál → Alacsony →
  Ultra-alacsony`). Settings persist in `localStorage`.
* While streaming: the D-pad and buttons go to the game (XInput HID reports), **Back** stops the stream.
* The build version is shown on the home screen and is also written into the host's Steam log
  (`MERES: RUN v=…`), so a measurement can always be tied to the exact build and settings.

## 3. Building

Requirements: **Windows + PowerShell**, the **Samsung Emscripten SDK** (Emscripten 1.39.4 *fastcomp*
bundle with the Samsung TV extensions), .NET 8 for the installer.

1. Download the Samsung Emscripten SDK (Samsung Developer: *WebAssembly → Download*) and unpack it so
   that `work/samsung-emscripten/emscripten-release-bundle/emsdk/` exists.
2. Build everything (library → Opus → app bundle):
   ```powershell
   powershell -ExecutionPolicy Bypass -File work\build-probe.ps1
   ```
   This produces `work/steam-tv/ihs.js`, `ihs.wasm`, `ihs.js.mem`, `ihs.worker.js`.
3. Run the tests:
   ```powershell
   node work\steam-tv\test.mjs        # TV UI / JS contract tests
   node work\test-metrics.mjs         # measurement + log-parsing contract tests
   powershell -ExecutionPolicy Bypass -File work\test-native.ps1   # native WASM library tests
   ```
4. Install to the TV:
   ```powershell
   dotnet run --project work\tv-device -- install-probe
   ```

### ⚠️ Configure these two addresses first

The addresses of the author's own machines are still in the source — change them for your network:

| Where | What | Note |
| --- | --- | --- |
| `work/steam-tv/probe.c` (3 places) | the streaming **PC** address (`192.168.1.9`) | the client looks for the host at this address |
| `work/tv-device/Program.cs` (line 12) | the **TV** address (`192.168.1.6`) | used by the sideload installer |

Making the PC address a UI setting instead of a compile-time constant would be a welcome contribution.

### Layout

```
work/steam-tv/        the app: probe.c (session/logging/HID), media.cpp (EMSS player, audio, metrics),
                      app.js + index.html (UI, watchdog, video clock), gamepad.c, config.xml
work/ihslib-reference the Steam in-home-streaming protocol library (LGPL-3.0, bundled)
work/protobuf-c, work/mbedtls, work/moonlight-platform/opus   build dependencies
work/tv-device/       tiny .NET sideloader (sdb install), used instead of Tizen Studio
work/compare-runs.mjs the measurement evaluator (see below)
outputs/              analysis documents (Hungarian) + the ready-to-install SteamTV.wgt
```

## 4. Measurements

The client forwards its own diagnostics to the host over the stats channel, so the host's
`streaming_log.txt` ends up containing the TV's log interleaved with Steam's own — and that single file
is enough to explain a stutter after the fact:

```powershell
node work\compare-runs.mjs "C:\Program Files (x86)\Steam\logs\streaming_log.txt" --list
node work\compare-runs.mjs "C:\Program Files (x86)\Steam\logs\streaming_log.txt" --session 5
node work\compare-runs.mjs "C:\Program Files (x86)\Steam\logs\streaming_log.txt" --session 5 --timeline
```

* `MERES` lines (every ~3 s): received/presented frames, host-side frame interval, local arrival gap,
  assembly time, SDK append time, lock waits, audio rate/gaps, loss counters, drop reasons.
* A 1 Hz health line, a `Render:` report from the browser, and `Idozites:` media-callback timings.
* `--timeline` **joins them per second** and annotates every second with the host's own pipeline events
  (encoder rebuilds, stream switches, adapter changes) and its `SessionStats` windows (host-side fps,
  network share/latency), then classifies each bad second as *frames never arrived* vs *frames arrived
  but were not shown*, and counts real **freezes** (a presented-frame gap ≥ 500 ms).

What that tooling established on the measured runs (documents in `outputs/`, Hungarian):

* the client side is fast — platform appends 0.0–0.5 ms even in the worst seconds, audio 100 frames/s
  with 0 errors, and the presented rate follows the arrival rate (r ≈ 0.86–0.93);
* the visible stutters were **arrival-side**: ~91% of bad seconds in the longest run, of which a third
  coincide with Steam rebuilding its capture/encoder pipeline (stream-mode switches Desktop ↔
  GameOverlay cost a 2–10 s freeze), the rest with the host producing fewer frames than requested
  (its own `SessionStats` shows 30–36 fps while no stage is marked slow);
* the TV-side latency mode and display re-init were tested as hypotheses and are documented as such.

## 5. Licensing

* **This project's own code** (everything under `work/steam-tv/`, `work/tv-device/`, the build and
  measurement scripts, `outputs/*.md`): MIT — see `LICENSE`.
* **Bundled third-party code keeps its own license:**
  * `work/ihslib-reference` — Steam In-Home Streaming Library, **LGPL-3.0** (`LICENSE` inside). It is
    bundled in source form; builds that link it must comply with the LGPL.
  * `work/mbedtls` — Apache-2.0 (or GPL-2.0, per its `LICENSE`).
  * `work/moonlight-platform/opus` — Opus, 3-clause BSD (`COPYING`).
  * `work/protobuf-c` — BSD (`LICENSE`).
* The **Samsung Emscripten SDK** and the Tizen WASM Player are Samsung's, redistributed only by
  Samsung; download them from Samsung Developer.

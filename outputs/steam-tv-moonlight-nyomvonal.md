# Steam TV — a Moonlight Tizen nyomvonal kivizsgálása (4. kör)

Állapot: **v1.4.1** fenn a tévén (`outputs/SteamTV.wgt`) — ebben a verzióban a kocka-stage jelentés
mellett az is biztosított, hogy a futás **tényleges késleltetési módja** mindig bekerül a naplóba
(lásd a mérési audit 11. szakaszát). Az utolsó mért futás (13:27–13:32, v1.4.0, `kesleltetes=0`,
`ultra-low időzítés`, hardveres dekóder) szolgál viszonyítási alapnak.

---

## 1. Miért néztem meg ezt a nyomvonalat

A felhasználói megfigyelés: a Moonlight Tizen is recsegő hangot ad csomagvesztés és hálózati jitter
nélkül is, vagyis a recsegés nem feltétlenül a mi kódunkban keletkezik. Ez pontosan az a kérdés, amit
egy független, ugyanazon a platformon futó klienssel lehet eldönteni — ezért nem elméletet kerestem,
hanem **konfiguráció-összehasonlítást**: ugyanabban az EMSS-módban futunk-e, mint a Moonlight.

## 2. Források

| Forrás | Mit adott |
| --- | --- |
| [brightcraft/moonlight-tizen wiki — Known Issues & Limitations](https://github.com/brightcraft/moonlight-tizen/wiki/Known-Issues-&-Limitations) | Tizen 9.0+ Game Mode regresszió (recsegő hang, akadás, fekete kép); „Video Stuttering … despite the FPS stats reporting the correct value" = Tizen display-sync hiba; 100 Mbps-os tévé-Ethernet burst probléma |
| [OneLiberty/moonlight-chrome-tizen issue #87 (Audio crackling)](https://github.com/OneLiberty/moonlight-chrome-tizen/issues/87) + kommentek | A recsegés a projektben régóta ismert, nincs rá bizonyított javítás; hálózati oldali (Wi-Fi csatorna, OpenWRT) munkamenet-javulások; a Samsung WASM doksi szerinti szoftveres Opus-dekódolás követelménye |
| `OneLiberty/moonlight-chrome-tizen`, `samsung_wasm` ág: `wasm/main.cpp`, `wasm/wasmplayer.cpp`, `wasm/auddec.cpp`, `wasm/moonlight_wasm.hpp` | **A döntő tény**: a Moonlight ugyanazt az EMSS-módot és ugyanazt az audió-utat használja, mint a mi kliensünk |
| [Samsung — Game Streaming on Tizen TV with Wasm](https://developer.samsung.com/smarttv/develop/extension-libraries/webassembly/game-streaming-on-tizen-tv-with-wasm.html), [Tizen WASM Player — Advanced features](https://developer.samsung.com/smarttv/develop/extension-libraries/webassembly/tizen-wasm-player/using-tizen-wasm-player/using-advanced-player-features.html) | Platform-oldali referencia (a doksi törzse JS-ből töltődik, ezért idézhető szöveget nem ad; a helyi, olvasott SDK-fejlécek a mérvadók) |
| `moonlight-chrome` commit `3ce2d68` („wait-free algorithm for audio queueing") | **Nem alkalmazható** — lásd 4.4 |

## 3. A döntő összehasonlítás: a Moonlight ugyanabban a módban fut, mint mi

| | Moonlight Tizen (`samsung_wasm`) | Steam TV kliens (v1.4.0) |
| --- | --- | --- |
| Stream source | `ElementaryMediaStreamSource(LatencyMode::kUltraLow, RenderingMode::kMediaElement)` (`wasm/main.cpp`, konstruktor) | `ElementaryMediaStreamSource(LatencyMode(), kMediaElement)` (`media.cpp` 155); `LatencyMode()` auto → `kUltraLow` (a tévé `has_ultra_low_latency=1`, `kesleltetes=0`) |
| Audió-track | `"audio/webm; codecs=\"pcm\""`, `SampleFormat::kS16`, `kStereo`, 48000 | ugyanaz (`media.cpp` 268–271) |
| Audió-dekódolás | saját `opus_multistream_decode` → PCM a platformnak | saját `opus_decode` → PCM (`AudioSubmit`) |
| Audió-beadás | `AppendPacket` (blokkoló), 5 ms-os frame-ek, 100 ms overflow-drop | `AppendPacketAsync`, 10/20 ms-os csomagok (`audio_batch.h`) |
| Videó-beadás | `AppendPacket` (blokkoló), opcionális frame pacing | `AppendPacketAsync` + stage-jelentés |
| Game Mode metadata | a `ForceGM` változatban `use.game.mode` | **nálunk is be van kapcsolva** (`config.xml` 10. sor) |
| Ismert tünetek | recsegő hang, akadás „helyes FPS mellett", fagyás, 1080p feletti torzulás | recsegő hang, mikro-akadás, rövid fagyás, „a kép is fos" |

A Moonlight wiki szerint Tizen 9.0-tól a platform Game Mode lánca megváltozott, a WASM-appoknál a
Game Mode nem aktiválódik megfelelően, és **éppen az Ultra Low Latency út** az, amit kerülni kell:
a dokumentált jó kombináció a Game Mode metadata **+ Low Latency** (nem Ultra Low Latency).
A mi jelenlegi konfigurációnk ennek a tükörképe: **Game Mode metadata be + Ultra Low Latency kérve**.

Ez a tévénk pontos generációja (Tizen 9.0), és a leírt tünetek egy az egyben a mieink. Ez azonban
**hipotézis, nem bizonyíték** — a döntést az 5.1 kísérlet hozza meg.

## 4. Jelöltek és ítéletek

### 4.1 Tizen 9.0 Game Mode / Ultra Low Latency regresszió — **ILLIK** (hipotézis)
A wiki szerint a tünetek: recsegő hang, nagy fps/bitráta melletti instabilitás, fagyás az első
kockán, 1080p felett torzulás. Nálunk: recsegés + mikro-akadás + rövid fagyás, ultra-low módban.
A jelenlegi méréseink szerint a kliens-oldali lánc az akadó másodpercekben **nem lassú**
(beadás 0,2–0,4 ms, `fed≈prez`, audió 99,97 frame/s, 0 hiba, 0 visszautasítás) — ez pontosan a wiki
„helyes FPS-statisztika mellett is akad" esete, vagyis a hiba a platform és a kliens között lehet,
nem a protokollkódban.

### 4.2 Tizen display-refresh inicializálási hiba — **ILLIK** (hipotézis)
A wiki külön szakaszban írja: a tévé nem inicializálja a maximális képfrissítést, ezért a kép
akadozik, miközben az app FPS-számai helyesek. Dokumentált munkamenet: **kapcsolj át egy bekapcsolt
HDMI-forrásra, majd onnan indítsd az appot.**
Nálunk ez a `Render:` sorok 60 fps ↔ 7,7 fps (p95 512 ms) ingadozása, a host `display 0.00`
oszlopa és a `dropAPI` +79/s — miközben a `raf` stabilan ~59,9 Hz és a kliens beadás gyors.

### 4.3 Szoftveres Opus-dekódolás kényszere — **LEZÁRVA, nálunk helyes**
A Samsung WASM doksi (issue #87-ben idézve): régebbi tévék nem tudnak Opus-t hardveresen dekódolni
low-latency módban, ezért az appnak kell libopusszal PCM-mé dekódolnia, és `audio/webm; codecs="pcm"`
+ `kS16` formában beadnia. **A mi kliensünk pontosan ezt teszi** — itt nincs javítanivaló.

### 4.4 `AppendPacketAsync` és a újrahasznált PCM-puffer — **ELVETVE mint fő ok**
Felmerült, hogy a `pcm` verem-puffer (és a `pcmBatch.data`) újrahasználata az aszinkron beadás
miatt recsegést okozhat. A helyi SDK-fejléc egyértelmű: *„For clear packets, `AppendPacketAsync()`
is functionally equivalent to `AppendPacket()` (i.e. it will validate the packet and return result
synchronously)"* — a mi csomagjaink titkosítatlanok (a PCM nem titkosított), tehát a hívás
szinkronként viselkedik. **Nem ez a fő ok**, de ha az 5.1 kísérlet nem hoz javulást, olcsó
(kb. 10 soros, visszavonható) próba marad: friss puffer minden beadáshoz.

### 4.5 `moonlight-chrome` wait-free audió-sor commit — **NEM ALKALMAZHATÓ**
A `3ce2d68` egy 2016-os PNaCl-korszakbeli commit: a saját `AudioPlayerSampleCallback`et cseréli
lock-mentes gyűrűpufferre. Az EMSS/WASM playerben **nincs ilyen callback** — a hangkimenetet a
platform birtokolja, mi csak csomagokat adunk be. Nincs mit átvenni.

### 4.6 100 Mbps-os tévé-Ethernet és burst — **NYITOTT, hálózati oldal**
A wiki külön szakasza: a tévé beépített Ethernet-portja 100 Mbps, a host Gigabit-es framingje
(kulcskockák!) burstöt okoz, ami a tévénél csomagvesztés/fagyás lesz; 5 GHz Wi-Fi gyakran jobb.
Nálunk a mért jelek (`win=754` age-alapú discard, `loss=4`, `varr/aarr` kiugró maximumok,
host `network 37,83 / 57,50 / 70,42` ms) ebbe az irányba mutatnak. **Kérdés a felhasználóhoz:
a tévé vezetéken vagy Wi-Fi-n van?** (A `192.168.1.6` cím ezt nem árulja el.)

## 5. A döntő kísérletek (nem kell új build)

### 5.1 E1 — késleltetési mód A/B (ez dönti el a 4.1 hipotézist)
1. Indítsd el az appot, és **stream indítása előtt** a távirányítóval nyomd meg a
   `Késleltetés (teszt)` gombot **kétszer**: `Automatikus → Normál → Alacsony`.
   (A gomb a beállítást a következő stream-indításra alkalmazza.)
2. `Big Picture indítása`, majd játsz egy ugyanolyan hosszú menetet, mint a 13:27-es futás.
3. Ellenőrzés a naplóban: a `MERES: RUN … kesleltetes=2` és a
   `Média: Videó-dekóder mód: hardveres (…, low időzítés)` sor igazolja, hogy tényleg Low módban fut.
   (v1.4.1-től ez a sor garantáltan bekerül a naplóba, és a `compare-runs.mjs --list` is kiírja
   `latency=low` formában — lásd a mérési audit 11. szakaszát.)
4. Kiértékelés: `node work/compare-runs.mjs "C:\Program Files (x86)\Steam\logs\streaming_log.txt" --list`,
   majd `--session N` — összevetve az ultra-low futással (`varr`, `win`, `loss`, `aarr`, hiányzó
   audió frame-ek, `Render:` minták). **A v1.4.1 mérési javítás önellenőrzése**: a `--list` sorban a
   `samples=` értéknek ~96-nak kell lennie 193 másodpercre (a 13:27-es futásban 7 volt, mert a
   `Média: MERES` sor a 30 s-os naplóosztályba esett) — ha ez megvan, a fő mérési sor végre 2
   másodpercenként érkezik, és a `SUMMARY` is megjön a futás végén.
5. Ha a recsegés/akadás érdemben csökken: a 4.1 hipotézis igazolva, és a minimális javítás az
   **alapértelmezett mód Low-ra állítása** (egy sor, visszavonható).

### 5.2 E2 — display-újrainicializálás (a 4.2 hipotézis)
Ugyanabban a buildben, kódváltozás nélkül: kapcsold be a HDMI-forrást (pl. konzol), válts át rá,
majd onnan indítsd a Steam TV appot és a streamet. Ha a `Render:` 7,7 fps-es szakaszai eltűnnek,
akkor a 4.2 pont a magyarázat, és ez nem javítható a kliens kódjában.

### 5.3 A már fenn lévő stage-jelentés ellenőrzése
Ugyanezekben a futásokban nézd meg a host `Slow framerate: game … network … decode … display …`
sorát: a `decode` mezőnek mostantól **nem 0,00**-nak kell lennie. Ez annak a bizonyítéka, hogy a
host végre a valós dekódolási időnket látja, és nem a hálózatot okolja minden le nem zárt kockáért
(a `game 1.00` fps-es limiter így oldódhat).

## 6. Amit ez a kör nem bizonyít

- Nem bizonyított, hogy a platform az ok: csak az, hogy **egy dokumentált platform-hibamód pontosan
  a mi konfigurációnkra és tüneteinkre illik**. Az 5.1 futás dönt.
- Ha E1 nem javít, a következő kör a hálózati/vételi oldal (per-másodperces `varr`/`win`/`loss`) és a
  host kódoló-újraépülései körül dolgozik, illetve a 4.4 szerinti puffercsere-próbát futtatja.
- A `display` oszlop továbbra is 0,00 marad (nincs megjelenítési callback `kMediaElement` módban) —
  ez dokumentált korlát, nem hiba.

## 7. Visszavonhatóság

Ebben a körben **nem változott kód**. Az 5.1 kísérlet a meglévő `Késleltetés (teszt)` gombbal és a
`localStorage`-ban tárolt értékkel megy vissza `Automatikus`-ra; az 5.2 tisztán tévé-oldali.

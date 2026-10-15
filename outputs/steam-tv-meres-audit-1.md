# Steam TV — átvétel, Codex-változások átvizsgálása és célzott mérések (1. kör)

Dátum: 2026-10-15. Kiindulás: a TV-n a **11:22:51-es csomag** (`outputs/SteamTV.wgt`, SHA256
`2505C1F2…C25B8`) futott, a munka 13:03–13:08 között félkész forrásmódosításokkal állt le (ezek
mérési számlálók voltak, build és bekötés nélkül). Ez a kör **audit + mérés**, nem refaktor.

Telepített új csomag: **v1.4.0** (a verzió a képernyőn és minden mérési sorban megjelenik),
telepítés: `install completed`.

---

## 1. Amit átvettem és átvizsgáltam

- Átnéztem a Codex 11 körét (`outputs/steam-tv-*.md`, 17:35 → 11:23) és a hozzájuk tartozó kódot:
  SDK CAS-hiba megkerülése, `AppendPacketAsync`, videó-idővonal a host időbélyegeiből, PCM-összevonás,
  külön track-zár, aszinkron teardown-szál, `Idozites:`/`Render:` mérések, diagnosztikai overlay.
- Lefuttattam három független auditot (reordering/retransmission, videó-út és mérhetőség,
  munkatörténet) — ezek eredménye beépült az alábbiakba.
- **Megállapítás: a 13:03–13:08 közötti forrásmódosítások nem voltak sem megépítve, sem bekötve**, és a
  11:22-es csomag óta **egyetlen tévés futás sem történt**. A „javított” állapot tehát hardveren nem
  igazolt; ezt a kört is mérési alapnak szánom, nem lezárásnak.

---

## 2. Mi hiányzott a pontos diagnózishoz (és most már mérve van)

| Hiányzó adat | Mit dönt el | Állapot |
|---|---|---|
| host-oldali kockaütem | PC/kódoló késik-e | **mérve**: `MERES vhost` (a host saját időbélyegeiből) |
| helyi kockaérkezési ütem | hálózat/vétel késik-e | **mérve**: `varr`, `vasm` |
| `AppendPacket` hívások közti idő + zárra várás | kliens lassít-e | **mérve**: `vint`, `vdec`, `vwait` |
| receive-oldali eldobások | a kliens fogadó útja veszít-e | **mérve**: `win=discard/overflow`, `part`, `loss` |
| audio érkezési ütem (helyi + host) | recsegés burst-érkezésből jön-e | **mérve**: `aarr`, `ahost` |
| ténylegesen prezentált kockák | megjelenik-e minden | **mérve**: `prez` (rVFC `presentedFrames`) + `Render:` sor |
| a telemetria saját vesztesége | hiányzó sor = nem történt meg / elveszett | **javítva + mérve**: külön mérési időbélyeg, `Naplo: kihagyva …` |
| kulcskocke-kérés oka szerint | a `ReportLost` hurkok | **részben**: `loss` összesen (ok szerinti bontás még nyitott) |

A mérések definíciója, a mit-lehet/mit-nem-lehet lista és az A/B protokoll:
`outputs/steam-tizen-measurements.md`. Kiértékelő eszköz: `work/compare-runs.mjs`.

---

## 3. Konkrét hibák, amiket találtam (és amit megtettem)

| # | Hiba | Bizonyíték | Állapot |
|---|---|---|---|
| 1 | **A telemetria 900 ms-os közös kapuja** stream közben elnyelte a méréseket: a `Render:`/`Idozites:` mentesült, de frissítette a közös időbélyeget | mért: `Média: Videó` **14/50 (28%)**, **29/162 (18%)**; `Él` **0** | **javítva** (külön `host_log_last_measurement_ms`), a kihagyások száma az `Él:` sorban látszik |
| 2 | **A `MERES`/`SUMMARY` sorok nem jutottak ki** volna a fenti kapu miatt | kód + a fenti arányok | **javítva** (mentes a 900 ms-os keret alól) |
| 3 | **A host `decode`/`display` oszlopa mindig 0,00**: a kliens minden elfogadott kockát `Displayed`-ként jelent, és soha nem hívja `IHS_SessionReportVideoFrameStage`-et | `media.cpp` + host napló | **nem változtattam** (viselkedést érint); dokumentálva |
| 4 | **A `SubmitFrame ReportLost` → kulcskocke-kérés korlátlan**, és minden megérkező kulcskocka újra felfegyverzi a kaput (önfenntartó hurok) | 10:08:29: **455 `Coming keyframe` / 25 s**, 1293 beadási kísérlet, **0 append** | **mérve** (`loss`), a korlátozás a mérés után |
| 5 | **A 450 ms-os videó-discard egyetlen hiányzó csomagért ~450 ms-ra megállítja a teljes videóláncot** (60 fps-en ~27 kocka) — a kódban „20 ms reorder tolerance” néven szerepel, valójában nem az | `window.c` + `ch_data.c`; a `Discarded` sor Verbose szinten van, ezért eddig láthatatlan volt | **mérve** (`win`), a küszöbhöz nem nyúltam |
| 6 | **A naplócsatorna maga veszteséges** (reliable, de retranszmisszió nélkül) → hiányzó sor nem megkülönböztethető a nem létezőtől | `ch_stats.c` | **enyhítve** (a kihagyás-számlálók + a mérési kapu javítása) |
| 7 | **Az ismeretlen csatornára érkező csomagok csendben eldobódnak** stream indulásakor | egy sessionben 222 `Unknown channel for packet` sor | dokumentálva, nem mérve |

Ami Codexből **helyesnek bizonyult** és nem nyúltam hozzá: az SDK CAS-hiba megkerülése (van kép), a
külön track-zár + `AppendPacketAsync` (a mérés szerint az append 0,13–5 ms, `vwait` kivételes esetben
nagy), az aszinkron teardown, a 20 ms-os PCM-összevonás, a `Render:`/`Idozites:` mérési keret.

---

## 4. A receive/reorder/retransmission logika ítélete

- **A csomag-átrendeződés javítása tart.** `window.c`: a beírási pozíció a *tail* id-jéhez igazodik
  (`writePos = (tail.pos + tailOffset + cap) % cap`), a „már feldolgozott” korai visszatérés csak akkor
  lép, ha a slot valóban a head mögött van, és a foglalt slot duplikátum. Ezért egy ablakon belüli,
  későn érkező csomag **mindig élő slotba kerül**, nem veszhet el.
- **Az egyetlen visszafordíthatatlan döntés az age-alapú `Discard`** (videó 450 ms, audió 200 ms): ez
  nem veszteséget okoz, hanem holtpontot old — de a fenti 5. pont szerint ez a holtpont maga a
  mikro-akadás.
- **Retransmission:** nincs give-up, nincs „túL korán elveszettnek nyilvánítás”; a törlés csak ACK/NACK
  hatására történik, a regisztráció a küldés előtt van. Ez helyes.
- 27 session alatt: `items overflow` 0, `partial frames was cleared` 0, „Partial frames stalled” 0,
  `Unexpected video frame sequence` 5 (mind 10-14-én), `Decoder reset` 2.

---

## 5. Mit bizonyítanak már a meglévő naplók

- **A 12:57:02–12:57:06 közötti akadás host-oldali volt**: a host naplója `Switching video stream from
  GameOverlay_MovieStream to Desktop_MovieStream`, NVENC-újraépítés, `Dropped 1 video frames`,
  60→30 target framerate, `desktop access lost` — a kliens közben nem veszített csomagot, nem kért
  kulcskockét, a zárra várás 0,24 ms volt.
- **A 10:58:09–13 közötti 7–10 FPS-es ablakban a kliens beadása 0,2–0,4 ms volt**, és a `fed`/`prez`
  együtt esett (r = 0,9218): a megjelenítés követi a beérkezést, nem a TV a szűk keresztmetszet.
- **A hang mérten egészséges** (99,97 frame/s, 0 hiba, 0 vissza), és a hang-maximumok nem esnek egybe a
  videó beeséseivel → a „hang blokkolja a képet” feltevés ezekkel az adatokkal nem igazolt.
- **Nincs valódi renderelt-fps mérés ezen a platformon.** A `prez` a compositorig bemutatott kockák
  száma (rVFC `presentedFrames`), a `dropAPI` ellentmond neki; a `currentTime` ultra-low módban a PTS-ekből
  számol. Ezért a naplóban és a dokumentumban is **külön** kezeljük a beadott / prezentált / dropAPI
  számokat, és egyiket sem nevezzük renderelt fps-nek.

---

## 6. Amit beépítettem (kizárólag mérés, minimális és visszavonható)

- **Verzió**: `TV_BUILD_VERSION = "1.4.0"` egy helyen (`probe.c`), megjelenik a UI-on (`#version`) és a
  `RUN`/`MERES`/`SUMMARY` sorokban.
- **`RUN`** sor stream induláskor (verzió + beállítások + késleltetés-mód).
- **`MERES`** sor 2 s-onként, fix mezősorrendben: `fed, prez, rvfc, clock, media, vpts, apts,
  vhost, varr, vasm, vdec, vwait, vint, aud, ahost, aarr, aapp, loss, rst, part, win, ref, drop,
  loop, pad, proxy`. Forrása: a media.cpp saját hisztogramjai + az új
  `IHS_SessionGetStreamMetrics()` (assembly/érkezés/host-ütem/veszteség/ablak/audio-gap számlálók) +
  a JS által a megosztott memóriába írt prezentációs adatok.
- **`SUMMARY`** a stream végén (összesített darabszámok és maximumok).
- **`Él:` sor bővítése**: napló-kihagyások okonként (`meres`, `szivveres`, `5ms-res`, `keret`).
- **JS→C megosztott mezők**: `presentedFrames`, `mediaTime`, gamepad-minta/max hézag; a főciklus méri a
  JS-proxy hívásokra fordított időt (`proxy=µs/s és max`).
- **A/B kapcsolók** (a következő streamre hatnak, naplózva): `Hang: be/ki` (már volt) és
  **`Késleltetés (teszt): Automatikus/Normál/Alacsony/Ultra-alacsony`** — utóbbi a késleltetési mód
  A/B-jéhez; a használt mód a `Videó-dekóder mód:` sorban `normal`/`low`/`ultra-low` néven jelenik meg.
- **Eszköz**: `work/compare-runs.mjs` — a naplóból futásonkénti táblázatot és jó/rossz összevetést ad.

**Nem változott:** párosítás, kontroller-kezelés, hang-út logikája, felbontás/fps/bitráta/kodek,
késleltetési mód alapértéke (auto), a média-út időzítései; nem került be fix puffer, frame drop vagy
új várakozás.

---

## 7. Ellenőrzési lépések (amit kérek)

1. Zárd be teljesen az appot, indítsd újra (a csomag v1.4.0 fenn van).
2. Egy futás: 1080p/60, auto bitráta, hang be, késleltetés auto. Jegyezd fel nagyjából, **mikor volt
   sima és mikor akadt** (a `MERES t=` mezőből visszakereshető).
3. Ezután a `Késleltetés (teszt)` gombbal egy futás **Normál**, egy **Ultra-alacsony** módban, és egy
   futás **hang ki** állapotban (a 4. pont A–E táblázata szerint).
4. Kiértékelés a PC-n:
   `node work/compare-runs.mjs "C:\Program Files (x86)\Steam\logs\streaming_log.txt" --list`, majd
   `--session N`. A záró táblázat megmondja, hogy a `vhost` (host) vagy a `varr`/`vasm` (hálózat/vétel)
   a nagyobb — ez dönti el, hogy a PC-oldali kódolást vagy a hálózatot kell javítani.

E nélkül a következő javítás önkényes lenne; a jelenlegi mérésekkel viszont a stutter minden jelöltje
(PC-kódoló, hálózat, fogadó út, platform, megjelenítés) egy futásból szétválasztható.

---

## 8. 2. kör: a mérési hiányok lezárása (ugyanabban a buildben, v1.4.0)

Az auditok két olyan hiányt jeleztek, ami nélkül a stutter oka nem bizonyítható; ezek most mérve vannak:

1. **Miért nem jutott el a kocka a platformig** — a `VideoSubmit` minden visszatérési útja külön
   számlálót kapott: `vdrop=no-track/empty/too-big/need-key/pts/append`. Eddig csak a
   platform-visszautasítás volt számolva (`ref`), így a „kocka eltűnt” esetek egy része láthatatlan volt.
2. **Miért kért a kliens kulcskockét** — a könyvtárban minden `VideoLossRequest` **okot** kapott:
   `lossr=sequence/wait/decrypt/stall/mismatch/submit`. Ez különíti el a hálózati veszteséget (sequence),
   a 200 ms-os várakozás-lejáratot (wait), a partial-frame holtpontot (stall, 150 ms), az összeállítási
   ütközést (mismatch) és a dekóder-visszautasítást (submit) — a 10:08-as 455 kulcskocke/hurok így
   számszerűen szétválik.
3. **A 450 ms-os videó-discard költsége közvetlenül mérve** — a data-worker mostantól legfeljebb 1/s
   Warn sort ír csatornánként:
   `Discarded N packets, no complete frame for X ms (tolerance Y ms)`.
   Ez a H3-ban leírt mechanizmus (egy hiányzó frame-head miatt a teljes videólánc áll akár ~450 ms-ig)
   egyetlen közvetlen bizonyítéka; eddig a `Discarded` sor Verbose szinten volt, ezért a naplóban
   „0” szerepelt, ami **nem** bizonyíték.
4. **Szerződés-teszt** (`work/test-metrics.mjs`): a `media.cpp` formátumszövegéből kiolvassa az emitált
   mezőket, összeveti a `compare-runs.mjs` által olvasott kulcsokkal, ellenőrzi a `vdrop`/`lossr`
   sorrendet, és szintetikus naplón ellenőrzi az aggregációt. **Az első futása azonnal talált egy valódi
   hibát a saját eszközömben** (`lastValue` a tömböt adta át a minta helyett → minden összesített érték
   `null` lett volna a riportban), és egy hiányzó mező-olvasást (`clock`) is jelzett.

A build és a telepítés: `BUILD=0`, `install completed`, a csomag `outputs/SteamTV.wgt` (v1.4.0).
Tesztek: `node work/steam-tv/test.mjs` ✓, `node work/test-metrics.mjs` ✓, `work/test-native.ps1` ✓.
Az appot nem indítottam el; a méréshez a 7. pont A–E futásai kellenek.

---

## 9. 3. kör: a 13:27–13:32-es tévés futás kiértékelése és az első bizonyított javítás

A felhasználó lefuttatta a v1.4.0-ás mérést („akad és a kép is fos"). A napló kiértékelése (a
`win=754`, `loss=4`, `rst=1`, 193 db 1 Hz minta és a host saját sorai):

**A bizonyítékok lánca**

1. **A kliens nem lassít az akadó másodpercekben.** A legrosszabb másodpercekben (fed 10–30 kocka/s)
   a platform-beadás **0,2–0,4 ms**, a medián másodpercben 2,8 ms; a `vwait` (zárra várás) 5 ms alatt.
   A `fed` és a `prez` együtt esik (10/10, 15/14, 29/28, 30/30) → a megjelenítés követi a beérkezést.
2. **A host saját naplója a hálózatot okolja, miközben a dekódolás/megjelenítés oszlopa 0,00:**
   `Slow framerate: game 1.00, capture 0.00, convert 0.00, encode 0.00, network 37.83 / 57.50 / 70.42,
   decode 0.00, display 0.00 (network)`; a SessionStats `AvgNetworkMS 15.8, AvgDecodeMS 0,
   AvgDisplayMS 0, AvgFrameMS 18.7` (53 fps).
   **Az ok mérhető: a kliens soha nem küldte el a kocka decode/upload eseményeit**, ezért a host
   minden le nem zárt kockát a hálózathoz könyvel, és a framerate-limiterje (game 1–10 fps)
   lefojtja a streamet — miközben a kliens oldal mérése szerint ott semmi sem lassú.
3. **A fogadó út valóban veszít:** `win=754` (a 450 ms-os age-alapú discard), `loss=4`
   kulcskocke-kérés, `rst=1` dekóder-reset; **369 hiányzó audió frame** (ez a recsegés valódi forrása,
   nem a kliens blokkolása: `hiba=0, vissza=0`).
4. **A megjelenítés oldala is sérül:** a `Render:` sorok 60 fps (jitter 7–12 ms) és **7,7 fps
   (p95 512 ms)** között ingadoznak, `dropAPI` 1367 → 1446 (+6/s), `raf~59,9 Hz`, `panel=NA`.
5. **A host kódolója többször újraépült** a futás alatt: `desktop access lost, reacquiring`,
   `Switching video stream … SecureDesktop_MovieStream …`, `Video Encoder Changed` (többször),
   `Dropped 1 video frames`. Minden újraépítés kulcskockával és minőség-újraramppal jár — ez a
   „kép is fos" másik, host-oldali oka.

**Az első, méréssel indokolt javítás (minimális és visszavonható)**

`media.cpp`: a kliens mostantól jelenti a hostnak a **kocka decode-kezdet/vég** eseményeit
(`IHS_SessionReportVideoFrameStage`, a `readyAt` → append-vég intervallummal). Ez az, amit ténylegesen
mérünk; a platform `kMediaElement` módban nem ad megjelenítési callbacket, ezért az upload-pár
ugyanazt az időbélyeget kapja (a `display` oszlop emiatt 0,00 marad — ez dokumentált korlát).
**Visszavonás:** a `kReportFrameStages` flag `false`-ra állítása (egyetlen szó), vagy a
`SteamTV-before-stage-reports.wgt` visszatelepítése.

Amit a következő futásnak igazolnia kell: a host `Slow framerate: …` sorában a `decode`/`display`
érték **nem 0,00**, és a limiter indoka nem (csak) a hálózat; ha a stutter emellett is megmarad, akkor
a hálózat/delivery az ok, és a további munka a Wi-Fi/host oldalra tartozik.

**Ebben a körben javított mérési hibák** (mindkettő a valós futás adatából derült ki):

- a `MERES` sorok **kumulatív** számlálókat osztottak a 2 s-os ablakkal → az első sor 1946 kocka/s-ot
  mutatott; mostantól minden ráta két minta **különbsége**;
- a maximumok (`vhost/varr/vasm/aarr`) a teljes munkamenet maximumai voltak, ezért a start-up csúcs
  minden későbbi ablakban „hibának" látszott; mostantól külön **ablak-maximum** mezők vannak
  (`IHS_SessionGetStreamMetrics` a beolvasáskor nullázza őket), a `SUMMARY` pedig a munkamenet-maximumot
  hozza.

---

## 10. 4. kör: a Moonlight Tizen nyomvonal (kódváltozás nélkül)

A felhasználói lead („a Moonlight Tizen is recseg, csomagvesztés nélkül is") kivizsgálása külön
dokumentumban: **`outputs/steam-tv-moonlight-nyomvonal.md`**.

A lényeg: a Moonlight Tizen (`samsung_wasm` ág) forrása szerint ugyanabban az EMSS-módban fut, mint a
mi klienünk — `ElementaryMediaStreamSource(kUltraLow, kMediaElement)`, `audio/webm; codecs="pcm"` +
`kS16`/stereo/48k, saját libopus-dekódolás, app-oldali beadás —, és a projekt wiki-je Tizen 9.0-tól
**éppen ezt az Ultra Low Latency utat** nevezi meg a recsegő hang / „helyes FPS mellett is akadó kép"
okaként (a dokumentált jó kombináció: Game Mode metadata + **Low Latency**). A mi konfigurációnk ennek
a tükörképe (Game Mode metadata be + Ultra Low kérve), és a tévénk Tizen 9.0.

Ez **hipotézis, nem bizonyíték**, ezért ebben a körben nem nyúltam a kódhoz. A döntést két, build
nélkül elvégezhető kísérlet hozza meg (részletesen a külön doksiban):

- **E1**: a `Késleltetés (teszt)` gomb kétszer (`Alacsony`), majd stream-indítás — a naplóban a
  `kesleltetes=2` és a „low időzítés" sor igazolja; utána `compare-runs.mjs --session N` összevetés a
  jelenlegi ultra-low futással.
- **E2**: bekapcsolt HDMI-forrásra váltás, majd onnan app-indítás (a wiki szerinti display-sync
  munkamenet) — a `Render:` 60 fps ↔ 7,7 fps ingadozásának megszűnését kell hoznia, ha a 4.2 pont az ok.

Emellett az ugyanezekben a futásokban ellenőrizendő, hogy a host `Slow framerate` sorában a `decode`
oszlop már **nem 0,00** (a 3. körben beépített stage-jelentés hatása).

---

## 11. 4. kör (folytatás): három mérési hiba javítása a késleltetés-A/B előtt — v1.4.1

Az E1 kísérlet (késleltetési mód A/B) előtt átnéztem, hogy a napló egyáltalán **attribútálható-e**
egy módhoz, illetve hogy a mérési sorok egyáltalán megérkeznek-e. Három valódi hibát találtam,
mindhárom a mérést rontja (a streamingmagot nem érinti):

1. **A mód-címke hibás volt.** A `media.cpp` EMSS-képesség sora
   `LatencyMode() == kUltraLow ? "ultra-low" : "normal"` hármas helyett kettős volt: **Low módban is
   „normal"-t írt volna**, így egy Low futás megkülönböztethetetlen lett volna egy Normál futástól.
   Javítva `LatencyModeName(LatencyMode())`-ra (a dekóder-sor már eddig is ezt használta).
2. **A módot azonosító sor kiesett a naplóból.** A `forward_to_host()` a „sima" osztályú sorokat
   30 másodperces szívverés-büdzsével engedi át, ezért a `Média: EMSS: available=…` sor **egyetlen
   mért munkamenetben sem** jutott el a hostig (0/3 a `streaming_log.txt`-ben, 2026-10-15).
3. **A fő mérési sor (MERES) 15× ritkábban érkezett, mint kellett volna — és a SUMMARY elveszett.**
   A büdzsé-osztályozás a **teljes** sort vizsgálta (`strncmp(line, "MERES ", 6)`), a média-réteg viszont
   `Média: ` előtaggal küldi a sorait (`CLIENT: TV: Média: MERES v=…`), így a MERES és a SUMMARY
   egyetlen mentesítési feltételbe sem illett bele, és a **30 másodperces** „sima" osztályba esett.
   Bizonyíték a 2026-10-15 13:27-es, 193 másodperces v1.4.0 futásból: **pontosan 7 MERES sor**,
   30/32 másodperces távolsággal (t=10, 42, 72, 102, 132, 163, 193) ~96 helyett, és a záró
   **SUMMARY egyáltalán nem érkezett meg** (0 sor). Miközben a `Média: Videó:` 1 Hz-es sor
   (ami a `Média: Videó` előtag miatt mérésnek számított) szabályosan, ~1/s-mal megjött — vagyis nem
   a hálózat vagy a host nyelte el őket, hanem a saját büdzsé-osztályozásunk.
   **Javítás:** az osztályozás mostantól az **előtag nélküli törzsön** történik (`body`), így a MERES
   és a SUMMARY is mentes; ez a stutter-ablakok azonosításának elsődleges bizonyítéka, és eddig
   7 pontból álltak a „legrosszabb ablakok" táblák ~96 helyett.

Mindhárom **mérési** változás, visszavonható: az 1. pont egyetlen kifejezés, a 2–3. pont a
mentesítési lánc feltételei (egy `body` mutató + `sizeof`-alapú előtag-illesztés).

**Verzió: v1.4.1** (a RUN sor így megkülönbözteti az E1 futást a 13:27-es v1.4.0-tól).

**A `compare-runs.mjs` mostantól kiírja a tényleges módot** (a dekóder-sorból; ha az nincs, a kért
override-ot `requested=` előtaggal), a `--list`, a munkamenet-fejléc és a záró összehasonlító tábla is.
Ellenőrzés a valódi naplón:

```
#1  2026-10-15 11:05:26  v=-       latency=ultra-low  samples=0
#2  2026-10-15 12:52:58  v=-       latency=ultra-low  samples=0
#3  2026-10-15 12:54:27  v=-       latency=-          samples=0
#4  2026-10-15 13:27:59  v=1.4.0   latency=ultra-low  samples=7
```

**Negatív, de fontos eredmény:** a naplóban **egyetlen nem-ultra-low futás sincs**. A módot rögzítő
sor csak 11:05-től létezik, és mindhárom rögzített futás `ultra-low` volt — vagyis az E1 kísérlet az
**első** olyan adatpont, ami a késleltetési módot mint változót vizsgálja. Retrospektív bizonyíték
ezért erre a hipotézisre nincs; az E1 futás dönt.

Tesztek a v1.4.1 buildhez: `node work/test-metrics.mjs` ✓, `node work/steam-tv/test.mjs` ✓,
`node work/compare-runs.mjs … --list` ✓ (a fenti kimenet). A `test-metrics.mjs` mostantól a
**napló-osztályozási szerződést** is ellenőrzi (a `body`-ra illesztett mentesítési lánc, a MERES/SUMMARY
mentessége, és hogy a régi, előtaggal hibás `strncmp(line, "MERES "…)` alak ne térhessen vissza).

**A javítás önellenőrzése a következő futásból:** a `--list` sorban a `samples=` ~96 kell legyen
193 másodpercre (a 13:27-es futásban 7 volt), és a futás végén meg kell jelennie a `SUMMARY` sornak.
Ez egyben azt jelenti, hogy az E1 A/B összehasonlítás végre ~96 ablakból dolgozik 7 helyett.

---

## 12. 5. kör: per-másodperces összekapcsolás és a hibaállítás

A 13:27-es futás adatai eddig szétválasztva voltak (1 Hz-es health sor, `Render:`, `Idozites:`, MERES),
ezért a „miért akadt" kérdésre nem lehetett belőlük válaszolni. Az új `--timeline` mód ezeket
**másodpercre összekapcsolja, és a host saját pipeline-eseményeivel együtt** írja ki.

**A döntő eredmény** (116 közös másodperc):

- `r(fed, Render fps) = 0,925` — a megjelenített képkockaszám a **beérkezést** követi, nem fordítva;
- a legrosszabb 10% másodpercben átlag `fed` **24,7/s** a 60/s medián helyett, Render fps 25,5 az 59,5 helyett;
- a 10 másodperces fagyás (13:30:02, `interval max 10047,9 ms`, `cb max 10045,10 ms`) **pontosan**
  a host `Changing adapter` + `Building new pipeline` + `Created encoder NVENC` + `Capture method set to
  Game D3D12 shared NT NV12` másodpercére esik, miközben a kliens beadása 0,2–0,3 ms;
- host-események a futásban: `stream-switch=5 adapter=5 pipeline=5 encoder-created=5 encoder=6 capture=6
  desktop-lost=2 frames-dropped=2 slow-framerate=17` (a host limiterje **game 1.00 fps**-ig),
  azaz **öt teljes kódoló-újraépítés 193 másodperc alatt**.

A teljes hibaállítás, a bizonyítékok táblázatosan és a pontos ellenőrzési lépések külön dokumentumban:
**`outputs/steam-tv-hibaallitas.md`**.

**Két új védőkorlát az elemzőben** (hogy a riport ne tudjon félrevezetni):

1. egy v1.4.0-s futás MERES rátái használhatatlanok (a kumulatív-számláló hiba), ezért ha a `fed`
   meghaladja a beállított fps háromszorosát, az elemző **kimondja**, hogy erre a futásra a
   `--timeline` (1 Hz-es sor) a mérvadó, ahelyett hogy hibás mediánból ítéletet mondana
   (a 13:27-es futás mediánja így „1946,5/s" volt);
2. a stream első másodpercei (indulás) kimaradnak a „legrosszabb másodperc" rangsorból — korábban a
   0 kockás induló másodperc lett a „legrosszabb", és a riport félrevezetően a platform-beadást okolta.

Tesztek: `node work/test-metrics.mjs` ✓, `node work/steam-tv/test.mjs` ✓, `--list` / `--session 4
--timeline` ✓ a valódi naplón. Az appot nem indítottam el; a cél az E1/E2/E3 futásokig nyitva marad.

---

## 13. 6. kör: kereszt-futás összehasonlítás (21 munkamenet, két naplófájlból)

Eddig csak az utolsó futást elemeztem. A rotált napló (`streaming_log.previous.txt`, 1,0 MB) **16 további
munkamenetet** tartalmaz a 10-14-i napokból, köztük a korábban „jó" állapotként jegyzett 16:29-est.
Az elemző mostantól ezeket is összevethetővé teszi:

- **health-only munkamenetek** (a `Render:` riport csak később jelent meg) is kapnak összegzést;
- a döntő tábla a **`medH fed/prez`** (a minden buildben létező 1 Hz-es sor mediánja), a **`bad seconds`**
  és annak a **stream hosszához viszonyított aránya** (a kliens saját `óra` mezőjéből, nem a naplósorok
  számából — különben egy 2 soros töredék 50%-os „rossz" arányt mutatott);
- **javított napló-parser hiba:** a „Streaming started" sorok órákon át összeolvadtak egy munkamenetbe
  (ha közben nem jött klienssor), ezért egy 17:23–20:52 közötti blokk host-eseményei a 20:52-es streamhez
  látszottak tartozni. Az összevonás mostantól **1 percre** korlátozott; ettől a rotált naplóban
  16 helyett **21 valódi munkamenet** lesz.

**Az eredmény (a 13:27-es futással együtt):**

| munkamenet | medH fed/prez | rossz másodperc (arány) | ebből host-eseményhez tapad |
| --- | --- | --- | --- |
| 10-14 16:29 („jó" kor) | 60/40 | 8 / 92,8 s (**9%**) | 7 |
| 10-14 16:57 | 55/38 | 22 / 55,2 s (**40%**) | 9 |
| 10-14 20:52 | 43/29 | 36 / 42,9 s (**84%**) | **23** |
| 10-14 21:20 | 58/35 | 34 / 127,3 s (27%) | 29 |
| 10-14 22:21 | 59/59 | 8 / 110,8 s (7%) | 7 |
| 10-15 10:26 | 60/59 | 20 / 190,3 s (11%) | 7 |
| 10-15 10:56 | 60/59 | 5 / 70,9 s (7%) | 5 |
| 10-15 12:54 | 60/60 | 5 / 160,8 s (3%) | 4 |
| **10-15 13:27 (elemzett)** | 60/59 | 47 / 250,4 s (**19%**) | 18 |

(A tábla a 15. szakaszban javított számítással készült.)

**Következtetések:**

1. **A tipikus másodperc minden buildben rendben van** (`medH fed` 55–60), a különbség a rossz
   másodpercek *számában* van (1% és 61% között).
2. **A legrosszabb munkamenetek host-churn-esek:** a 20:52-es (61%) és a 16:57-es (34%) futásban a rossz
   másodpercek többsége pipeline-újraépítéshez tapad (16, illetve 8). A 13:27-es futás 15%-a szintén
   16 host-eseményhez kötődik.
3. **Az ultra-low késleltetési mód önmagában nem határozza meg az eredményt:** ultra-low futások
   szórása **1%–61%** (12:54 → 1%, 20:52 → 61%). Ez gyengíti a Moonlight-wiki ULL-hipotézisét mint
   fő okot — a kontrollált E1 kísérlet így is megmarad, de a várakozás szerény.
4. **Egy mérési csapda, amit jelezni kell:** a `prez` (megjelenített) oszlop **nem hasonlítható össze**
   a 10-14 22:02 előtti és utáni buildek között. A `steam-tv-60fps-status.md` szerint az a build
   változtatta meg a mérést (a kihagyott JS callback többé nem számít elveszett kockának); előtte minden
   build ~0,6× (`40/60`), utána ~1,0× (`59/59`) értéket adott. Az eszköz ezt ki is írja a tábla alatt,
   és a `medH fed` (beérkezés) az, ami minden buildben összevethető.

---

## 14. 7. kör: miért nem történt meg eddig az E1 kísérlet — és a javítás (v1.4.2)

A felhasználó visszakérdezett, hogy **hogyan** kell az E1/E2/E3b futásokat elvégezni. Az utasítás
megírása közben derült ki a valódi ok, amiért a késleltetési A/B soha nem futott le:

**A `Késleltetés (teszt)` gomb nem volt elérhető a távirányítóval.** Az `app.js` `moveFocus()` kizárólag
a `focusables()` listáján belül mozgatja a fókuszt, a lista viszont ez volt:

```js
['pair', 'start', 'resolution', 'fps', 'bitrate', 'audio', 'codec', 'diagnostics']   // 'latency' hiányzott
```

A `latency` gomb a HTML-ben létezik és a panel alján látszik, de a fókuszgyűrűből hiányzott, ezért
D-pad/távirányító segítségével **egyszer sem lehetett megnyomni** — a mérés így fizikailag kivitelezhetetlen
volt a kanapéról. Ez egyben magyarázza a korábbi körök eredménytelen kéréseit.

**Javítás (egy szó, teljesen visszavonható):** a `focusables()` lista mostantól tartalmazza a `latency`
gombot. **Verzió: v1.4.2** (megkülönbözteti a korábbi, fókuszhibás v1.4.1-től).

**Regressziós őr:** a `work/steam-tv/test.mjs` mostantól kiolvassa az `index.html` összes
`<button id="…">` azonosítóját, és megköveteli, hogy **mindegyik benne legyen a fókuszgyűrűben**
(a `disabled`/`hidden` állapotokat a teszt a futásidejű viselkedés miatt figyelmen kívül hagyja).
Ellenőrizve mindkét irányban: a javítással zöld, a `latency` eltávolításakor pedig elbukik
(`button 'latency' exists in index.html but the remote focus ring cannot reach it`).

Emellett ebben a körben az elemző megkapta az **audió-beadási ütem** kimutatását is:

- a 13:27-es futásban **100 audio frame/s érkezik, és 48 track-írás/s** történik → a 20 ms-os csomagolás
  **működik** (nem minden frame külön írás);
- **0 olyan másodperc van, amikor frame érkezett volna, de írás nem történt** → a tévé hangbemenete nem
  kapott lyukat a kliens oldaláról;
- a legnagyobb egyedi írás **148,8 ms**, a hiányzó frame-ek (369–384) pedig a futás elejére, a
  host-churn ablakára esnek — a recsegés így a *beérkező* hangcsomagok kimaradásából ered (a kliens a
  mintórát folytonosan viszi tovább, ezért a vágás hallható kattanás), nem a kliens írási üteméből.
  A mintaillesztés (PLC) korábbi eltávolítása tudatos döntés volt; ezt az adat nem cáfolja.
  A gap-lista másodpercre bontva is megjelenik:
  `13:28:01(+38) 13:28:03(+25) 13:28:04(+43) … 13:31:42(+4)` — vagyis a hanglyukak **a 13:28:00–13:28:17
  közötti churn-ablakra** koncentrálódnak (plusz három kisebb a 13:31-es szakaszban).

---

## 15. 8. kör: a stall-számítás valódi hibája — ami a saját méréseimet is érintette

Az attribúciós logikára írt **szintetikus szerződés-teszt** (ismert válaszokkal) azonnal talált egy
hibát, ami a valódi futások minden eddigi számát érintette:

> A `stallSummary()` a stream másodperceit így szűrte: `r.clock === null || r.clock >= 5`. Azokban a
> másodpercekben viszont, amelyekben **nem volt saját 1 Hz-es health sor** (csak `Render:` riport), a
> `clock` mező **`undefined`**, nem `null` — így ezek a másodpercek **kimaradtak minden stall-számításból**.
> Pont azok, amelyek a legrosszabbak (a 13:29-es klaszterben épp ezek voltak a `fed` nélküli sorok).
> A szintetikus teszten 5 éhező másodpercből 4 látszott.

**Javítás:** `r.clock === null || r.clock === undefined || r.clock >= 5`. **A valódi számok ettől
megváltoznak** (ezért a korábbi körök számait itt pontosítom):

| futás | rossz másodperc (régi számítás) | rossz másodperc (javított) | ebből host-újraépítéshez |
| --- | --- | --- | --- |
| **10-15 13:27** | 38 / 206 (15%) | **47 / 240 (19%)** | 18 |
| 10-14 20:52 | 26 / 42,9 s (61%) | **36 / 42,9 s (84%)** | 23 |
| 10-14 21:20 | 15 / 127,3 s (12%) | **34 / 127,3 s (27%)** | 29 |
| 10-14 16:29 („jó" kor) | 7 / 92,8 s (8%) | **8 / 92,8 s (9%)** | 7 |
| 10-14 16:57 | 19 / 55,2 s (34%) | **22 / 55,2 s (40%)** | 9 |

A 13:27-es futás tehát **19%** rossz másodperc, ebből **18** tapad host pipeline-újraépítéshez; a
host-limiter jelentéséhez egy sem. A host-oldali churn továbbra is a legnagyobb *azonosítható* tényező,
de a rossz másodpercek többsége (29) továbbra sem köthető egyetlen host-eseményhez sem — ezek azok,
amiket a következő futásoknak kell magyarázniuk.

**A javított eszköz új őrei** (`work/test-metrics.mjs`, szintetikus napló ismert válaszokkal):
egy tiszta 20 másodperces szakasz **0** rossz másodpercet adhat; egy 5 éhező másodperces szakasz
host-újraépítéssel **pontosan 5**-öt; a `Render`-only másodperc **nem eshet ki**; a `hiányzó`
számláló ugrása **egy** gap-másodpercet ad, összesen 7-tel; a szomszédos minta kölcsönzése működik;
és a 10 másodpercre lévő „Streaming started" pár **egy** munkamenetbe olvad, a fél órával későbbi nem.
Emellett a rövid/töredék munkamenetekre a százalék helyett `(n/a)` jelenik meg (a `clock` nem haladt
elő, korábban „800%" volt).

---

## 16. 9. kör: a rossz másodpercek szerkezete — beérkezés vs megjelenítés

Az elemző mostantól minden rossz másodpercet besorol: **nem érkezett meg** (`fed < 45/s`) vagy
**megérkezett, de nem mutatta meg** (`fed ≥ 45/s`, mégis `Render fps < 45`). A döntő tábla új
`arr/pres` oszlopa ezt mutatja, és ez az a pár, amit az **E1** (késleltetési mód) és az **E2**
(display-újrainicializálás) érdemben mozdíthat — a beérkezési oldal a host/hálózaté.

**Amit a meglévő futásokból tudni lehet** (csak ott van megjelenítési jel, ahol van `Render:` riport,
azaz 10-15 12:52-től; a többinél az eszköz `n/a`-t ír, nem nullát):

| futás | rossz másodperc | beérkezés | megjelenítés | `r(fed, Render fps)` |
| --- | --- | --- | --- | --- |
| 10-15 12:52 (52,6 s) | 8 | 4 | 4 | 0,045 |
| 10-15 12:54 (160,8 s) | 5 | 0 | 5 | 0,010 |
| **10-15 13:27 (250,4 s)** | **47** | **43** | **4** | 0,857 |

A hosszú, elemzett futásban tehát a rossz másodpercek **91%-a beérkezési** oldali (43/47) — ott az
E1/E2 legfeljebb 4 másodpercre hathat. A két rövidebb futásban viszont kevert a kép (4/4, illetve 0/5),
és a 12:54-esben a beérkezés és a megjelenítés **korrelálatlan** (`r = 0,010`) — vagyis a megjelenítési
oldal nem mindenhol elhanyagolható. **Ez az oka, hogy az E2 kísérletet nem ejtem el**, csak a
várakozást igazítom az adathoz.

Ez egyben a **legfontosabb szerkezeti eredmény** az egész mérésből: a kliens nem azért akad, mert nem
tudja megjeleníteni, amit kap — hanem mert nem kap. A további munkát tehát a beérkezési úton kell
folytatni (host-pacing, host pipeline-churn, hálózati burst), és a kliens oldalon nincs mit javítani
addig, amíg ez az arány ilyen egyoldalú.

---

## 17. 10. kör: az első „jó" futás teljes méréssel (v1.4.2, 2026-10-15 16:01:24)

A felhasználó jelezte, hogy ez a menet jónak tűnt. A napló igazolja, és **ez az első olyan futás, ahol
a mérés minden eleme (66/52 MERES minta, `Render:`, `Idozites:`, host `SessionStats`, host-események)
együtt van** — ezért viszonyítási alapként rögzítem.

| | 13:27 (v1.4.0) | 14:18 (v1.4.2) | **16:01 (v1.4.2)** |
| --- | --- | --- | --- |
| rossz másodperc | 47 / 241 (19%) | 132 / 200 (66%) | **16 / 201 (8%)** |
| ≥ 0,5 s fagyás | **2** (10 048 ms, 512 ms) | 0 (max 284 ms) | **1** (2094 ms) |
| `fed` medián | 39,5/s | 39,5/s | **56,7/s** |
| host fps (ablakok) | 43,7–57,7 | 30–36 | **59,4 → 37,3 → 58,2** |
| fogadó út | `win=754 loss=4` | `win=36 loss=2` | **`win=21 loss=0 rst=0`** |
| hiányzó audió frame | 384 | 36 | **21** |
| beadás (SDK) | 13,3 ms átlag | 2,4 ms | **2,42 ms** |
| hang-írás | 48/s | 49/s | **50/s** |

**A futás egyetlen valódi fagyása (16:02:08, 2094 ms) a host stream-mód váltása:**
`16:02:06 >>> Switching video stream from Desktop_MovieStream to GameOverlay_MovieStream_28764`, majd
`16:02:08 adapter + pipeline + encoder-created + encoder + capture → Game D3D12 shared NT NV12`.
Ugyanez a minta adta a 13:27-es futás **10 másodperces** fagyását is (ott is Desktop → GameOverlay).
A visszaváltás 16:04:37-kor 322 ms-ot kért.

A rossz másodpercek eloszlása a host ablakaiban: **0** a 16:01:46-ig tartó 59,4 fps-es szakaszban,
**11** a 16:01:46–16:02:00 közötti 37,3 fps-es szakaszban (a játék előtérbe kerülése előtt), és **2** a
58,2 fps-es játékszakaszban. Vagyis a „jó" futás is a host fps-ét követi — most jó volt a host.

**Ebben a körben javított mérési hiányosság:** a `RUN` sor (a futás beállításai: bitráta, fps, kodek,
késleltetés-override) a 15:59-es és 16:01-es futásból **hiányzott** a naplóból (`settings: -`), mert a
`MERES: RUN v=…` sor — a `MERES` tag és a kettőspont miatt — nem illett a mentesített `"MERES "`
előtagba, így a 30 s-os „sima" osztályba esett. **Javítva (v1.4.3):** a `RUN v=` és `MERES: RUN` is
mentes; a szerződés-teszt ezt is ellenőrzi. Ez azért fontos, mert a kísérletek (E3b bitráta, E1 mód)
beállításait **a naplóból kell visszaigazolni**.

**Ugyanebben a körben, a host saját ablakszámításából (`SessionStats`):** az eszköz mostantól kiolvassa
a host öt ablakát (`AvgFrameMS`, `StdDevFrameMS`, `Slow*Percent`, `AvgNetworkMS`) és **megszámolja az
ablakba eső rossz kliens-másodperceket**. Ebből derült ki a futás legfontosabb host-oldali ténye:
a 38 rossz másodpercből **26** abba a két ablakba esik, amelyekben a host csak **43,7–46,3 fps**-t mért
a saját maga által kiküldött kockákból, miközben `SlowNetworkPercent = 0,0` és a hálózati késleltetés
7,5 ± 4,6 ms volt — vagyis ott **a host nem 60 fps-t küldött**, és a hálózat nem volt szűk
keresztmetszet. A 13:28:47-es ablak fordítva: `SlowNetwork 20,9%`, 35,9 ± **76,2** ms (a churn és az
újra-ramp szakasza). Emellett a `Slow framerate: game X.XX` mezőről kiderült, hogy **nem a játék fps-e**
(minden ablakban `SlowGamePercent = 0`), ezért az eszköz szövegezése is pontosítva lett.

**A kör második fele — két saját hibám javítása és az E3 válasz.** Az „1 Hz-es health sor vs napló
másodperc-határ" fáziscsúszás miatt néhány másodpercben csak `Render:` sor volt, és az eszköz ezt
„nem érkezett semmi"-ként mutatta: a 13:29-es klaszterben emiatt állt elő a téves „host 44 fps, kliens
27 fps" kép. A nyers napló szerint ott a vétel **31–43 kocka/s** volt, és a megjelenítés ugyanennyi
(31,6/31,1/31,4/43,5) — vagyis **a vétel és a megjelenítés együtt mozgott**, a host pedig ~44 fps-t
küldött. Az eszköz mostantól a szomszédos 1 Hz-es mintát `~` jelöléssel kölcsönzi; ezzel a korreláció
0,925 → **0,857** (sűrűbb párhalmaz), a rossz másodpercek száma változatlan (38/206).

Emellett a felhasználó megerősítette: **a tévé és a PC is vezetékes** (nem Wi-Fi). Ez a Moonlight-wiki
100 Mbps-os tévé-Ethernet burst-szakaszát teszi relevánssá, és a mért jelek is oda mutatnak
(`SlowNetwork 20,9%`, 35,9 ± 76,2 ms a 13:28:47-es ablakban, `win=754`, `aarr max 498 ms`). A konkrét,
kódváltozás nélküli lépések (host NIC 100 Mbps Full Duplex / flow control / linksebesség-ellenőrzés)
a hibaállítás 2.8 pontjában vannak.

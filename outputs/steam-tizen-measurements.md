# Steam TV — mérési jegyzet (mi hiányzik, mit mérünk, hogyan hasonlítsd össze)

Érvényes: **v1.4.0** (a verzió egyetlen helyen: `work/steam-tv/probe.c`, `TV_BUILD_VERSION`; a
képernyőn és minden mérési sorban ugyanez a szöveg jelenik meg). Ez a fájl a `media.h`-ban hivatkozott
dokumentum.

Cél: a felhasználó panasza szerint a stream **ugyanazon a tévén néha tökéletesen sima, máskor
mikro-akadozó, időnként rövid képfagyással és recsegő hanggal**. Az itteni mérések célja nem az
általános optimalizálás, hanem hogy **egy futásból meg lehessen mondani, hol keletkezik az
egyenetlenség**: a PC kódolójában, a hálózaton, a kliens fogadó útjában, a platform dekóderében vagy a
megjelenítésben.

---

## 1. Mi hiányzott a pontos diagnózishoz (és miért)

A 2026-10-14/15-i auditok (reordering/retransmission, videó-út, munkatörténet) szerint ezek a
mennyiségek **egyáltalán nem voltak mérve**, ezért a találgatás nem volt kizárható:

| Hiányzó adat | Mit dönt el | Előtte mi volt |
|---|---|---|
| **Host-oldali kockaütem** (a host saját időbélyegei közötti különbség) | a PC kódolója késik-e | csak közvetett: a host átlag 56,7 kocka/s-ot adott, és 10:58:08–10:58:13 között 7–10 FPS-t |
| **Helyi kockaérkezési ütem** (kész kocka → következő kész kocka) | a hálózat/fogadás késik-e | semmi; a napló csak a beérkezési *átlagot* tartalmazta |
| **Képkocka-összeállítási idő** (első fragmentum → teljes kocka) | egy kocka fragmentumai szétszóródva érkeznek-e | semmi (a „READY” pont nem a nyers csomagérkezés) |
| **`AppendPacket` hívások közötti idő és a zárra várás** | a kliens lassít-e, vagy a platformhívás lassú | csak egy átlag, a zárra várás nélkül |
| **Receive-oldali eldobások** (age-alapú `Discard`, ablak-overflow, partial-clear, kulcskocke-kérés) | a kliens fogadó útja veszít-e | `Discarded` Verbose szinten → a naplóban **strukturális nulla**, nem bizonyíték |
| **Audio érkezési ütem** (helyi és host-oldali) | a hang recsegése burst-érkezésből jön-e | csak a dekódolt frame-ek száma és a hibaszámlálók (mindig 0) |
| **Ténylegesen megjelenített kockák** | a platform jelenít-e meg mindent | `presentedFrames` (rVFC) — de a naplóba nem került bele, és a `dropAPI` ellentmond neki |
| **A telemetria saját vesztesége** | egy hiányzó sor „nem történt meg” vagy „elveszett” | semmi; a közös 900 ms-os kapu a stream közben a sorok **72–82%-át** elnyelte |
| **Miért kért kulcskockét a kliens** | a `ReportLost` hurkok száma ok szerint | egyetlen összesített szám sem volt |

Két mérési hiba, ami eddig **hamis képhez** vezetett:

1. **A közös naplókapu.** `probe.c` `forward_to_host()`-ja egyetlen `host_log_last_ms` időbélyeggel
   döntött a 900 ms-os mérési keretről, miközben a `Render:`/`Idozites:` sorok mentesültek a kapu alól,
   de ők is frissítették azt. Mért következmény (10-15): `Média: Videó` **14/50 (28%)** és
   **29/162 (18%)**, `Őr` 36%/32%, `Él` **0** — vagyis a fő telemetria pont streamelés közben tűnt el.
   **Javítva:** külön időbélyeg (`host_log_last_measurement_ms`), és a kihagyott sorok száma
   okonként megjelenik az `Él:` sorban (`Naplo: kihagyva meres …`).
2. **A „beadott fps” visszaszámolása** a naplóból (`sum(delta)/óra`) a hiányzó sorok miatt
   11,4 kocka/s-ot mutatott 60 helyett. Ezért mostantól a **soronkénti** értékek a mérvadók, és a
   `MERES` sorok száma is látszik a táblázatban.

---

## 2. Mit mér a mostani build

Minden sor a host `streaming_log.txt`-jébe kerül `CLIENT: TV: ` előtaggal; a `v=` mező mindig a build
verziója, tehát egy napló futásra visszakövethető.

- **`RUN`** — a stream indulásakor: verzió, felbontás, fps, bitrátakorlát, kodek, hang, késleltetés-mód.
- **`MERES`** — 2 másodpercenként, fix mezősorrendben (gépi összehasonlításra):
  | mező | jelentés |
  |---|---|
  | `t` | a stream kezdete óta eltelt idő (s) |
  | `fed` | a platformnak beadott képkockák száma/s — **nem** megjelenített fps |
  | `prez` | a böngésző `presentedFrames` számlálójának növekménye/s (compositor-szintű prezentáció) |
  | `rvfc` | `requestVideoFrameCallback` visszahívások/s |
  | `clock` / `media` | a video-elem `currentTime`-ja / az utolsó prezentált kocka `mediaTime`-ja |
  | `vpts` / `apts` | a kliens által adott videó- és hang-PTS (s) |
  | `vhost=avg/max` | **host-oldali** kockaintervallum (a host saját időbélyegeiből, hálózat nélkül) |
  | `varr=avg/max` | **helyi** kockaérkezési intervallum (kész → kész) |
  | `vasm=avg/max` | kocka-összeállítási idő (első fragmentum → teljes kocka) |
  | `vdec=avg/max` | az SDK `AppendPacketAsync` hívás hossza |
  | `vwait=avg/max` | várakozás a média-zárra (config lock) a beadás előtt |
  | `vint=avg/max` | a submit callbackek közötti idő |
  | `aud=rate/hiba/vissza/hiányzó` | audió frame/s, dekódolási hibák, visszautasítások, kihagyott frame-ek |
  | `ahost` / `aarr` | audió intervallum a host / a helyi vétel szerint (avg/max) |
  | `aapp=avg/max(hívás)` | hangbeadás hossza és száma/s |
  | `loss` `rst` `part` | kulcskocke-kérések, dekóder-resetek, eldobott fragmentumok (kumulatív) |
  | `win=discard/overflow` | a fogadóablak age-alapú eldobásai / túlcsordulásai (kumulatív) |
  | `vdrop=none/empty/big/key/pts/append` | **miért nem jutott el a kocka a platformig** (minden `VideoSubmit` visszatérési út külön számlálója) |
  | `lossr=seq/wait/decrypt/stall/mismatch/submit` | **miért kért a kliens kulcskockét** (sorrend-ugrás, várakozás-lejárat, dekódolási hiba, partial-stall, összeállítási ütközés, dekóder-visszautasítás) |
  | `ref=video/audio` | a platform által visszautasított beadások (kumulatív) |
  | `drop` | a kernel által eldobott felső irányú csomagok (kumulatív) |
  | `loop` `pad` `proxy` | főciklus-iterációk/s, gamepad-minták/s és max hézag, JS-proxy hívásokra fordított µs/s és max |
- **`SUMMARY`** — a stream végén: összesített darabszámok és maximumok ugyanabban a névtérben.
- **`Video:`/`Audio:` Warn sor a discard költségéről** (legfeljebb 1/s csatornánként):
  `Discarded N packets, no complete frame for X ms (tolerance Y ms)`. Ez a 450/200 ms-os
  age-alapú discard **egyetlen közvetlen mérése**: megmutatja, hogy egy hiányzó frame-head miatt a
  csatorna mennyi ideig nem adott ki egyetlen kész frame-et sem (60 fps-en 450 ms ≈ 27 kocka).
- **Szerződés-teszt**: `node work/test-metrics.mjs` — a `media.cpp` formátumszövegéből kiolvassa az
  emitált mezőket, összeveti a `compare-runs.mjs` által olvasott kulcsokkal, ellenőrzi a
  `vdrop`/`lossr` sorrendet, és szintetikus naplón lefuttatja az aggregációt. Ha egy mezőt átnevezünk,
  ez a teszt bukik, nem pedig csendben „-” lesz a riportban.
- **`Render:`** (app.js) — a prezentációs intervallumok p50/p95/max, `coalesced`, `dropAPI`
  (`getVideoPlaybackQuality`), `rAF` becslés, `panel=NA`.
- **`Él:`** — 2 s-onként: vezérlőcsatorna-csomagok, datagram-növekmény, nyugtázás/ismétlés/függőben,
  HID-számlálók, és a napló-kihagyások okonként.

---

## 3. Amit lehet és amit NEM lehet mérni ezen a platformon

- **Nincs valódi „renderelt fps” API.** A Samsung EMSS SDK nem ad panel-kimeneti számlálót
  (`GetBufferedAmount`, `OnFramePresented`, decoded/dropped számláló nincs). Ezért:
  - `fed` = **beadott** kockák/s (a dekóder kapta) — nem megjelenített;
  - `prez` = a böngésző compositor-ig bemutatott kockáinak száma (rVFC `presentedFrames`) — ez a
    legközelebbi elérhető dolog, de **nem a panel kimenete**;
  - `dropAPI` = a böngésző `getVideoPlaybackQuality()` értéke; mért esetben **ellentmond** a `prez`-nek
    (+2405 „drop” 162 s alatt ~60/s `prez` mellett), ezért önmagában nem bizonyíték.
  A három szám **három különböző dolgot** jelent; a dokumentumban és a naplóban is így kell kezelni.
- **A platform `currentTime`-ja és a lejátszási pozíció** ultra-low latency módban a **beadott
  PTS-ekből** számolódik, ezért nem bizonyítja, hogy egy kocka kirajzolódott.
- **A `Discarded %u packets` sor** Verbose szinten van, és a `diagnostic_filter.h` kiszűri: a „0” a
  naplóban nem bizonyíték. A tényleges szám a `MERES win=` mezőben látszik (számlálóból).
- **A receive-oldali veszteség darabszintű bontása** (melyik fragment hiányzott) ma nem mérhető: a
  data-csatornán nincs üzenetszintű napló, és a `ch_data.c` a nem-Unreliable típusokat némán eldobja.
- **A host reakciója** a `k_EStreamDataLost`-ra nem mérhető kliens oldalról; csak a host naplójából
  (kulcskocke-küldés, `Dropped N video frames`, `Slow framerate … (network)`).
- **A panel tényleges frissítési frekvenciája** (`panel=NA`): nem kérdezhető le; a `rAF` csak becslés.

---

## 4. Kontrollált A/B tesztek (a módosítások nem válnak véglegessé)

Az app UI-jában két kapcsoló szolgálja ezt; mindkettő a **következő** stream indításakor érvényesül, és
a használt érték a `RUN` sorban naplózódik:

1. **`Hang: be/ki`** — a hang teljes kikapcsolása (`stream_audio`). Ha a kép akadása hang nélkül
   megszűnik, a hang-út blokkolja a képet; ha nem, a két út független.
2. **`Késleltetés (teszt)`** — `Automatikus / Normál / Alacsony / Ultra-alacsony`. Ez a *késleltetési
   mód* A/B-je: ultra-low módban a platform nem puffereol, az alkalmazás felel a szinkronért; normál
   módban a platform ütemezi a PTS szerint. A mód a `Videó-dekóder mód:` sorban is megjelenik
   (`normal` / `low` / `ultra-low`).

Ajánlott menet egy hibás és egy jó futás rögzítéséhez (mindig **ugyanaz a tartalom** a PC-n):

| # | beállítás | mit bizonyít |
|---|---|---|
| A | 1080p60, auto bitráta, hang be, késleltetés auto | alapállapot (a panaszos eset) |
| B | ugyanaz, **hang ki** | a hang-út blokkolja-e a képet |
| C | ugyanaz, **Normál** késleltetés | a puffereletlen mód a hibás-e |
| D | ugyanaz, **Ultra-alacsony** késleltetés | visszaigazolás |
| E | 1080p**30**, hang be | ha 30-on sima, a 60 fps a szűk keresztmetszet |

Minden futásnál jegyezd fel: körülbelül mikor volt sima és mikor akadt (a `MERES` sorok `t` mezője
alapján visszakereshető), és a PC-n fusson ugyanaz a jelenet.

---

## 5. Kiolvasás és összehasonlítás

```powershell
# a legutóbbi futások listája (verzió, beállítások, mintaszám)
node work/compare-runs.mjs "C:\Program Files (x86)\Steam\logs\streaming_log.txt" --list

# egy futás részletes táblája
node work/compare-runs.mjs "C:\Program Files (x86)\Steam\logs\streaming_log.txt" --session 3

# a futás MÁSODPERCENKÉNTI képe: health + Render: + Idozites: + MERES egy sorban, a host saját
# pipeline-eseményeivel (kódoló-újraépítés, stream-váltás, adapter-csere) együtt — ez dönti el, hogy
# egy rossz másodperc a kliensé, a hálózaté vagy a host-oldali churné
node work/compare-runs.mjs "C:\Program Files (x86)\Steam\logs\streaming_log.txt" --session 3 --timeline

# két naplófájl együtt (a rotált előző is)
node work/compare-runs.mjs "C:\Program Files (x86)\Steam\logs\streaming_log.txt" "C:\Program Files (x86)\Steam\logs\streaming_log.previous.txt"

# gépi feldolgozáshoz
node work/compare-runs.mjs "C:\Program Files (x86)\Steam\logs\streaming_log.txt" --json
```

### 5.1 A futás egyetlen összehasonlítható száma: a `stalls:` sor

A `--session N` kimenetének elején (a `totals:` sor után) ez a négy sor dönti el, hogy egy futás jó
vagy rossz, és hogy **mit lehet egyáltalán javítani**:

```
stalls: 47 bad seconds of 240 (fed<45: 43, Render fps<45: 24); 34 seconds borrowed the neighbouring 1 Hz sample (~)
        host pipeline rebuild within 2 s: 18 of 36 events; host limiter report within 5 s: 0; not accounted for by the host: 29
        (of those: arrival below 45/s 27, frames arrived but were not shown 2)
        r(fed, Render fps) = 0.857; worst fed 0/s, worst Render fps 1.4; audio frames missing 384 (max 384)
        audio: median 100 frame/s in, 48 track writes/s; seconds with frames but zero writes: 0; worst single write 148.8 ms
        audio gaps (cumulative missing jumped): 13:28:01(+38) 13:28:03(+25) …
```

- **`bad seconds`** — a `fed` vagy a `Render fps` 45 alá esett másodpercek (a stream induló másodpercei
  nélkül; ha egy másodpercben nincs saját 1 Hz-es sor, a szomszédos mintát `~`-gal kölcsönzi).
- **`arrival` / `frames arrived but were not shown`** — a döntő bontás: az előbbi a host/hálózaté,
  az utóbbi az egyetlen csoport, amit a kliens/platform-oldali beállítás (E1 késleltetési mód,
  E2 display-újrainicializálás) érdemben mozdíthat. **Ha nincs `Render:` adat, a második érték `n/a`**,
  nem 0 — a hiányzó adat nem „nincs probléma".
- **`host pipeline rebuild within 2 s`** — hány rossz másodperc tapad a host kódoló/capture
  újraépítéséhez (a host-eseményeket az elemző a host saját soraiból olvassa).
- **`host limiter report within 5 s`** — a host `Slow framerate:` jelentéseihez tapadó rossz
  másodpercek (a jelentés `game` mezője **nem** a játék fps-e, lásd a `SessionStats` táblát).
- **`not accounted for by the host`** — a maradék; ez az, amit a következő futásnak kell magyaráznia.
- **`audio …`** — a hang-beadás üteme: 100 frame/s bemenethez ~50 írás/s tartozik, ha a 20 ms-os
  csomagolás működik; `zero writes` > 0 viszont valódi lyuk a tévé hangbemenetében.

A `--timeline` kimenete ugyanezt másodpercre bontva mutatja, a host `SessionStats` ablakaival együtt
(host fps = 1000/AvgFrameMS, `SlowNetwork%`, hálózati szórás, és az ablakba eső rossz kliens-másodpercek).

### 5.2 Két napló együtt, 25 munkamenet

A záró tábla minden munkamenetet egy sorban hoz (`medH fed/prez`, rossz másodpercek aránya,
`arr/pres`, host-kötött, korreláció, késleltetési mód). **Build-közi összehasonlításnál csak a `fed`
(beérkezés) használható:** a `prez` jelentése 2026-10-14 22:02-kor megváltozott (a kihagyott JS
callback többé nem számít elveszett kockának), előtte minden build ~0,6×, utána ~1,0× értéket adott.
Az elemző ezt ki is írja a tábla alatt.

Értelmezés (a záró táblázat oszlopai szerint):

| minta | következtetés |
|---|---|
| `vhost` kicsi, `varr` nagy | a kockák időben elhagyták a hostot, a **hálózat/vétel** késleltette őket |
| `vhost` nagy (≈ `varr`) | a **host/PC** adott későn kockát (kódoló vagy játék-oldali stall) |
| mindkettő kicsi, `fed` alacsony | a host eleve keveset küldött (tartalom vagy saját limiter) |
| `vasm` nagy | egy kocka fragmentumai szétszóródva érkeztek |
| `vdec` nagy, `vwait` kicsi | maga a **platformhívás** lassú |
| `vwait` nagy | a média-zár foglalt (prepare/teardown), nem a hálózat |
| `loss` / `win` nő | a kliens fogadó útja kért kulcskockét vagy dobott csomagot |
| `aarr` nagy, `ahost` kicsi | a hang burst-ösen érkezik (recsegés forrása) |
| `drop` nő | a felső irányú (Wi-Fi) küldés torlódik |

---

## 6. Amit a meglévő naplók már bizonyítanak

- **A csomag-átrendeződés javítása tart** (`window.c`): egy ablakon belüli, későn érkező csomag
  mindig élő slotba kerül, nem veszhet el; duplikátum nem íródik be kétszer. Az egyetlen
  visszafordíthatatlan döntés az age-alapú `Discard` (videó 450 ms, audió 200 ms), és az nem okoz
  veszteséget, csak holtpontot old. 27 session alatt: `items overflow` 0, `partial frames was cleared`
  0, „Partial frames stalled” 0.
- **A 12:57:02–12:57:06 közötti akadás host-oldali volt:** a host naplója `Switching video stream from
  GameOverlay_MovieStream to Desktop_MovieStream`, NVENC-újraépítés, `Dropped 1 video frames`, 60→30
  target framerate, `desktop access lost`; a kliens közben nem veszített csomagot, nem kért kulcskockét,
  a zárra várás 0,24 ms volt.
- **A 10:58:09–13 közötti „7–10 FPS” ablakban a kliens beadása 0,2–0,4 ms volt** (nem a kliens lassított),
  és a `fed`/`prez` együtt esett (Pearson r = 0,9218): a megjelenítés követi a beérkezést.
- **A hang útja mérten egészséges:** 99,97 frame/s átlag, 0 dekóderhiba, 0 visszautasítás; a
  hang-append maximumok nem esnek egybe a videó `fed` beeséseivel → a „hang blokkolja a képet” feltevés
  ezekkel az adatokkal **nem igazolt**.
- **A `SubmitFrame ReportLost` → kulcskocke-kérés út korlátlan:** a 10:08:29-es sessionben 25 s alatt
  455 `Coming keyframe` és 1293 beadási kísérlet **0 tényleges appenddel** — ez önfenntartó hurok volt.
  (Ez a build még a javítás előtti; a mostani build `loss=` számlálója méri.)
- **A 450 ms-os videó-discard egyetlen hiányzó csomagért akár ~450 ms-ra megállítja a teljes videóláncot**
  (a worker addig nem kap kész frame-et), ami 60 fps-en ~27 kocka — ez a mikro-akadás egyik konkrét,
  kód szintű mechanizmusa. Eddig azért nem látszott, mert a `Discarded` sor Verbose szinten van.

---

## 7. Következő lépések (mérés után, nem előtte)

1. Egy hibás és egy jó futás rögzítése a 4. pont protokollja szerint (A–E), majd `compare-runs.mjs`.
2. Ha `vhost` nagy: a PC-oldali kódolást kell vizsgálni (NVENC-újraépítés, játék-oldali stall,
   `Slow framerate … (network)` a host naplójában).
3. Ha `varr`/`vasm` nagy, `vhost` kicsi: a hálózat/Wi-Fi és a fogadó ablak a cél (a 450 ms-os discard
   hatásának mérése, esetleg csökkentése — de csak a mérés után).
4. Ha `loss` nő: a kulcskocke-kérő utak sebességkorlátja (a `ReportLost` hurkok) a cél.
5. A `Média: Videó` 1 Hz-es sorba továbbra is érdemes bekötni a `IHS_SessionReportVideoFrameStage`
   hívásokat, mert a host `decode`/`display` oszlopa ma mindig 0,00 — ezt egyelőre **nem** változtattuk
   meg, mert a kód viselkedését is érinti.

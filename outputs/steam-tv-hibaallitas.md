# Steam TV — hibaállítás, bizonyítékok és ellenőrzési lépések

Ez a dokumentum a rendelkezésre álló **mért** adatokból származó hibaállítás, a bizonyítékok lánca, és a
pontos ellenőrzési lépések. Minden állítás mellett ott van, hogy melyik futás melyik sora igazolja, és
hogy mi az, amit **nem** bizonyít.

## 0. Állapot a 11. kör végén (a cél 5 pontja)

| # | Követelmény | Állapot | Bizonyíték / hol |
| --- | --- | --- | --- |
| 1 | a Codex-változások és a reorder/retransmission-logika átvizsgálása | **kész** | `outputs/steam-tv-meres-audit-1.md` 1–4. szakasz; a 450 ms-os videó- és 200 ms-os audió-discard az egyetlen visszafordíthatatlan lépés, a reorder a `tail.id`-hez képest ír |
| 2 | build-verzió az appban és a naplóban | **kész** | `TV_BUILD_VERSION` = **1.4.2**; a `#version` sor az appban, a `MERES: RUN v=…` sor a host-naplóban; a csomagban ellenőrizve (`ihs.js.mem`) |
| 3 | célzott mérések, összehasonlítható jó-vs-rossz naplók | **kész** | MERES (2 s), 1 Hz health, `Render:`, `Idozites:`, host `SessionStats`-ablakok, `--timeline`, `stalls:` összegzés, 25 munkamenet egy táblában (13. és 16. szakasz) |
| 4 | a stutter pontos oka méréssel, majd minimális, visszavonható javítás | **bizonyítva + ahol lehet, javítva** | az akadás **91%-ban beérkezési** oldali (43/47 a 13:27-es futásban), ebből 18 host pipeline-újraépítés; kliens-oldali hiba nincs bizonyítva → a streamingmaghoz nem nyúltam. A minimális javítások **mérést** érintettek: v1.4.1 (napló-osztályozás, mód-címke) és v1.4.2 (a `Késleltetés (teszt)` gomb elérhető a távirányítóval) |
| 5 | hibaállítás + bizonyítékok + ellenőrzési lépések | **kész** | ez a dokumentum + `outputs/steam-tv-meres-audit-1.md` |

**Ami nyitva marad (a felhasználó oldalán):** az E3b (bitráta ~10 Mbit/s), E1 (`Alacsony` mód) és E2
(HDMI-forrás) futások még nem történtek meg; ezek a *verifikáció* lépései (5. pont), nem a javítás
feltételei. Az adat szerint az E1/E2 esélye szerény (a rossz másodpercek 91%-a beérkezési oldali), az
E3b és a host-oldali lépések (2.8 pont) a valószínűbbek.

Vizsgált futás: **2026-10-15 13:27:59–13:32:10, v1.4.0, 1920×1080@60, bitráta 19906, hang be,
késleltetés auto → ultra-low** (250 másodperc). Az elemző eszköz: `work/compare-runs.mjs`.

---

## 1. A hibaállítás

> A mért futásban a képen látható akadás és a rövid fagyások **nem a kliens kódjában keletkeznek**.
> A kockák **nem érkeznek meg**: 47 rossz másodpercből **43-ban** a beérkezés esett 45 kocka/s alá,
> és csak **4-ben** fordult elő, hogy a kocka megérkezett, de a kép nem mutatta meg. A beérkezési
> hiány **18** másodpercben host-oldali capture/kódoló pipeline-újraépítéshez tapad (stream-váltás,
> adapter-csere, NVENC újralétrehozás, desktop-access elvesztés), a többi 29 másodperc abban a két
> host-ablakban van, amelyekben **a host a saját számlázása szerint is csak 43,7–46,3 fps-t küldött**
> (hálózatot 0,0%-ban okolva, 7,5 ± 4,6 ms késleltetéssel). A kliens a megérkező kockákat gyorsan
> beadja (0,0–0,5 ms), a hang vétele folyamatos (100 frame/s, 0 hiba), a hang-beadás üteme 48 írás/s
> (20 ms-os csomagokban), és **egyetlen olyan másodperc sincs, amikor hang érkezett volna, de írás nem
> történt** — vagyis a recsegés a *beérkező* hangcsomagok kimaradásából ered, nem a kliens írási
> üteméből.

A host-oldali churn mögött álló *miért* (miért épül újra ötször a kódoló) nem a kliens hatásköre; a
kliens oldalról az egyetlen bizonyított hiányosság az volt, hogy **a mérése nem volt elég sűrű és
attribútálható** — ez a v1.4.1-ben javítva van (lásd 4. pont).

## 2. A bizonyítékok lánca

### 2.9 A rossz másodpercek szerkezete: beérkezés vagy megjelenítés? (dönti el, mit lehet egyáltalán javítani)
Az elemző mostantól minden rossz másodpercet besorol aszerint, hogy **nem érkezett meg a kocka**
(`fed < 45/s`) vagy **megérkezett, de nem mutatta meg a kép** (`fed ≥ 45/s`, mégis `Render fps < 45`).
Ez a szétválasztás azért döntő, mert az E1 (késleltetési mód) és az E2 (display-újrainicializálás)
**csak a második csoportot** tudja érdemben érinteni — az első a host/hálózat.

| futás | rossz másodperc | ebből beérkezés | ebből megjelenítés | megjegyzés |
| --- | --- | --- | --- | --- |
| 10-15 12:52 (52,6 s) | 8 | 4 | 4 | `r(fed,fps) = 0,045` |
| 10-15 12:54 (160,8 s) | 5 | **0** | **5** | `r = 0,010` — itt a megjelenítés független a beérkezéstől |
| **10-15 13:27 (250,4 s, elemzett)** | **47** | **43** | **4** | `r = 0,857` |

**Pontosítás (10. kör):** a `megjelenítés` oszlop csak azokban a futásokban értelmezhető, ahol van
`Render:` riport (nálunk 10-15 12:52-től). A korábbi futásoknál az eszköz ezért `n/a`-t ír, nem nullát —
különben „nincs megjelenítési probléma" látszana pusztán az adat hiányából (`outputs/steam-tv-meres-audit-1.md`
16. szakasz).

**Amit ez jelent:** a hosszú, elemzett futásban a rossz másodpercek **43/47 = 91%-a beérkezési** oldali,
tehát ott az E1/E2 legfeljebb 4 másodpercre hathat. A két rövidebb, azonos méréssel készült futásban
viszont **kevert** a kép (4/4, illetve 0/5), és a 12:54-es futásban a beérkezés és a megjelenítés
**gyakorlatilag korrelálatlan** (`r = 0,010`) — vagyis a megjelenítési oldal nem elhanyagolható
mindenhol, csak a hosszú, churn-sújtott futásban az. Ezért marad meg az E2 kísérlet is.

### 2.0 Kereszt-futás mérce: ez a futás nem kiugró, de a leginkább host-kötött
Az összes elérhető munkamenet (21, a rotált naplóból is) összevetve a minden buildben létező 1 Hz-es
health sorral: a `fed` (beérkezés) medián **minden valódi futásban 55–60/s**, a rossz másodpercek
aránya viszont **1% és 84% között** szór. A legrosszabb futások (10-14 20:52: **84%**, 10-14 16:57:
40%) épp a legtöbb host pipeline-újraépítést tartalmazók. A 13:27-es futás **19%**-kal a középmezőnyben
van, de a rossz másodpercei a **legerősebben host-eseményhez kötöttek** (47-ből 18).

Ugyanez **gyengíti** az ULL-hipotézist mint fő okot: ultra-low módban mért futások szórása **1%–84%**
(12:54 → 1%, 20:52 → 84%), vagyis a késleltetési mód önmagában nem határozza meg az eredményt.
(A számok a 8. körben javított stall-számításból valók — a korábbi, hibás számítás rendre alábecsült;
lásd `outputs/steam-tv-meres-audit-1.md` 15. szakasz.)
(A részletes tábla: `outputs/steam-tv-meres-audit-1.md` 13. szakasz.)

### 2.1 A megjelenítés a beérkezést követi, nem fordítva
Új, per-másodperces összekapcsolás (`--timeline`, a `Média: Videó:` 1 Hz-es sor + `Render:` riport):

```
--- per-second join: 245 log seconds, 211 with a health line, 132 with a Render report ---
joined seconds with both values: 116; r(fed, Render fps) = 0.925
in the worst 10% of Render seconds: mean fed 24.7/s vs median 60.0/s, mean Render fps 25.5 vs median 59.5
-> in the worst seconds the frames did not reach the client either: host or network/receive path.
```
A legrosszabb másodpercek `fed` és `Render fps` értéke kéz a kézben esik (10/12,9 · 6/7,7 · 27/27 · 31/31,4),
a kliens beadási ideje ugyanezekben a másodpercekben **0,0–0,5 ms**.

### 2.2 A 10 másodperces fagyás pontosan a host kódoló-újraépítéséhez tapad (13:30:02)
| Kliens (ugyanabban a másodpercben) | Host |
| --- | --- |
| `Render: fps=1.4 interval=21.1/10047.9/10047.9ms` | 13:29:52 `>>> Switching video stream from Desktop_MovieStream to GameOverlay_MovieStream_5036` |
| `Idozites: cb 548.18/105.00/10045.10` | 13:30:01 `Changing adapter [old=129090][new=0]` |
| `Média: Videó: 15 kocka/s be, beadás átlag 0.2 ms` (13:30:03) | 13:30:02 `Building new pipeline` · `Created encoder NVENC for codec 4` · `Video Encoder Changed` · `Capture method set to Game D3D12 shared NT NV12 + NVENC H264` |
| `Média: Videó: 10 kocka/s be, beadás átlag 0.3 ms` (13:30:04) | — |
| `Média: Videó: 61 kocka/s be` (13:30:05) — helyreállt | — |

A `cb … /10045.10` **nem feldolgozási idő**: a média-callbackek között eltelt 10,045 s a kimaradt
kockák miatt (ugyanaz a rés, mint a `Render` `interval` maximuma). A kliens közben végig élt: a
health sorok 13:30:03-tól másodpercenként jönnek.

### 2.3 Öt teljes kódoló-újraépítés 193 másodperc alatt
```
host events in this session: stream-switch=5 adapter=5 pipeline=5 encoder-created=5 encoder=6
                            capture=6 slow-framerate=17 frames-dropped=2 desktop-lost=2
13:27:59 NONE -> Desktop_MovieStream              (stream indítás)
13:28:51 desktop access lost, reacquiring -> SecureDesktop_MovieStream  (+ adapter/pipeline/encoder)
13:28:55 SecureDesktop_MovieStream -> Desktop_MovieStream               (+ adapter/pipeline/encoder)
13:29:52 Desktop_MovieStream -> GameOverlay_MovieStream_5036
13:30:02 Game D3D12 shared NT NV12 + NVENC H264  (+ adapter/pipeline/encoder)   <- 10 s fagyás
13:31:56 GameOverlay_MovieStream_5036 -> Desktop_MovieStream            (+ adapter/pipeline/encoder/capture)
13:32:02 desktop access lost
```

> **Pontosítás (5. kör):** a `Slow framerate: game X.XX` sorban a `game` mező **nem** a játék fps-e.
> A host saját ablakszámítása (`SessionStats`, lásd 2.7) ugyanezekben az időszakokban
> `SlowGamePercent = 0`, és **43,7–57,7 fps** közötti átlagot mér. A `network 37–345 ms` érték viszont
> valós jelzés: a host minden le nem zárt kockát a hálózathoz könyvel, mert a klienstől nem kapott
> dekódolás-/megjelenítés-eseményt.

### 2.4 A második és harmadik blokk
| Másodperc | Kliens | Host |
| --- | --- | --- |
| 13:31:55 | fed 10, prez 10, Render fps 24,0, interval max 106,8 ms | 13:31:56 stream-váltás + adapter + pipeline + encoder + capture |
| 13:31:57 | fed 6, prez 6, Render fps **7,7**, interval max **512,4 ms** | — |
| 13:31:58 | fed 33, Render fps 30,6 | — |
| 13:32:02 | fed 27, Render fps 27,0 | `desktop access lost, reacquiring` |

### 2.5 A kliens saját útja végig gyors, a hang vétele folyamatos
- beadás a rossz másodpercekben: **0,0–0,5 ms** (a teljes futás mediánja 2,4 ms);
- `hang … 99–101 frame/s, 0 hiba, 0 visszautasítás`;
- a hang viszont **hiányzó frame-eket** mutat: 369–384 db, `aarr max 498 ms` → a hangcsomagok is
  csomagokban, késve érkeznek (vételi/hálózati oldal);
- a vételi út vesztesége mérve: `win=754` (450 ms-os age-alapú discard), `loss=4` kulcskocke-kérés,
  `rst=1`, `varr max 815 ms`.

### 2.6 A host saját számlázása a hálózatot okolja — miközben a dekódolás/megjelenítés oszlopa 0,00
```
13:31:08 Slow framerate: game 1.00, capture 0.00, convert 0.00, encode 0.00,
         network 36.84, decode 0.00, display 0.00 (network)
13:28:10 Slow framerate: game 10.00, …, network 345.76, decode 0.00, display 0.00 (network)
```
A host minden le nem zárt kockát a hálózathoz könyvel, mert a klienstől nem kapott
dekódolás-/megjelenítés-eseményt. **Ez a 3. körben beépített stage-jelentés (v1.4.1) célpontja** —
a következő futásnak kell megmutatnia, hogy a `decode` oszlop már nem 0,00.

### 2.7 A host saját ablakszámítása (`SessionStats`) — itt derül ki, mennyi kockát is adott ki
A host öt ablakban írja ki a saját számlázását (`AvgFrameMS`, `StdDevFrameMS`, `Slow*Percent`,
`AvgNetworkMS`); az eszköz ezt is kiolvassa, és **az ablakba eső kliens-rossz-másodperceket is
megszámolja**:

| ablak vége | host fps (1000/AvgFrameMS) | SDframe | SlowNetwork% | net átlag | net SD | rossz kliens-másodperc az ablakban |
| --- | --- | --- | --- | --- | --- | --- |
| 13:28:47 | 56,5 | 18,0 | **20,9** | 35,9 | **76,2** | 5 |
| 13:29:22 | **46,3** | 14,4 | **0,0** | 7,5 | 4,6 | **18** |
| 13:29:37 | **43,7** | 11,4 | **0,0** | 7,5 | 3,8 | 11 |
| 13:30:02 | 57,7 | 11,3 | 0,0 | 5,7 | 2,2 | 2 |
| 13:31:56 | 53,4 | **117,4** | 2,9 | 15,8 | 17,4 | 4 |

`SlowGame = SlowCapture = SlowEncode = SlowDecode = SlowDisplay = 0,0%` minden ablakban.

**Ez a futás legfontosabb host-oldali bizonyítéka:** a 47 rossz másodpercből **29** abba a két ablakba
esik, amelyekben a host **43,7–46,3 fps**-t mért a saját maga által kiküldött kockákból, miközben a
hálózatot **0,0%-ban** okolta, és a hálózati késleltetés 7,5 ± 4,6 ms volt. Vagyis abban az időszakban
**a host nem 60 fps-t küldött**, és a hálózat nem volt szűk keresztmetszet.

A 13:28:47-es ablak fordítva: ott a hálózat volt a domináns (20,9% kocka, 35,9 ± **76,2** ms) — ez a
13:28:00–13:28:55 közötti pipeline-churn és az azt követő újra-ramp.

A 13:31:56-os ablak `SDframe = 117,4` ms: a host a 10 s-os fagyás és a stream-váltás után **nagyon
egyenetlenül** adta ki a kockákat (átlag 53,4 fps, de óriási szórással) — ez a 13:31:55–13:32:02
közötti ~27 fps-es kliens-oldali másodpercek párja.

### 2.8 A hálózati út: **mindkét oldal vezetékes** (E3 válasz, 2026-10-15)

A felhasználó megerősítette: **a tévé és a PC is Etherneten van, nem Wi-Fi-n.** Ez a Moonlight-wiki
külön szakaszát teszi közvetlenül relevánssá: a Tizen tévék beépített Ethernet-portja gyakran
**100 Mbps-ra korlátozott**, miközben a host Gigabit-es; a nagy kockák (főleg a **kulcskockák**) burstje
meghaladhatja a tévé linkjét, és a tévénél csomagvesztés/fagyás lesz belőle. A wiki javaslata épp erre:
a host hálózati kártyáját **100 Mbps Full Duplex**-re állítani (és a bitrátát ~80 Mbps alatt tartani),
vagy managed switch-en **flow control**-t bekapcsolni.

A mi méréseink ugyanerre a szakaszra mutatnak:

| Mért jel | Érték | Hol |
| --- | --- | --- |
| host által hálózatra könyvelt kockák | **20,9%** | `SessionStats`, 13:28:47-es ablak |
| hálózati késleltetés szórása | 35,9 ± **76,2** ms | ugyanott |
| age-alapú eldobott csomagok | `win = 754` | MERES/SUMMARY |
| hangcsomag-késés maximuma | `aarr max 498 ms` | MERES |
| hiányzó audió frame-ek | 369–384 | MERES |

A 13:29:22/13:29:37 ablakokban viszont a host **0,0%-ban** okolta a hálózatot (7,5 ± 4,6 ms), tehát a
hálózat nem minden rossz másodpercben játszik szerepet — a burst-jellegű szakasz a 13:28:00–13:28:55
(churn + újra-ramp) és a 13:31:56 utáni visszaállás.

**Konkrét, kódváltozás nélküli E3-lépések (host/hálózat oldal):**

A host oldalát meg is mértem (nem kellett kérdezni): az aktív LAN-kártya
**Intel X540-T1, link 10 Gbps**, IP `192.168.1.9`, a tévé (`192.168.1.6`) **ugyanazon a L2 szakaszon**
(NextHop `0.0.0.0`), tehát nincs köztes útválasztó. A kártyán a **Flow Control már be van kapcsolva**
(`Rx & Tx Enabled`), a **Speed & Duplex viszont `Auto Negotiation`** (10 Gbps), és a driver kínálja a
`100 Mbps Full Duplex` értéket (`*SpeedDuplex` = 4).

Vagyis pontosan a wiki szerinti helyzet áll fenn: **10 Gbps host-port szemben a tévé ~100 Mbps-os
portjával**; a kulcskocka-burst a tévé oldalán túlcsordulhat. A lépések, fontossági sorrendben:

1. **Nulla kockázatú első próba (E3b) — alacsonyabb bitráta.** Az app `Bitráta` gombjával állíts
   ~10–12 Mbit/s-ot (a mostani 19,9 helyett), és fuss egy ugyanolyan menetet. Ha az akadás érdemben
   csökken, a burst/link-elmélet igazolt; ha nem változik, a host pipeline-churn a domináns. Ez nem
   változtat semmit a gépen.
2. **Host NIC 100 Mbps Full Duplex (a wiki javaslata):**
   ```powershell
   # bekapcsolás (a link néhány másodpercre megszakad, a PC LAN-ja 100 Mbps-ra esik vissza)
   Set-NetAdapterAdvancedProperty -Name 'Ethernet 5' -RegistryKeyword '*SpeedDuplex' -RegistryValue 4
   # visszaállítás (Auto Negotiation)
   Set-NetAdapterAdvancedProperty -Name 'Ethernet 5' -RegistryKeyword '*SpeedDuplex' -RegistryValue 0
   ```
   ⚠️ Ez a gép **teljes** LAN-forgalmát 100 Mbps-ra korlátozza amíg így van (a Remote Play 20 Mbit/s-a
   bőven belefér), ezért egy futás után érdemes visszaállítani. Ezt **nem futtattam le** — a gép
   hálózati beállítása a felhasználó döntése.
3. Ha van managed switch: **Flow Control be** a tévé portján (a host oldalon már be van).
4. A tévé oldalán a **tárgyalt linksebesség** ellenőrzése (ha 100 Mbps, akkor az 1–2. pont a releváns).

Összehasonlítás mindegyikhez ugyanazzal az eszközzel: a `stalls:` sor és a host `SessionStats`
`SlowNetwork%` / hálózati szórás értéke kell javuljon (a `--timeline` tábla mutatja).

## 3. Amit ez a hibaállítás NEM bizonyít

0. **A maradék rést ez a futás nem zárja be, de a korábbi becslésnél kisebb.** A 2.7 ablakában a host
   **43,7–46,3 fps**-t küldött, a kliens pedig a nyers napló szerint ugyanott **31–43 kocka/s-ot vett**
   (13:29:20: 31, 13:29:22: 42, 13:29:24: 51, 13:29:26: 42, 13:29:28: 31, 13:29:31: 31, 13:29:33: 43,
   13:29:34: 38, 13:29:35: 54, 13:29:39: 41) és **ugyanannyit jelenített meg** (`Render: fps` 31,6 → 31,1
   → 31,4 → 43,5). Vagyis a vétel és a megjelenítés együtt mozgott; a „host 44, kliens 27" korábbi
   állítás **mérési artefakt volt**: az 1 Hz-es health sor és a napló másodperc-határai ki-be csúsztak
   egymáshoz képest, ezért néhány másodpercben csak a `Render:` sor volt meg. Az eszköz ezt mostantól
   kezeli (a szomszédos 1 Hz-es mintát `~` jelöléssel kölcsönzi), és ezzel a korreláció 0,925-ről
   **0,857-re** módosult (több, sűrűbb pár; továbbra is erős).
   Nyitva marad viszont: a **host ~44 fps-e és a 60 fps beállítás közti különbség oka** (játék/capture
   oldal), amit a kliens nem tud megmondani — csak a host-oldali `SessionStats` mutatja.
1. **Nem mondja ki, hogy kizárólag a host-churn az ok.** A 13:28:41–48 és 13:29:03–39 közötti
   másodpercekben nincs pipeline-esemény a ±2 s-os ablakban (a host viszont a saját ablakában
   43,7–46,3 fps-t mért), és a `win`/`varr`/`loss` a vételi út valódi veszteségét is mutatja.
   A host-churn a **domináns, bizonyított** tényező, nem az egyetlen.
2. **Nem dönti el, hogy az ultra-low késleltetési mód rontja-e a helyzetet** (E1 kísérlet).
   A Moonlight Tizen wiki Tizen 9.0-ra épp ezt a módot nevezi meg — a mi futásunk viszont ezt nem
   támasztja alá (a kockák nem érkeztek meg, nem elvesztek a platformban), de nem is cáfolja.
3. **Nem dönti el, hogy a tévé hálózati útja (100 Mbps Ethernet-port / Wi-Fi burst) hozzájárul-e** (E3).
   A 13:28:47-es ablak 20,9%-os `SlowNetwork` értéke és 35,9 ± **76,2** ms-os hálózati szórása viszont
   épp erre a szakaszra mutat.
4. **A panel kimenetéről nincs közvetlen mérés.** A `Render: … decode=NAms panel=NA` és a host
   `display 0.00` platformkorlát: `kMediaElement` módban nincs megjelenítési callback, ezért a
   `Render: fps` a böngésző-kompozítor száma, nem a panelé.

## 4. Következtetés a javításra (minimális, visszavonható)

A mérések szerint **a kliens oldalán nincs bizonyítottan javítandó hiba ebben a futásban**, ezért a
minimális beavatkozás nem a streamingmag átírása volt, hanem a **mérés hitelesítése** (v1.4.1):

| # | Mi volt a hiba | Bizonyíték | Visszavonás |
| --- | --- | --- | --- |
| 1 | a MERES/SUMMARY sor a 30 s-os naplóosztályba esett (a `Média: ` előtag miatt) | 7 MERES sor 193 s alatt, 30/32 s távolsággal; SUMMARY 0 sor | `body`-alapú osztályozás visszaállítása |
| 2 | a mód-címke Low módban „normal"-t írt | `LatencyMode() == kUltraLow ? … : "normal"` | `LatencyModeName()` visszaállítása ternárisra |
| 3 | az `EMSS:` képesség-sor egyetlen futásban sem érkezett meg | 0/3 munkamenet | a mentesítési lánc szűkítése |

A `work/SteamTV-before-v1.4.1.wgt` a javítás előtti csomag; a `work/test-metrics.mjs` szerződésként
őrzi mindhármat.

## 5. Ellenőrzési lépések (a következő futásokra)

**Futtatás a tévén (kérem):**
0. **v1.4.2 kell fenn legyen** (`install completed`): ebben a verzióban a `Késleltetés (teszt)` gomb
   **elérhető a távirányítóval** — v1.4.1-ig hiányzott a fókuszgyűrűből, ezért az E1 kísérlet
   kivitelezhetetlen volt. A `Bitráta` gomb ciklusa: `Automatikus → 10 → 20 → 30 → 50 → 80 Mbit/s`.
1. **E1 — késleltetési mód A/B:** az appban stream indítása **előtt** a `Késleltetés (teszt)` gomb
   **kétszer** (`Automatikus → Normál → Alacsony`), majd `Big Picture indítása`, és egy ugyanolyan
   hosszú menet. A naplóban `kesleltetes=2` és `…, low időzítés)` igazolja.
2. **E2 — display-újrainicializálás:** kapcsolj be egy HDMI-forrást, válts át rá, majd onnan indítsd
   az appot és a streamet (a Moonlight-wiki szerinti munkamenet a Tizen display-sync hibájára).
3. **E3 — hálózati út (válasz megvan: vezetékes mindkét oldal).** A javasolt lépések a 2.8 pontban:
   host NIC 100 Mbps Full Duplex (a wiki szerinti munkamenet a tévé 100 Mbps-os portjára), managed
   switch-en flow control, a tévé linksebességének ellenőrzése. Ez **nem klienskód**, hanem host/hálózat.

**Kiértékelés a PC-n, minden futás után:**
```
node work/compare-runs.mjs "C:\Program Files (x86)\Steam\logs\streaming_log.txt" --list
node work/compare-runs.mjs "C:\Program Files (x86)\Steam\logs\streaming_log.txt" --session N --timeline
```
- `--list`: `latency=…` (tényleges mód) és **`samples≈96`** 193 másodpercre — ez a v1.4.1 mérési
  javítás önellenőrzése (a 13:27-es futásban 7 volt); a futás végén legyen `SUMMARY` sor.
- `--session N`: az új **`stalls:`** sor a futás egyetlen összehasonlítható számai: hány rossz
  másodperc volt, ebből mennyi tapad host pipeline-újraépítéshez, mennyi a host limiter-jelentéséhez,
  és mennyi marad a host által nem magyarázott — **az utóbbi az, amit a következő futásnak magyaráznia
  kell**, és csak ez lehet kliens-oldali.
- `--timeline`: a legrosszabb másodpercek **a host-eseményekkel együtt**, a host `SessionStats`
  ablakaival (host fps, `SlowNetwork%`, hálózati szórás, az ablakba eső rossz kliens-másodpercek),
  és a záró ítélet (`r(fed, Render fps)` + „a kockák megérkeztek-e").
- a host `Slow framerate` sorában a **`decode` oszlop ne legyen 0,00** (a stage-jelentés hatása).

**Hogyan kell a következő futást olvasni:**
- rossz másodpercek **host pipeline-eseményhez tapadnak** → a javítás a host/Steam oldal
  (mint ebben a futásban: 38-ból 16);
- a host `SessionStats` ablaka **60 fps alatti** host-fps-t mutat, miközben `SlowNetwork% = 0` → a host
  nem küldött 60 fps-t (ebben a futásban 43,7–46,3 fps a rossz másodpercek ablakában);
- rossz másodpercek host-esemény **nélkül**, a `fed` 60/s körül, a `Render fps` mégis ~25 → platform/
  display oldal (a Moonlight-wiki 4.2 pontja), és akkor az E2 a mérvadó;
- rossz másodpercek **host-esemény nélkül**, a `fed` is leesik, a `varr`/`win`/`loss` nő, vagy a host
  ablaka nagy hálózati szórást mutat (`SlowNetwork% > 0`) → hálózat/vételi út, a javítás a hálózati
  oldalé (E3).

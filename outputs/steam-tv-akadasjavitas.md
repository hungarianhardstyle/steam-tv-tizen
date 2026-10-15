# Teljes stream akadásának javítása – 2026. október 14.

A felhasználó a videó-időbélyeg javítása után hang-, kép- és kontrollerakadást jelentett. A 21:23–21:25-ös naplóban a videó és hang idővonala körülbelül 3,4 másodperccel eltért. A videó már a küldő óráját használta, a hang viszont csak a dekódolt minták idejét összegezte, ezért a kiesett hangcsomagok ideje végleg eltűnt. Az előző javítás nem volt teljes.

Mindkét sáv most ugyanahhoz az egyszer felvett Steam időbélyeghez normalizál. A hang callback előtt az adott csomag időbélyege elérhető a médiakódnak is. A veszteség időbeli rést hagy; nincs pótló PCM-csomagáradat. A negatív vagy a sáv utolsó időpontjához képest ismétlődő/hátrafelé menő csomag kimarad. Az ultra-low mód megmarad, a közös zár és a korábbi SDK/CAS-javítás változatlanul védi a médiát.

A Steamnek jelentett hálózati mérésben külön bizonyított túlcsordulás volt: egy enyhén negatív fogadási–küldési különbség unsigned kivonással óráknak látszott, ami a napló több millió milliszekundumos hálózati értékeit adta. Az ilyen mérés kimarad, a valódi 32 bites óra-körbefordulás továbbra is érvényes. Az új natív teszt ellenőrzi mindkét esetet és a helyes pozitív mérést.

A kontroller küldési sorának torlódása a jelenlegi naplóval nem bizonyított. A requestVideoFrameCallback hívások száma sem bizonyítja a tényleges kijelzett FPS-t, így a korábbi 39/60-as értéket nem tekintjük biztos képkockavesztési mérésnek. A hálózati kiesés és a platformhívások egyenetlensége külön akadásforrás maradhat.

A duplikált/régi hangcsomag ellenőrzése a dekódolás előtt történik, így az eldobott csomag nem módosítja az állapottartó Opus dekódert. A közös origót az első érvényes csomag állítja be. Az időszámítás jelenlegi korlátja körülbelül 9 órás egybefüggő stream; ennél hosszabb futásra origó-áthelyezés kell.

A végső teljes Tizen/WASM build sikeres. A natív WASM tesztek (közös időalap, csomagvesztés időrése, óra-körbefordulás, hálózati negatív minta) és a JS tesztek sikeresek. A tévé `install completed` választ adott; az appot nem indítottam el. A friss aláírt csomag az `outputs/SteamTV.wgt`. Az akadás megszűnését és a kontroller tényleges késleltetését a javított csomaggal még nem ellenőriztük.

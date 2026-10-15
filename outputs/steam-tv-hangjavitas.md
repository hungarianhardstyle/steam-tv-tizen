# Hangakadás javítása – 2026. október 14.

A felhasználó megerősítette, hogy a 20:14-es SDK/CAS-javítás után van kép. A 20:52–20:53-as naplóban a hangdekódolási és beadási hibaszámlálók nullák, miközben a videó PTS-e és a lejátszó órája jelentősen lemarad a hang mögött: például 38,1 s videó / 46,0 s hang. A videó minden beérkezett kockára csak 1/60 másodpercet lépett, pedig a Steam változó számú kockát küldött. Ez biztos idővonalhiba; az akadozó hanggal való kapcsolat indokolt, de a javítás utáni hardveres eredmény még nem ismert.

A videó most a PC kocka-időbélyegeinek különbségeivel halad (1/65536 s egység). Az első kocka egyszer a hang aktuális pozíciójára igazodik. A 32 bites körbefordulás működik; ismétlődő vagy hátrafelé menő időbélyegnél a névleges képkockaidő a tartalék. A timestamp az összerakott kocka első töredékéből származik, nem a legutoljára beérkezett másik töredékből. A közös médiazár és a korábbi CAS-javítás megmaradt.

A natív regressziós tesztek külön ellenőrzik a kimaradó képkockák időközét, az időbélyeg körbefordulását és a tartalék időlépést. A hálózati csomagvesztés és a hosszabb platformhívások is látszanak a naplóban; ezek külön akadásforrások maradhatnak.

A teljes build, a natív WASM tesztek és a JS tesztek sikeresek. A független subagent ellenőrzése nem talált új blokkoló hibát vagy race-t. A tévé `install completed` válasszal megerősítette az új csomag telepítését; az appot nem indítottam el. A friss aláírt csomag az `outputs/SteamTV.wgt` fájlban található.

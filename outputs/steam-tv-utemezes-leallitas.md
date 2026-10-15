# Kép–hang ütemezés és válaszképes leállítás – 2026. október 15.

Állapot: a callbacken belüli 100 ms-os ütemezés a 10:09-es hardveres regresszió miatt visszavonva. Az aszinkron leállítás megmaradt. A későbbi állapotot a `steam-tv-utemezes-regresszio.md` rögzíti; az alábbi szöveg a korábbi változat története.

A 22:51–22:53-as kézi próba megerősítette, hogy a korábbi összevonás 100 Opus frame/s mellett körülbelül 50 PCM append/s-t ad, 0 dekóderhibával és 0 visszautasítással. A kép többnyire 57–61 megjelenített FPS körül haladt, a PTS-ek együtt jártak, de a felhasználó továbbra is hang/kép akadozást és Big Picture bezárásakor fagyást jelentett. A Steam bezáráskor megkapta a lemondást; a korábbi kliensmenü csak a teljes szinkron cleanup után tért volna vissza.

## Változások

- Mindkét médiaworker közös monotón órához ütemezi a beadást, 100 ms előtöltéssel. A Samsung ultra-low módja nem pufferel és azonnal renderel, ezért önmagában a helyes PTS és a 20 ms-os PCM összevonás nem simítja ki az érkezési ingadozást. A naplóban 91 ms-os késési csúcs is szerepelt. A 100 ms hozzáadott várakozás tudatos késleltetési kompromisszum; ennél hosszabb hálózati/forráskiesést nem fed le.
- A várakozás a médiazáron kívül történik, a meglévő IHS packetablakok tartják a sorban váró csomagokat. A főciklus és a socket fogadó szála nem vár médiahatáridőre. A PCM-adatot a beadás előtt saját puffer tartja meg. A leállítás atomikus generációváltással megszakítja a várakozást; új player esetén a régi frame nem kerülhet az új lejátszóhoz. Sérült, irreálisan előre mutató időbélyeg nem parkoltathatja a workert másodpercekig.
- Külön cleanup szál kezeli a Steam lemondását, a még élő diagnosztikai hívás kivárását, a session/provider felszabadítását és a lejátszó lezárását. A főciklus addig is fut, a menü látható, és nem enged új streamet az előző cleanup befejezése előtt. A lejátszó leállítása csak a régi médiaworkerek bevárása után történik. A SDK oldalszálról támogatott, böngészőre proxyzó műveleteit ezért nem a felület főciklusán várjuk.
- Öt másodperc után megjelenik a még folyamatban lévő cleanup szakasza. A Vissza gomb eközben közvetlenül kiléphet. Egy beragadó platformhívást nem tekintünk sikeres cleanupnak, és nem szabadítunk fel előtte használt erőforrást vagy engedünk új kapcsolatot. Szálindítási hiba esetén sincs blokkoló szinkron tartalékút.

## Ellenőrzés

A JS teszt igazolja a takarítás közbeni start/pair tiltást, a késői callbackek kezelését, a Vissza gombot és a completion utáni újraindítás engedélyezését. A natív teszt valódi IHS session/provider cleanupot futtat, majd szándékosan blokkolt médialezárás mellett ellenőrzi, hogy a completion-kapu nem nyílik ki idő előtt. A pacing tesztek ellenőrzik az előtöltést, a lejárt határidőt, a megszakítást, a generációváltást és az érvénytelen órát. A meglévő kontroller-, PCM-batch-, párosítási és csomagablak-tesztek is sikeresek.

A leállítás konkrét beragadó belső hívását a korábbi Steam-napló nem azonosította. A teljes 60 FPS, folyamatos hang és tényleges újraindítható cleanup még nincs az új csomaggal hardveren igazolva. A tévén az appot nem indítjuk el automatikusan.

A végső teljes Tizen/WASM build, a natív WASM tesztek és a JS tesztek sikeresek. A tévé `install completed` választ adott az új csomagra. Az appot nem indítottam el. A friss aláírt csomag másolata `outputs/SteamTV.wgt`.

# Leállítás utáni fekete képernyő – 2026. október 15.

A Big Picture bezárása után a böngészős watchdog a korábbi, futó streamet jelző natív fázisból újra bekapcsolhatta a videónézetet. A natív fázis és a képjelző most már a leállítás kezdetén és végén alaphelyzetbe kerül. A watchdog nem aktiválhat videónézetet lezárás közben vagy lezárt munkamenetből.

A státuszmező egy teljes egészében elrejtett kártyában volt. Kikerült a kártyából, ezért a leállítás állapota a videónézetben is látható marad. A JS regressziós teszt a tényleges HTML-t és a lezárás utáni, szándékosan elavult natív fázist is ellenőrzi; az utóbbi nem indíthatja újra a videót. A natív tesztek és a teljes build sikeresek.

A felhasználó szerint a tévén a hang és a kép is enyhén receg még. A 10:25–10:29 közötti naplóban a normál szakaszban körülbelül 60 videókocka/s, 100 audio frame/s, 50 PCM-beadás/s, nulla audio dekóderhiba és nulla visszautasítás szerepel. Ezek az átlagok nem bizonyítják az egyenletes megjelenítést. Az összesített médiadiagnosztika most a hiányzó audio frame-ek számát is közli; a figyelmeztetések szűrése ezt korábban elfedhette. A lejátszás ütemezése ebben a csomagban nem változott, a recegés megoldását nem állítjuk igazoltnak.

A tévé az aláírt csomag telepítését `install completed` válasszal megerősítette. A csomag másolata `outputs/SteamTV.wgt`. Nem indítottuk el az alkalmazást; a tévés kézi ellenőrzés hátravan.

# ProShadeLite (addon ReShade, original)

## Build
- **Fara nimic instalat**: pune folderul pe un repo GitHub, push → tab-ul *Actions* → artifact `ProShadeLite` (contine `.addon64` + `.fx`).
- **Local (Windows)**: instaleaza Visual Studio (C++), CMake, Git → ruleaza `build.bat`.

## Instalare
1. ReShade 6.x **cu suport pentru add-ons** (la instalare alegi varianta "with add-on support"), pe jocul tau.
2. `ProShadeLite.addon64` → langa `ReShade64.dll`/`dxgi.dll` (folderul jocului / FiveM).
3. `ProShadeLite.fx` → `reshade-shaders\Shaders\`.
4. In joc deschizi ReShade (Home), tab-ul **Add-ons → ProShadeLite**.

## Configurare shadere ("Remove ...")
Hash-urile shaderelor difera de la joc la joc, deci nu pot fi incluse dinainte:
1. Tab **Setup** → bifeaza *Scaneaza shadere*.
2. Mergi unde e efectul (ploaie, ceata, ...). Cu `<` `>` treci prin shadere; cel ales dispare imediat din imagine.
3. Cand dispare exact efectul dorit: alege optiunea in lista → **Atribuie shaderul ales**.
4. Se salveaza in `ProShadeLite.ini`; se poate copia pe alt PC.
**FPS Boost** activeaza dintr-o data toate optiunile pe care le-ai configurat din lista (ploaie, ceata, fum, dungi, orbe, halouri, nori, zapada, balti, glow, soare).

## Optiuni
| Optiune | Mecanism |
|---|---|
| Remove Clouds/Sun/Moon/Stars, Light Streaks/Orbs, Glow, Halos, Shadows, Sunlight, Contact Shadows, Fog, Rain, Snow, Smoke, Puddles, Hide Game UI, Streamer Mode | skip draw pe shadere atribuite |
| Brightness, Brighter Nights, Lighter Shadows, Sky Color | `ProShadeLite.fx` (Sky Color cere depth buffer) |
| Before / After | opreste toate efectele + skip-urile (compari rapid) |
| Protect UI From Effects (Early) | efectele ReShade se aplica inainte de primul draw de UI (necesita shader de UI atribuit la *Hide Game UI*); Classic = comportamentul normal |
| Take a screenshot | salveaza `.bmp` in `ProShadeLite Screenshots` |

## Limitari (sincer)
- Nu a fost rulat in joc de mine: codul compileaza sintactic fata de API-ul ReShade, dar comportamentul in FiveM/GTA V trebuie testat.
- *Early* depinde de cum randeaza jocul UI-ul; daca arata gresit, foloseste *Classic*.
- Unele servere interzic ReShade sau modificari care ascund ceata/ploaie; verifica regulile serverului.

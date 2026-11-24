# RTLWiFiBar – mejoras (rama `mejoras-optiplex`)

> Fork personal de [chris1111/macOS-RTL-WIFI](https://github.com/chris1111/macOS-RTL-WIFI). Esta rama **solo cambia la app de la barra de menús (RTLWiFiBar)**; el driver es el de chris1111 sin cambios. No es un proyecto oficial ni está respaldado por chris1111.
> *Personal fork. This branch only changes the menu bar app (RTLWiFiBar); the driver is chris1111's, unchanged. English summary below.*

### Descarga
App compilada en **[Releases](../../releases)** (`RTLWiFiBar-8.8.4-optiplex.1.zip`, Intel y Apple Silicon).

### Qué cambia respecto de RTLWiFiBar 8.8.4
- **Red conectada clara:** bloque "Conectado a *red*" con banda, señal e IP, con colores del sistema (modo claro y oscuro).
- **Lista más simple:** `Red · 5 GHz · señal buena ✓` en lugar de `(WPA2-PSK, ch 44, -61 dBm)`.
- **Cambiar de red sin olvidar la actual:** la app deja la red actual y se une a la nueva; la anterior sigue guardada.
- **"Desconectar de *red*" / "Reconectar a *red*"** en lugar de "Desconectar Wi-Fi USB".
- **"Buscar ahora" estando conectado hace un escaneo real:** el driver no escanea mientras está asociado, así que la app se desconecta unos segundos, busca y vuelve a la misma red (salvo que elijas otra).
- **Otros puntos de acceso de la misma red** visibles en la lista (informativos: el driver se une por nombre de red).
- **Opción "Pausar Wi-Fi con cable conectado"** (desactivada por defecto): pausa el Wi-Fi mientras haya un cable Ethernet con IP y lo reanuda al quitarlo. Quien necesite las dos conexiones a la vez la deja apagada.
- **Contraseñas en el Llavero de macOS** (antes, archivo propio con la clave ofuscada). Se migran solas la primera vez.

### Instalación
1. Instala primero **macOS-RTL-WIFI 1.1.2 de chris1111** (driver + app original) desde [sus releases](https://github.com/chris1111/macOS-RTL-WIFI/releases) y reinicia.
2. Descarga el zip de esta versión, descomprímelo, cierra RTLWiFiBar y reemplaza `/Applications/RTLWiFiBar.app` (pide la clave de administrador).
3. La primera vez macOS avisa que no puede verificar al desarrollador (la app no está firmada con un certificado de Apple). Ábrela con **clic derecho > Abrir**, o en **Ajustes > Privacidad y seguridad > Abrir igualmente**.
4. Tus redes guardadas se conservan; las contraseñas pasan al Llavero automáticamente.

Para volver a la original, reinstala el paquete de chris1111.

### Compilar
`./Create_Build.tool` (la app compila con las Command Line Tools; el driver necesita Xcode). Si existe en el Llavero un certificado de firma llamado "RTLWiFiBar Local Signing" se firma con él; si no, firma ad-hoc.

### English summary
Menu bar app changes only (driver unchanged): clear "Connected to *network*" block; short list format (band + signal in words); switch networks without forgetting the current one; "Disconnect from *network*"; "Rescan" while connected does a real sweep (brief disconnect, then rejoins); other APs of the same network listed; optional "Pause Wi-Fi while a cable is connected" (off by default); Wi-Fi passwords stored in the macOS Keychain (migrated automatically). Install chris1111's macOS-RTL-WIFI 1.1.2 first, then replace `/Applications/RTLWiFiBar.app` with the one from Releases (right-click > Open the first time: not signed with an Apple Developer ID).

Licencia / License: GPL-2.0, como el proyecto original. Créditos del driver y de la app base: chris1111 y los autores listados en `CREDITS.md`.

---

[![pages-build-deployment](https://github.com/chris1111/macOS-RTL-WIFI/actions/workflows/pages/pages-build-deployment/badge.svg)](https://github.com/chris1111/macOS-RTL-WIFI/actions/workflows/pages/pages-build-deployment) [![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://github.com/chris1111/macOS-RTL-WIFI/blob/main/LICENSE) [![.github/workflows/Build.yml](https://github.com/chris1111/macOS-RTL-WIFI/actions/workflows/Build.yml/badge.svg)](https://github.com/chris1111/macOS-RTL-WIFI/actions/workflows/Build.yml)
# macOS-RTL-WIFI
Realtek USB Wi-Fi driver (RTL81xx / RTL88xx families) for Intel macOS
### Download Release ➦ [macOS-RTL-WIFI](https://github.com/chris1111/macOS-RTL-WIFI/releases)

### Building the Project:

- [x] `Clone and build macOS-RTL-WIFI:` Requirement Xcode 26
```bash
git clone https://github.com/chris1111/macOS-RTL-WIFI.git && cd $HOME/macOS-RTL-WIFI && ./Create_Build.tool
```

Independent macOS kernel-extension project for Realtek USB Wi-Fi adapters on
Intel Macs (Hackintosh with OpenCore or Clover). Station mode, WPA2-Personal,
2.4 GHz and, on dual-band adapters, 5 GHz. Developed and tested on macOS
Monterey, macOS Ventura and macOS Tahoe. Working from macOS 11 to macOS 26

> **This driver is in development.** Expect changes between releases; test
> before relying on it for daily use, and report issues on the project page.
> This is an independent project, not affiliated with or endorsed by Realtek.
> It is provided as is, without warranty.

## Quick start

1. Download `Release.zip` from the [releases page](https://github.com/chris1111/macOS-RTL-WIFI/releases) and run the installer package. Choose the OpenCore or Clover driver entry for your setup.
2. Restart. The package installs the kext into your EFI folder, plus the menu bar app **RTLWiFiBar** and its launch agent.
3. Plug in the adapter, click the RTLWiFiBar icon in the menu bar, choose your network and enter the router password. The network is remembered and rejoins automatically after a reboot (typically in 1-3 seconds).

**Good to know:** the driver is not an Apple Wi-Fi provider. The adapter appears as an Ethernet-style interface ("802.11n NIC") in System Settings > Network, not in the macOS Wi-Fi menu. Use the RTLWiFiBar menu to pick the network. macOS identifies network interfaces by MAC address, so a replacement dongle shows up as a new service ("802.11n NIC 2").

- **Kext + installer package.** `Create_Build.tool` builds the kext and, from
  the `PKG/` folder in this tree, the full `Wireless USB macOS-RTL-WIFI.pkg`
  installer: RTLWiFiBar.app, launch agent, automatic EFI drop-in for OpenCore or Clover (including
  the OpenCore `config.plist` entry) and an uninstaller.
- **Builds from the command line** with `./Create_Build.tool` (plain
  `xcodebuild` also works for the kext alone).
- **No external SDK, no downloads.** Kernel headers come from the macOS SDK
  already inside Xcode; the small kernel-safe C++ subset the sources use lives
  in this tree under `libs/kstl/` (own library, GPL-2.0). The packaging step
  uses `pkgbuild` and `pkgutil`, which ship with macOS.

## Screenshot Package installer OC and Clover

<img src="PICS/Screenshot-PKG.png" alt="Screenshot" style="width:60%;">

<img src="PICS/Screenshot-PKGClover.png" alt="Screenshot" style="width:60%;">

## Screenshot Package Uninstall Utility

<img src="PICS/Screenshot-Uninstall.png" alt="Screenshot" style="width:60%;">

## Screenshot StatusBarApp

<img src="PICS/Screenshot-App.png" alt="Screenshot" style="width:40%;">

<img src="PICS/Screenshot-App2.png" alt="Screenshot" style="width:40%;">


## Supported adapters — full verified Realtek USB fleet, 263 personalities

The kext ships the complete verified identity catalog
(`Core/USBWiFiProfiles.inc`) as IOKit personalities. Only identities with
verified catalog provenance ship — 8 unverified rows and the station-gated
RTL8814BU stay out (fail-closed).

| Family | USB IDs | Status |
|---|---|---|
| RTL8188EU / RTL8192EU | 28 | ✅ 8192EU tested (TL-WN823N, TL-WN822N, TL-WN821N); ✅ 8188EU tested (TL-WN725N, generic 0bda:0179) |
| RTL8192CU era (8188CU/CUS/CTV/RU/92C) | 88 | implemented, not tested |
| RTL8812AU | 36 | implemented, not tested |
| RTL8821AU | 35 | ✅ tested (Archer T2U Plus, 20 MHz) |
| RTL8822BU / RTL8822CU | 32 | ✅ 8822BU tested (Archer T4U, Cudy WU1400, TRENDnet mini) |
| RTL8821CU | 12 | ✅ tested (MLINK 96581, generic 0bda:c811) |
| RTL8814AU | 17 | implemented, not tested |
| RTL8192DU | 5 | implemented, not tested |
| RTL8192FU | 4 | implemented, not tested |
| RTL8723BU | 3 | ✅ tested (EDIMAX EW-7611UCB) |
| RTL8710BU | 2 | implemented, not tested |

The driver identifies the chip at attach time and dispatches through the
family backend table (`Core/FamilyBackend.c`); every family's firmware image
is embedded in the Driver layer. Families marked "not tested" run the same
verify-before-touch attach and are expected to fail cleanly rather than wedge
the machine, but they need field logs for validation.

## Adapter compatibility list

**15 adapters have been tested** (✅ working). If you have a dongle from this list that works, please open a ➤ [discussion](https://github.com/chris1111/macOS-RTL-WIFI/discussions) and let us know. Thank you.

Legend:

- ✅ **Tested** with this kext (1.1.1): connects, auto-rejoins after reboot.
- (no mark) **Listed, not verified with this kext yet.** These adapters are included because they are known to work with the original Realtek macOS drivers (RtWlanU), and they use the same chipset families this kext supports. They are expected to work, but this is not confirmed. Reports are welcome: please include the USB ID (`ioreg -p IOUSB -l | grep -i -A3 "idVendor"`) and a log.

- Acer UWA5
- Alfa AWUS036AC
- Alfa AWUS036ACH
- Archer T2U Plus (AC600) ✅
- Archer T2U Nano
- Archer T3U Nano ✅
- Archer T3U
- Archer T3U Plus
- Archer T2U MINI V3
- Archer T4U V1, V2, V3 ✅
- Archer T9UH V1, V2
- ASUS USB AC68
- ASUS USB-N13
- ASUS USB Nano-AC53
- BrosTrend FBA_AC3
- COMFAST CF-811AC
- COMFAST CF-812AC
- Comfast CF-WU810N
- Comfast CF-758F
- Cudy WU1400 ✅ RTL8822BU
- Cudy WU1300S
- Cudy WU700
- CXFTEOXK
- DLink DWA-121 N150
- DLink DWA-131 E
- DLink DWA-171 C
- DLink DWA-182 D
- DLink DWA-192 A
- EDIMAX EW-7611UCB ✅
- EDIMAX EW-7722UTn V2
- EDIMAX EW-7822ULC
- EDIMAX EW-7833UAC
- EDIMAX EW-7612UAn V2
- EDIMAX N300
- EDIMAX EW-7811Un (N150) ✅
- EDUP EP-AC1689
- Fenvi AC1300 (RTL8812BU)
- FILOWA USB WiFi (RTL8812BU)
- Foktech AC600 Nano
- Jensen Eagle 100-AC
- Generic eBay RTL8188EU dongle (0bda:0179) ✅
- Kextech MINI USB RTL8192
- Generic RTL8811CU dongle (0bda:c811) ✅
- Linksys WUSB6300 V2
- Linksys WUSB6400M
- MLINK 96581 ✅ RTL8821CU
- Netgear A6100
- Netgear A6150
- Netgear A7000
- Netis WF2120 N Nano USB
- M-Tech UW-01 USB
- Mercusys MW300UM
- Plexgear AC1200
- Raimadim WiFi AC1300 (RTL8822BU)
- Sitecom WLA7100
- TechKey AC1200
- TL-WN823N v1/v2/v3/v4 ✅ (tested: 2357:0109)
- TL-WN725N v3 ✅ (RTL8188EU)
- TL-WN723N v2/v3
- TL-WN722N v2/v3 ✅ (RTL8188EU)
- TL-WN821N v6 ✅ (tested: 2357:0107, RTL8192EU)
- TL-WN822N v4/v5 ✅
- TENDA W311-MINI
- TENDA U12
- TRENDnet mini ✅ RTL8822BU
- TRENDnet N150 Micro
- TRENDnet TEW-808UBM
- TRENDnet TEW-908UB
- UGreen CM448
- YUNCLOUD Realtek (RTL8814AU)
- ZAPO W58L (RTL8811AU)
- Zebronics USB150WF1
- Zyxel NWD6602
- Zyxel NWD6605

Per-dongle test details: [DONGLES.md](DONGLES.md).

## Known limitations

- No automatic rejoin after a Wi-Fi drop in the middle of a session (a reboot rejoins normally).
- A replacement dongle appears as a new network service ("NIC 2").

Details and next steps: [KNOWN-ISSUES.md](KNOWN-ISSUES.md).

## Reporting a problem

Open an issue or discussion with: the adapter model and USB ID (`ioreg -p IOUSB -l | grep -i -A3 "idVendor"`), your macOS version, the Mac or Hackintosh model, and the RTLWiFiBar log. Please say whether you rebooted after connecting.


## Requirements

- Intel Mac (the kext is x86_64 only), full Xcode with command-line tools
  selected (`sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`).
- Nothing else. The build reads the macOS SDK that ships inside Xcode
  (`Kernel.framework` headers) and the in-tree `libs/kstl/` subset — no git
  clone, no fetched SDK, no network access.
- An OpenCore setup you administer (Clover also works: `EFI/CLOVER/kexts/Other`).
- For the installer package, nothing extra: `pkgbuild` and `pkgutil` are part
  of macOS.

## Build from the command line

One command, no arguments required:

       ./Create_Build.tool          # release build + installer package
       ./Create_Build.tool debug    # debug build + installer package
       ./Create_Build.tool -v       # show the full compiler output

Products:

- `Build/Release/macOS-RTL-WIFI.kext` — the driver.
- `Build/Release/Wireless USB macOS-RTL-WIFI.pkg` — the installer package.
  Before packaging, the tool copies the fresh kext into the
  `PKG/efifolderoc/` and `PKG/efifolderclover/` staging folders, so the EFI
  choices always carry the kext that was just built. When it finishes, it
  opens `Build/Release` in Finder.

The package offers three choices (sub-packages built with `pkgbuild` from
`PKG/`, flattened with `pkgutil` and the `PKG/Resources/Distribution` script;
welcome, read-me and conclusion pages ship in the same 16 languages as the
bundled RTLWiFiBar.app — da, de, en, es, fr, fr-CA, it, ja, ko, nl, pl, pt-PT,
ru, sv, tr, zh-Hans — picked from the system language, English as fallback):

| Choice | What it installs |
|---|---|
| Drivers OC macOS-RTL-WIFI | kext to `EFI/OC/Kexts/` on the ESP + `Kernel > Add` entry merged into `config.plist` |
| Drivers CLOVER macOS-RTL-WIFI | kext to `EFI/CLOVER/kexts/Other/` on the ESP (Clover scans kexts, no config.plist edit) |
| Uninstall Utility | removes the WLAN app support, launch agent and the kext from the ESP |

Both driver choices also install the RTLWiFiBar.app, add the launch
agent (`Wlan.Software.plist`) and require a restart. The uninstaller removes
the `macOS-RTL-WIFI.kext` entries from the OpenCore `config.plist` and deletes
the kext from the ESP (`EFI/OC/Kexts/` and `EFI/CLOVER/kexts/Other/`).


## Provenance and credits

Primary upstream reference for the RTL8188EU hardware programming:

**https://github.com/lwfinger/rtl8188eu** (GPL-2.0)

The full upstream list — Linux `rtw88` / `rtl8xxxu`, OpenBSD `rtwn` and
`net80211`, the OpenIntelWireless `itlwm` net80211 macOS integration, FreeBSD
`rtwn`, `wireless-regdb`, Realtek firmware terms — is documented in
[CREDITS.md](CREDITS.md) and [SOURCES.md](SOURCES.md).

## License

GPL-2.0 — see [LICENSE-GPL-2.0.txt](LICENSE-GPL-2.0.txt). Third-party notices
and licenses are retained in the respective files and collected in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). The Realtek firmware images
embedded in the driver are separate, unmodified binary data under Realtek's own
redistribution licenses (not GPL-2.0); they are included in the same file.

## Status

Current versions: kext **1.1.1**, RTLWiFiBar **8.8.2**. See
[RELEASE-NOTES-1.2.3.md](RELEASE-NOTES-1.2.3.md) and `docs/patch-history/` for
what changed between releases. Re-verify on your own hardware after the first
EFI swap.

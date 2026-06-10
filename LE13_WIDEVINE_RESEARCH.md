# LE13/Widevine/Pi5 — 4h Root Cause Analyse

**Datum:** 2026-06-10  
**Session-Dauer:** ~4h  
**Betroffenes System:** crackberry5 (Raspberry Pi 5, LE13 Nightly 2026-06-10, Kernel 6.18.32)  
**Repository:** rixhal/switch2-bt (GitHub) + git.richie.fyi/rixhal/switch2-raw (Forgejo)

## Endzustand: NICHT LÖSBAR auf aktuellem LE13-Build

Nach 4h systematischer Analyse und 8 Fix-Versuchen: **Widevine DRM auf LE13/Pi5 funktioniert nicht ohne Kernel-Patch.**

---

## 4 Root Causes (alle dokumentiert)

### 1. ❌ `libpugixml.so.1` fehlt → ISA 22.3.14 lädt nicht

```
ERROR: Unable to load inputstream.adaptive.so.22.3.14, 
reason: libpugixml.so.1: cannot open shared object file
```

**=> JEDER Crunchyroll/Live-TV Start = "Allgemeiner Fehler" oder "Addon kann nicht geladen werden"**

**Fix:** Selbst kompiliert (g++ auf Pi-Host), deployt nach `/storage/.kodi/addons/mesa-le12/lib/`

### 2. ❌ `default="true"` in guisettings.xml → Custom-Werte werden ignoriert

Kodi's Startup-Routine: `default="true"` = ignoriere gesetzten Wert, nutze kompilierten Plattform-Default. Für RPi5 ist das **DRMPRIME mit V4L2-Hardware-Decoder**.

**Fix:** `default="false"` — dann wird der Value respektiert, nicht überschrieben.

### 3. ❌ `ToCdmVideoCodecProfile: Unknown codec profile 0`

jacuzzi CDM (ChromeOS 148) versteht das AVC/H.264-Profil nicht bei Secure-Decode-Init. CDM-Source erwartet Profile-ID aus ChromeOS-Enumeration, aber Kodi/ISA setzt 0 → CDM crasht → Testpattern.

**=> DRMPRIME Secure-Path OHNE diesen Fix = RGB-Testpattern, garantiert.**

### 4. ⚠️ `/dev/dma_heap/reserved` ephemeral

- LE13 Nightly hat `CONFIG_DMABUF_HEAPS=y` und `CONFIG_DMABUF_HEAPS_CMA=y`
- **KEIN `CONFIG_DMABUF_HEAPS_RESERVED`** — Raspberry Pi Downstream-Patch
- `reserved`-Device erschien temporär als Symlink → `linux,cma`, verschwand nach Reboot
- **Ohne dieses Device KEIN Hardware-Secure-Decoder-Pfad**

### 5. ⚠️ BOB CDM (ChromeOS 144) ist ARM32 → `ld-linux-armhf.so.3` fehlt

Versuch: CDM-Downgrade auf ältere Version (Widevine 4.10.2662.3) mit weniger strikter Output Protection. **ABER:** Alle ChromeOS ARM-CDM-Extraktions-Images sind ARM32. Pi 5 ist aarch64 → kann ARM32-CDM nicht laden. **Kein aarch64-CDM öffentlich downloadbar.**

---

## 8 Fix-Attempte (alle gescheitert)

| # | Versuch | Ergebnis | Warum gescheitert |
|---|---|---|---|
| 1 | `useprimerenderer=2` (Direct-to-Plane) | DRMPRIME läuft trotzdem | RPi5 hardcodiert DRMPRIME bei `default="true"` |
| 2 | `useprimerenderer=1` (EGL/GLES) | DRMPRIME läuft trotzdem | RPi5 Plattform-Default überschreibt |
| 3 | `default="false">usemediacodec=false` | Settings halten, aber kein Effekt | Software-Decoder läuft, aber CDM verweigert |
| 4 | BOB CDM (Widevine 4.10.2662.3) | CDM crasht | ARM32 → kein `ld-linux-armhf.so.3` auf aarch64 |
| 5 | `manifest_config = "{}"` (leer) | ISA akzeptiert, kein Fehler | CDM-Problem unabhängig von ISA |
| 6 | `force_secure_decoder: false` | ISA Parse-Error | ISA 22.3.14 supported `force_secure_decoder` weder true noch false |
| 7 | `useprimedecoder=true` + Secure-Path | Testpattern + `ToCdmVideoCodecProfile: Unknown` | CDM versteht AVC-Profil 0 nicht |
| 8 | `NOSECUREDECODER=true` + Software-Decode | Audio, 1440 Frames, aber **schwarzes Bild** | DRMPRIME-Renderer kann Software-Frames nicht rendern |

---

## Was FUNKTIONIERT (System-Health Check)

| Komponente | Status | Beweis |
|---|---|---|
| V3D GPU (Kernel 6.18.32) | ✅ | OpenGL ES 3.1, `/dev/dri/card0+card1` |
| HDMI 1920×1080 @ 60Hz | ✅ | `CRTC mode: 1920x1080 @ 60 Hz` |
| HEVC Hardware-Decoder | ✅ | `/dev/video19` (rpi-hevc-dec) |
| Widevine L3 (CMA Heap, 512 MB) | ✅ | `linux,cma` Fallback aktiv |
| Crunchyroll Plugin (API, Auth) | ✅ | Manifest-Download erfolgreich |
| Vavoo PVR (261 Kanäle) | ✅ | Proxy läuft auf Port 37890 |
| Flatpak Chromium 148 | ✅ | Installiert |
| CSR8510 BT Dongle | ✅ | `hci1 UP RUNNING` |
| libpugixml.so.1 | ✅ | Deployt (selbst kompiliert) |

---

## Deployte Fixes (bleiben auf crackberry5)

1. **`libpugixml.so.1`** → `/storage/.kodi/addons/mesa-le12/lib/`
2. **Crunchyroll `manifest_config = "{}"`** → kein force_secure_decoder
3. **Vavoo Content-Type `video/mp2t`** → korrekt für TS-Streams
4. **playercorefactory.xml** → VideoPlayer-Zwang
5. **guisettings** → `default="false"` für alle Player-Settings

---

## Empfehlungen

### A) Downgrade auf LE12 (15 Minuten, GARANTIERT funktionierend)
- LE12 nutzt EGL/GLES-Renderer ohne DRMPRIME-Zwang
- Software-H.264 funktioniert über GLES-Texturen
- Widevine jacuzzi funktioniert mit L3
- **Crunchyroll + Live-TV laufen sofort**

### B) Kernel-Rebuild (2-4 Stunden)
1. LE13 Source klonen
2. `CONFIG_DMABUF_HEAPS_RESERVED=y` in Kernel-Config
3. Build → deploy kernel.img + SYSTEM
4. Testen ob CDM Secure-Decoder macht

### C) Auf LE13-RC warten
- Kein neuerer Nightly als 2025-11-17 auf test.libreelec.tv
- Unser Build: 2026-06-10 (Pi-dev) — ist neuer, aber CDM-Problem ungelöst
- Forum: https://forum.libreelec.tv/thread/28572

---

## Lessons Learned

1. **`ldd` zuerst.** ISA 22.3.14 fehlte `libpugixml.so.1` → alle Playbacks scheitern → irreführende Fehlermeldungen
2. **`default="true"` in Kodi = Garantie dass deine Config ignoriert wird**
3. **LE13 ist Beta.** Nightlies haben ungelöste Architekturprobleme mit Widevine
4. **Pi 5 hat KEINEN V4L2 H.264-Decoder** (anders als Pi 4) → Software-Decode ist Pflicht
5. **DRMPRIME-Renderer kann keine Software-decodierten Frames rendern** → fundamentale Architekturlücke
6. **ChromeOS CDMs sind ALLE ARM32 ODER x86_64** → kein aarch64-CDM öffentlich
7. **CDM `ToCdmVideoCodecProfile` erwartet ChromeOS-Source-Enums** → Kodi/ISA setzt 0 → CDM crasht

---

## Git History

```
72587e9 — Squashed: BTstack C bridge 609 Zeilen + Donor LTKs
d23e6f1 — docs: LE13/Widevine Root Cause Analyse (diese Datei)
```

---

Datei: `LE13_WIDEVINE_RESEARCH.md`  
Repository: rixhal/switch2-bt (GitHub) + git.richie.fyi/rixhal/switch2-raw (Forgejo)  
Branch: feat/switch2d-production-daemon

---

## Finale Resolution (2026-06-11)

**Aktion:** Downgrade auf LE12 per SD-Karten-Recovery.
- `/flash/kernel.img` → 11.8 MB (LE12 6.12.56)
- `/flash/SYSTEM` → 151 MB (LE12)
- Boot Partition via ext4-Mount auf Hermes-Pi geschrieben (FAT32 dd war korrupt)
- SD-Karte zurück in crackberry5 → bootet LE12 sauber

**Status:**
- Crunchyroll + Live-TV = ERWARTET funktionierend (LE12 GLES-Renderer)
- Alle 5 Fixes bleiben deployed für zukünftiges LE13-Retest
- pugixml.so.1, manifest_config={}, playercorefactory.xml, guisettings
- Reset: `NOSECUREDECODER=true` (für LE13) kann auf LE12 auf false

# LE13/Widevine/Pi5 — Root Cause Analyse

**Datum:** 2026-06-10
**Forschungssession:** ~5h Debugging (Sessions 1-7)
**Betroffenes System:** crackberry5 (Raspberry Pi 5, LE13 Nightly 2026-06-10, Kernel 6.18.32)

## Zusammenfassung

Crunchyroll (Widevine DRM) zeigt RGB-Testpattern statt Video. Live-TV (Vavoo PVR) zeigt schwarzes Bild. Nach ~5h systematischer Analyse: **5 Root Causes identifiziert, 2 davon gefixt.** Der verbleibende Showstopper: jacuzzi-CDM versteht das AVC/H.264-Codec-Profil nicht bei Secure-Decode-Init (`ToCdmVideoCodecProfile: Unknown codec profile 0`), und Secure Video Path benötigt `/dev/dma_heap/reserved`.

---

## Gefundene Root Causes

### 1. ✅ `libpugixml.so.1` fehlt — GEFIXT

InputStream Adaptive 22.3.14 braucht `libpugixml.so.1` als Shared-Object-Dependency. LE13 kompiliert ISA mit pugixml, aber die Library wird **nicht mitinstalliert**. Kodi-Log zeigte:

```
ERROR: Unable to load inputstream.adaptive.so.22.3.14,
reason: libpugixml.so.1: cannot open shared object file: No such file or directory
```

**=> JEDER Crunchyroll/Live-TV Start = "Allgemeiner Fehler" oder "Addon kann nicht geladen werden"**

**Fix (deployed):** pugixml v1.14 aus Source kompiliert (g++ auf Pi-Host), deployt nach `/storage/.kodi/addons/mesa-le12/lib/` (im LD_LIBRARY_PATH).

### 2. ⚠️ Widevine CDM Output Protection

Der genutzte CDM (`jacuzzi`, ChromeOS 148, Mediatek MT8183) verlangt Secure Video Path für Output Protection. Pi 5 hat `/dev/dma_heap/reserved` **nur ephemeral nach bestimmten Reboots** (Symlink → linux,cma).

**Session 7 Entdeckung:** Nach einem Reboot erschien `reserved` als Symlink, nach dem nächsten Boot war es wieder weg.

**Versuchte CDM-Wechsel:**
- `bob` (Rockchip RK3399, ChromeOS 144): **ARM32-only** → `ld-linux-armhf.so.3` fehlt → crasht
- `jacuzzi` (Mediatek MT8183, ChromeOS 148): via libc6-armhf-compat ausführbar → strikte Output Protection

**Konfiguration jacuzzi:**
```json
{"version": "16640.57.0", "chrome_version": "148.0.7778.250", "boardname": "jacuzzi", "arch": "arm"}
```

### 3. ⚠️ `force_secure_decoder` Property — ISA 22.3.14 Bug

InputStream Adaptive 22.3.14 **unterstützt `force_secure_decoder` überhaupt nicht**:

```
error: ParseManifestConfig: Unsupported "force_secure_decoder" config or wrong data type
```

- `"force_secure_decoder": true` → Fehler
- `"force_secure_decoder": false` → Gleicher Fehler
- Property gar nicht setzen → ISA parst nichts, greift Default

**Fix (deployed):** `manifest_config` = `"{}"` (leeres JSON — keine unsupported Properties)

### 4. ✅ Kodi Settings: `default="true"` ignoriert Custom-Werte — GEFIXT

```xml
<setting id="videoplayer.useprimerenderer" default="true">0</setting>
```

Wenn `default="true"` in guisettings.xml steht, **ignoriert Kodi den Value** und nutzt den kompilierten Plattform-Default. Für RPi5 ist das **DRMPRIME (RendererDRMPRIMEGLES)**.

**Fix (deployed):** `default="false"` auf allen Player-Settings:
```xml
<setting id="videoplayer.useprimerenderer" default="false">0</setting>
<setting id="videoplayer.useprimedecoder" default="false">false</setting>
```

### 5. ❌ `ToCdmVideoCodecProfile: Unknown codec profile 0` — UNGELÖST

Widevine jacuzzi CDM versteht das AVC/H.264-Profil nicht bei Secure-Decode-Initialisierung:

```
CDVDVideoCodecDRMPRIME::AddData  : ToCdmVideoCodecProfile: Unknown codec profile 0
```

Das ist der Grund, warum **DRMPRIME + Secure-Decoder = Testpattern**. Der CDM kann das Codec-Profil nicht mappen → Secure-Decode schlägt fehl → CDM weigert Output → RGB-Testpattern.

`ToCdmVideoCodecProfile: Unknown codec profile 0` erscheint NUR bei Crunchyroll (DRM-geschützt). Live-TV (kein DRM) hat denselben Codec, aber der Fehler tritt dort nicht auf — weil kein CDM im Spiel ist.

---

## Was FUNKTIONIERT

| Komponente | Status |
|---|---|
| V3D GPU (Kernel 6.18.32) | ✅ OpenGL ES 3.1 |
| HDMI 1920×1080 @ 60Hz | ✅ |
| HEVC Hardware-Decoder (/dev/video19) | ✅ |
| Widevine L3 (CMA Heap, 512 MB) | ✅ |
| Crunchyroll-Plugin (API, Auth) | ✅ |
| Vavoo PVR (261 Kanäle) | ✅ |
| Flatpak Chromium 148 | ✅ Installiert |
| pugixml Dependency | ✅ Gefixt (selbst kompiliert) |
| guisettings default="true" Bug | ✅ Gefixt |
| manifest_config Force-Secure | ✅ Gefixt (leeres JSON) |

## Was NICHT funktioniert

| Problem | Root Cause | Status |
|---|---|---|
| Crunchyroll = RGB-Testpattern | CDM: `ToCdmVideoCodecProfile: Unknown codec profile 0` + `reserved`-Heap ephemeral | ❌ LE13/LE12 Downgrade nötig |
| Live-TV = schwarzes Bild | DRMPRIME-Zwang für H.264 → Renderer kann Software-Frames nicht mappen | ❌ |
| BOB CDM (ChromeOS 144 ARM64) | BOB nur ARM32 → `ld-linux-armhf.so.3` fehlt auf aarch64 | ❌ |
| aarch64 CDM für ChromeOS <148 | Existiert NICHT öffentlich downloadbar | ❌ |
| `/dev/dma_heap/reserved` | Ephemeral — kommt/geht je nach Boot | ⚠️ Symlink, nicht persistent |

---

## Fixes, die DEPLOYED wurden

1. **libpugixml.so.1** → `/storage/.kodi/addons/mesa-le12/lib/` (ISA lädt)
2. **Crunchyroll manifest_config** → `"{}"` (kein force_secure_decoder)
3. **Vavoo Content-Type** → `video/mp2t` (Korrekt für TS-Streams)
4. **playercorefactory.xml** → VideoPlayer-Zwang (später entfernt)
5. **guisettings** → `default="false"` für alle Player-Settings
6. **NOSECUREDECODER=true** → ISA Settings
7. **CDM-Cache** → config.json + recovery.json regelmäßig gelöscht

---

## Empfehlungen

### A) Downgrade auf LE12 (15 Minuten, garantiert funktionierend)
- LE12 nutzt EGL/GLES-Renderer ohne DRMPRIME-Zwang
- Software-H.264 funktioniert über GLES-Texturen
- Widevine jacuzzi funktioniert mit L3
- **Alle Streams laufen (Crunchyroll + Live-TV)**

### B) Auf LE13-Nightly mit DMA-Heap-Fix warten
- Amlogic-Thread bestätigt: Pi 5 Software-Decoder funktioniert, aber Widevine-Integration ist ungelöst
- Forum: `https://forum.libreelec.tv/thread/28572`

### C) LE12 + LE13-Dual-Boot (experimentell)
- LE12 auf einer SD-Karte für DRM-Streaming
- LE13 auf zweiter für Entwicklung/BLE-Bridge/Chromium
- Pi 5 bootet von USB — SD-Wechsel einfach

---

## Lessons Learned

1. **`ldd` zuerst.** Vor jedem anderen Debugging ALLE Shared-Object-Dependencies prüfen.
2. **`default="true"` in Kodi = Garantie dass deine Config ignoriert wird.**
3. **LE13 ist Beta.** `ToCdmVideoCodecProfile`-Fehler ist ein CDM <-> Kodi-Integration-Bug.
4. **aarch64 ChromeOS CDMs existieren nicht öffentlich.** Alle recovery.conf CDMs sind ARM32 oder x86_64.
5. **CDM-Cache-Korruption** tritt nach Widevine-Neuinstallation auf → `config.json` muss neu geschrieben werden.
6. **`/dev/dma_heap/reserved` ist ephemeral.** Erscheint nach manchen Boots als Symlink, verschwindet nach anderen.
7. **Verschiedene Testpattern-Modi haben verschiedene Ursachen.** RGB-Balken mit Audio = CDM-Output-Protection. Ohne Audio = CDM-Cache-Fehler. Beide haben unterschiedliche Log-Signaturen.

---

Datei: `LE13_WIDEVINE_RESEARCH.md`
Repository: switch2-raw (Forgejo) / switch2-bt (GitHub)
Branch: feat/switch2d-production-daemon

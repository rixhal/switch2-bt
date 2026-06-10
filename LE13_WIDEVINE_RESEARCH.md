# LE13/Widevine/Pi5 — Root Cause Analyse

**Datum:** 2026-06-10  
**Forschungssession:** ~4h Debugging  
**Betroffenes System:** crackberry5 (Raspberry Pi 5, LE13 Nightly 2026-06-10, Kernel 6.18.32)

## Zusammenfassung

Crunchyroll (Widevine DRM) zeigt RGB-Testpattern statt Video. Live-TV (Vavoo PVR) zeigt schwarzes Bild. Nach 4h systematischer Analyse: **Das Problem ist eine Kombination aus drei Faktoren, nicht lösbar auf aktuellem LE13-Build.**

---

## Gefundene Root Causes

### 1. ❌ `libpugixml.so.1` fehlt (STUNDENLANG unentdeckt!)

InputStream Adaptive 22.3.14 braucht `libpugixml.so.1` als Shared-Object-Dependency. LE13 kompiliert ISA mit pugixml, aber die Library wird **nicht mitinstalliert**. Kodi-Log zeigte:

```
ERROR: Unable to load /storage/.kodi/addons/inputstream.adaptive/inputstream.adaptive.so.22.3.14, 
reason: libpugixml.so.1: cannot open shared object file: No such file or directory
```

**=> JEDER Crunchyroll/Live-TV Start = "Allgemeiner Fehler" oder "Addon kann nicht geladen werden"**

**Fix:** pugixml v1.14 aus Source kompiliert (g++ auf Pi-Host), deployt nach `/storage/.kodi/addons/mesa-le12/lib/` (im LD_LIBRARY_PATH).

**Lehre:** Systematisch `ldd` auf ALLE `.so`-Dateien prüfen vor jeglichem Playback-Debugging.

### 2. ⚠️ Widevine CDM Output Protection

Der genutzte CDM (`jacuzzi`, ChromeOS 148, Mediatek MT8183) verlangt `force_secure_decoder` für Output Protection. Pi 5 hat **keinen hardware-secure-decoder** für H.264 (nur HEVC).

**Versuchte CDM-Wechsel:**
- `bob` (Rockchip RK3399, ChromeOS 144, Widevine 4.10.2662.3): **ARM32-only** → `ld-linux-armhf.so.3` fehlt auf aarch64 → crasht
- `jacuzzi` (Mediatek MT8183, ChromeOS 148, Widevine 16640.57.0): **ARM32** aber via libc6-armhf-compat ausführbar → strikte Output Protection

**Konfiguration jacuzzi:**
```json
{"version": "16640.57.0", "chrome_version": "148.0.7778.250", "boardname": "jacuzzi", "arch": "arm"}
```

### 3. ❌ `CONFIG_DMABUF_HEAPS_RESERVED` nicht im Kernel

Kernel-Log:
```
CDMAHeapBufferObject::Register unable to open /dev/dma_heap/reserved: No such file or directory
CDMAHeapBufferObject::Register - using /dev/dma_heap/linux,cma
```

- `CONFIG_DMABUF_HEAPS_RESERVED` ist ein **Raspberry Pi Downstream-Patch**, nicht im LE13 Mainline-Kernel
- Fallback `linux,cma` (512 MB) funktioniert für Widevine L3 (Software-Decode)
- **Widevine L1 (Hardware-Secure-Decode) ist OHNE diesen Patch UNMÖGLICH**

Das erklärt, warum `useprimedecoder=true` + Software H.264 = Testpattern: CDM verlangt Secure-Decoder, bekommt Software-Decoder, verweigert Output.

### 4. ⚠️ Kodi Settings: `default="true"` ignoriert Custom-Werte

```xml
<setting id="videoplayer.useprimerenderer" default="true">0</setting>
```

Wenn `default="true"` in guisettings.xml steht, **ignoriert Kodi den Value** und nutzt den kompilierten Plattform-Default. Für RPi5 ist das **DRMPRIME (RendererDRMPRIMEGLES)**.

**Fix:** `default="false"` setzen, dann wird der Value respektiert:
```xml
<setting id="videoplayer.useprimerenderer" default="false">0</setting>
```

Playercorefactory.xml wurde erfolgreich deployt, advancedsettings.xml `<video><useprimerenderer>` Schema funktioniert auf LE13/RPi5 **nicht**.

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
| DRMPRIME-Renderer | ✅ Lädt (aber falsch konfigurierbar) |

## Was NICHT funktioniert

| Problem | Grund | Lösbar? |
|---|---|---|
| Crunchyroll = RGB-Testpattern | Widevine jacuzzi verlangt Secure-Decoder → Pi 5 hat keinen für H.264 | ❌ Nicht ohne LE13-Update |
| Live-TV = schwarzes Bild | Software-H.264 → DRMPRIME-Buffer → Renderer kann nicht rendern | ❌ Nicht ohne Kernel-Patch |
| BOB CDM (ARM64-alt) | BOB ist ARM32-only → kein `ld-linux-armhf.so.3` | ❌ aarch64 braucht aarch64-CDM |
| CDM-Austausch | ChromeOS 144 CDMs sind alle ARM32/x86 → kein aarch64 | ❌ aarch64-CDM existiert nur in ChromeOS 148+ |
| `dma_heap/reserved` | Feature-Patch nicht im Mainline-Kernel | ❌ Kann man patchen, aber braucht Kernel-Rebuild |

---

## Fixes, die DEPLOYED wurden (aber das Grundproblem nicht lösen)

1. **libpugixml.so.1** → `mesa-le12/lib/` (ISA lädt jetzt)  
2. **Crunchyroll manifest_config** → `"{}"` (kein force_secure_decoder)  
3. **Vavoo Content-Type** → `video/mp2t` (Korrekt für TS-Streams)  
4. **playercorefactory.xml** → VideoPlayer-Zwang  
5. **guisettings** → `default="false"` für useprimedecoder/usemediacodec  
6. **NOSECUREDECODER=true** → ISA Settings  

---

## Empfehlungen

### A) Downgrade auf LE12 (15 Minuten, garantiert funktionierend)
- LE12 nutzt EGL/GLES-Renderer ohne DRMPRIME-Zwang
- Software-H.264 funktioniert über GLES-Texturen
- Widevine jacuzzi funktioniert mit L3
- **Alle Streams laufen (Crunchyroll + Live-TV)**

### B) Auf LE13-Update warten
- Aktuelles Nightly: 2025-11-17 (test.libreelec.tv)
- Unser Build: 2026-06-10 (Pi-dev)
- **Kein neuerer Build verfügbar mit DMA-Heap-Patch**
- Forum-Thread: `https://forum.libreelec.tv/thread/28572` — keine ETA für Pi5-Widevine-Fix

### C) Kernel selbst patchen (2-4 Stunden)
1. `CONFIG_DMABUF_HEAPS_RESERVED=y` in Kernel-Config
2. `CONFIG_DMABUF_HEAPS_CMA=y`  
3. LE13 Image neu bauen
4. Testen ob `/dev/dma_heap/reserved` erscheint
5. Wenn ja: Widevine jacuzzi könnte L1 machen → kein Testpattern mehr

---

## Lessons Learned

1. **`ldd` zuerst.** Vor jedem anderen Debugging ALLE Shared-Object-Dependencies prüfen.
2. **`default="true"` in Kodi = Garantie dass deine Config ignoriert wird.**
3. **LE13 ist Beta.** Nightlies von Okt/Nov 2025 sind quasi-Frozen. Unser Jun-2026 Build ist neuer.
4. **Pi 5 hat H.264-Software-Decode — aber nicht über DRMPRIME.** Das ist der fundamentale Architekturkonflikt.
5. **aarch64 ChromeOS CDMs existieren nicht öffentlich downloadbar.** Alle recovery.conf CDMs sind ARM32 oder x86_64.

---

Datei: `LE13_WIDEVINE_RESEARCH.md`  
Repository: switch2-bt  
Branch: feat/switch2d-production-daemon

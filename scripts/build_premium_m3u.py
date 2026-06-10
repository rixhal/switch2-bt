#!/usr/bin/env python3
"""Build combined M3U: Premium (dndnscloud filtered) + ÖR (josxha filtered)."""
import re, urllib.request

# ── Premium from dndnscloud (via Vavoo proxy, filtered) ──
KEEPERS = {
    "SKY CINEMA":     "SKY CINEMA PREMIERE",
    "SKY SPORT":      "SKY SPORT F1",
    "SKY ATLANTIC":   "SKY ATLANTIC",
    "SKY NATURE":     "SKY NATURE",
    "SKY DOCUMENTARIES": "SKY DOCUMENTARIES",
    "AXN":            "AXN",
    "WARNER TV":      "WARNER TV FILM",
    "DELUXE":         "DELUXE MUSIC",
    "NAT GEO":        "NAT GEO",
    "EUROSPORT":      "EUROSPORT 1",
    "DISCOVERY":      "DISCOVERY CHANNEL",
}
DROP = {"SILVERLINE", "LEIPZIG FERNSEHEN", "PARLAMENTSFERNSEHEN 3",
        "TOGGO PLUS", "BOOMERANG", "CARTOON NETWORK", "DELUXE DANCE",
        "DELUXE RAP", "MTV"}

def filter_premium(raw):
    entries = []
    current = None
    for line in raw.split("\n"):
        if line.startswith("#EXTINF"):
            current = line
        elif line.startswith("http") and current:
            entries.append((current, line))
            current = None
    result = []
    for extinf, url in entries:
        m = re.search(r'tvg-name="([^"]*)"', extinf)
        name = m.group(1) if m else ""
        if name in DROP:
            continue
        keep = True
        for prefix, keeper in KEEPERS.items():
            if name.upper().startswith(prefix) and name.upper() != keeper.upper():
                keep = False
                break
        if not keep:
            continue
        extinf = re.sub(r'group-title="[^"]*"', 'group-title="Premium"', extinf)
        result.append((extinf, url))
    return result

# ── ÖR from josxha/german-tv-m3u (filter: only national + news) ──
OER_KEEP = {
    "Das Erste HD", "ZDF HD", "3sat", "ARTE HD", "phoenix HD", "phoenix",
    "tagesschau24", "ARD-alpha", "one HD", "ZDFneo HD", "ZDFinfo HD",
    "KiKA", "WELT", "DF1 HD", "Dokusat", "ANIXE HD",
}

def filter_oer(raw):
    entries = []
    current = None
    for line in raw.split("\n"):
        if line.startswith("#EXTINF"):
            current = line
        elif line.startswith("http") and current:
            entries.append((current, line))
            current = None
    result = []
    for extinf, url in entries:
        m = re.search(r'tvg-name="([^"]*)"', extinf)
        name = m.group(1) if m else ""
        if name not in OER_KEEP:
            continue
        # Normalize group-title
        extinf = re.sub(r'group-title="[^"]*"', 'group-title="Öffentlich-Rechtliche"', extinf)
        result.append((extinf, url))
    return result

# Fetch both
with urllib.request.urlopen("http://127.0.0.1:37890/playlist.m3u") as f:
    premium_raw = f.read().decode()
with urllib.request.urlopen("https://raw.githubusercontent.com/josxha/german-tv-m3u/main/german-tv.m3u") as f:
    oer_raw = f.read().decode()

premium = filter_premium(premium_raw)
oer = filter_oer(oer_raw)

# Output
print('#EXTM3U')
print('#KODIPROP:inputstream=inputstream.ffmpegdirect')
print('#KODIPROP:inputstream.ffmpegdirect.manifest_type=hls')
print('#KODIPROP:inputstream.ffmpegdirect.is_realtime_stream=true')
print('#KODIPROP:inputstream.ffmpegdirect.playback_as_live=true')

for extinf, url in oer + premium:
    print(extinf)
    print(url)

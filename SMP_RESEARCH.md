# 2026-06-04 — BTstack SMP Encryption: Switch 2 Pro Controller

## Ergebnis
Die SMP-Pairing-Sequenz mit dem Nintendo Switch 2 Pro Controller läuft korrekt an (SM_EVENT_PAIRING_STARTED + Just Works Confirm), aber SM_EVENT_PAIRING_COMPLETE und HCI_EVENT_ENCRYPTION_CHANGE werden **nie** ausgelöst. Ursachenanalyse ergibt mehrere mögliche Root Causes, wobei `SM_AUTHREQ_NO_BONDING` in Kombination mit Secure Connections der wahrscheinlichste Kandidat ist. Der Controller scheint Bonding und/oder MITM zu verlangen — dies ist konsistent mit dem Verhalten anderer Nintendo-HID-Controller.

## Quellen
- Perplexity/OpenRouter Deep Research (primär)
- [BTstack sm_pairing_peripheral.c — Official Example](https://github.com/bluekitchen/btstack/blob/master/example/sm_pairing_peripheral.c)
- [BTstack sm.h — Security Manager API Header](https://github.com/bluekitchen/btstack/blob/master/src/ble/sm.h)
- [BTstack hci.c — Event Handler Dispatch](https://github.com/bluekitchen/btstack/blob/master/src/hci.c)
- [BlueKitchen BTstack Manual — Embedded Examples](https://bluekitchen-gmbh.com/btstack/examples/generated/)
- [BTstack CHANGELOG.md](https://github.com/bluekitchen/btstack/blob/master/CHANGELOG.md)

## Relevante Fakten

### 1. HCI_EVENT_ENCRYPTION_CHANGE Routing
- **HCI_EVENT_ENCRYPTION_CHANGE** wird über `hci_emit_event` an ALLE registrierten HCI-Handler dispatched — NICHT über `sm_add_event_handler`
- SM hat einen internen Handler, der via `hci_add_event_handler_for_security_manager()` am **KOPF** der Event-Queue registriert wird (verarbeitet Events als ERSTER)
- User-Registrierte Handler (`hci_add_event_handler`) kommen am TAIL → sehen Events NACH SM
- `sm_add_event_handler` registriert Handler für SM-generierte Events (SM_EVENT_PAIRING_STARTED, SM_EVENT_PAIRING_COMPLETE etc.) — das sind KEINE HCI-Events
- **Fazit**: HCI_EVENT_ENCRYPTION_CHANGE muss sowohl im SM-internen als auch im User-HCI-Handler beobachtbar sein. Wenn es nie auftaucht: der Controller sendet es nicht → Pairing hat nicht bis zur Encryption-Phase abgeschlossen.

### 2. BTstack SM API — Kritische Konfigurations-Optionen
- `sm_set_request_security(bool enable)` — "Let Peripheral request an encrypted connection right after connecting" — **potenziell entscheidend!** Wird im Beispiel NICHT gesetzt.
- `sm_set_accepted_stk_generation_methods(uint8_t)` — "Bonding is stopped if the resulting one isn't in the list" — wenn Methoden nicht akzeptiert werden, bricht Pairing ab
- `sm_set_encryption_key_size_range(min, max)` — Default 16/16. Eventuell zu restriktiv für bestimmte Controller?
- `sm_allow_ltk_reconstruction_without_le_device_db_entry(int allow)` — Default 1. Relevant für Re-Encryption ohne Bonding-DB
- `gatt_client_set_required_security_level(LEVEL_2)` — im Beispiel auskommentiert, erzwingt Authentifizierung bei GATT-Zugriffen

### 3. Pairing-Konfigurationen aus dem BTstack-Beispiel
```
// LE Secure Connections, Just Works (empfohlen für Test):
sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION);

// Mit Bonding:
sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);

// Legacy Just Works:
sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
sm_set_authentication_requirements(0);  // <-- KEIN SC, KEIN BONDING
```

**Wichtig**: Das Beispiel verwendet `SM_AUTHREQ_SECURE_CONNECTION` (nicht `SM_AUTHREQ_NO_BONDING`!). `SM_AUTHREQ_NO_BONDING` = 0x00, was Legacy Just Works ohne SC entspricht. Mit SC muss der Auth-Flag `SM_AUTHREQ_SECURE_CONNECTION` (0x08) sein.

### 4. Event-Flow bei korrektem Pairing
1. GAP_SUBEVENT_LE_CONNECTION_COMPLETE (im HCI-Handler)
2. `sm_request_pairing(con_handle)` → SM_EVENT_PAIRING_STARTED (im SM-Handler)
3. SM_EVENT_JUST_WORKS_REQUEST → `sm_just_works_confirm(con_handle)`
4. SMP Key Exchange (intern)
5. SM_EVENT_PAIRING_COMPLETE mit status SUCCESS (im SM-Handler)
6. HCI_EVENT_ENCRYPTION_CHANGE (im HCI-Handler)
7. GATT-Zugriffe jetzt möglich (encrypted)

## Fallstricke (wichtig!)
- **NO_BONDING + SC-Kombination**: `SM_AUTHREQ_NO_BONDING` (0x00) ist Legacy ohne SC. Mit SC muss mindestens `SM_AUTHREQ_SECURE_CONNECTION` (0x08) gesetzt sein. Kombination `SM_AUTHREQ_SECURE_CONNECTION` OHNE `SM_AUTHREQ_BONDING` ist möglich aber ungewöhnlich.
- **Switch 2 Controller Bonding-Requirement**: Nintendo Controller (Switch 1) erfordern typischerweise BONDING mit LTK-Speicherung für Re-Connects. Switch 2 Controller könnte strikter sein und NO_BONDING komplett ablehnen.
- **Key Distribution Flags**: Key-Dist=0x02 (EncKey only) sagt dem Controller "ich will nur den Encryption Key, speichere keine Identity". Der Controller könnte darauf bestehen, Identity Key zu verteilen (0x01 = IdKey, 0x07 = alle). Ohne IdKey-Distribution kein Bonding → kein Reconnect möglich.
- **sm_request_pairing vs. Auto-Pairing**: `sm_request_pairing` ist explizit und erzwingt Pairing sofort. Auto-Pairing via GATT-Zugriff (z.B. `gatt_client_discover_primary_services`) triggert Pairing erst wenn nötig. Bei Write Not Permitted: Pairing wurde nicht automatisch getriggert → Pairing war nicht erfolgreich.
- **HCI- vs SM-Handler Trennung**: Wenn `HCI_EVENT_ENCRYPTION_CHANGE` im falschen Handler gesucht wird (SM statt HCI), wird es nie gefunden. Beide Handler müssen korrekt registriert sein.
- **Event-Handler-Ordering**: SM's interner Handler kommt VOR User-Handlern in der Dispatch-Queue. Wenn SM den Encryption-Change intern konsumiert und keinen SM_EVENT_PAIRING_COMPLETE generiert → Bug im Flow.

## Dead Ends Confirmed
- **Ohne SC**: Authentication Failure (0x05) — Controller akzeptiert kein Legacy Pairing → SC MUSS an sein
- **Nur HCI-Events im SM-Handler suchen**: Wird nie funktionieren — SM-Handler bekommt nur SM-generierte Events, nicht rohe HCI-Events
- **web_search für "Switch 2 Pro Controller BLE GATT"**: Keine verwertbaren Ergebnisse — Gerät zu neu, keine öffentliche Reverse-Engineering-Doku

## Beeinflusste Komponenten
- `switch2-bridge` — BTstack BLE Daemon (vermutlich in ~/projects/ oder ~/src/)
- `btstack` — Security Manager Konfiguration
- `knowledgebase/research/btstack-smp-encryption-switch2.md` — dieser Report

## Next Action (höchste Priorität)

### 🔥 PRIMÄR: SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING
```
// Statt SM_AUTHREQ_NO_BONDING:
sm_set_authentication_requirements(
    SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING
);
sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
```
Und im HCI-Connection-Complete-Handler ZUSÄTZLICH zum SM-Handler:
```c
case HCI_EVENT_ENCRYPTION_CHANGE:
    printf("ENCRYPTION CHANGE: status=0x%02x, handle=0x%04x\n", 
           packet[2], little_endian_read_16(packet, 3));
    break;
```

### Sekundär (falls primär nicht hilft):
1. `sm_set_request_security(true)` setzen — zwingt Peripheral zum Security Request nach Connect
2. Key-Dist auf 0x07 (EncKey + IdKey + SignKey) statt 0x02
3. `sm_set_encryption_key_size_range(7, 16)` — Key-Size-Range lockern
4. `gatt_client_set_required_security_level(LEVEL_2)` — GATT-Security-Level setzen

## All URLs for Manual Review
1. https://github.com/bluekitchen/btstack/blob/master/example/sm_pairing_peripheral.c
2. https://github.com/bluekitchen/btstack/blob/master/src/ble/sm.h
3. https://github.com/bluekitchen/btstack/blob/master/src/hci.c
4. https://github.com/bluekitchen/btstack/blob/master/README.md
5. https://github.com/bluekitchen/btstack/blob/master/CHANGELOG.md
6. https://bluekitchen-gmbh.com/btstack/examples/generated/
7. https://stackoverflow.com/questions/62147384/raspberry-ble-encryption-pairing
8. https://stackoverflow.com/questions/78767932/what-smp-packet-is-missing-here-for-the-ble-pairing-to-finish-successfully

## How This Was Built
- 6 SearXNG-Queries (search.richie.fyi) → 404 (nginx proxy down, /search Endpunkt nicht erreichbar)
- Fallback: 12 web_search-Queries → wenig verwertbare Ergebnisse (Switch 2 zu neu, BTstack-Spezifika schwer zu crawlen)
- Primär: 1 Perplexity/OpenRouter Deep Research Query → detaillierte Analyse
- Sekundär: Direktes Abrufen von 3 BTstack-Quelldateien von raw.githubusercontent.com → API-Doku, Event-Routing

## Research Method Effectiveness
- **Perplexity/OpenRouter**: ★★★★★ (5/5) — Lieferte detaillierte technische Analyse mit Architektur-Insights
- **web_search**: ★☆☆☆☆ (1/5) — Surface-level, kein BTstack/Switch-2-spezifischer Inhalt
- **SearXNG**: ★☆☆☆☆ (0/5) — Instanz down (nginx 404 auf /search)
- **GitHub Raw Source**: ★★★★☆ (4/5) — Direkter Zugriff auf sm.h, hci.c, sm_pairing_peripheral.c — sehr wertvoll für API-Verständnis

# Phase B Log — GATT Discovery & SMP Encryption

## Durchbruch: UUID-gefilterte GATT Discovery (Session 03.06.2026)

### Entdeckte Characteristics (btmon-verifiziert)
- **Input Report:** `ab7de9be-89fe-49ad-828f-118f09df7fd2` → Handle `0x000a` (Notify)
- **Command Write:** `649d4ac9-8eb7-4e6c-af44-1ea54fe5f005` → Handle `0x0014` (Write Without Response)

### GATT-Discovery-Erkenntnisse

| Versuch | API | Ergebnis |
|---------|-----|----------|
| `gatt_client_discover_characteristics_for_handle_range_by_uuid16` | uuid16 | Findet NICHTS — uuid16-Feld immer 0 bei 128-bit UUIDs |
| `gatt_client_discover_characteristics_for_handle_range_by_uuid128` mit `filter_with_uuid=true` | uuid128+Filter | Blockiert ALLE Results |
| `gatt_client_discover_characteristics_for_handle_range_by_uuid128` OHNE filter, 2-Phasen | uuid128 | ✅ Funktioniert! |

**Lösung:** Zwei sequentielle `gatt_client_discover_characteristics_for_handle_range_by_uuid128`-Aufrufe:
1. Phase 1: Input Report UUID → Handle `0x000a`
2. Phase 2: Command Write UUID → Handle `0x0014`

Kein `filter_with_uuid` setzen — der Filter blockiert auch valide Matches.

### SMP / Pairing-Erkenntnisse

1. **SM-Events kommen NIE im Haupt-`packet_handler` an** — müssen über `sm_add_event_handler()` mit separatem Handler registriert werden
2. **Ohne Secure Connections** → `0x05` Authentication Failure
3. **Mit Secure Connections** → Pairing läuft durch, Controller akzeptiert Legacy Just Works (NoInputNoOutput, No Bonding)
4. **Key Distribution:** Controller will `none`, Host sendet `IdKey` → Pairing hängt → `SM_AUTHREQ_NO_BONDING` + Key-Dist auf `0x02` reduziert

### Encryption: Aktueller Blocker

- Pair-Command gesendet → Controller antwortet `0x03` (Write Not Permitted)
- **Root Cause:** Controller verlangt verschlüsselten Link für Pair-Commands
- SMP-Encryption muss vor GATT-Characteristic-Writes abgeschlossen sein

### Stabile Betriebs-Sequenz

```bash
killall -9 switch2_btstack_bridge bluetoothd
hciconfig hci1 down
/storage/switch2-bt/switch2_btstack_bridge -u 1
```

`hciconfig hci1 down` VOR Bridge-Start ist **zwingend** — BTstack bindet das Interface selbst, ein aktives hci1 blockiert den Raw-HCI-Socket.

### Controller Specs

- **BDADDR:** `E0:EF:BF:3B:C6:76` (Nintendo, public)
- **reconnect_mac:** `00:00:00:00:00:00` (kein Bonding)
- **GATT-Services erst nach erfolgreichem SMP-Pairing sichtbar**
- **ATT_MTU:** wird auf 65 gesetzt (`0xAB` Event)

### Referenzen

- CareyScott: Command Write `649d4ac9-8eb7-4e6c-af44-1ea54fe5f005`, Input Notify `ab7de9be-89fe-49ad-828f-118f09df7fd2`
- GitHub: `https://github.com/rixhal/switch2-bt`

### Nächster Schritt

SMP-Encryption abschließen → Pair-Commands senden → Notifications abonnieren → Input Reports parsen → UHID-Gamepad

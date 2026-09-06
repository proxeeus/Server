# Trilogy Client Support — EQEmu Implementation Plan

**Client Version:** EverQuest Trilogy (v29c/v30), build date "8-09-2001 14:25"  
**Primary Reference:** `c:\eqemu\source\EQClassic\` (fully functional Trilogy-compatible server)  
**Template:** `common/patches/titanium.*` (oldest supported EQEmu client)  
**Status:** Planning phase — no code written yet

---

## Verified File Paths

| Resource | Path |
|---|---|
| EQClassic opcodes | `EQClassic/Common/Include/eq_opcodes.h` |
| EQClassic packet structs | `EQClassic/Common/Include/eq_packet_structs.h` |
| EQClassic PlayerProfile | `EQClassic/Common/Include/PlayerProfile.h` (8104 bytes) |
| EQClassic login structs | `EQClassic/LS/Login/login_structs.h` |
| EQClassic login opcodes | `EQClassic/LS/Login/login_opcodes.h` |
| EQClassic DES crypto | `EQClassic/LS/Login/EQCrypto.h/.cpp` |
| EQClassic world client | `EQClassic/World/Source/client_process.cpp` |
| EQEmu version enum | `Server/common/emu_versions.h` |
| EQEmu patch template | `Server/common/patches/titanium.*` |
| EQEmu CMake | `Server/common/CMakeLists.txt` |
| EQEmu patches registration | `Server/common/patches/patches.cpp` |

---

## Critical Protocol Differences: Trilogy vs Titanium

| Aspect | Trilogy (EQClassic) | Titanium (EQEmu baseline) |
|---|---|---|
| `LoginInfo_Struct` | 40-byte crypt block | 488 bytes |
| `PlayerProfile_Struct` size | 8104 bytes | Larger (adds AAs, LDoN, etc.) |
| Spell book slots | 256 | 400 |
| Memorized spell slots | 8 | 8 (same) |
| Visible buff count | 15 | 25 |
| Inventory slots | 30 | 22 (worn) + 8 (main) |
| Container (bag) item slots | 80 total (8 bags × 10) | Different layout |
| Bank slots | 8 | 16 |
| Cursor bag slots | 10 | Different |
| Group size | 6 | 6 (same) |
| `CharacterSelect_Struct` size | 1248 bytes | 1704 bytes |
| `ServerZoneEntry_Struct` size | 312 bytes | Different |
| `Spawn_Struct` size | 164 bytes | 383 bytes |
| AA points | None | Present |
| LDoN points | None | Present |
| Augmentation system | None | Present |
| Login encryption | DES (3DES, Verant key) | Simpler XOR |
| Expansion bits | Trilogy = RoK+SoV+SoL (0x07) | Higher |
| Client version string | "8-09-2001 14:25" | "Oct 31 2005 10:33:37" |
| Zone entry opcode | 0x2a20 | Different hex mapping |
| LoginInfo world opcode | 0x5818 (`WS_SEND_LOGIN_INFO`) | Same internal `OP_SendLoginInfo` |
| EnterWorld opcode | 0x0180 | 0x4D20 |

---

## Phase 1: Environment Discovery & Core Definitions

### Task 1.1 — Confirm Reference Material (DONE — See above table)

All file paths verified. EQClassic is the sole reference.

### Task 1.2 — Add `ClientVersion::Trilogy` to `emu_versions.h`

**File:** `Server/common/emu_versions.h`

Insert `Trilogy` before `Client62` in both the `ClientVersion` enum and the `MobVersion` enum. Trilogy pre-dates Client62 (2005) by ~4 years.

```cpp
// In enum class ClientVersion : uint32
Trilogy,   // Build: '8-09-2001 14:25'  (ADD — index 1, shift others up)
Client62,  // Build: 'Aug  4 2005 15:40:59'  (was 1, now 2)
Titanium,  // Build: 'Oct 31 2005 10:33:37'  (was 2, now 3)
// ... rest shift by 1
```

Also update:
- `ClientVersionBitmask`: add `bitTrilogy = 0x00000001`, shift existing bits left
- `maskTitaniumAndEarlier` → include Trilogy
- `ClientVersionCount` automatically adjusts via `LastClientVersion`
- `MobVersion` enum: add `Trilogy` and `OfflineTrilogy` entries
- `UCSVersion` enum: add `ucsTrilogyChat` / `ucsTrilogyMail` chars

**Impact of shifting enum values:** Any code that stores/compares raw integer values of `ClientVersion` (e.g. in database `client_version` columns) will need a migration. Search with `grep -r "ClientVersion::" --include="*.cpp"` and audit all switch statements.

### Task 1.3 — Register Trilogy in the Strategy Selector

**Files to update:**
- `Server/common/patches/patches.cpp` — add `Trilogy::Register(into)` and `Trilogy::Reload()` **before** Titanium calls (register in ascending client version order)
- `Server/common/patches/patches.h` — no change needed (generic interface)
- `Server/common/CMakeLists.txt` — add `patches/trilogy.cpp`, `patches/trilogy_limits.cpp` to source list and all four `.h` files to headers list

The `Trilogy::Register()` call must precede `Titanium::Register()` so the stream identifier can distinguish the two protocols from the first packet's size and opcode.

---

## Phase 2: Network & Stream Layer (The "Handshake")

### Task 2.1 — Analyze the Trilogy Session Protocol

**Key finding from EQClassic:** The `APPLAYER` packet container used by EQClassic is:
```c
struct APPLAYER {
    int32  size;     // payload size
    int16  opcode;   // operation code
    uchar* pBuffer;  // packet data
};
```
This is functionally equivalent to EQEmu's `EQApplicationPacket`. The Daybreak protocol layer (`OP_SessionRequest`, `OP_SessionResponse`, `OP_Ack`) appears to be the same wire-level encoding.

**CRC:** EQClassic uses `EQPacket::GenerateCRC()` with a private lookup table. EQEmu's `crc32.h` uses a similar table. The CRC32 algorithm must be compared byte-for-byte.

**Encryption:**
- Login server password path: DES (OpenSSL `DES_cbc_encrypt` with Verant key) — used only for the `LoginInfo_Struct` credential block.
- In-zone packet encryption: Same XOR-based stream encoding as EQEmu (`FLAG_ENCODED = 0x04`). No special zone-level crypto.
- Conclusion: the EQStream layer itself does **not** need a new crypto path; only the login credential parsing needs to handle the 40-byte DES crypt block.

### Task 2.2 — Session Signature Identification

EQEmu identifies a connecting client by the **size and opcode of the first packet** sent after the Daybreak `OP_SessionRequest` exchange completes. For Titanium, that first application packet is:
- Opcode: `OP_SendLoginInfo` (world) or `OP_ZoneEntry` (zone)
- Size: `sizeof(structs::LoginInfo_Struct)` = 488 bytes (world) or `sizeof(structs::ClientZoneEntry_Struct)` (zone)

For Trilogy, the equivalent first packets are:
- **World:** opcode `0x5818` (`WS_SEND_LOGIN_INFO`), size = `sizeof(Trilogy::structs::LoginInfo_Struct)` = 40 bytes + variable tail
- **Zone:** opcode `0x2a20` (`OP_ZoneEntry`), size = `sizeof(Trilogy::structs::ClientZoneEntry_Struct)`

These sizes **must not collide** with Titanium's sizes or the identifier will misclassify clients. Verify with static assertions after structs are written.

### Task 2.3 — Compression/Encoding Toggles

EQClassic's `packet_functions.h` provides `DeflatePacket()` / `InflatePacket()` (zlib) and `EncryptSpawnPacket()` / `EncryptZoneSpawnPacket()` (XOR). Trilogy clients support:
- Zlib compression (flag `0x01`) — same as later clients; no change needed
- XOR encoding (flag `0x04`) — same encoding key derivation; no change needed

**Action:** No changes to `EQStream`. The session-level handshake is identical. Only the application-layer opcodes and struct layouts differ.

---

## Phase 3: Opcode & Struct Foundations

### Task 3.1 — Create `common/patches/trilogy_ops.h`

This file lists all `E()` and `D()` macro pairs for opcodes that need encoding or decoding. Use `titanium_ops.h` as the format template.

**Minimum initial set (Phase 3 scope):**

Encodes (server → Trilogy client):
```
E(OP_SendCharInfo)       // CharacterSelect_Struct
E(OP_PlayerProfile)      // PlayerProfile_Struct (8104 bytes)
E(OP_ZoneEntry)          // ServerZoneEntry_Struct
E(OP_NewZone)            // NewZone_Struct
E(OP_NewSpawn)           // Spawn_Struct
E(OP_ZoneSpawns)         // array of Spawn_Struct
E(OP_DeleteSpawn)        // EntityId_Struct
E(OP_SpawnAppearance)    // SpawnAppearance_Struct
E(OP_ChannelMessage)     // ChannelMessage_Struct
E(OP_Damage)             // CombatDamage_Struct
E(OP_Action)             // Action_Struct
E(OP_Death)              // Death_Struct
E(OP_ManaChange)         // ManaChange_Struct
E(OP_HPUpdate)           // HPUpdate_Struct
E(OP_Buff)               // SpellBuff_Struct
E(OP_WearChange)         // WearChange_Struct
E(OP_ClientUpdate)       // PlayerPositionUpdateServer_Struct
E(OP_MemorizeSpell)      // MemorizeSpell_Struct
E(OP_CastOn)             // CastOn_Struct
E(OP_MobUpdate)          // MobUpdate_Struct
```

Decodes (Trilogy client → server):
```
D(OP_ZoneEntry)          // ClientZoneEntry_Struct
D(OP_ClientUpdate)       // PlayerPositionUpdateClient_Struct
D(OP_CastSpell)          // CastSpell_Struct
D(OP_ChannelMessage)     // ChannelMessage_Struct
D(OP_Attack)             // Attack_Struct
D(OP_MoveItem)           // MoveItem_Struct
D(OP_CharacterCreate)    // CharCreate_Struct
D(OP_DeleteCharacter)    // DeleteCharacter_Struct
D(OP_WearChange)         // WearChange_Struct
D(OP_MemorizeSpell)      // MemorizeSpell_Struct
D(OP_LoadSpellSet)       // LoadSpellSet_Struct
D(OP_BeginCast)          // BeginCast_Struct
D(OP_ConsumeFoodDrink)   // Consume_Struct
D(OP_SetServerFilter)    // SetServerFilter_Struct
D(OP_WhoAllRequest)      // WhoAllRequest_Struct
D(OP_TradeSkillCombine)  // TradeSkillCombine_Struct
```

**Opcode configuration file** (`patch_trilogy.conf`): Must map all internal `OP_` names to Trilogy hex values from `eq_opcodes.h`. This `.conf` file lives in the `patches/` runtime directory (same location as `patch_titanium.conf`).

Key mappings from EQClassic (internal name → Trilogy hex):
```
OP_SendLoginInfo    = 0x5818   # WS_SEND_LOGIN_INFO
OP_SendCharInfo     = 0x4720   # OP_SendCharInfo
OP_PlayerProfile    = 0x2d20   # OP_PlayerProfile
OP_ZoneEntry        = 0x2a20   # OP_ZoneEntry
OP_NewSpawn         = 0x4921   # OP_NewSpawn
OP_ZoneSpawns       = 0x6121   # OP_ZoneSpawns
OP_DeleteSpawn      = 0x2b20   # OP_DeleteSpawn
OP_NewZone          = 0x5b20   # OP_NewZone
OP_Death            = 0x4a20   # OP_Death
OP_MobUpdate        = 0xa120   # OP_MobUpdate
OP_ZoneChange       = 0xa320   # OP_ZoneChange
OP_ClientUpdate     = 0xf320   # OP_ClientUpdate
OP_SpawnAppearance  = 0xf520   # OP_SPawnAppearance (typo preserved from EQClassic)
OP_WearChange       = 0x9220   # OP_WearChange
OP_ChannelMessage   = 0x0721   # OP_ChannelMessage
OP_Action           = 0x5820   # OP_Action
OP_BeginCast        = 0xa920   # OP_BeginCast
OP_CastSpell        = 0x7e21   # OP_CastSpell
OP_Buff             = 0x3221   # OP_Buff
OP_ManaChange       = 0x7f21   # OP_ManaChange
OP_MemorizeSpell    = 0x8221   # OP_MemorizeSpell
OP_MoveItem         = 0x2c21   # OP_MoveItem
OP_AutoAttack       = 0x5121   # OP_AutoAttack
OP_Attack           = 0x9f20   # OP_Attack
OP_HPUpdate         = 0xb220   # OP_HPUpdate
OP_ExpUpdate        = 0x9921   # OP_ExpUpdate
OP_SkillUpdate      = 0x8921   # OP_SkillUpdate
OP_LevelUpdate      = 0x9821   # OP_LevelUpdate
OP_CharacterCreate  = 0x4920   # OP_CharacterCreate
OP_DeleteCharacter  = 0x5a20   # OP_DeleteCharacter
OP_NAMEAPPROVAL     = 0x8B20   # OP_NAMEAPPROVAL
OP_ENTERWORLD       = 0x0180   # OP_ENTERWORLD
OP_ShopRequest      = 0x0b20
OP_ShopPlayerBuy    = 0x2720
OP_ShopPlayerSell   = 0x2820
OP_LootItem         = 0xA020
OP_LootRequest      = 0x4e20
OP_LootComplete     = 0x4421
OP_DropItem         = 0x3520
OP_PickupItem       = 0x3620
OP_TradeSkillCombine = 0x0521
OP_InspectRequest   = 0xb520
OP_InspectAnswer    = 0xb620
OP_GroupInvite      = 0x4020
OP_GroupFollow      = 0x4220
OP_GroupDeclineInvite = 0x4120
OP_GroupQuit        = 0x4420
OP_GroupUpdate      = 0x2640
OP_Consider         = 0x3721
OP_AckPacket        = 0x0019   # Low-level Daybreak ack — confirm value
```

### Task 3.2 — Create `common/patches/trilogy_structs.h`

Top-level namespace: `Trilogy::structs`. All structures inside `#pragma pack(1)`.

**Constants (from EQClassic PlayerProfile.h and eq_packet_structs.h):**
```cpp
namespace Trilogy { namespace structs {
    static const uint32 BUFF_COUNT              = 15;   // visible buffs in PP
    static const uint32 SPELL_BOOK_SIZE         = 256;
    static const uint32 SPELL_MEMORY_SIZE       = 8;
    static const uint32 INVENTORY_SLOTS         = 30;   // main worn + general
    static const uint32 CONTAINER_SLOTS         = 80;   // bag item IDs (10 bags × 8)
    static const uint32 CURSOR_BAG_SLOTS        = 10;
    static const uint32 BANK_SLOTS              = 8;
    static const uint32 BANK_CONTAINER_SLOTS    = 80;   // bank bag items
    static const uint32 MAX_CHARACTERS          = 10;   // char select sends 10
    static const uint32 LANGUAGE_COUNT          = 24;
    static const uint32 SKILL_COUNT             = 74;
    static const uint32 GROUP_MEMBERS           = 6;
    static const uint32 BIND_LOCATIONS          = 5;
    static const uint32 PC_MAX_NAME_LENGTH      = 30;   // from EQClassic config.h
    static const uint32 PC_SURNAME_MAX_LENGTH   = 20;
    static const uint32 NPC_MAX_NAME_LENGTH     = 30;
}}
```

**Structures to port (in priority order):**

1. `SpellBuff_Struct` (10 bytes) — from EQClassic `PlayerProfile.h`
2. `ItemProperties_Struct` (10 bytes) — from EQClassic `PlayerProfile.h`
3. `LoginInfo_Struct` — login credential packet (40-byte crypt + variable)
4. `EnterWorld_Struct` — character name + flags (note: opcode 0x0180, size differs from Titanium's 72-byte struct)
5. `NameApproval` — character name approval (64 + race + class + deity)
6. `CharacterSelect_Struct` (1248 bytes) — 10 character slots
7. `CharWeapon_Struct` — weapons on character select screen
8. `PlayerProfile_Struct` (8104 bytes) — the full character blob
9. `ServerZoneEntry_Struct` (312 bytes) — zone entry from server
10. `ClientZoneEntry_Struct` — zone entry from client (name only)
11. `NewZone_Struct` (372 bytes) — zone metadata
12. `Spawn_Struct` (164 bytes) — entity appearance
13. `NewSpawn_Struct` — wraps Spawn_Struct for OP_NewSpawn
14. `SpawnAppearance_Struct` — state change (sitting, invisible, etc.)
15. `WearChange_Struct` — equipment change notification
16. `Death_Struct` — entity death
17. `HPUpdate_Struct` — HP delta
18. `ManaChange_Struct` — mana delta
19. `Action_Struct` — combat action / spell action
20. `CastSpell_Struct` — client spell cast request
21. `BeginCast_Struct` — cast bar notification
22. `CastOn_Struct` — spell landed on target
23. `MemorizeSpell_Struct` — memorize/forget/scribe
24. `ChannelMessage_Struct` — chat message
25. `MobUpdate_Struct` / `ClientUpdate_Struct` — position updates
26. `CharCreate_Struct` — character creation
27. `MoveItem_Struct` — inventory move
28. `ConsumeFoodDrink_Struct` — food/drink consumption
29. `Consider_Struct` — /con result
30. `GroupUpdate_Struct` — group membership

**Buffer Over-read Guards (critical):** When porting PlayerProfile, all array iterations **must** use the Trilogy constants, never the server-side `BUFF_COUNT`, `MAX_SPELLS`, or inventory maximums:
```cpp
// WRONG — will over-read from Trilogy client buffer:
for (int i = 0; i < MAX_PP_SPELLBOOK; i++) ...

// CORRECT — use Trilogy limit:
for (int i = 0; i < Trilogy::structs::SPELL_BOOK_SIZE; i++) ...
```

### Task 3.3 — Port Pre-Login Structs

From `EQClassic/LS/Login/login_structs.h`:

- `ServerList_Struct` — old client format (int8 numservers, not int16)
- `ServerListServerFlags_Struct` — `{int8 greenname; int32 usercount; int8 unknown[8];}`
- `ServerListEndFlags_Struct` — `{int32 admin; int8 zeroes_a[8]; int8 kunark; int8 velious; int8 zeroes_b[11];}`
- `SessionId_Struct` — 10-char session_id + 7 unused + int32 unknown
- `LoginInfo_Struct` — 40-byte crypt block (DES-encrypted username+password)

**Note on DES:** The `LoginInfo_Struct` from EQClassic contains a 40-byte DES-encrypted block. EQEmu's world server receives this in `ProcessOP_SendLoginInfo()`. The decryption path must be added to the world server's client login handler behind a `ClientVersion == Trilogy` check. The OpenSSL DES key from EQClassic (`EQCrypto.h`) must be included.

---

## Phase 4: The Patch Translation Layer

### Task 4.1 — Create `common/patches/trilogy.h`

Mirror `titanium.h` exactly but with namespace `Trilogy`:
```cpp
#ifndef COMMON_TRILOGY_H
#define COMMON_TRILOGY_H
#include "../struct_strategy.h"
class EQStreamIdentifier;
namespace Trilogy {
    extern void Register(EQStreamIdentifier &into);
    extern void Reload();
    class Strategy : public StructStrategy {
    public:
        Strategy();
    protected:
        virtual std::string Describe() const;
        virtual const EQ::versions::ClientVersion ClientVersion() const;
        #include "ss_declare.h"
        #include "trilogy_ops.h"
    };
}
#endif
```

### Task 4.2 — Create `common/patches/trilogy.cpp` skeleton

Structure follows `titanium.cpp` exactly:

```cpp
namespace Trilogy {
    static const char *name = "Trilogy";
    static OpcodeManager *opcodes = nullptr;
    static Strategy struct_strategy;

    void Register(EQStreamIdentifier &into) {
        // Load patch_trilogy.conf
        // Register world signature:
        //   first_length = sizeof(structs::LoginInfo_Struct)
        //   first_eq_opcode = opcodes->EmuToEQ(OP_SendLoginInfo)
        // Register zone signature:
        //   ignore_eq_opcode = opcodes->EmuToEQ(OP_AckPacket)
        //   first_length = sizeof(structs::ClientZoneEntry_Struct)
        //   first_eq_opcode = opcodes->EmuToEQ(OP_ZoneEntry)
    }

    Strategy::Strategy() : StructStrategy() {
        #include "ss_register.h"
        #include "trilogy_ops.h"
    }

    const EQ::versions::ClientVersion Strategy::ClientVersion() const {
        return EQ::versions::ClientVersion::Trilogy;
    }
}
```

### Task 4.3 — Create `common/patches/trilogy_limits.h/.cpp`

Define Trilogy-era inventory slot mapping constants (analogous to `titanium_limits.h`). Key limits:

```cpp
// trilogy_limits.h
namespace Trilogy {
    // Inventory slot IDs as the Trilogy client expects them
    // Main inventory (worn + general): slots 0–29
    // Cursor: slot 30
    // Bank: slots 2000–2007
    // Container sub-slots: offset into bag position
    static const int16 SLOT_CURSOR    = 30;
    static const int16 SLOT_BANK_BEGIN = 2000;
    static const int16 SLOT_BANK_END   = 2007;

    int16 ServerToTrilogySlot(uint32 server_slot);
    uint32 TrilogyToServerSlot(int16 trilogy_slot);
}
```

The slot mapping logic must be derived from EQClassic's `MoveItem` handling and compared against Titanium's `ServerToTitaniumSlot()`.

---

## Phase 5: Character Selection ("Essential Three" Part 1)

### Task 5.1 — Map `CharacterSelect_Struct`

EQClassic `CharacterSelect_Struct` (1248 bytes):
- 10 names × 30 bytes = 300 bytes
- 10 levels (int8) = 10
- 10 classes (int8) = 10
- 10 races (int16) = 20
- 10 zones × 20 bytes = 200 bytes (note: field at 0540, not 0340 — the zone array starts later due to 200 unknown bytes)
- 10 genders (int8) = 10
- 10 faces (int8) = 10
- Equipment: 10 chars × 9 slots = 90 bytes + 2 unknown padding = 92
- Colors: 10 chars × 9 slots × 4 bytes (RGBA) = 360 bytes
- Unknown: 20 + 228 padding bytes

**Encode `OP_SendCharInfo`:** Read from server's `CharacterSelectEntry_Struct` and pack into `Trilogy::structs::CharacterSelect_Struct`. Strip fields not present in Trilogy (AAs, LDoN, augments). Weapons use a separate `CharWeapon_Struct` (20 bytes: 10 right-hand + 10 left-hand uint16 item IDs) sent alongside or embedded.

### Task 5.2 — Character Creation Decode

`OP_CharacterCreate` (0x4920): Decode incoming Trilogy `CharCreate_Struct` into EQEmu's internal `CharCreate_Struct`. Key fields: name, gender, race, class, deity, STR/STA/AGI/DEX/INT/WIS/CHA start stats, face, start zone.

Validation: Trilogy had no half-elves as a new option, no Vah Shir, etc. Race/class combinations must be constrained.

### Task 5.3 — Enter World Request

`OP_ENTERWORLD` (0x0180): Client sends character name in a simple string. Server responds with `OP_PlayerProfile` + `OP_NewZone` + `OP_ZoneEntry`.

Note: Titanium uses `OP_EnterWorld = 0x4D20` with a 72-byte struct `{char name[64]; uint32 tutorial; uint32 return_home;}`. Trilogy's `EnterWorld` is just the character name at opcode 0x0180. The Decode handler must extract only the name string without expecting the tutorial/return_home fields.

---

## Phase 6: World & Zone Entry ("Essential Three" Part 2)

### Task 6.1 — Map `PlayerProfile_Struct` (8104 bytes)

This is the most complex translation. Encode from EQEmu internal `PlayerProfile_Struct` to `Trilogy::structs::PlayerProfile_Struct`.

**Field-by-field mapping (selected critical fields):**

| EQEmu internal field | Trilogy struct offset | Notes |
|---|---|---|
| `name` | 0004 | Direct copy, 30 bytes |
| `last_name` | 0034 | Direct copy, 20 bytes |
| `gender` | 0054 | Direct |
| `deity` | 0055 | Direct |
| `race` | 0056 | int16 |
| `class_` | 0058 | int8 |
| `level` | 0060 | int8 |
| `exp` | 0064 | int32 |
| `mana` | 0070 | int16 |
| `face` | 0072 | int8 |
| `cur_hp` | 0120 | int16 |
| `STR`–`WIS` stats | 0123–0129 | 7 × int8 |
| `languages[24]` | 0130 | 24 bytes direct |
| `inventory[30]` | 0168 | 30 × uint16 item IDs |
| `inventoryitemPointers[30]` | 0228 | Zero-fill (client-side pointers) |
| `invItemProperties[30]` | 0348 | 30 × ItemProperties_Struct (10 bytes each) |
| `buffs[15]` | 0648 | **15 buffs only** (10 bytes each = 150 bytes) |
| `containerinv[80]` | 0798 | 80 × uint16 bag item IDs |
| `cursorbaginventory[10]` | 0958 | 10 × uint16 |
| `bagItemProperties[80]` | 0978 | 80 × ItemProperties_Struct |
| `cursorItemProperties[10]` | 1778 | 10 × ItemProperties_Struct |
| `spell_book[256]` | 1878 | **256 shorts** — clamp server's larger array |
| `spell_memory[8]` | 2390 | 8 shorts |
| `y, x, z, heading` | 2408–2420 | 4 × float |
| `current_zone[15]` | 2424 | Zone short name |
| `platinum/gold/silver/copper` | 2460–2472 | 4 × int32 |
| `bank_platinum/…` | 2476–2488 | 4 × int32 |
| `cursor_platinum/…` | 2492–2504 | 4 × int32 |
| `skills[74]` | 2508 | 74 bytes |
| `autosplit` | 2744 | int8 |
| `pk_acknowledge` | 2748 | int8 — client-written Pkill dialog answer, NOT the PVP flag |
| `gm` | 2764 | int8 |
| `discplineAvailable` | 2788 | int8 |
| `hungerlevel` | 2812 | int32 |
| `thirstlevel` | 2816 | int32 |
| `bind_point_zone[20]` | 2844 | Zone short name |
| `start_point_zone[4][20]` | 2864 | 4 alternate start zones |
| `bankinvitemproperties[8]` | 2944 | 8 × ItemProperties_Struct |
| `bankbagitemproperties[80]` | 3024 | 80 × ItemProperties_Struct |
| `bind_location[3][5]` | 3828 | Float array (x, y, z per bind) |
| `bank_inv[8]` | 3980 | 8 × int16 bank slot IDs |
| `bank_cont_inv[80]` | 3996 | 80 × int16 bank container IDs |
| `guildid` | 4158 | int16 |
| `fatigue` | 4170 | int8 |
| `pvp` | 4171 | int8 — the client's live PVP flag (mirrored from SpawnAppearance type 4) |
| `anon` | 4173 | int8 |
| `guildrank` | 4175 | GUILDRANK (int8) |
| `drunkeness` | 4176 | int8 |
| `spellSlotRefresh[8]` | 4180 | 8 × uint32 |
| `abilityCooldown` | 4216 | uint32 |
| `GroupMembers[6][48]` | 4220 | 6 × 48 chars |
| `logtime` | 8100 | int32 |
| padding bytes | various | Zero-fill the `unknown*` blocks |
| `checksum[4]` | 0000 | Compute or zero-fill (server validates separately) |

**Spells guard:** The EQEmu server stores up to `MAX_PP_SPELLBOOK` (400+) spells. When encoding to Trilogy, loop only `min(server_count, SPELL_BOOK_SIZE)` = 256. Set remaining Trilogy slots to 0xFFFF (empty). This prevents buffer over-read when the client-side spell book array is indexed.

**Items not present in Trilogy PP:** AA experience, AA spent points, LDoN points, tribute data, expedition data — all omitted. Zero-fill or skip.

### Task 6.2 — Implement `ZoneEntry` Translation

**Encode `OP_ZoneEntry`** (server → client, `ServerZoneEntry_Struct`, 312 bytes):

EQClassic offsets verified:
```
[000] checksum[4]
[004] sze_unknown1
[005] name[30]
[035] zone[15]
[050] sze_unknown2[6]
[056] y (float)
[060] x (float)
[064] z (float)
[068] heading (float)
[072] sze_unknown3[76]
[148] guildeqid (int16)
[157] class_ (int8)
[158] race (int16)
[160] gender (int8)
[161] level (int8)
[164] pvp (int8)
[167] face (int8)
[168] helmet (int8)
[216] npc_armor_graphic (int8)
[236] walkspeed (float)
[240] runspeed (float)
[256] anon (int8)
[280] Surname[20]
[302] deity (int16)
```

**Decode `OP_ZoneEntry`** (client → server): The client sends its name and current zone when first entering a zone. Extract these fields, match to the server's `ClientZoneEntry_Struct`.

**IP/Port format:** EQClassic zone server redirect uses `OP_ZoneServerInfo` (0x0480) with IP as a dotted-string (ASCII) and port as uint16. Titanium uses a binary IP (4-byte packed). The `ZoneServerInfo_Struct` encode handler must format the IP as a null-terminated ASCII string for Trilogy.

### Task 6.3 — Map `Spawn_Struct` (164 bytes)

EQClassic `Spawn_Struct` layout (164 bytes):
```
[000] size (float)
[004] walkspeed (float)
[008] runspeed (float)
[012] equipcolors[7] (int32 each) — RGBA armor tints
[040] padding to [049]
[049] heading (int8)
[050] deltaHeading (sint8)
[051] y_pos (sint16)
[053] x_pos (sint16)
[055] z_pos (sint16)
[057] deltaY:10, deltaZ:10, deltaX:10 (packed bitfield)
[062] spawn_id (int16)
[064] body_type (TBodyType int16)
[066] pet_owner_id (int16)
[068] cur_hp (sint16)
[070] GuildID (uint16)
[072] race (int8)
[073] NPC (int8)  — 0=Player, 1=NPC, 2=PC Corpse, 3=NPC Corpse
[074] class_ (int8)
[075] gender (int8)
[076] level (int8)
[077] invis (int8)
[079] pvp (int8)
[080] anim_type (int8)
[081] light (int8)
[082] anon (int8)
[083] AFK (int8)
[085] LD (int8)
[086] GM (int8)
[088] npc_armor_graphic (int8) — 0xFF=Player, 0=none, 1=leather, 2=chain, 3=plate
[089] npc_helm_graphic (int8)
[091] equipment[9] (int8 each) — helm/chest/arm/bracer/hand/leg/boot/weapon1/weapon2
[100] name[30]
[130] Surname[20]
[150] guildrank (GUILDRANK)
[152] deity (int8)
```

Coordinate note: Trilogy uses `sint16` for positions (fixed-point with implied scale of 1/8 or direct units — verify against EQClassic zone server). Titanium uses float. Apply coordinate conversion in encode/decode.

Velocity bitfield: The packed `deltaY:10, deltaZ:10, deltaX:10` field uses C bitfield packing — compiler-dependent. Use explicit bit manipulation rather than relying on the struct bitfield layout.

---

## Phase 7: Gameplay Mechanics

### Task 7.1 — Movement (`ClientUpdate` / `MobUpdate`)

`OP_ClientUpdate` (0xf320) decode: Client sends position update. Extract `y_pos, x_pos, z_pos, heading, deltaY/Z/X` from Trilogy's packed format. Convert coordinates to EQEmu's float-based internal format.

`OP_MobUpdate` (0xa120) encode: Server sends NPC position updates. Pack into Trilogy's sint16 + bitfield format. Use same scale factor as ZoneEntry positions.

### Task 7.2 — Combat

`OP_AutoAttack` (0x5121): 1-byte toggle (1=on, 0=off). Simple passthrough decode.

`OP_Attack` (0x9f20) encode: Send `CombatDamage` results. Fill `Action_Struct` with target, source, damage type, amount.

`OP_Death` (0x4a20) encode: Pack entity death notification.

`OP_BeginCast` (0xa920) encode: Notify client a cast bar should appear (target entity ID + cast time).

### Task 7.3 — Communication

`OP_ChannelMessage` (0x0721): Variable-length packet. Structure: sender name (null-terminated) + channel byte + language byte + message (null-terminated). Compare carefully with Titanium's `ChannelMessage_Struct` which includes more fields.

`OP_SpecialMesg` (0x8021): System messages and NPC speech. Map to EQEmu's `SpecialMesg_Struct`.

`OP_Emote` (see opcode list): Not explicitly listed in EQClassic ops. Handled via ChannelMessage with emote channel type.

---

## Phase 8: Testing & Verification

### Task 8.1 — Debug Login Sequence

Verification steps:
1. Capture Trilogy client connecting to EQEmu with Wireshark/tcpdump
2. Confirm stream identifier selects `Trilogy_world` (not `Titanium_world`)
3. Verify `LoginInfo_Struct` is correctly decoded (40-byte DES block decrypts to valid account name)
4. Confirm character list arrives on character select screen (`CharacterSelect_Struct` 1248 bytes)

Expected packet flow:
```
Client → Server: OP_SessionRequest (Daybreak layer)
Server → Client: OP_SessionResponse (Daybreak layer)
Client → Server: OP_SendLoginInfo (0x5818, 40 bytes)  ← first app packet, identifies Trilogy
Server → Client: OP_LoginApproved / expansion info
Server → Client: OP_SendCharInfo (0x4720)
Client → Server: OP_ENTERWORLD (0x0180, character name string)
```

### Task 8.2 — Debug World/Zone Handoff

Expected zone entry flow:
```
Client → Server: OP_ZoneEntry (0x2a20, ClientZoneEntry_Struct — name string)
Server → Client: OP_PlayerProfile (0x2d20, 8104 bytes)
Server → Client: OP_NewZone (0x5b20, 372 bytes)
Server → Client: OP_ZoneEntry (0x2a20, ServerZoneEntry_Struct, 312 bytes — player's own spawn)
Server → Client: OP_ZoneSpawns (0x6121, array of Spawn_Struct 164 bytes each)
Server → Client: OP_NewSpawn (0x4921, individual Spawn_Struct) for each NPC
```

Log any packet size mismatches between expected and actual using `LogNetcode`.

### Task 8.3 — Inventory & Spell Book Integrity

Checks:
1. Log in with a character having items in all 30 inventory slots + bags — verify none are lost
2. Scribe 256 spells and verify none overflow (spell_book array boundary)
3. Move items between slots — verify `MoveItem` decode maps Trilogy slot IDs back to server slot IDs correctly
4. Zone out and back in — verify PlayerProfile re-encodes correctly

---

## Files to Create (in order of dependency)

| File | Purpose | Dependency |
|---|---|---|
| `patches/trilogy_ops.h` | E()/D() opcode list | None |
| `patches/trilogy_structs.h` | Packed struct definitions | EQClassic headers (reference only) |
| `patches/trilogy_limits.h` | Slot mapping constants/declarations | `trilogy_structs.h` |
| `patches/trilogy_limits.cpp` | Slot mapping implementation | `trilogy_limits.h` |
| `patches/trilogy.h` | Strategy class header | `trilogy_ops.h` |
| `patches/trilogy.cpp` | Encoder/decoder implementation | All above |
| `patch_trilogy.conf` | Runtime opcode mapping file | Deploy to patches/ runtime dir |

## Files to Modify (in order)

| File | Change |
|---|---|
| `common/emu_versions.h` | Add `Trilogy` to `ClientVersion` and `MobVersion` enums, update bitmasks |
| `common/emu_versions.cpp` | Add name strings, conversion functions for Trilogy |
| `patches/patches.cpp` | Add `Trilogy::Register()` and `Trilogy::Reload()` calls |
| `common/CMakeLists.txt` | Add trilogy source and header files to build |

## Files to Audit (for ClientVersion switch statements)

After adding `Trilogy` to the enum, these files must be audited for switch statements that need a `case ClientVersion::Trilogy:` branch:

```
common/emu_versions.cpp
common/patches/patches.cpp
world/client.cpp           (expansion info, char select)
zone/client.cpp            (packet handler)
zone/zonedb.cpp            (any version-conditional DB queries)
```

Search command: `grep -rn "ClientVersion::Titanium\|ClientVersion::Client62" Server/ --include="*.cpp" --include="*.h"`

---

## Risk Register

| Risk | Mitigation |
|---|---|
| Enum value shift breaks DB columns storing raw `ClientVersion` int | Audit DB schema; add `Trilogy = 1` before existing values and shift rest; write DB migration |
| `LoginInfo_Struct` size collision with Titanium's 488-byte struct | Verify: Trilogy world first packet is 40 bytes at 0x5818; Titanium is 488 bytes at same opcode. Size difference is the discriminator. |
| Coordinate scale mismatch (sint16 vs float) | Measure empirically: stand at known /loc in Trilogy and compare Spawn_Struct values against expected float coordinates |
| DES key changes between EQ versions | Use the exact `verant_key` from `EQClassic/LS/Login/EQCrypto.cpp` |
| Spell book over-read if server has > 256 spells memorized | Use `min()` guard in encode loop |
| Missing `patch_trilogy.conf` at runtime | Add check in `Register()` that logs clearly if file not found |

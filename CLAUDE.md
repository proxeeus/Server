# CLAUDE.md — EQEmu Server (with Trilogy client support)

Guidance for working in this repository. This is the EQEmulator core server (a from-scratch
open-source EverQuest server in C++), extended with a substantial custom layer that adds
support for the **EverQuest Trilogy client (v29c/v30, build "8-09-2001 14:25")** — a client
that predates every client EQEmu officially supports.

---

## ⛔ Never build this project

**Do NOT run CMake, MSBuild, `make`, or any compile/link step.** The user (proxeeus) compiles
locally and will report errors back. Do not attempt to "verify it builds." Just write correct
code and explain it. Build commands below are documentation only — for the user, not for you.

---

## What this codebase is

EQEmu is three cooperating server processes plus shared libraries, all built from one CMake tree:

| Process | Source dir | Role |
|---|---|---|
| **loginserver** | `loginserver/` | Account auth, hands clients a server list, redirects to world |
| **world** | `world/` | Character select, account/char management, dispatches players to zones |
| **zone** | `zone/` | The actual game simulation — one process per running zone |
| (shared) | `common/` | Networking, packet structs, DB layer, opcode translation, rules |
| (shared) | `shared_memory/` | Loads item/spell/etc. into shared memory for zone processes |

Other targets: `ucs/` (universal chat), `queryserv/` (query/logging), `eqlaunch/` (zone
launcher), `hc/` (headless client). DB is MySQL/MariaDB (200+ tables). Quests run in **Perl**
(`embparser*`, `perl_*.cpp`) and **Lua** (`lua_*.cpp`).

### Build (reference only — do not run)
```
mkdir build && cd build
cmake -G "..." -DEQEMU_BUILD_TESTS=ON -DEQEMU_BUILD_LOGIN=ON ..   # see BUILD.md
```
Adding a new `.cpp`/`.h` requires editing the relevant `CMakeLists.txt` (one per target dir:
`common/`, `world/`, `zone/`, `loginserver/`). Flag the user when a file needs registering.

---

## The client patch / translation layer (`common/patches/`)

EQEmu's internal code speaks **one** packet format (effectively the Titanium/RoF2 internal
struct set). Each supported client gets a "patch" that translates between that internal format
and the client's wire format. Supported patches: `titanium`, `sof`, `sod`, `uf`, `rof`, `rof2`,
and **`trilogy`**.

Each patch is a set of files following a fixed template (compare against `titanium.*`):

| File | Purpose |
|---|---|
| `<patch>_ops.h` | `E(OP_x)` / `D(OP_x)` macro list — which opcodes get **E**ncoders / **D**ecoders |
| `<patch>_structs.h` | `#pragma pack(1)` wire structs in namespace `<Patch>::structs` |
| `<patch>_limits.h/.cpp` | Inventory slot-id mapping (`ServerToXSlot` / `XToServerSlot`) and limits |
| `<patch>.h/.cpp` | `Strategy` class: per-opcode `ENCODE(...)` / `DECODE(...)` bodies + `Register()` |
| `patch_<patch>.conf` | Runtime file mapping internal `OP_` names → that client's hex opcodes |

`patches.cpp::Register()` registers every patch's stream signature in **ascending client-version
order** so `EQStreamIdentifier` can classify a connecting client by the size+opcode of its first
application packet. Client versions live in `common/emu_versions.h` (`enum ClientVersion`);
`Trilogy` was inserted as the earliest version.

**Buffer-safety rule for encoders:** always iterate arrays with the *client's* limit
(`Trilogy::structs::SPELL_BOOK_SIZE` = 256, `BUFF_COUNT` = 15, etc.), never the server-side
maximum — the client's buffer is smaller and over-reading corrupts it.

---

## Trilogy client support — the heart of the custom work

The Trilogy client is far older than anything EQEmu was built for: 8104-byte PlayerProfile,
256 spell slots, 15 buffs, sint16 spawn coordinates, DES login crypto, and a different
low-level session protocol ("EQNetwork" / Verant, not Daybreak). Supporting it required two
things working together:

### 1. The patch translation layer (`common/patches/trilogy*`)
Standard EQEmu patch (see table above) handling struct/opcode translation where the normal
`EQStream` path is used (primarily the early stages). Key reference: `trilogy_plan.md` at repo
root documents the original field-by-field struct mapping and opcode table.

### 2. A bespoke EQNetwork session layer (the bulk of the runtime work)
The Trilogy client's low-level UDP protocol differs enough that it is **not** run through the
normal `EQStream` machinery for the main session. Instead, raw UDP datagrams the normal stack
can't identify are forwarded to dedicated handler classes, one per process:

| Class | File | Listens / hooks |
|---|---|---|
| `TrilogyLoginServer` | `loginserver/trilogy_ls.*` | own UDP **5998**; server list, then redirect |
| `TrilogyWorldServer` | `world/trilogy_world.*` (+ `world/trilogy_ls.*`) | world UDP **9000** via `OnUnknownPacket`; char-select, zone redirect |
| `TrilogyZoneServer` | `zone/trilogy_zone.*` | zone UDP via `OnUnknownPacket`; full in-zone gameplay |
| `TrilogyClient` / `TrilogyStream` | `zone/trilogy_client.*` | a real `Client` subclass representing the player in the entity system |

**Wiring** (zone, in `zone/main.cpp` ~L510–681): a `TrilogyZoneServer` instance is created;
`eqsm->OnUnknownPacket(...)` routes unrecognized datagrams into `OnRawPacket`; `SetSendFn(...)`
gives it the UDP send callback; `trilogy_zone.Tick()` is pumped each main-loop iteration.

#### How `TrilogyClient` integrates with the engine
`TrilogyClient : public Client` (see `zone/trilogy_client.h`). Subclassing `Client` means
`entity_list`, aggro/hate, groups/raids, `CastToClient()`, and Titanium-client visibility all
work unchanged. It overrides `QueuePacket`/`FastQueuePacket` to **intercept every outgoing
internal packet**, translate it to EQNetwork wire format, and send it via
`TrilogyZoneServer::SendToSession`. Opcodes with no translator are silently dropped (not
crashing beats completeness). `TrilogyStream` is a null `EQStreamInterface` that always reports
`ESTABLISHED` — actual I/O flows through `TrilogyZoneServer`, not the stream.

#### EQNetwork session protocol (what `TrilogyZoneServer`/`TrilogyWorldServer` speak)
Verant's pre-Daybreak protocol: SEQSTART/SEQ data packets, ARQ/ARSP acks (the `gsq/arq/asq/
cli_arq/ack_due/sack_init/seq_sent` fields on `Session`), fragment reassembly (`frag_groups`),
and CRC. The zone-in handshake is a state machine: `CONNECTING1→2→3→4→5→CONNECTED` keyed off the
client opcodes it's waiting for at each step (`OP_SetDataRate 0xe821` → `OP_ZoneEntry 0x2a20`
→ `0x5d20` → `0x0a20` → `0xd820`). See the `ZoneState` enum and `Session` struct in
`zone/trilogy_zone.h` — it is heavily commented and is the best map of runtime behavior.

---

## Trilogy gotchas (hard-won — read before touching this code)

These cost real debugging time. Verify the referenced symbol still exists before relying on it.

- **Opcodes are little-endian wire values** written as `0xNN20` (e.g. `OP_WearChange=0x9220`,
  `OP_ClientUpdate=0xf320`, `OP_ChannelMessage=0x0721`). The canonical map is in `trilogy_plan.md`
  and `common/patches/trilogy_ops.h` / `patch_trilogy.conf`.
- **`TrilogyStream` is always ESTABLISHED** → camp/logout cannot be detected in `Client::Process`;
  it must be handled in `TrilogyZoneServer` (OP_Camp `0x0722`, ~29s teardown).
- **`OnRawPacket` pre-inserts the session**, so `is_new` in `OnDatagram` is always false, and
  CRC-bad packets can create ghost sessions unless `OnRawPacket` pre-stamps `last_pkt`. Timeouts:
  CONNECTED 300s vs CONNECTING* 120s.
- **WearChange echo at zone-in:** must echo `0x9220` back during CONNECTING3/4, or the client
  sends `0x0a20` too early → ZoneSpawns race → crash.
- **Door action byte is forwarded UNCHANGED** (same 0x02/0x03 as EQEmu — do NOT invert; inverting
  caused the 2–3-click bug). `OP_SpawnDoor=0x9520`, `OP_ClickDoor=0x8d20`.
- **Zone-out crash (`0x004c7752`/`0xff000082`):** fix is sending an **empty** zone_name in the
  `0xa320` approval (skips a stale entry lookup), and `SendCloseToSession` right after so EQNetwork
  nulls the connection-table entry before the player zones back. Ordering `0x0480` before `0xa320`
  alone does NOT fix it.
- **Zone-transition coordinates:** heading is sent **/2 not ×2** (×2 causes neck-snap). Zone-in
  detection guard uses 2× radius. See `project_trilogy_zone_transition` memory for dead-ends.
- **NPC/system text must go via `OP_SpecialMesg 0x8021`** formatted as `name says,'text'`, NOT
  chan-8 `0x0721` — the PC Trilogy client truncates long chan-8 text and mangles `[brackets]`.
- **Inventory slots:** EQ::invslot is RoF2-style here; cursor = slot **33** (EQEmu) / slot 0 (wire);
  bank DB slots 2000–2007. EQEmu's invisible **cursor queue at DB slots 8000–8010** persists (Velious
  has none) — drain slots 33 + 8000–8010 before `m_inv` loads in `SendInventoryItems`.
- **Currency on relog:** `InitTrilogyFields` must call `database.LoadCharacterCurrency` —
  `LoadCharacterData` only reads `character_data`, not `character_currency`. Skipping it zeroes
  `m_pp` money and every Save overwrites the real balance.
- **m_inv is stale after direct DB moves:** buy/sell/bank/trade handlers in `TrilogyZoneServer`
  mutate the inventory DB directly and are self-contained rather than going through `m_inv`.
- **Deity:** char sheet reads PP byte 4152 (`bank_cont_inv[78]`) as a raw EQEmu deity ID (201–216);
  the compact byte-55 value is ignored by the char sheet.

The `memory/` directory (auto-loaded each session via `MEMORY.md`) holds the full, current set of
these findings with file/line specifics — consult it for anything Trilogy-related.

---

## Reference material

- **`EQClassic/`** (sibling dir under `c:\eqemu\source`) — a fully working Trilogy-compatible
  server, the authoritative reference for opcodes, struct layouts, channel numbers, and the DES
  login key. Key files: `Common/Include/eq_opcodes.h`, `eq_packet_structs.h`, `PlayerProfile.h`,
  `LS/Login/login_structs.h`, `LS/Login/EQCrypto.*`.
- **`EQMacEmuTrilogy/`** — another Trilogy-era reference implementation. DO NOT IMPLEMENT WITHOUT ASKING. EQClassic is the default authoritative source.
- **`trilogy_plan.md`** (repo root) — original implementation plan: struct field maps, opcode
  table, protocol-difference table, risk register. Historical but still an accurate struct/opcode
  reference.
- **`packetsFiltered.txt`** (repo root) — captured packet traces.

---

## Conventions

- **Platform:** Windows 11 / PowerShell (this dev box) but the code is cross-platform (`win32` vs
  POSIX guards exist). Use PowerShell syntax for any shell work.
- **Style:** match the surrounding file. Existing EQEmu code uses tabs; the Trilogy additions are
  heavily commented with section banners (`// ==== ... ====`) — keep that density when editing them.
- **GPLv3.** Every source file carries the standard EQEmu license header.
- **Types:** fixed-width (`uint16`, `sint16`, `uint32`) throughout packet/struct code. Wire structs
  are always `#pragma pack(1)`.
- When changing wire structs, keep them byte-exact against the EQClassic reference and re-check any
  `static_assert` on struct size — a size change can break stream identification.


## General tips
- Write Debug/Info logs going to zone/world console when possible via LogInfo, especially when debugging or investigating something.
- NEVER, EVER assume something cannot be done due to a limitation from the Trilogy client.
- When missing "context" for Trilogy client implementations (packets contents, hex values etc), add LogInfo logging exactly the data and cross-reference
it with EQClassic source
= Diagnostic logging is king.
- Don't give up on implementations by imagining anything about "v29c's internal limits (entity count, animation slots, corpse count over time)"; worst
case scenario, investigate and propose workarounds. NEVER EVER assume we "reached the limit of what we can do", this client has been implemented in EQClassic. So the client WORKS.
- There IS NO SUCH THING AS "We reached the limits of the client, we can't do this." Again, this stuff has been implemented before.
- There is no "Trilogy client memory leak we can't fix".
- For Trilogy Client implementation, always remember EQClassic IS the protocol implementation needed by the client.

## Golden rules
- Ask, don't assume. If something is unclear, ask before writing a single line. Never make silent assumptions about intent, architecture, or requirements. When running unattended, pick the most reasonable interpretation, proceed, and record the assumption rather than blocking.
- Implement the simplest solution for simple problems, better solutions for harder problems. Do not over-engineer or add flexibility that isn't needed yet. 
- Don't touch unrelated code but please do surface bad code or design smells you discover with me so we can address them as a separate issue.
- Flag uncertainty explicitly. If you're unsure about something, see point 1 above. If it makes sense to do so, conduct a small, localised and low-risk experiment and bring the hypothesis and results to me to discuss. Confidence without certainty causes more damage than admitting a gap.
- I'm always open to ideas on better ways to do things. Please don't hesitate to suggest a better way, or one that has long lasting impact over a tactical change. (as a few examples)
- Stop asking for EQClassic packet captures. There are none.
- NEVER EVER suggest "accepting the lesser of two evils" / abandon for the user.
- Always ASK before thinking a refactor is needed because somehow a new source has been found.
# U1 Cloud Filament Sync — Design Document

**Status:** Research complete, implementation not started
**Target:** Snapmaker U1 (4-slot multi-material printer running Klipper firmware)
**Goal:** Bambu-Studio-style "Sync filaments with AMS" button that pulls the four currently-loaded filaments (vendor / material / color) from the printer into the slicer's filament chips, **without requiring the user to enable LAN Mode on the printer.**

---

## 1. Why this design exists

Snapmaker Orca already does this in its **Device tab** (a Flutter webview that subscribes to AWS-IoT-tunneled Moonraker JSON-RPC), but exposes no slicer-side button. The native FullSpectrum/Orca tree we forked has the **Bambu "Sync with AMS" button intact** (`Sidebar::sync_ams_list()` in `src/slic3r/GUI/Plater.cpp:8244`), but no data source wired into it — it's still expecting Bambu's MQTT broker. This feature is the missing data source.

Constraint: cannot require LAN Mode. The reason: enabling LAN Mode breaks Snapmaker's mobile-app integration and other cloud-bound features the user depends on for routine monitoring.

---

## 2. Architecture (validated against logs + official docs)

```
┌─────────────────────────────────┐         ┌─────────────────────────┐
│ SnapSync (this fork of Orca)    │         │ Snapmaker U1            │
│                                 │         │  Klipper + Moonraker    │
│ Sidebar [Sync with AMS] button  │         │  + Snapmaker mqtt_agent │
└───────────────┬─────────────────┘         └──────────┬──────────────┘
                │                                       │
                │  (1) OAuth login                     │
                │  ──► id.snapmaker.com                │
                │  ◄── session token                   │
                │                                       │
                │  (2) Exchange token + sn for AWS     │
                │      IoT mTLS material               │
                │  ──► api.snapmaker.com/api/...   (TBD)│
                │  ◄── {ca, cert, key, clientId,       │
                │       ip, port}                       │
                │                                       │
                │  (3) MQTTS connect (mutual TLS)      │
                │  ──────────────────►  AWS IoT Core   │
                │  ◄──────────────────  (broker)       │◄─── printer also connected
                │                                       │
                │  (4) Publish JSON-RPC                │
                │      to topic <SN>/data/request      │
                │  ──► printer.objects.query           │
                │      {objects: {print_task_config:   │
                │                 null}}               │
                │                                       │
                │  (5) Receive on <SN>/data/response   │
                │  ◄── {result: {status:               │
                │        {print_task_config: {...}}}}  │
                │                                       │
                │  (6) Map response → filament_ams_list│
                │      → existing sync_ams_list path   │
                │      → 4 filament chips update       │
```

### 2.1 Why this works without LAN Mode

The printer's outbound connection to AWS IoT Core is **always on** in cloud mode (that's how the Snapmaker mobile app sees the printer at all). The slicer connects to the **same broker** with its own per-device certificate; both sides talk by publishing/subscribing to the same `<SN>/...` topic namespace. The slicer never directly contacts the printer's IP, so the printer's local network exposure is irrelevant.

This was verified live from the user's running Snapmaker Orca session — see "Evidence" below.

---

## 3. The data we want and where it lives

The Snapmaker U1's Klipper config registers a **custom printer object** named `print_task_config`. Querying it with standard Moonraker JSON-RPC yields:

```json
{
  "filament_vendor":     ["Snapmaker","Generic","Snapmaker","Snapmaker"],
  "filament_type":       ["PLA","PLA","PLA","PLA"],
  "filament_sub_type":   ["SnapSpeed","","SnapSpeed","SnapSpeed"],
  "filament_color":      [4278716941, 4280191205, 4293340957, 4293058267],
  "filament_color_rgba": ["080A0DFF","1E88E5FF","E72F1DFF","E2DEDBFF"],
  "filament_official":   [true,false,true,true],
  "filament_sku":        [900001, 0, 900002, 900000],
  "extruder_map_table":  [1, 3, 2, 3, /* 28 more slots */ ],
  "extruders_used":      [false, true, false, true],
  "filament_edit":       [false, true, false, false],
  "filament_exist":      [true, true, true, true],
  "filament_color_multi":[
    {"alpha":255, "colors":["080A0D"], "mode":0, "nums":1},
    {"alpha":255, "colors":["1E88E5"], "mode":0, "nums":1},
    {"alpha":255, "colors":["E72F1D"], "mode":0, "nums":1},
    {"alpha":255, "colors":["E2DEDB"], "mode":0, "nums":1}
  ],
  "nozzle_diameters":    [0.4, 0.4, 0.4, 0.4]
}
```

Index `i ∈ [0..3]` = slot `i+1` in the U1's UI.

### 3.1 Mapping to OrcaSlicer's `filament_ams_list`

`filament_ams_list` is a `std::map<int, DynamicPrintConfig>` — one entry per AMS slot, each holding standard filament config options. We populate slot `i` as:

| `print_task_config` field | OrcaSlicer config option |
|---|---|
| `filament_type[i]` (`"PLA"`) | `filament_type[0]` |
| `filament_vendor[i]` (`"Snapmaker"`) | `filament_vendor[0]` |
| `filament_color_rgba[i]` (`"080A0DFF"`) | `filament_colour[0]` (drop the alpha → `"#080A0D"`) |
| `filament_sub_type[i]` (`"SnapSpeed"`) | filament-preset name hint for `sync_ams_list()`'s matcher |
| `filament_sku[i]` (`900001`) | `filament_id[0]` (SKU is Snapmaker's stable filament ID) |
| `nozzle_diameters[i]` | informational only — already in printer preset |

Slots where `filament_exist[i] == false` are skipped (= empty slot).

Once `filament_ams_list` is populated, the **existing** `PresetBundle::sync_ams_list(unknowns)` machinery (called by the existing Bambu sync button at `Plater.cpp:8301`) does the rest: matches each AMS entry against the loaded filament presets by `filament_id` first, then `filament_type`+`filament_vendor`, falls back to "Generic PLA" when nothing matches, updates filament colors, refreshes the sidebar.

**Net implementation cost on the slicer side:** populate the map, then call existing code.

---

## 4. Validation against official documentation

### 4.1 Moonraker JSON-RPC

Verified against `arksine/moonraker` docs via Context7 (`/arksine/moonraker`, source reputation: High).

**Confirmed:**
- ✅ Method name `printer.objects.query` is exact.
- ✅ `params.objects` is `{<object_name>: null | [field, field, ...]}` — `null` returns all fields. We use `null` for `print_task_config`.
- ✅ Response shape is `{jsonrpc:"2.0", id, result: {eventtime, status: {<object_name>: {...}}}}`.
- ✅ The JSON-RPC API is **transport-agnostic** — Moonraker exposes it identically over HTTP, WebSocket, and (in Snapmaker's case) MQTT. The MQTT tunneling is a Snapmaker addition implemented by their `mqtt_agent` Klipper component; it does not alter the request/response payload.

**One asterisk:** Moonraker docs do not document `print_task_config` because it is a **Snapmaker-custom Klipper module** not part of upstream Klipper. We are calling a standard API to fetch a vendor-specific object — that contract is fine, but the object's field list is not formally guaranteed by anyone. If Snapmaker firmware updates rename fields, our sync will silently degrade until we update the mapping. Mitigation: log unknown fields, fall back to "Generic" when fields are missing, ship a version check.

### 4.2 AWS IoT Core MQTT

Verified against AWS IoT Developer Guide (`docs.aws.amazon.com/iot/.../mqtt.html`).

**Confirmed:**
- ✅ Endpoint hostname pattern `<accountprefix>.iot.<region>.amazonaws.com:8883` is canonical. The U1's broker `a1pr8yczi3n0se.iot.us-west-1.amazonaws.com:8883` matches exactly — Snapmaker hosts this in `us-west-1`.
- ✅ Port 8883 is the documented mutual-TLS MQTT port.
- ✅ Per-device X.509 client certificates are the standard AWS IoT auth method. We have `ca`, `cert`, `key`, `clientId` from the live log line confirming Snapmaker uses the exact same flow.
- ✅ Topics are arbitrary user-defined namespaces governed by IoT policies. Snapmaker's `<SN>/data/request` / `<SN>/data/response` / `<SN>/status` / `<SN>/notification` are all valid topic strings under AWS IoT's rules (max 256 bytes per segment, etc.).

### 4.3 Eclipse Paho MQTT C client

Already statically linked into both Snapmaker Orca (verified by `strings`) and our SnapSync fork (`src/mqtt/`, exposed as `Slic3r::MqttClient`). Supports mTLS via `(ca, cert, key)` constructor — which is exactly what `SSWCP_MqttAgent_Instance::sw_create_mqtt_client()` (SSWCP.cpp:4971) already accepts as parameters from JS. We will call that constructor directly from native code instead of going through the JS bridge.

---

## 5. Evidence (live log capture from Snapmaker Orca v2.3.1)

User's running session, file `~/Library/Application Support/Snapmaker_Orca/log/2026-05-27-12-17-20.log.0`.

### Cert and broker (line ~240)
```
DeviceConnectService, _connectToOrca, endpoint:
  {authCode: , userid: 112585, code: 12345678,
   sn: 81100260324103625F63,
   ca: ***, cert: ***, key: ***,
   clientId: 3525e2419152fb514ebe2444f21413522fe483f304dc7b6afaeb188aa739b7ba,
   port: 8883,
   ip: a1pr8yczi3n0se.iot.us-west-1.amazonaws.com,
   devName: U1, id: 154527, productId: 1, link_mode: wan,
   publishTopics:   {config: /config/request/<clientId>,  data: /data/request,  request: /request},
   subscribeTopics: {config: /config/response/<clientId>, data: /data/response, error: /error,
                     response: /response, status: /status, notification: /notification}}
```

### Filament query (line ~245)
```
sendCommand.handleRequest, command:
  {jsonrpc: 2.0,
   method: printer.objects.query,
   params: {objects: {print_task_config: null, extruder: [...], extruder1: [...], ...}},
   id: 1779899197086010}
```

### Filament response (cached, line ~246)
The `_deviceFilamentInfoMap` payload shown in §3 above, keyed by serial `81100260324103625F63`.

These three log lines, taken together, are the complete contract for the feature. No reverse-engineering required — Snapmaker's Flutter client logs every byte of what it does.

---

## 6. Implementation plan

### 6.1 What's already in the tree (zero work)
- `src/slic3r/Utils/MQTT.cpp` — Paho-backed `MqttClient` with mTLS support
- `src/slic3r/Utils/MoonRaker.cpp` — `Moonraker_Mqtt` host class with `Connect()`, `Send_Request()`, response callbacks
- `src/slic3r/GUI/WebSMUserLoginDialog.cpp` — OAuth login to id.snapmaker.com
- `src/slic3r/GUI/WebDeviceDialog.cpp` — "add a Snapmaker device" modal webview that does the entire cert exchange (see §6.3)
- `src/libslic3r/AppConfig.cpp` (`get_devices()` / `save_device_info()`) + `DeviceInfo` struct — already persists `ca/cert/key/clientId/ip/port/sn` in `Snapmaker_Orca.conf`
- `src/slic3r/GUI/Plater.cpp::Sidebar::sync_ams_list()` — the button & sync dialog, already populates `filament_ams_list` and triggers the matcher
- `PresetBundle::sync_ams_list(unknowns)` — the matcher itself

### 6.2 What needs to be written

**A. `src/slic3r/Utils/SnapmakerCloud.{hpp,cpp}`** (~300 LOC)
- `class SnapmakerCloudClient`
  - `bool login_with_token(const std::string& session_token)` — uses existing OAuth state
  - `std::vector<DeviceInfo> list_user_devices()` — call Snapmaker REST endpoint TBD
  - `MqttCredentials fetch_iot_credentials(const std::string& sn)` — call Snapmaker REST endpoint TBD (this is the only unresolved piece — task #5)
  - `nlohmann::json query_printer_objects(const std::string& sn, const json& object_request)` — wraps the MQTT publish + response correlation; reuses `Moonraker_Mqtt` infrastructure
- `struct MqttCredentials { string ca; string cert; string key; string clientId; string host; int port; }`
- `struct PrintTaskConfig { vector<string> vendor, type, sub_type, color_rgba; vector<int> sku; vector<bool> used, exists; ... }`

**B. `src/slic3r/GUI/Plater.cpp` change** (~30 LOC inside existing `sync_ams_list()`)
- When `obj` is null OR the active printer's `printer_model` matches Snapmaker U1, branch into a new helper `sync_ams_list_from_snapmaker_cloud()`
- The helper instantiates `SnapmakerCloudClient`, fetches `print_task_config`, populates `preset_bundle->filament_ams_list`, then falls through to the existing matcher path (`sync_ams_list(unknowns)`, `on_filaments_change`, etc.) — no duplication.

**C. Tests / sandbox**
- Add a small CLI sandbox in `sandboxes/` that takes (sn, session-token-from-env) and prints the parsed `print_task_config`. Easier to iterate than the full UI.

### 6.3 ~~Open question~~ Resolved: cert exchange is already in the tree

Original concern: how does SnapSync go from "user is logged in via OAuth" to having `{ca, cert, key, clientId, host, port}` for a specific printer? Looked deeper and found we don't need to do this ourselves at all.

**Mechanism, fully traced through the source:**

- `src/libslic3r/AppConfig.hpp:33` — `struct DeviceInfo` with NLOHMANN_DEFINE_TYPE_INTRUSIVE has `ca, cert, key, clientId, ip, port, sn, userid, …` — exact AWS IoT mTLS fields.
- `src/libslic3r/AppConfig.cpp:1496/1529` — `save_device_info()` / `get_devices()` persist this struct to `Snapmaker_Orca.conf` under a top-level `"devices"` array.
- `src/slic3r/GUI/WebDeviceDialog.cpp` — modal webview dialog. Loads an embedded HTML page served via `LOCALHOST_URL:PAGE_HTTP_PORT/...`. That page's JavaScript (not C++) hits Snapmaker's REST API directly using the webview's HTTPS stack, receives `{ca, cert, key, clientId, ip, port}`, then bridges back into C++ via `sw_AddDevice` → `AppConfig::save_device_info()`.
- `src/slic3r/GUI/SSWCP.cpp:4724` — `sw_AddDevice` opens that dialog. `sw_GetLocalDevices` (line 4639) returns the persisted list. `sw_connect()` (line 4075) is **empty in both FullSpectrum and upstream Snapmaker** — by design; connection is initiated via `sw_create_mqtt_client` which is fed the already-persisted certs from JS.

**Note on the FullSpectrum vs upstream Snapmaker comparison:** initial assumption was that ratdoux had stripped cert-exchange code from FullSpectrum. Actually wrong — `sw_connect()` is empty in upstream Snapmaker too. The cert-exchange code intentionally lives only in the bundled webview's JavaScript (so OAuth cookie/session is handled cleanly by the webview), not in C++. FullSpectrum inherited the architecture intact.

**Practical consequence for SnapSync:** the flow becomes "ask AppConfig for the device; if none, open WebDeviceDialog (existing code) for the user to add one; once present, just use the stored certs." Zero new cert-exchange code, ~0 new REST endpoints to reverse-engineer.

---

## 7. Risks and unknowns

| Risk | Severity | Mitigation |
|---|---|---|
| `print_task_config` field names change in firmware update | Medium | Defensive parsing, fall back to "Generic" per slot, version-gate behind a build-info check |
| AWS IoT broker host changes (region migration) | Low | Re-fetch via cert-exchange endpoint each session — never hardcode the hostname |
| Snapmaker rate-limits cert exchange | Low | Cache certs on disk; refresh on TLS error only |
| Cert-exchange endpoint requires undocumented parameters (pairing code? device fingerprint?) | **Highest open risk** | First post-research task is to find the exact request shape — see §6.3 |
| ToS / EULA implications of calling a private API | Medium | This is the same API Snapmaker's official open-source slicer calls. We're not impersonating, not bypassing auth, not scraping — just substituting one open-source client for another against the user's own account. |
| Plugin / firmware update changes the wire protocol | Medium | Wire protocol is documented Moonraker JSON-RPC; only the Snapmaker-custom object payload is at risk (see row 1). |

---

## 8. What this design is **not**

- Not a reverse-engineered API — every byte on the wire is documented Moonraker JSON-RPC or AWS IoT MQTT.
- Not a Bambu Studio fork — we keep OrcaSlicer's existing AMS sync UI and just supply a new data source.
- Not LAN-dependent — works identically whether the user is on the same network as the printer or across the country.
- Not a replacement for Snapmaker Orca — users wanting the full Device tab still need it. SnapSync only adds the missing slicer-side sync button.

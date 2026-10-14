# Internet Visualizer

**A real-time world map of your Windows machine's live network connections**,
rendered with [raylib](https://github.com/raysan5/raylib).

Internet Visualizer finds your network adapters, reads the OS's live TCP/UDP
endpoint tables, resolves each remote IP asynchronously (reverse DNS + IP
geolocation), and draws the result as curved arcs and moving packets on a
world map, with hover/select details, a process list and per-adapter traffic
counters.

All data shown is real data read from the operating system. There are no
sample, simulated or fabricated connections, geolocations or statistics.

> **Screenshot placeholder** — run the app and drop a screenshot here
> (`docs/screenshot.png`) before publishing the repository.

```
   NetworkMonitor (worker thread)        GeoLocation (worker thread)
        |  NetSnapshot (mutex)                |  GeoInfo / hostname results
        v                                     v
   ConnectionTracker  ---> ConnectionArch objects <---+
        |                     |                       |
        +---------------------+-----------------------+
                              |
                              v
   MapRenderer + ConnectionArchRenderer (Raylib main thread)
```

The Raylib thread only renders and handles input; every blocking OS or network
call happens on one of the two worker threads.

---

## Features

- **Live connection arcs** on an equirectangular world map, with animated
  packets travelling sender → receiver when the direction is known.
- **Network adapter selector** — a dropdown next to the status-bar
  `interface:` label lists every usable adapter (Ethernet, Wi-Fi, VPN/virtual,
  VMware, Hyper-V, Bluetooth…). Pick one to scope monitoring to it, or leave it
  on **all adapters** (the default).
- **Per-interface traffic counters** (bytes in/out, and a rate derived between
  polls) for the selected adapter, from `GetIfTable2`.
- **Process list** of the connections on screen, with process name, PID,
  remote host and state; scrollable, and rows glow when that process has real
  traffic.
- **Connection list + details panel** with local/remote endpoints, state,
  direction and resolved host/domain.
- **Asynchronous geolocation and reverse DNS** (never blocks the UI), with an
  in-memory cache, rate limiting and a privacy filter.
- **Adjustable graticule** (keyboard `+` / `-` and an on-map toggle).
- **Live adapter changes** — adapters added/removed/renamed while the app runs
  are picked up; a disconnected selected adapter falls back gracefully.

---

## Requirements / OS support

- **Windows 10 or Windows 11, 64-bit.** The networking layer is built on the
  Windows IP Helper API (`iphlpapi`), Winsock (`ws2_32`) and WinHTTP
  (`winhttp`); there is currently no Linux/macOS build.
- No third-party runtime is required — the MinGW runtime is linked statically,
  so the shipped `.exe` depends only on Windows system DLLs.

### Permissions

- **No administrator rights are required** for the core features (adapters,
  interface counters, TCP/UDP tables, process names, geolocation).
- **Some per-connection TCP byte counters** (Windows TCP ESTATS) are only
  available when the process runs **elevated**; without elevation those rows
  show `n/a`. The process panel still pulses on connection churn so activity is
  visible, and interface-level totals remain real.
- **Installing the MSI to `Program Files` requires elevation** (a UAC prompt),
  which is normal for a per-machine installer.

---

## Installation

### From the MSI (recommended)

1. Download `InternetVisualizer-<version>.msi` from the Releases page.
2. Double-click it and approve the UAC prompt.
3. Launch **Internet Visualizer** from the Start Menu.

The installer places the executable and its `assets` folder in
`Program Files\Internet Visualizer`, adds a Start Menu shortcut, registers an
entry in *Apps & features*, and supports in-place **upgrades** (installing a
newer MSI replaces the older version) and a clean **uninstall**.

> The installer is currently **unsigned**. Windows SmartScreen may show a
> "Windows protected your PC" prompt the first time — choose *More info →
> Run anyway*, or sign the MSI with an Authenticode certificate of your own.

### From source

Requirements:

- Windows 10/11 (x64)
- **CMake ≥ 3.22**
- **MinGW-w64 GCC** (CLion's bundled toolchain works; GCC 15 tested). MSVC is
  not currently supported because the TCP-state header shim targets MinGW.
- Internet access on the first build (raylib is fetched from GitHub via
  `FetchContent` and pinned to a specific commit for reproducibility).

```sh
# configure + build (from the project root)
cmake -S . -B cmake-build-release -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release -j 8

# run from the build directory so assets/ resolves
cd cmake-build-release
./Internet_Visualizer.exe
```

---

## Controls

| Input                     | Action                                                   |
|---------------------------|----------------------------------------------------------|
| Left click (map)          | Select an arc                                            |
| Left click (list/process) | Select a connection / process row                        |
| Right click               | Clear selection                                          |
| Mouse wheel               | Scroll the connection list / process list / adapter menu |
| Status-bar `interface ▾`  | Open the adapter selector and choose an adapter          |
| `Esc`                     | Close the adapter menu                                   |
| `G`                       | Toggle geolocation lookups on/off                        |
| `+` / `-`                 | Widen / narrow the grid spacing (clamped to 10°–30°)     |
| Map `[-]` button          | Toggle the longitude/latitude grid on/off                |

---

## Network adapters and monitoring scope

The adapter dropdown sits immediately right of the `interface:` label in the
status bar. It shows the currently monitored adapter, opens **upwards** so it
never leaves the window, is scrollable, and lists real adapters with their
type, IPv4 address and up/down state. Virtual, VPN, and disconnected adapters
are included; the software loopback pseudo-interface is excluded because it
carries no external traffic.

Selecting an adapter switches:

- the **interface counters** and rates to that adapter, and
- the **connection set** shown on the map, in the lists and in the process
  panel.

> **How connections are attributed to an adapter (honest note).** Windows
> never reports *which interface* a TCP/UDP connection uses through the
> `GetExtendedTcpTable` API. Internet Visualizer attributes a connection to the
> selected adapter only when the connection's **local address equals a unicast
> address of that adapter** — the one signal the OS actually provides. Sockets
> with an unspecified local address (`0.0.0.0` / `::`, e.g. listeners) cannot be
> tied to a specific interface and are therefore hidden when a single adapter is
> selected. The default **all adapters** mode shows everything and is
> machine-wide, not per-interface.

Adapter selection is keyed by the adapter's **stable GUID** (`AdapterName`), so
it survives renames and reconnects. If the selected adapter disappears (unplug,
VPN drop), the app falls back to the primary interface and re-selects the
adapter automatically if it returns.

---

## How the data is gathered (all native APIs, no shell commands)

| Data                     | Source                                                                  |
|--------------------------|-------------------------------------------------------------------------|
| Primary interface        | `GetIpForwardTable2` — interface owning the lowest-metric default route |
| Adapter list / addresses | `GetAdaptersAddresses` (+ `GetIpInterfaceEntry` for the metric)         |
| Traffic counters/rates   | `GetIfTable2` `InOctets`/`OutOctets`, differenced between polls         |
| TCP endpoints + PID      | `GetExtendedTcpTable` (`TCP_TABLE_OWNER_PID_ALL`, v4 + v6)              |
| UDP endpoints + PID      | `GetExtendedUdpTable` (`UDP_TABLE_OWNER_PID`)                          |
| Process names            | `OpenProcess` + `QueryFullProcessImageNameW` (LRU-cached)              |
| Per-connection TCP bytes | Windows TCP ESTATS (`GetPerTcpConnectionEStats`, v4 + v6) when available |
| Reverse DNS              | `getnameinfo` (`NI_NAMEREQD`) on the geo worker thread, cached         |
| Public IP + geolocation  | WinHTTP HTTPS to the configured provider, on the geo worker thread     |

### Direction inference

TCP direction is inferred from the endpoint state plus whether the local port
appears in the listener table (collected for **both** address families before
any endpoint is classified):

- `SYN_SENT`, `FIN_WAIT*`, `TIME_WAIT` → **outbound** (we opened it)
- `SYN_RCVD`, `LAST_ACK` → **inbound** (we were the listener)
- `ESTABLISHED`, `CLOSE_WAIT` → inbound if the local port is a listening port,
  otherwise outbound
- `LISTEN` → listener (no arc; it has no remote peer)

UDP rows on Windows expose only the local bind, so they carry no remote
endpoint, direction `unknown`, and never produce an arc.

---

## Geolocation and privacy

The default provider is **`https://ipwho.is`** (no API key required). To use a
compatible provider, call:

```c
GeoLocation_SetProvider(geo, "https://api.example.com/geo");
```

A compatible endpoint answers `GET /<ip-address>` with JSON containing `ip`,
`country`, `city`, `latitude`, `longitude` (flat `lat`/`lon` also accepted),
and optionally `connection.isp`/`connection.asn` and `timezone.id`. `GET /`
(no argument) must return the caller's own IP in the `ip` field; that response
anchors the local endpoint to your public egress (a NAT approximation).

Privacy: **only globally routable unicast addresses are ever sent** to the
provider — private, loopback, link-local, CGNAT, multicast and reserved
addresses are classified locally and never leave the machine. The worker
enforces a minimum gap between requests, backs off on HTTP 429, and caches
results in memory (2048 slots). Press `G` to disable lookups entirely at
runtime (arcs for already-resolved IPs remain).

---

## Map projection

`assets/wm.png` is a Natural Earth land mask in plate-carrée (equirectangular)
projection, with exact geographic edges longitude **-180° to +180°** and
latitude **+90° to -90°**. `MapProjection.h` declares these asset bounds and the
viewport preserves their 2:1 aspect ratio. The map, grid, arcs, endpoint
markers and cursor readout all share the same projection transform. Connections
that cross the antimeridian are drawn on both sides of the seam.

---

## Repository layout

| Path                               | Responsibility                                          |
|------------------------------------|---------------------------------------------------------|
| `main.cpp`                         | C++ entry point: layout, input, main loop, adapter UI, wiring |
| `src/NetTypes.h/.c`                | Shared value types (`NetAddr`, `NetConnection`, keys, formatting) |
| `src/NetPlatform.h/.c`             | Thin mutex/thread/time wrappers                         |
| `src/NetworkMonitor.h/.c`          | Worker thread polling the OS; adapter enumeration/selection; publishes `NetSnapshot`s |
| `src/ConnectionArch.h/.c`          | One tracked connection + its endpoints over time        |
| `src/ConnectionTracker.h/.c`       | Reconciles snapshots with the arch set; stable IDs, expiry |
| `src/GeoLocation.h/.c`             | Geo worker thread, WinHTTP client, JSON parsing, caches |
| `src/MapProjection.h/.c`           | The shared lon/lat ↔ screen transform                   |
| `src/MapRenderer.h/.c`             | Map image, graticule, cursor readout                    |
| `src/ConnectionArchRenderer.h/.c`  | Arcs, markers, connection list, details panel           |
| `src/UiText.h/.c`                  | Font loading + overflow-safe text helpers               |
| `assets/`                          | Map image, bundled JetBrains Mono fonts, `OFL.txt`, app icon |
| `Include/stb_image.h`              | Vendored single-header image loader (raylib dependency) |
| `packaging/`                       | WiX installer authoring (`InternetVisualizer.wxs`) + `build-msi.ps1` |
| `CMakeLists.txt`                   | Build definition (C11 / C++20, MinGW)                    |

---

## Building the Windows installer

The MSI is produced with the [WiX Toolset](https://wixtoolset.org/) v7:

```powershell
# one-time (WiX v7 binary is subject to the OSMF EULA; free below US$10k/yr revenue)
dotnet tool install --global wix --add-source https://api.nuget.org/v3/index.json
wix eula accept wix7      # or let build-msi.ps1 pass -acceptEula wix7

# after a Release build exists in cmake-build-release/
powershell -ExecutionPolicy Bypass -File packaging/build-msi.ps1 -Version 1.0.0
# -> packaging/out/InternetVisualizer-1.0.0.msi
```

See `docs/RELEASE.md` for the full release checklist.

---

## Troubleshooting

- **The window is blank / fonts look wrong.** Run the executable from the
  folder that contains the `assets` directory (its own directory, or the build
  directory). The MSI and the CMake build both copy/nest `assets` next to the
  exe.
- **No connections appear.** Make sure you have network activity; the app shows
  only real OS connections. VPN/proxy adapters may carry the traffic you expect
  on another adapter — try the **all adapters** view.
- **A process row is `pid:<n>`.** The process could not be opened with
  `PROCESS_QUERY_LIMITED_INFORMATION` (protected/system process); the PID is
  still correct.
- **`n/a` byte counters.** Per-connection TCP ESTATS requires elevation; run as
  administrator for those numbers.
- **Build fails fetching raylib.** The first configure needs internet access to
  clone the pinned raylib commit; retry with a working connection.
- **`cmake_minimum_required` error.** You need CMake ≥ 4.2.
- **MSVC build fails (`TCP_STATE_*` undeclared).** Use the MinGW toolchain
  (`-G "MinGW Makefiles"`); the header shim targets MinGW.

---

## Limitations

- **Windows TCP ESTATS counters** are queried per connection and may be
  unavailable for unsupported, protected or short-lived connections (and
  require elevation). UDP endpoint tables provide no per-socket counters, so
  UDP byte activity is not attributed. Interface-level totals remain real
  `GetIfTable2` counters.
- **UDP has no remote peer on Windows**, so UDP sockets appear in the list but
  never produce arcs.
- **Per-adapter connection attribution** relies on the local address (see
  above); sockets with an unspecified local address cannot be attributed.
- **Geolocation is approximate** (city/region level at best, often the ISP's
  POP rather than the true peer) and depends on a third-party service. Offline
  use degrades gracefully: connections still appear, arcs just wait for data.
- **The local endpoint is anchored to your public IP** (the provider's "self"
  lookup), a NAT approximation. With a VPN/proxy active, arcs originate from the
  VPN exit's location.
- **IPv6**: endpoints are enumerated for both families; unique-local and
  link-local addresses are never sent for geolocation.
- The connection tables are capped at 512 rows (`NET_MAX_CONNS`) and adapters at
  24 (`NET_MAX_IFACES`); extremely large tables are truncated.

---

## Clean shutdown

On window close the main thread signals both worker threads, joins them, and
then frees every structure — no detached threads and no leaked OS handles.

---

## License

Internet Visualizer is released under the **MIT License** — see
[`LICENSE`](LICENSE).

Bundled/used third-party components and their licenses are documented in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md): raylib (zlib), `stb_image.h`
(public domain / MIT) and the JetBrains Mono fonts (SIL OFL 1.1, full text in
`assets/OFL.txt`).

## Contributing

Issues and pull requests are welcome. Please keep the
"no fabricated data" principle: never display a connection, geolocation or
statistic the OS did not actually provide. See [`CHANGELOG.md`](CHANGELOG.md)
for what has changed.

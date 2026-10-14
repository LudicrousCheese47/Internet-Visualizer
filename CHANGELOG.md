# Changelog

All notable changes to Internet Visualizer are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-10-11

First public release.

### Added

- **Network adapter selector** – a dropdown next to the status-bar `interface:`
  indicator lists every usable Windows adapter (Ethernet, Wi-Fi, virtual/VPN,
  VMware, Hyper-V…) with its type, IP and up/down state.
- **Per-adapter monitoring** – selecting an adapter switches the traffic
  counters/rates *and* the observed connection set (by local-IP attribution)
  to that adapter. The default view is "all adapters", which preserves the
  original whole-machine behavior.
- **Live adapter changes** – adapters added, removed, disconnected or renamed
  while running are re-enumerated every poll; if the selected adapter
  disappears the app falls back to the primary interface and re-selects the
  adapter automatically if it comes back (e.g. a USB adapter is re-plugged).
- Windows installer (`InternetVisualizer-1.0.0.msi`, WiX): installs the exe,
  fonts and map asset into `Program Files`, creates a Start Menu shortcut,
  supports upgrade and clean uninstall.
- `LICENSE` (MIT), `THIRD_PARTY_NOTICES.md`, `assets/OFL.txt` (JetBrains Mono
  font license), `.gitignore`, CI workflow for Windows builds.
- This `CHANGELOG.md` and a `docs/RELEASE.md` release checklist.

### Changed

- Map grid step now adjustable (`+` / `-`) and clamped to 10–30 degrees; the
  `-` button on the map toggles the graticule on/off.
- Connections crossing the antimeridian (360° seam) are now drawn on the world
  map instead of running off the edge.
- Connection rows label websites by provider domain → reverse-DNS hostname →
  process name, and the map pulses on real byte deltas or connection churn.

### Fixed

- **Arcs not spawning (critical).** Two geolocation bugs meant no arc could ever
  be drawn when the provider was unreachable:
  - The HTTP 429 rate-limit backoff was immediately overwritten by the 0.4 s
    minimum request gap, so the worker hammered the provider every 0.4 s and
    earned a permanent rate limit. The backoff is now a floor that the minimum
    gap can never shorten.
  - The "self" lookup (which anchors the local endpoint of every arc) ran
    exactly once at startup. A single 429 left `selfValid` false forever, so
    even perfectly resolved remote endpoints produced no arc. It now retries
    every 30 s until it succeeds.
- **Automatic geolocation provider failover.** If the configured provider keeps
  answering 429/403, the worker now switches to a fallback
  (`freeipapi.com`, whose response fields are also accepted by the JSON parser)
  so coordinates keep resolving instead of the map staying empty.
- Arc geometry for routes that cross the date line (e.g. North America →
  Asia) is now correct.

### Removed

- Stray `build/` output directory and the obsolete `search_calls.js` scratch
  script.

## Prior history

Development of the core visualization (raylib world map, live TCP/UDP
connections, geolocation, process list) happened before this changelog
existed; those changes are not separately recorded.
# Release Checklist — Internet Visualizer 1.0.0

Version: 1.0.0 (2026-10-11)
Installer: `packaging/out/InternetVisualizer-1.0.0.msi` (737,280 bytes)
Executable: `cmake-build-release/Internet_Visualizer.exe` (1,526,268 bytes, built 2026-10-11 21:36)
Build toolchain: MinGW Makefiles + GCC (CMake 3.22+, C11 / C++20)

## Completed
- [x] Network adapter dropdown implemented in `main.cpp` (drawn beside status-bar `interface:`)
- [x] Dynamic adapter enumeration via `GetAdaptersAddresses` / `GetIfTable2` (`src/NetworkMonitor.c`)
- [x] Adapter selection switches monitoring via `NetworkMonitor_SetSelectedInterface` (stable GUID id)
- [x] Per-adapter traffic counters and rate estimation for selected interface
- [x] Connection attribution by local-address match to selected adapter (`ApplyIfaceFilter`)
- [x] Live adapter add/remove/disconnect handled; fallback to primary when selected disappears
- [x] Thread-safe snapshot publish / consume (mutex-guarded `NetSnapshot`)
- [x] Clean shutdown (worker joined, handles freed, mutex destroyed)
- [x] Windows MSI (`packaging/InternetVisualizer.wxs` + `build-msi.ps1`) — verified via `msiexec /a`
- [x] MSI contains exe + assets (`wm.png`, fonts, `OFL.txt`, `icon.ico` used for shortcut)
- [x] README.md, CHANGELOG.md, LICENSE (MIT), THIRD_PARTY_NOTICES.md, `.gitignore`
- [x] Build passes (`cmake --build cmake-build-release`)
- [x] Functional adapter test passed (enumeration, selection, switching, fallback, worker lifecycle)

## Verified
- Adapter enumeration shows 10 interfaces (Ethernet, Wi-Fi, VMware, Hyper-V vEthernet, Bluetooth, Virtual)
- Selecting adapter updates `selectedIsSpecific`, counters reset (`rateIn = -1` first poll), connection set filtered
- Unknown adapter falls back with status message (`not present - monitoring primary`)
- Empty id resets to `all adapters`
- Worker thread handles live selection changes without restart
- MSI installs to `Program Files\Internet Visualizer`, creates Start Menu shortcut,
  supports upgrade (`MajorUpgrade`) and uninstall with `RemoveFolder`
- No secrets or absolute user paths in tracked source; encoding valid UTF-8

## Not completed / missing
- [ ] `.github/workflows/` CI workflow for repeatable Windows builds (changelog references one; it does not exist in repo root — only inside raylib `_deps`)
- [ ] `docs/RELEASE.md` previously referenced by README / CHANGELOG (created in this session)
- [ ] MSI code-signing / Authenticode certificate (documented as unsigned; Windows SmartScreen expected)
- [ ] Screenshot asset (`docs/screenshot.png` — placeholder noted in README)

## Before a public GitHub release
1. Confirm adapter selection works end-to-end on a clean Windows 10/11 machine
2. Confirm arcs render with geolocation resolved (depends on `https://ipwho.is` / network)
3. Sign MSI if a certificate is obtained; otherwise document SmartScreen expectation
4. Create `.github/workflows/windows.yml` if CI repeatability is needed
5. Add `docs/screenshot.png` and replace README placeholder
6. Authorize any public release / push explicitly (per instructions — not done here)

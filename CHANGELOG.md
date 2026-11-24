# Changelog

All notable changes to this project are documented here.

## [1.0.1] - 2026-11-24

### Fixed

- Fixed a startup crash (`0xC0000005`) in games that run with Windows
  Application Compatibility layers. `AcGenral.dll` calls
  `SetAppCompatStringPointer` before the proxy has resolved the real DXGI
  export; the export is now a null-safe wrapper that defers and replays the
  call after initialization instead of jumping through a null slot.
- Fixed the DXGI factory hook for `CreateSwapChainForComposition`. It patched
  vtable slot 24 (`IDXGIFactory4::EnumWarpAdapter`) instead of slot 21, which
  could break adapter enumeration in Unity games.
- Debug logging is now compiled only into diagnostic builds
  (`VSYNC_DEBUG_LOG`); release DLLs contain no logging code.

### Added

- `build_debug.bat` for diagnostic DLLs that log to
  `%TEMP%\vsync_proxy_debug.log.<pid>`.
- Regression coverage for `SetAppCompatStringPointer`,
  `IDXGIFactory4::EnumWarpAdapter`, and D3D9 `COPY` swap-effect interval
  forcing.

## [1.0.0] - 2026-11-17

### Added

- DXGI proxy for DirectX 10, 11 and 12 with hooks for `Present` and
  `Present1`.
- Hooks for `CreateDXGIFactory`, `CreateDXGIFactory1` and
  `CreateDXGIFactory2`, including all swap-chain creation methods.
- Direct3D 9 and 9Ex proxy covering device creation, additional swap chains,
  `Reset`, `ResetEx`, `Present` and `PresentEx`.
- True forwarding of every standard `dxgi.dll` (19) and `d3d9.dll` (17)
  export through naked jump stubs.
- Optional `vsync.ini` configuration with a clamped `SyncInterval` of 1 to 4.
- x64 and x86 builds with static runtime linkage and stripped symbols.
- End-to-end probes for both APIs and both architectures.
- GitHub Actions workflow that builds all four DLLs on every push.

### Fixed

- Direct3D 9Ex creation hooks are now installed even when the application
  calls `Direct3DCreate9` before `Direct3DCreate9Ex`.

[1.0.0]: https://github.com/abhaychoudhary120/universal-vsync-proxy/releases/tag/v1.0.0
[1.0.1]: https://github.com/abhaychoudhary120/universal-vsync-proxy/releases/tag/v1.0.1

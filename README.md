# Universal V-Sync Proxy

**Force V-Sync in DirectX 10/11/12 and Direct3D 9 games — without changing a single visual setting.**

[![Build](https://github.com/abhaychoudhary120/universal-vsync-proxy/actions/workflows/build.yml/badge.svg)](https://github.com/abhaychoudhary120/universal-vsync-proxy/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-informational)

Universal V-Sync Proxy is a set of four drop-in DLL proxies that enforce
vertical sync at the presentation layer. Resolution, textures, shaders,
anti-aliasing, mipmaps, render targets and every other rendering setting are
passed through untouched.

## Download

Get the latest prebuilt package from the
[**Releases page**](https://github.com/abhaychoudhary120/universal-vsync-proxy/releases/latest).

| DLL | Architecture | Games / API |
| --- | --- | --- |
| `x64/dxgi.dll` | 64-bit | DirectX 10, 11, 12 |
| `x86/dxgi.dll` | 32-bit | DirectX 10, 11, 12 |
| `x64/d3d9.dll` | 64-bit | Direct3D 9 / 9Ex |
| `x86/d3d9.dll` | 32-bit | Direct3D 9 / 9Ex |

Each DLL is standalone, statically linked and stripped. Only Windows system
DLLs are imported — no Visual C++ redistributable, no extra runtime.

## Quick start

1. **Pick the right DLL.** Match both the API and the game's architecture.
   A 64-bit Direct3D 9 game needs `x64/d3d9.dll`; a 32-bit DXGI game needs
   `x86/dxgi.dll`.
2. **Copy it next to the game executable** and keep the original file name
   (`dxgi.dll` or `d3d9.dll`). Windows loads it before the system copy.
3. **Start the game.** V-Sync is now requested on every present.

Optional: copy `vsync.ini.example` to the same folder as the DLL, rename it to
`vsync.ini`, and adjust the interval.

To uninstall, delete the proxy DLL.

> Use only one proxy per API per game. Do not mix x64 and x86 binaries. If a
> launcher changes the working directory, place the DLL next to the real game
> executable or inject it with your mod loader.

## Features

- **Zero visual changes** — only presentation timing is modified.
- **DXGI:** forces a positive `SyncInterval` and removes
  `DXGI_PRESENT_ALLOW_TEARING`.
- **D3D9:** forces `D3DPRESENT_INTERVAL_ONE` on device creation, additional
  swap chains, `Reset` and `ResetEx`.
- **Complete forwarding** — all 19 `dxgi.dll` and all 17 `d3d9.dll` system
  exports are forwarded to the genuine DLL.
- **Zero per-frame overhead** — no allocation, file I/O or locks in `Present`.
- **x64 and x86** builds for modern and legacy games.
- **Optional multiplier** — `SyncInterval=1..4` for half, third or quarter
  refresh-rate pacing.

## Configuration

The proxy reads `vsync.ini` once, next to the DLL, during process attach.

```ini
[VSync]
; 1 = normal V-Sync, 2 = half refresh rate, 3/4 = thirds/quarters.
SyncInterval=1
```

| Value | Behavior |
| --- | --- |
| missing file / key | defaults to `1` |
| below `1` | clamped to `1` |
| above `4` | clamped to `4` |

DXGI games that request `SyncInterval=0` are raised to the configured value;
values already greater than zero are left alone. D3D9 always uses
`D3DPRESENT_INTERVAL_ONE`.

## How it works

### DXGI (`dxgi.dll`)

The proxy exports the real `CreateDXGIFactory`, `CreateDXGIFactory1` and
`CreateDXGIFactory2`, then hooks the returned factory's `CreateSwapChain`,
`CreateSwapChainForHwnd`, `CreateSwapChainForCoreWindow` and
`CreateSwapChainForComposition` methods. Every returned swap chain gets
`Present` (vtable slot 8) and `Present1` (slot 22) hooked. The hooks rewrite
the sync interval, clear the tearing flag and call the original method.

This also covers `D3D11CreateDeviceAndSwapChain`, because D3D11 creates its
factory through the same DXGI exports.

### Direct3D 9 (`d3d9.dll`)

The proxy exports `Direct3DCreate9`, `Direct3DCreate9Ex`,
`Direct3DCreate9On12` and `Direct3DCreate9On12Ex`. It hooks
`IDirect3D9::CreateDevice` / `IDirect3D9Ex::CreateDeviceEx`, plus
`CreateAdditionalSwapChain`, `Reset` and `ResetEx` on the device vtable.
`PresentationInterval` is forced to `D3DPRESENT_INTERVAL_ONE` before the real
call, so D3D9 performs native V-Sync timing. `Present` and `PresentEx` are
hooked as pass-throughs so every device path is covered.

### Forwarding

The genuine system DLL is loaded once from `%SystemRoot%\System32`
(`SysWOW64` for 32-bit processes) using an absolute path. Standard exports are
forwarded through naked jump stubs, so games that import functions other than
the device creators keep working. The proxy refuses to initialize if it cannot
load the real DLL or resolve the creation exports.

## Compatibility

- Windows 10 and Windows 11.
- 64-bit and 32-bit (WOW64) processes.
- DirectX 10, 11, 12 and Direct3D 9 / 9Ex.
- Games that use Windows Application Compatibility layers (`RUNASADMIN`,
  `HIGHDPIAWARE`, `DISABLEDXMAXIMIZEDWINDOWEDMODE`); compat exports are safe
  to call during loader initialization.
- Tested end-to-end on Windows 11 with x64 and x86 DXGI/D3D9 probes and with
  Unity games running under Application Compatibility layers.
- Not covered: Direct3D 8 and earlier, OpenGL, Vulkan, and games that do not
  present through DXGI or D3D9.

## Build from source

Requirements: 64-bit and 32-bit [MinGW-w64](https://www.mingw-w64.org/)
toolchains. The WinLibs builds are known to work.

```bat
git clone https://github.com/abhaychoudhary120/universal-vsync-proxy.git
cd universal-vsync-proxy
build.bat
```

`build.bat` checks common MinGW installation paths, verifies each compiler's
target architecture, and writes:

```
build\x64\dxgi.dll
build\x64\d3d9.dll
build\x86\dxgi.dll
build\x86\d3d9.dll
```

The GitHub Actions workflow builds the same four DLLs on every push using
MSYS2 MinGW-w64 packages.

## Verification

The `tests` folder contains end-to-end probes:

- `tests/dxgi_probe.cpp` creates a D3D11 device and both flip and legacy swap
  chains, checks the factory and `Present`/`Present1` hooks, calls
  `Present(0, DXGI_PRESENT_ALLOW_TEARING)`, covers
  `D3D11CreateDeviceAndSwapChain`, and calls the AppCompat export
  `SetAppCompatStringPointer` to catch early-call regressions.
- `tests/d3d9_probe.cpp` creates base and Ex devices with
  `D3DPRESENT_INTERVAL_IMMEDIATE`, then verifies that creation, additional
  swap chains, `Reset` and `ResetEx` rewrite the interval to
  `D3DPRESENT_INTERVAL_ONE`. It deliberately creates the base device before
  `Direct3DCreate9Ex` to cover both entry-point orders.

Build a probe, run it from a folder containing the matching proxy DLL, and
expect `RESULT: PASS`:

```bat
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ ^
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX ^
    tests\dxgi_probe.cpp -o build\verify\x64\dxgi_probe.exe ^
    -ldxgi -ld3d11 -luser32
```

All four combinations (x64/x86 × DXGI/D3D9) pass on the development machine.
Export parity against the real system DLLs is exact: 19/19 for DXGI and 17/17
for D3D9.

## Troubleshooting

**The game still tears.** Confirm the game actually uses DXGI or D3D9. Games
using Vulkan, OpenGL or Direct3D 8 are not intercepted. Also check that the
game executable, not its launcher, is the process loading the DLL.

**The game does not start.** Make sure the architecture matches the game
(32-bit vs 64-bit) and that only one proxy per API is present. Remove the DLL
to confirm it is the cause.

**Anti-cheat or antivirus complains.** Proxy DLLs and unsigned binaries can
trigger heuristics. Use the project only where permitted by the game's rules
and your local policy.

**A launcher uses a different working directory.** Place the DLL beside the
real game executable, or use the launcher's injection support. Copying to the
launcher folder does not help if it starts the game from elsewhere.

## Limitations

- Direct3D 8 and earlier, OpenGL and Vulkan are not covered.
- The proxy forces presentation timing, not a specific frame rate cap.
- Fullscreen-exclusive mode and driver-level overrides can still change the
  effective result.
- `vsync.ini` is global per DLL instance, not per process or per swap chain.
- Some anti-cheat systems block proxy DLLs entirely.

## License

[MIT](LICENSE) © 2026 Abhay Choudhary

Not affiliated with Microsoft. DirectX, Direct3D and Windows are trademarks of
Microsoft Corporation. Use at your own risk.

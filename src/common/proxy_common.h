#pragma once

#include <windows.h>
#include <cstddef>

namespace vsync_proxy {

// The optional vsync.ini value is intentionally small: DXGI treats the
// interval as a number of vblank periods, and values above 4 are not useful
// for a V-Sync forcer.
constexpr unsigned kDefaultSyncInterval = 1;
constexpr unsigned kMaxSyncInterval = 4;

struct Config {
    unsigned sync_interval = kDefaultSyncInterval;
};

// Reads vsync.ini next to this DLL exactly once, during DLL_PROCESS_ATTACH.
// Missing file / invalid values fall back to 1.
void LoadConfig(HMODULE self, Config& config) noexcept;

// Loads the real system DLL from the architecture-appropriate System32
// (SysWOW64 for 32-bit processes) using an absolute path.
HMODULE LoadSystemLibrary(const wchar_t* filename) noexcept;

void* ResolveExport(HMODULE module, const char* name) noexcept;

// Replaces one function pointer in a COM vtable. VirtualProtect is only used
// during hook installation, never from Present.
bool PatchVtableSlot(void** vtable, std::size_t index, void* replacement,
                     void** original) noexcept;

inline void** VtableOf(void* object) noexcept {
    return *reinterpret_cast<void***>(object);
}

// Serializes hook installation. This is deliberately not used by Present.
void LockHookInstall() noexcept;
void UnlockHookInstall() noexcept;

}  // namespace vsync_proxy

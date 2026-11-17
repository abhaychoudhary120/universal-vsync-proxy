#include "common/proxy_common.h"

#include <cstring>

namespace {

constexpr std::size_t kMaxPath = 32768;
LONG g_hook_install_lock = 0;

std::size_t CopyDirectoryPrefix(wchar_t* path, std::size_t length) noexcept {
    while (length > 0 && path[length - 1] != L'\\' && path[length - 1] != L'/') {
        --length;
    }
    return length;
}

}  // namespace

namespace vsync_proxy {

void LoadConfig(HMODULE self, Config& config) noexcept {
    config.sync_interval = kDefaultSyncInterval;

    wchar_t path[kMaxPath];
    const DWORD length = GetModuleFileNameW(
        self, path, static_cast<DWORD>(kMaxPath));
    if (length == 0 || length >= kMaxPath) {
        return;
    }

    const std::size_t directory = CopyDirectoryPrefix(path, length);
    static constexpr wchar_t kConfigName[] = L"vsync.ini";
    constexpr std::size_t kConfigNameLength =
        (sizeof(kConfigName) / sizeof(kConfigName[0])) - 1;
    if (directory + kConfigNameLength + 1 > kMaxPath) {
        return;
    }

    std::memcpy(path + directory, kConfigName, sizeof(kConfigName));

    UINT value = GetPrivateProfileIntW(
        L"VSync", L"SyncInterval", kDefaultSyncInterval, path);
    if (value < 1) {
        value = 1;
    } else if (value > kMaxSyncInterval) {
        value = kMaxSyncInterval;
    }
    config.sync_interval = value;
}

HMODULE LoadSystemLibrary(const wchar_t* filename) noexcept {
    wchar_t path[kMaxPath];
    const UINT length = GetSystemDirectoryW(path, static_cast<UINT>(kMaxPath));
    if (length == 0 || length >= kMaxPath) {
        return nullptr;
    }

    std::size_t position = length;
    if (position + 1 >= kMaxPath) {
        return nullptr;
    }
    if (path[position - 1] != L'\\' && path[position - 1] != L'/') {
        path[position++] = L'\\';
    }

    const int filename_length = lstrlenW(filename);
    if (filename_length <= 0 ||
        position + static_cast<std::size_t>(filename_length) >= kMaxPath) {
        return nullptr;
    }

    std::memcpy(path + position, filename,
                (static_cast<std::size_t>(filename_length) + 1) *
                    sizeof(wchar_t));

    return LoadLibraryW(path);
}

void* ResolveExport(HMODULE module, const char* name) noexcept {
    return module == nullptr ? nullptr
                             : reinterpret_cast<void*>(
                                   GetProcAddress(module, name));
}

bool PatchVtableSlot(void** vtable, std::size_t index, void* replacement,
                     void** original) noexcept {
    if (vtable == nullptr || replacement == nullptr) {
        return false;
    }

    void** slot = vtable + index;
    DWORD previous_protection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE,
                        &previous_protection)) {
        return false;
    }

    const void* previous_value = *slot;
    *slot = replacement;

    DWORD ignored = 0;
    const BOOL restored =
        VirtualProtect(slot, sizeof(*slot), previous_protection, &ignored);
    if (original != nullptr) {
        *original = const_cast<void*>(previous_value);
    }
    return restored != FALSE;
}

void LockHookInstall() noexcept {
    while (InterlockedCompareExchange(&g_hook_install_lock, 1, 0) != 0) {
        YieldProcessor();
    }
}

void UnlockHookInstall() noexcept {
    InterlockedExchange(&g_hook_install_lock, 0);
}

}  // namespace vsync_proxy

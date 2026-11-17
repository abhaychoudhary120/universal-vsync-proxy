// Direct3D 9 proxy.
//
// V-Sync is enforced natively by forcing D3DPRESENT_INTERVAL_ONE on the swap
// chain at device creation, additional-swap-chain creation and reset time.
// There is no CPU spin-wait and no rendering state is touched.

#include <windows.h>
#include <d3d9.h>

#include "common/forwarder.h"
#include "common/proxy_common.h"
#include "d3d9_forwarders.h"

// ---------------------------------------------------------------------------
// Forwarded exports.
// ---------------------------------------------------------------------------
extern "C" {
#define VS_DEFINE_FORWARD_PTR(name) void* g_fwd_##name = nullptr;
VS_D3D9_FORWARD_LIST(VS_DEFINE_FORWARD_PTR)
#undef VS_DEFINE_FORWARD_PTR
}

#define VS_DEFINE_STUB(name) VS_DEFINE_FORWARD_STUB(name)
VS_D3D9_FORWARD_LIST(VS_DEFINE_STUB)
#undef VS_DEFINE_STUB

namespace {

using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);
using Direct3DCreate9ExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
using Direct3DCreate9On12Fn = IDirect3D9*(WINAPI*)(UINT, void*, UINT);
using Direct3DCreate9On12ExFn =
    HRESULT(WINAPI*)(UINT, void*, UINT, IDirect3D9Ex**);

using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
    IDirect3DDevice9**);
using CreateDeviceExFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
    D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);

using CreateAdditionalSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**);
using ResetFn =
    HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetExFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
using PresentExFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*,
    DWORD);

// IDirect3D9 / IDirect3D9Ex vtable slots.
constexpr std::size_t kDirect3DCreateDevice = 16;
constexpr std::size_t kDirect3DCreateDeviceEx = 20;

// IDirect3DDevice9 / IDirect3DDevice9Ex vtable slots.
constexpr std::size_t kDeviceCreateAdditionalSwapChain = 13;
constexpr std::size_t kDeviceReset = 16;
constexpr std::size_t kDevicePresent = 17;
constexpr std::size_t kDevicePresentEx = 121;
constexpr std::size_t kDeviceResetEx = 132;

constexpr std::size_t kMaxDeviceHookSets = 8;

struct DeviceHookSet {
    void** vtable = nullptr;
    CreateAdditionalSwapChainFn create_additional_swap_chain = nullptr;
    ResetFn reset = nullptr;
    PresentFn present = nullptr;
    PresentExFn present_ex = nullptr;
    ResetExFn reset_ex = nullptr;
};

vsync_proxy::Config g_config;
DeviceHookSet g_device_hooks[kMaxDeviceHookSets] = {};

Direct3DCreate9Fn g_create_d3d9 = nullptr;
Direct3DCreate9ExFn g_create_d3d9_ex = nullptr;
Direct3DCreate9On12Fn g_create_d3d9_on12 = nullptr;
Direct3DCreate9On12ExFn g_create_d3d9_on12_ex = nullptr;

void** g_d3d9_vtable = nullptr;
CreateDeviceFn g_orig_create_device = nullptr;
CreateDeviceExFn g_orig_create_device_ex = nullptr;

void InstallDeviceHooks(IDirect3DDevice9* device) noexcept;
HRESULT STDMETHODCALLTYPE CreateDeviceHook(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
    IDirect3DDevice9**);
HRESULT STDMETHODCALLTYPE CreateDeviceExHook(
    IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
    D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);

// The base creation slot is patched on every distinct vtable seen. The real
// implementation behind the slot is shared, so the first captured pointer is
// reused for later vtables.
void PatchCreateDeviceSlot(void** vtable) noexcept {
    if (vtable == nullptr ||
        vtable[kDirect3DCreateDevice] ==
            reinterpret_cast<void*>(&CreateDeviceHook)) {
        return;
    }
    void* previous = nullptr;
    if (vsync_proxy::PatchVtableSlot(
            vtable, kDirect3DCreateDevice,
            reinterpret_cast<void*>(&CreateDeviceHook), &previous)) {
        if (g_orig_create_device == nullptr) {
            g_orig_create_device = reinterpret_cast<CreateDeviceFn>(previous);
        }
        if (g_d3d9_vtable == nullptr) {
            g_d3d9_vtable = vtable;
        }
    }
}

void PatchCreateDeviceExSlot(void** vtable) noexcept {
    if (vtable == nullptr ||
        vtable[kDirect3DCreateDeviceEx] ==
            reinterpret_cast<void*>(&CreateDeviceExHook)) {
        return;
    }
    void* previous = nullptr;
    if (vsync_proxy::PatchVtableSlot(
            vtable, kDirect3DCreateDeviceEx,
            reinterpret_cast<void*>(&CreateDeviceExHook), &previous) &&
        g_orig_create_device_ex == nullptr) {
        g_orig_create_device_ex =
            reinterpret_cast<CreateDeviceExFn>(previous);
    }
}

void InstallDirect3DHooks(IUnknown* direct3d) noexcept {
    if (direct3d == nullptr) {
        return;
    }

    void** vtable = vsync_proxy::VtableOf(direct3d);

    IDirect3D9Ex* direct3d_ex = nullptr;
    const bool has_ex =
        SUCCEEDED(direct3d->QueryInterface(
            __uuidof(IDirect3D9Ex),
            reinterpret_cast<void**>(&direct3d_ex))) &&
        direct3d_ex != nullptr;

    vsync_proxy::LockHookInstall();
    PatchCreateDeviceSlot(vtable);
    if (has_ex) {
        void** ex_vtable = vsync_proxy::VtableOf(direct3d_ex);
        if (ex_vtable != vtable) {
            PatchCreateDeviceSlot(ex_vtable);
        }
        PatchCreateDeviceExSlot(ex_vtable);
    }
    vsync_proxy::UnlockHookInstall();

    if (direct3d_ex != nullptr) {
        direct3d_ex->Release();
    }
}

template <std::size_t Slot>
HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChainHook(
    IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters,
    IDirect3DSwapChain9** swap_chain) {
    if (parameters != nullptr) {
        parameters->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    }
    return g_device_hooks[Slot].create_additional_swap_chain(
        device, parameters, swap_chain);
}

template <std::size_t Slot>
HRESULT STDMETHODCALLTYPE ResetHook(IDirect3DDevice9* device,
                                    D3DPRESENT_PARAMETERS* parameters) {
    if (parameters != nullptr) {
        parameters->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    }
    return g_device_hooks[Slot].reset(device, parameters);
}

template <std::size_t Slot>
HRESULT STDMETHODCALLTYPE PresentHook(IDirect3DDevice9* device,
                                      const RECT* source_rect,
                                      const RECT* destination_rect,
                                      HWND destination_window,
                                      const RGNDATA* dirty_region) {
    return g_device_hooks[Slot].present(device, source_rect, destination_rect,
                                        destination_window, dirty_region);
}

template <std::size_t Slot>
HRESULT STDMETHODCALLTYPE ResetExHook(IDirect3DDevice9Ex* device,
                                      D3DPRESENT_PARAMETERS* parameters,
                                      D3DDISPLAYMODEEX* display_mode) {
    if (parameters != nullptr) {
        parameters->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    }
    return g_device_hooks[Slot].reset_ex(device, parameters, display_mode);
}

template <std::size_t Slot>
HRESULT STDMETHODCALLTYPE PresentExHook(IDirect3DDevice9Ex* device,
                                        const RECT* source_rect,
                                        const RECT* destination_rect,
                                        HWND destination_window,
                                        const RGNDATA* dirty_region,
                                        DWORD flags) {
    return g_device_hooks[Slot].present_ex(
        device, source_rect, destination_rect, destination_window,
        dirty_region, flags);
}

template <std::size_t Slot>
bool InstallDeviceHooksAt(IDirect3DDevice9* device,
                          IDirect3DDevice9Ex* device_ex) {
    DeviceHookSet& hooks = g_device_hooks[Slot];
    void** vtable = vsync_proxy::VtableOf(device);
    hooks.vtable = vtable;

    const bool base_hooks_ok =
        vsync_proxy::PatchVtableSlot(
            vtable, kDeviceCreateAdditionalSwapChain,
            reinterpret_cast<void*>(&CreateAdditionalSwapChainHook<Slot>),
            reinterpret_cast<void**>(
                &hooks.create_additional_swap_chain)) &&
        vsync_proxy::PatchVtableSlot(
            vtable, kDeviceReset,
            reinterpret_cast<void*>(&ResetHook<Slot>),
            reinterpret_cast<void**>(&hooks.reset)) &&
        vsync_proxy::PatchVtableSlot(
            vtable, kDevicePresent,
            reinterpret_cast<void*>(&PresentHook<Slot>),
            reinterpret_cast<void**>(&hooks.present));

    if (device_ex != nullptr) {
        void** ex_vtable = vsync_proxy::VtableOf(device_ex);
        vsync_proxy::PatchVtableSlot(
            ex_vtable, kDevicePresentEx,
            reinterpret_cast<void*>(&PresentExHook<Slot>),
            reinterpret_cast<void**>(&hooks.present_ex));
        vsync_proxy::PatchVtableSlot(
            ex_vtable, kDeviceResetEx,
            reinterpret_cast<void*>(&ResetExHook<Slot>),
            reinterpret_cast<void**>(&hooks.reset_ex));
    }

    return base_hooks_ok;
}

using DeviceHookInstaller =
    bool (*)(IDirect3DDevice9*, IDirect3DDevice9Ex*);

constexpr DeviceHookInstaller kDeviceHookInstallers[kMaxDeviceHookSets] = {
    &InstallDeviceHooksAt<0>, &InstallDeviceHooksAt<1>,
    &InstallDeviceHooksAt<2>, &InstallDeviceHooksAt<3>,
    &InstallDeviceHooksAt<4>, &InstallDeviceHooksAt<5>,
    &InstallDeviceHooksAt<6>, &InstallDeviceHooksAt<7>,
};

void InstallDeviceHooks(IDirect3DDevice9* device) noexcept {
    if (device == nullptr) {
        return;
    }

    IDirect3DDevice9Ex* device_ex = nullptr;
    if (FAILED(device->QueryInterface(
            __uuidof(IDirect3DDevice9Ex),
            reinterpret_cast<void**>(&device_ex)))) {
        device_ex = nullptr;
    }

    void** vtable = vsync_proxy::VtableOf(device);

    vsync_proxy::LockHookInstall();
    bool known_vtable = false;
    std::size_t free_slot = kMaxDeviceHookSets;
    for (std::size_t i = 0; i < kMaxDeviceHookSets; ++i) {
        if (g_device_hooks[i].vtable == vtable) {
            known_vtable = true;
            break;
        }
        if (g_device_hooks[i].vtable == nullptr &&
            free_slot == kMaxDeviceHookSets) {
            free_slot = i;
        }
    }

    if (!known_vtable && free_slot < kMaxDeviceHookSets) {
        // Mark the vtable as claimed even if a VirtualProtect call fails, so a
        // retry can never install a second hook over an already-patched slot.
        kDeviceHookInstallers[free_slot](device, device_ex);
    }
    vsync_proxy::UnlockHookInstall();

    if (device_ex != nullptr) {
        device_ex->Release();
    }
}

HRESULT STDMETHODCALLTYPE CreateDeviceHook(
    IDirect3D9* direct3d, UINT adapter, D3DDEVTYPE device_type, HWND window,
    DWORD behavior_flags, D3DPRESENT_PARAMETERS* parameters,
    IDirect3DDevice9** device) {
    if (parameters != nullptr) {
        parameters->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    }
    const HRESULT result = g_orig_create_device(
        direct3d, adapter, device_type, window, behavior_flags, parameters,
        device);
    if (SUCCEEDED(result) && device != nullptr && *device != nullptr) {
        InstallDeviceHooks(*device);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE CreateDeviceExHook(
    IDirect3D9Ex* direct3d, UINT adapter, D3DDEVTYPE device_type, HWND window,
    DWORD behavior_flags, D3DPRESENT_PARAMETERS* parameters,
    D3DDISPLAYMODEEX* display_mode, IDirect3DDevice9Ex** device) {
    if (parameters != nullptr) {
        parameters->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    }
    const HRESULT result = g_orig_create_device_ex(
        direct3d, adapter, device_type, window, behavior_flags, parameters,
        display_mode, device);
    if (SUCCEEDED(result) && device != nullptr && *device != nullptr) {
        InstallDeviceHooks(reinterpret_cast<IDirect3DDevice9*>(*device));
    }
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public exports.
// ---------------------------------------------------------------------------
extern "C" IDirect3D9* WINAPI ProxyDirect3DCreate9(UINT sdk_version) {
    if (g_create_d3d9 == nullptr) {
        return nullptr;
    }
    IDirect3D9* direct3d = g_create_d3d9(sdk_version);
    if (direct3d != nullptr) {
        InstallDirect3DHooks(reinterpret_cast<IUnknown*>(direct3d));
    }
    return direct3d;
}

extern "C" HRESULT WINAPI ProxyDirect3DCreate9Ex(UINT sdk_version,
                                                  IDirect3D9Ex** direct3d) {
    if (g_create_d3d9_ex == nullptr) {
        return D3DERR_NOTAVAILABLE;
    }
    const HRESULT result = g_create_d3d9_ex(sdk_version, direct3d);
    if (SUCCEEDED(result) && direct3d != nullptr && *direct3d != nullptr) {
        InstallDirect3DHooks(reinterpret_cast<IUnknown*>(*direct3d));
    }
    return result;
}

extern "C" IDirect3D9* WINAPI ProxyDirect3DCreate9On12(
    UINT sdk_version, void* override_list, UINT override_count) {
    if (g_create_d3d9_on12 == nullptr) {
        return nullptr;
    }
    IDirect3D9* direct3d =
        g_create_d3d9_on12(sdk_version, override_list, override_count);
    if (direct3d != nullptr) {
        InstallDirect3DHooks(reinterpret_cast<IUnknown*>(direct3d));
    }
    return direct3d;
}

extern "C" HRESULT WINAPI ProxyDirect3DCreate9On12Ex(
    UINT sdk_version, void* override_list, UINT override_count,
    IDirect3D9Ex** direct3d) {
    if (g_create_d3d9_on12_ex == nullptr) {
        return D3DERR_NOTAVAILABLE;
    }
    const HRESULT result = g_create_d3d9_on12_ex(
        sdk_version, override_list, override_count, direct3d);
    if (SUCCEEDED(result) && direct3d != nullptr && *direct3d != nullptr) {
        InstallDirect3DHooks(reinterpret_cast<IUnknown*>(*direct3d));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Initialization.
// ---------------------------------------------------------------------------
extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        vsync_proxy::LoadConfig(instance, g_config);

        HMODULE real = vsync_proxy::LoadSystemLibrary(L"d3d9.dll");
        if (real == nullptr || real == reinterpret_cast<HMODULE>(instance)) {
            return FALSE;
        }

        g_create_d3d9 = reinterpret_cast<Direct3DCreate9Fn>(
            vsync_proxy::ResolveExport(real, "Direct3DCreate9"));
        g_create_d3d9_ex = reinterpret_cast<Direct3DCreate9ExFn>(
            vsync_proxy::ResolveExport(real, "Direct3DCreate9Ex"));
        g_create_d3d9_on12 = reinterpret_cast<Direct3DCreate9On12Fn>(
            vsync_proxy::ResolveExport(real, "Direct3DCreate9On12"));
        g_create_d3d9_on12_ex = reinterpret_cast<Direct3DCreate9On12ExFn>(
            vsync_proxy::ResolveExport(real, "Direct3DCreate9On12Ex"));

#define VS_RESOLVE_FORWARD(name) \
    g_fwd_##name = vsync_proxy::ResolveExport(real, #name);
        VS_D3D9_FORWARD_LIST(VS_RESOLVE_FORWARD)
#undef VS_RESOLVE_FORWARD

        if (g_create_d3d9 == nullptr && g_create_d3d9_ex == nullptr &&
            g_create_d3d9_on12 == nullptr &&
            g_create_d3d9_on12_ex == nullptr) {
            return FALSE;
        }
    }
    return TRUE;
}

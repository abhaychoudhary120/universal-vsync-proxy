// DXGI proxy for DirectX 10, 11 and 12.
//
// Only timing is touched: Present/Present1 force a positive sync interval and
// remove ALLOW_TEARING. Buffers, formats, descriptor contents, shaders and
// resources are passed through untouched.

#include <windows.h>
#include <dxgi1_2.h>

#include "common/forwarder.h"
#include "common/proxy_common.h"
#include "dxgi_forwarders.h"

// ---------------------------------------------------------------------------
// Forwarded exports: one writable target slot and one naked jump stub each.
// The target slots are filled once in DllMain and are never written again.
// ---------------------------------------------------------------------------
extern "C" {
#define VS_DEFINE_FORWARD_PTR(name) void* g_fwd_##name = nullptr;
VS_DXGI_FORWARD_LIST(VS_DEFINE_FORWARD_PTR)
#undef VS_DEFINE_FORWARD_PTR
}

#define VS_DEFINE_STUB(name) VS_DEFINE_FORWARD_STUB(name)
VS_DXGI_FORWARD_LIST(VS_DEFINE_STUB)
#undef VS_DEFINE_STUB

namespace {

using CreateDXGIFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateDXGIFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
    IDXGISwapChain1**);
using CreateSwapChainForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
    IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
    IDXGISwapChain1**);

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

// Factory vtable layout (stable since DXGI 1.0 / 1.2).
constexpr std::size_t kFactoryCreateSwapChain = 10;
constexpr std::size_t kFactoryCreateSwapChainForHwnd = 15;
constexpr std::size_t kFactoryCreateSwapChainForCoreWindow = 16;
constexpr std::size_t kFactoryCreateSwapChainForComposition = 24;

// Swap-chain vtable layout (IDXGISwapChain / IDXGISwapChain1).
constexpr std::size_t kSwapChainPresent = 8;
constexpr std::size_t kSwapChainPresent1 = 22;

vsync_proxy::Config g_config;
HMODULE g_self_module = nullptr;

CreateDXGIFactoryFn g_create_factory = nullptr;
CreateDXGIFactoryFn g_create_factory1 = nullptr;
CreateDXGIFactory2Fn g_create_factory2 = nullptr;

void** g_factory_vtable = nullptr;
CreateSwapChainFn g_orig_create_swap_chain = nullptr;
CreateSwapChainForHwndFn g_orig_create_swap_chain_for_hwnd = nullptr;
CreateSwapChainForCoreWindowFn g_orig_create_swap_chain_for_core_window =
    nullptr;
CreateSwapChainForCompositionFn g_orig_create_swap_chain_for_composition =
    nullptr;

void** g_swap_chain_vtable = nullptr;
PresentFn g_orig_present = nullptr;
Present1Fn g_orig_present1 = nullptr;

HRESULT STDMETHODCALLTYPE CreateSwapChainHook(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
HRESULT STDMETHODCALLTYPE CreateSwapChainForHwndHook(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
    IDXGISwapChain1**);
HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindowHook(
    IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
    IDXGIOutput*, IDXGISwapChain1**);
HRESULT STDMETHODCALLTYPE CreateSwapChainForCompositionHook(
    IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
    IDXGISwapChain1**);
HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain*, UINT, UINT);
HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1*, UINT, UINT,
                                      const DXGI_PRESENT_PARAMETERS*);

void InstallSwapChainHooks(IDXGISwapChain* swap_chain) noexcept;

void InstallFactoryHooks(IUnknown* factory) noexcept {
    if (factory == nullptr) {
        return;
    }

    void** vtable = vsync_proxy::VtableOf(factory);

    // Query outside the hook-install critical section. This reference is only
    // used to decide whether the IDXGIFactory2 creation methods exist.
    IDXGIFactory2* factory2 = nullptr;
    const bool has_factory2 =
        SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2),
                                          reinterpret_cast<void**>(&factory2))) &&
        factory2 != nullptr;

    vsync_proxy::LockHookInstall();
    if (g_factory_vtable == nullptr) {
        const bool patched = vsync_proxy::PatchVtableSlot(
            vtable, kFactoryCreateSwapChain,
            reinterpret_cast<void*>(&CreateSwapChainHook),
            reinterpret_cast<void**>(&g_orig_create_swap_chain));

        if (patched && has_factory2) {
            vsync_proxy::PatchVtableSlot(
                vtable, kFactoryCreateSwapChainForHwnd,
                reinterpret_cast<void*>(&CreateSwapChainForHwndHook),
                reinterpret_cast<void**>(&g_orig_create_swap_chain_for_hwnd));
            vsync_proxy::PatchVtableSlot(
                vtable, kFactoryCreateSwapChainForCoreWindow,
                reinterpret_cast<void*>(&CreateSwapChainForCoreWindowHook),
                reinterpret_cast<void**>(
                    &g_orig_create_swap_chain_for_core_window));
            vsync_proxy::PatchVtableSlot(
                vtable, kFactoryCreateSwapChainForComposition,
                reinterpret_cast<void*>(&CreateSwapChainForCompositionHook),
                reinterpret_cast<void**>(
                    &g_orig_create_swap_chain_for_composition));
        }

        if (patched) {
            g_factory_vtable = vtable;
        }
    }
    vsync_proxy::UnlockHookInstall();

    if (factory2 != nullptr) {
        factory2->Release();
    }
}

void InstallSwapChainHooks(IDXGISwapChain* swap_chain) noexcept {
    if (swap_chain == nullptr) {
        return;
    }

    void** vtable = vsync_proxy::VtableOf(swap_chain);

    IDXGISwapChain1* swap_chain1 = nullptr;
    const bool has_present1 =
        SUCCEEDED(swap_chain->QueryInterface(
            __uuidof(IDXGISwapChain1),
            reinterpret_cast<void**>(&swap_chain1))) &&
        swap_chain1 != nullptr;

    vsync_proxy::LockHookInstall();
    if (g_swap_chain_vtable == nullptr) {
        const bool patched = vsync_proxy::PatchVtableSlot(
            vtable, kSwapChainPresent,
            reinterpret_cast<void*>(&PresentHook),
            reinterpret_cast<void**>(&g_orig_present));

        if (patched) {
            if (has_present1) {
                vsync_proxy::PatchVtableSlot(
                    vtable, kSwapChainPresent1,
                    reinterpret_cast<void*>(&Present1Hook),
                    reinterpret_cast<void**>(&g_orig_present1));
            }
            g_swap_chain_vtable = vtable;
        }
    }
    vsync_proxy::UnlockHookInstall();

    if (swap_chain1 != nullptr) {
        swap_chain1->Release();
    }
}

// ---------------------------------------------------------------------------
// Factory creation methods: call the real method, then arm Present on the
// returned swap chain. No swap-chain descriptor is modified.
// ---------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE CreateSwapChainHook(
    IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
    IDXGISwapChain** swap_chain) {
    const HRESULT result =
        g_orig_create_swap_chain(factory, device, desc, swap_chain);
    if (SUCCEEDED(result) && swap_chain != nullptr &&
        *swap_chain != nullptr) {
        InstallSwapChainHooks(*swap_chain);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForHwndHook(
    IDXGIFactory2* factory, IUnknown* device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    IDXGIOutput* restrict_to_output, IDXGISwapChain1** swap_chain) {
    const HRESULT result = g_orig_create_swap_chain_for_hwnd(
        factory, device, window, desc, fullscreen_desc, restrict_to_output,
        swap_chain);
    if (SUCCEEDED(result) && swap_chain != nullptr &&
        *swap_chain != nullptr) {
        InstallSwapChainHooks(*swap_chain);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindowHook(
    IDXGIFactory2* factory, IUnknown* device, IUnknown* window,
    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrict_to_output,
    IDXGISwapChain1** swap_chain) {
    const HRESULT result = g_orig_create_swap_chain_for_core_window(
        factory, device, window, desc, restrict_to_output, swap_chain);
    if (SUCCEEDED(result) && swap_chain != nullptr &&
        *swap_chain != nullptr) {
        InstallSwapChainHooks(*swap_chain);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForCompositionHook(
    IDXGIFactory2* factory, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
    IDXGIOutput* restrict_to_output, IDXGISwapChain1** swap_chain) {
    const HRESULT result = g_orig_create_swap_chain_for_composition(
        factory, device, desc, restrict_to_output, swap_chain);
    if (SUCCEEDED(result) && swap_chain != nullptr &&
        *swap_chain != nullptr) {
        InstallSwapChainHooks(*swap_chain);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Present hot path. These functions contain no allocation, no I/O and no
// lock. Only two integer operations and the original call remain.
// ---------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swap_chain,
                                      UINT sync_interval, UINT flags) {
    if (sync_interval < 1) {
        sync_interval = g_config.sync_interval;
    }
    flags &= ~static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING);
    return g_orig_present(swap_chain, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE Present1Hook(
    IDXGISwapChain1* swap_chain, UINT sync_interval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* present_parameters) {
    if (sync_interval < 1) {
        sync_interval = g_config.sync_interval;
    }
    flags &= ~static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING);
    return g_orig_present1(swap_chain, sync_interval, flags,
                           present_parameters);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public exports.
// ---------------------------------------------------------------------------
extern "C" HRESULT WINAPI ProxyCreateDXGIFactory(REFIID riid,
                                                  void** factory) {
    if (g_create_factory == nullptr) {
        return E_FAIL;
    }
    const HRESULT result = g_create_factory(riid, factory);
    if (SUCCEEDED(result) && factory != nullptr && *factory != nullptr) {
        InstallFactoryHooks(reinterpret_cast<IUnknown*>(*factory));
    }
    return result;
}

extern "C" HRESULT WINAPI ProxyCreateDXGIFactory1(REFIID riid,
                                                   void** factory) {
    if (g_create_factory1 == nullptr) {
        return E_FAIL;
    }
    const HRESULT result = g_create_factory1(riid, factory);
    if (SUCCEEDED(result) && factory != nullptr && *factory != nullptr) {
        InstallFactoryHooks(reinterpret_cast<IUnknown*>(*factory));
    }
    return result;
}

extern "C" HRESULT WINAPI ProxyCreateDXGIFactory2(UINT flags, REFIID riid,
                                                   void** factory) {
    if (g_create_factory2 == nullptr) {
        return DXGI_ERROR_UNSUPPORTED;
    }
    const HRESULT result = g_create_factory2(flags, riid, factory);
    if (SUCCEEDED(result) && factory != nullptr && *factory != nullptr) {
        InstallFactoryHooks(reinterpret_cast<IUnknown*>(*factory));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Initialization. The real DLL is loaded once, under the loader lock, so every
// forwarded slot is valid before the host can call an import.
// ---------------------------------------------------------------------------
extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self_module = instance;
        DisableThreadLibraryCalls(instance);
        vsync_proxy::LoadConfig(instance, g_config);

        HMODULE real = vsync_proxy::LoadSystemLibrary(L"dxgi.dll");
        if (real == nullptr || real == reinterpret_cast<HMODULE>(instance)) {
            return FALSE;
        }

        g_create_factory = reinterpret_cast<CreateDXGIFactoryFn>(
            vsync_proxy::ResolveExport(real, "CreateDXGIFactory"));
        g_create_factory1 = reinterpret_cast<CreateDXGIFactoryFn>(
            vsync_proxy::ResolveExport(real, "CreateDXGIFactory1"));
        g_create_factory2 = reinterpret_cast<CreateDXGIFactory2Fn>(
            vsync_proxy::ResolveExport(real, "CreateDXGIFactory2"));

#define VS_RESOLVE_FORWARD(name) \
    g_fwd_##name = vsync_proxy::ResolveExport(real, #name);
        VS_DXGI_FORWARD_LIST(VS_RESOLVE_FORWARD)
#undef VS_RESOLVE_FORWARD

        if (g_create_factory == nullptr && g_create_factory1 == nullptr &&
            g_create_factory2 == nullptr) {
            return FALSE;
        }
    }
    return TRUE;
}

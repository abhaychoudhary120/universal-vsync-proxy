// DXGI proxy for DirectX 10, 11 and 12.
//
// Only timing is touched: Present/Present1 force a positive sync interval and
// remove ALLOW_TEARING. Buffers, formats, descriptor contents, shaders and
// resources are passed through untouched.

#include <windows.h>
#include <dxgi1_2.h>

#include "common/debug_log.h"
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
using SetAppCompatStringPointerFn = void(WINAPI*)(ULONG_PTR, const char*);

// Factory vtable layout (stable since DXGI 1.0 / 1.2).
constexpr std::size_t kFactoryCreateSwapChain = 10;
constexpr std::size_t kFactoryCreateSwapChainForHwnd = 15;
constexpr std::size_t kFactoryCreateSwapChainForCoreWindow = 16;
constexpr std::size_t kFactoryCreateSwapChainForComposition = 21;

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

// Filled by AppCompat before DllMain can resolve the real dxgi export.
ULONG_PTR g_pending_compat_value = 0;
const char* g_pending_compat_text = nullptr;
bool g_compat_pending = false;

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

    VS_LOG_PTR("InstallFactoryHooks factory", factory);
    VS_LOG_PTR("InstallFactoryHooks vtable", vtable);

    // Query outside the hook-install critical section. This reference is only
    // used to decide whether the IDXGIFactory2 creation methods exist.
    IDXGIFactory2* factory2 = nullptr;
    const bool has_factory2 =
        SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2),
                                          reinterpret_cast<void**>(&factory2))) &&
        factory2 != nullptr;

    VS_LOG_PTR("InstallFactoryHooks factory2", factory2);

    vsync_proxy::LockHookInstall();
    if (g_factory_vtable == nullptr) {
        const bool patched = vsync_proxy::PatchVtableSlot(
            vtable, kFactoryCreateSwapChain,
            reinterpret_cast<void*>(&CreateSwapChainHook),
            reinterpret_cast<void**>(&g_orig_create_swap_chain));

        VS_LOG_PTR("InstallFactoryHooks patched CreateSwapChain",
                   reinterpret_cast<void*>(patched));
        VS_LOG_PTR("InstallFactoryHooks orig CreateSwapChain",
                   reinterpret_cast<void*>(g_orig_create_swap_chain));

        if (patched && has_factory2) {
            const bool patched_hwnd [[maybe_unused]] =
                vsync_proxy::PatchVtableSlot(
                    vtable, kFactoryCreateSwapChainForHwnd,
                    reinterpret_cast<void*>(&CreateSwapChainForHwndHook),
                    reinterpret_cast<void**>(
                        &g_orig_create_swap_chain_for_hwnd));
            const bool patched_core [[maybe_unused]] =
                vsync_proxy::PatchVtableSlot(
                    vtable, kFactoryCreateSwapChainForCoreWindow,
                    reinterpret_cast<void*>(&CreateSwapChainForCoreWindowHook),
                    reinterpret_cast<void**>(
                        &g_orig_create_swap_chain_for_core_window));
            const bool patched_comp [[maybe_unused]] =
                vsync_proxy::PatchVtableSlot(
                    vtable, kFactoryCreateSwapChainForComposition,
                    reinterpret_cast<void*>(
                        &CreateSwapChainForCompositionHook),
                    reinterpret_cast<void**>(
                        &g_orig_create_swap_chain_for_composition));

            VS_LOG_PTR("InstallFactoryHooks patched hwnd",
                       reinterpret_cast<void*>(patched_hwnd));
            VS_LOG_PTR("InstallFactoryHooks patched core",
                       reinterpret_cast<void*>(patched_core));
            VS_LOG_PTR("InstallFactoryHooks patched composition",
                       reinterpret_cast<void*>(patched_comp));
            VS_LOG_PTR("InstallFactoryHooks orig hwnd",
                       reinterpret_cast<void*>(g_orig_create_swap_chain_for_hwnd));
            VS_LOG_PTR(
                "InstallFactoryHooks orig core",
                reinterpret_cast<void*>(g_orig_create_swap_chain_for_core_window));
            VS_LOG_PTR(
                "InstallFactoryHooks orig composition",
                reinterpret_cast<void*>(
                    g_orig_create_swap_chain_for_composition));
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

    VS_LOG_PTR("InstallSwapChainHooks chain", swap_chain);
    VS_LOG_PTR("InstallSwapChainHooks vtable", vtable);

    IDXGISwapChain1* swap_chain1 = nullptr;
    const bool has_present1 =
        SUCCEEDED(swap_chain->QueryInterface(
            __uuidof(IDXGISwapChain1),
            reinterpret_cast<void**>(&swap_chain1))) &&
        swap_chain1 != nullptr;

    VS_LOG_PTR("InstallSwapChainHooks chain1", swap_chain1);

    vsync_proxy::LockHookInstall();
    if (g_swap_chain_vtable == nullptr) {
        const bool patched = vsync_proxy::PatchVtableSlot(
            vtable, kSwapChainPresent,
            reinterpret_cast<void*>(&PresentHook),
            reinterpret_cast<void**>(&g_orig_present));

        VS_LOG_PTR("InstallSwapChainHooks patched Present",
                   reinterpret_cast<void*>(patched));
        VS_LOG_PTR("InstallSwapChainHooks orig Present",
                   reinterpret_cast<void*>(g_orig_present));

        if (patched) {
            if (has_present1) {
                const bool patched_present1 [[maybe_unused]] =
                    vsync_proxy::PatchVtableSlot(
                        vtable, kSwapChainPresent1,
                        reinterpret_cast<void*>(&Present1Hook),
                        reinterpret_cast<void**>(&g_orig_present1));
                VS_LOG_PTR("InstallSwapChainHooks patched Present1",
                           reinterpret_cast<void*>(patched_present1));
                VS_LOG_PTR("InstallSwapChainHooks orig Present1",
                           reinterpret_cast<void*>(g_orig_present1));
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
    VS_LOG("CreateSwapChainHook enter");
    VS_LOG_PTR("CreateSwapChainHook orig",
               reinterpret_cast<void*>(g_orig_create_swap_chain));
    if (g_orig_create_swap_chain == nullptr) {
        return E_FAIL;
    }
    const HRESULT result =
        g_orig_create_swap_chain(factory, device, desc, swap_chain);
    VS_LOG_PTR("CreateSwapChainHook result chain",
               swap_chain == nullptr ? nullptr : *swap_chain);
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
    VS_LOG("CreateSwapChainForHwndHook enter");
    VS_LOG_PTR("CreateSwapChainForHwndHook orig",
               reinterpret_cast<void*>(g_orig_create_swap_chain_for_hwnd));
    if (g_orig_create_swap_chain_for_hwnd == nullptr) {
        return E_FAIL;
    }
    const HRESULT result = g_orig_create_swap_chain_for_hwnd(
        factory, device, window, desc, fullscreen_desc, restrict_to_output,
        swap_chain);
    VS_LOG_PTR("CreateSwapChainForHwndHook result chain",
               swap_chain == nullptr ? nullptr : *swap_chain);
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
    if (g_orig_create_swap_chain_for_core_window == nullptr) {
        return E_FAIL;
    }
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
    if (g_orig_create_swap_chain_for_composition == nullptr) {
        return E_FAIL;
    }
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
    if (g_orig_present == nullptr) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (sync_interval < 1) {
        sync_interval = g_config.sync_interval;
    }
    flags &= ~static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING);
    return g_orig_present(swap_chain, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE Present1Hook(
    IDXGISwapChain1* swap_chain, UINT sync_interval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* present_parameters) {
    if (g_orig_present1 == nullptr) {
        return DXGI_ERROR_INVALID_CALL;
    }
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
    VS_LOG("ProxyCreateDXGIFactory enter");
    if (g_create_factory == nullptr) {
        return E_FAIL;
    }
    const HRESULT result = g_create_factory(riid, factory);
    VS_LOG_PTR("ProxyCreateDXGIFactory result",
               SUCCEEDED(result) && factory != nullptr ? *factory : nullptr);
    if (SUCCEEDED(result) && factory != nullptr && *factory != nullptr) {
        InstallFactoryHooks(reinterpret_cast<IUnknown*>(*factory));
    }
    return result;
}

extern "C" HRESULT WINAPI ProxyCreateDXGIFactory1(REFIID riid,
                                                   void** factory) {
    VS_LOG("ProxyCreateDXGIFactory1 enter");
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
    VS_LOG("ProxyCreateDXGIFactory2 enter");
    VS_LOG_PTR("ProxyCreateDXGIFactory2 real",
               reinterpret_cast<void*>(g_create_factory2));
    if (g_create_factory2 == nullptr) {
        return DXGI_ERROR_UNSUPPORTED;
    }
    const HRESULT result = g_create_factory2(flags, riid, factory);
    VS_LOG_PTR("ProxyCreateDXGIFactory2 result",
               SUCCEEDED(result) && factory != nullptr ? *factory : nullptr);
    if (SUCCEEDED(result) && factory != nullptr && *factory != nullptr) {
        InstallFactoryHooks(reinterpret_cast<IUnknown*>(*factory));
    }
    VS_LOG("ProxyCreateDXGIFactory2 leave");
    return result;
}

// AppCompat (AcGenral.dll) calls this export while the process is still
// initializing, potentially before this DLL's entry point has resolved the
// real function. Cache the arguments until DllMain finishes and replay them.
// Returning early without touching the arguments is safe in either case.
extern "C" void WINAPI ProxySetAppCompatStringPointer(ULONG_PTR value,
                                                      const char* text) {
    VS_ODS("ProxySetAppCompatStringPointer enter");
    VS_LOG("ProxySetAppCompatStringPointer enter");
    if (g_fwd_SetAppCompatStringPointer == nullptr) {
        VS_ODS("ProxySetAppCompatStringPointer deferred");
        VS_LOG("ProxySetAppCompatStringPointer deferred");
        g_pending_compat_value = value;
        g_pending_compat_text = text;
        g_compat_pending = true;
        return;
    }
    VS_LOG("ProxySetAppCompatStringPointer forwarding");
    reinterpret_cast<SetAppCompatStringPointerFn>(
        g_fwd_SetAppCompatStringPointer)(value, text);
}

// ---------------------------------------------------------------------------
// Initialization. The real DLL is loaded once, under the loader lock, so every
// forwarded slot is valid before the host can call an import.
// ---------------------------------------------------------------------------
extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        VS_ODS("dxgi DllMain enter");
        g_self_module = instance;
        DisableThreadLibraryCalls(instance);
#if defined(VSYNC_DEBUG_LOG)
        vsync_proxy::DebugInstall(instance);
#endif
        VS_LOG("dxgi DllMain attach");
        VS_LOG_PTR("dxgi DllMain instance", instance);

        vsync_proxy::LoadConfig(instance, g_config);
        VS_LOG_PTR("dxgi DllMain config interval",
                   reinterpret_cast<void*>(
                       static_cast<ULONG_PTR>(g_config.sync_interval)));

        HMODULE real = vsync_proxy::LoadSystemLibrary(L"dxgi.dll");
        VS_LOG_PTR("dxgi DllMain real module", real);
        if (real == nullptr || real == reinterpret_cast<HMODULE>(instance)) {
            VS_LOG("dxgi DllMain real module invalid");
            return FALSE;
        }

        g_create_factory = reinterpret_cast<CreateDXGIFactoryFn>(
            vsync_proxy::ResolveExport(real, "CreateDXGIFactory"));
        g_create_factory1 = reinterpret_cast<CreateDXGIFactoryFn>(
            vsync_proxy::ResolveExport(real, "CreateDXGIFactory1"));
        g_create_factory2 = reinterpret_cast<CreateDXGIFactory2Fn>(
            vsync_proxy::ResolveExport(real, "CreateDXGIFactory2"));

        VS_LOG_PTR("dxgi real CreateDXGIFactory",
                   reinterpret_cast<void*>(g_create_factory));
        VS_LOG_PTR("dxgi real CreateDXGIFactory1",
                   reinterpret_cast<void*>(g_create_factory1));
        VS_LOG_PTR("dxgi real CreateDXGIFactory2",
                   reinterpret_cast<void*>(g_create_factory2));

#define VS_RESOLVE_FORWARD(name) \
    g_fwd_##name = vsync_proxy::ResolveExport(real, #name);
        VS_DXGI_FORWARD_LIST(VS_RESOLVE_FORWARD)
#undef VS_RESOLVE_FORWARD

        if (g_create_factory == nullptr && g_create_factory1 == nullptr &&
            g_create_factory2 == nullptr) {
            VS_LOG("dxgi DllMain no factory exports");
            return FALSE;
        }

        if (g_compat_pending && g_fwd_SetAppCompatStringPointer != nullptr) {
            reinterpret_cast<SetAppCompatStringPointerFn>(
                g_fwd_SetAppCompatStringPointer)(g_pending_compat_value,
                                                 g_pending_compat_text);
            g_compat_pending = false;
            VS_LOG("dxgi DllMain replayed AppCompat string");
        }
        VS_LOG("dxgi DllMain attach complete");
    }
    return TRUE;
}

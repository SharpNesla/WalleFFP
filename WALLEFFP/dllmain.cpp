#include <Windows.h>
#include <string>
#include <Shlwapi.h>
#include "d3d9_proxy.h"
#include <atomic>

#pragma comment(lib, "Shlwapi.lib")

// Export the expected d3d9.dll entry points without changing linkage of the
// declarations in the Windows SDK headers.
#ifdef _WIN64
#pragma comment(linker, "/export:Direct3DCreate9")
#pragma comment(linker, "/export:Direct3DCreate9Ex")
#else
#pragma comment(linker, "/export:Direct3DCreate9=_Direct3DCreate9@4")
#pragma comment(linker, "/export:Direct3DCreate9Ex=_Direct3DCreate9Ex@8")
#endif

static const wchar_t* kRemixD3D9Name = L"d3d9_remix.dll";
static HMODULE g_remixModule = nullptr;
static HMODULE g_fallbackModule = nullptr;
static std::atomic<LONG> g_remixObjectRefs{0};

using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);
using Direct3DCreate9ExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);

static Direct3DCreate9Fn g_remixDirect3DCreate9 = nullptr;
static Direct3DCreate9ExFn g_remixDirect3DCreate9Ex = nullptr;

static Direct3DCreate9Fn g_systemDirect3DCreate9 = nullptr;
static Direct3DCreate9ExFn g_systemDirect3DCreate9Ex = nullptr;

static void LoadRemixOrSystem(HMODULE hModule) {
    // Determine our DLL directory
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(hModule, path, MAX_PATH)) {
        MessageBoxA(NULL, "Failed to locate proxy DLL path.", "d3d9 proxy error", MB_ICONERROR | MB_OK);
        ExitProcess(1);
    }

    wchar_t dir[MAX_PATH] = {};
    wcscpy_s(dir, path);
    PathRemoveFileSpecW(dir);

    // Build full path to the remix DLL (in same directory)
    wchar_t remixPath[MAX_PATH] = {};
    PathCombineW(remixPath, dir, kRemixD3D9Name);

    // Try loading the remix DLL from the same directory as this proxy
    g_remixModule = LoadLibraryW(remixPath);

    if (g_remixModule) {
        g_remixDirect3DCreate9 = reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(g_remixModule, "Direct3DCreate9"));
        g_remixDirect3DCreate9Ex = reinterpret_cast<Direct3DCreate9ExFn>(GetProcAddress(g_remixModule, "Direct3DCreate9Ex"));
        if (!g_remixDirect3DCreate9) {
            MessageBoxA(NULL, "Loaded d3d9_remix.dll but Direct3DCreate9 was not found.", "d3d9 proxy error", MB_ICONERROR | MB_OK);
            ExitProcess(1);
        }
        return;
    }

    // If local remix not present, try to load system d3d9.dll directly (fallback)
    wchar_t systemDir[MAX_PATH] = {};
    if (GetSystemDirectoryW(systemDir, MAX_PATH)) {
        wchar_t sysD3DPath[MAX_PATH] = {};
        PathCombineW(sysD3DPath, systemDir, L"d3d9.dll");
        g_fallbackModule = LoadLibraryW(sysD3DPath);
    }

    if (!g_fallbackModule) {
        // Last resort: try default LoadLibrary that will search system directories
        g_fallbackModule = LoadLibraryW(L"d3d9.dll");
    }

    if (!g_fallbackModule) {
        MessageBoxA(NULL, "Failed to load RTX Remix d3d9_remix.dll and could not find system d3d9.dll.", "d3d9 proxy error", MB_ICONERROR | MB_OK);
        ExitProcess(1);
    }

    g_systemDirect3DCreate9 = reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(g_fallbackModule, "Direct3DCreate9"));
    g_systemDirect3DCreate9Ex = reinterpret_cast<Direct3DCreate9ExFn>(GetProcAddress(g_fallbackModule, "Direct3DCreate9Ex"));
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        LoadRemixOrSystem(hModule);
        break;
    case DLL_PROCESS_DETACH:
        // Keep Remix loaded if any proxy objects are still alive.
        if (g_remixModule && g_remixObjectRefs.load() == 0) FreeLibrary(g_remixModule);
        if (g_fallbackModule) FreeLibrary(g_fallbackModule);
        break;
    }
    return TRUE;
}

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
    // If we have the Remix create fn, call that to get the real IDirect3D9
    if (g_remixDirect3DCreate9) {
        IDirect3D9* real = g_remixDirect3DCreate9(SDKVersion);
        if (real) {
            D3D9Proxy* proxy = new D3D9Proxy(real);
            return proxy;
        }
        return nullptr;
    }

    // Otherwise fallback to system
    if (g_systemDirect3DCreate9) {
        IDirect3D9* real = g_systemDirect3DCreate9(SDKVersion);
        if (real) {
            D3D9Proxy* proxy = new D3D9Proxy(real);
            return proxy;
        }
    }
    return nullptr;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppOut) {
    if (!ppOut) return E_POINTER;
    if (g_remixDirect3DCreate9Ex) {
        IDirect3D9Ex* realex = nullptr;
        HRESULT hr = g_remixDirect3DCreate9Ex(SDKVersion, &realex);
        if (SUCCEEDED(hr) && realex) {
            auto* proxy = new D3D9Proxy(realex);
            realex->Release();
            *ppOut = proxy;
            return hr;
        }
        return hr;
    }

    if (g_systemDirect3DCreate9Ex) {
        IDirect3D9Ex* realex = nullptr;
        HRESULT hr = g_systemDirect3DCreate9Ex(SDKVersion, &realex);
        if (SUCCEEDED(hr) && realex) {
            auto* proxy = new D3D9Proxy(realex);
            realex->Release();
            *ppOut = proxy;
            return hr;
        }
        return hr;
    }

    // If neither function exists, fail gracefully
    return E_NOTIMPL;
}

extern "C" void ProxyAddRemixRef() {
    if (g_remixModule) {
        g_remixObjectRefs.fetch_add(1);
    }
}

extern "C" void ProxyReleaseRemixRef() {
    if (g_remixModule) {
        g_remixObjectRefs.fetch_sub(1);
    }
}

extern "C" bool ProxyIsUsingRemix() {
    return g_remixModule != nullptr;
}

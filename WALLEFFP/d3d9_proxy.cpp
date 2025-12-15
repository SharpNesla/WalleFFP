#include "d3d9_proxy.h"
#include "d3d9_device_proxy.h"

#include <algorithm>
#include <cstdio>

D3D9Proxy::D3D9Proxy(IDirect3D9* real) : m_real(real) {
    if (m_real) {
        m_real->AddRef();
        m_real->QueryInterface(__uuidof(IDirect3D9Ex), reinterpret_cast<void**>(&m_realEx));
    }
    ProxyAddRemixRef();
}

D3D9Proxy::~D3D9Proxy() {
    if (m_realEx) {
        m_realEx->Release();
        m_realEx = nullptr;
    }
    if (m_real) {
        m_real->Release();
        m_real = nullptr;
    }
    ProxyReleaseRemixRef();
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D9) || riid == __uuidof(IDirect3D9Ex)) {
        *ppvObj = static_cast<IDirect3D9Ex*>(this);
        AddRef();
        return S_OK;
    }

    return m_real ? m_real->QueryInterface(riid, ppvObj) : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D3D9Proxy::AddRef() {
    return ++m_refCount;
}

ULONG STDMETHODCALLTYPE D3D9Proxy::Release() {
    ULONG ref = --m_refCount;
    if (ref == 0) delete this;
    return ref;
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::RegisterSoftwareDevice(void* pInitializeFunction) {
    return m_real->RegisterSoftwareDevice(pInitializeFunction);
}

UINT STDMETHODCALLTYPE D3D9Proxy::GetAdapterCount() {
    return m_real->GetAdapterCount();
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) {
    return m_real->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
}

UINT STDMETHODCALLTYPE D3D9Proxy::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) {
    return m_real->GetAdapterModeCount(Adapter, Format);
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) {
    return m_real->EnumAdapterModes(Adapter, Format, Mode, pMode);
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) {
    return m_real->GetAdapterDisplayMode(Adapter, pMode);
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) {
    return m_real->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed);
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) {
    return m_real->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) {
    return m_real->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) {
    return m_real->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) {
    return m_real->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) {
    HRESULT hr = m_real->GetDeviceCaps(Adapter, DeviceType, pCaps);
    if (FAILED(hr) || !pCaps) return hr;

    // Many DX9-era games choose different rendering paths based on shader model
    // and reported capabilities. By advertising PS 1.4 / VS 1.1 and weak caps,
    // we encourage simpler fixed-function or near-FFP pipelines. These are easier
    // for RTX Remix to interpret and reconstruct into a consistent scene graph.
    // Log original capabilities for debug verification
    {
        char buff[256] = {};
        int origPSMaj = (pCaps->PixelShaderVersion >> 8) & 0xff;
        int origPSMin = pCaps->PixelShaderVersion & 0xff;
        int origVSMaj = (pCaps->VertexShaderVersion >> 8) & 0xff;
        int origVSMin = pCaps->VertexShaderVersion & 0xff;
        sprintf_s(buff, sizeof(buff), "D3D9Proxy::GetDeviceCaps Adapter=%u orig PS=%d.%d VS=%d.%d MaxTex=%u MaxBlendStages=%u", Adapter, origPSMaj, origPSMin, origVSMaj, origVSMin, pCaps->MaxSimultaneousTextures, pCaps->MaxTextureBlendStages);
        OutputDebugStringA(buff);
    }

    pCaps->PixelShaderVersion = D3DPS_VERSION(1, 4);
    pCaps->VertexShaderVersion = D3DVS_VERSION(1, 1);

    pCaps->MaxSimultaneousTextures = std::min<UINT>(pCaps->MaxSimultaneousTextures, 4u);
    pCaps->MaxTextureBlendStages = std::min<UINT>(pCaps->MaxTextureBlendStages, 4u);

    // Log spoofed capabilities for debug verification
    {
        char buff[256] = {};
        int spoofPSMaj = (pCaps->PixelShaderVersion >> 8) & 0xff;
        int spoofPSMin = pCaps->PixelShaderVersion & 0xff;
        int spoofVSMaj = (pCaps->VertexShaderVersion >> 8) & 0xff;
        int spoofVSMin = pCaps->VertexShaderVersion & 0xff;
        sprintf_s(buff, sizeof(buff), "D3D9Proxy::GetDeviceCaps Adapter=%u spoof PS=%d.%d VS=%d.%d MaxTex=%u MaxBlendStages=%u", Adapter, spoofPSMaj, spoofPSMin, spoofVSMaj, spoofVSMin, pCaps->MaxSimultaneousTextures, pCaps->MaxTextureBlendStages);
        OutputDebugStringA(buff);
    }

    pCaps->TextureCaps |= D3DPTEXTURECAPS_POW2;
    pCaps->TextureCaps |= D3DPTEXTURECAPS_ALPHAPALETTE;

    return D3D_OK;
}

HMONITOR STDMETHODCALLTYPE D3D9Proxy::GetAdapterMonitor(UINT Adapter) {
    return m_real->GetAdapterMonitor(Adapter);
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
                                                  D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface) {
    if (!ppReturnedDeviceInterface) return E_POINTER;
    *ppReturnedDeviceInterface = nullptr;

    IDirect3DDevice9* realDevice = nullptr;
    HRESULT hr = m_real->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, &realDevice);
    if (FAILED(hr) || !realDevice) return hr;

    auto* proxyDevice = new D3D9DeviceProxy(realDevice);
    {
        char buff[128] = {};
        sprintf_s(buff, sizeof(buff), "D3D9Proxy::CreateDevice Adapter=%u DeviceType=%08x BehaviorFlags=%08x -> returning proxy device", Adapter, static_cast<unsigned int>(DeviceType), BehaviorFlags);
        OutputDebugStringA(buff);
    }
    realDevice->Release();
    *ppReturnedDeviceInterface = proxyDevice;
    return hr;
}

UINT STDMETHODCALLTYPE D3D9Proxy::GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter) {
    return m_realEx ? m_realEx->GetAdapterModeCountEx(Adapter, pFilter) : 0;
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter, UINT Mode, D3DDISPLAYMODEEX* pMode) {
    return m_realEx ? m_realEx->EnumAdapterModesEx(Adapter, pFilter, Mode, pMode) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation) {
    return m_realEx ? m_realEx->GetAdapterDisplayModeEx(Adapter, pMode, pRotation) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
                                                    D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode,
                                                    IDirect3DDevice9Ex** ppReturnedDeviceInterface) {
    if (!ppReturnedDeviceInterface) return E_POINTER;
    *ppReturnedDeviceInterface = nullptr;
    if (!m_realEx) return D3DERR_INVALIDCALL;

    // IDirect3DDevice9Ex includes extra methods. To avoid unsafe vtable casts,
    // we return the real Ex device unwrapped. CreateDevice (non-Ex) is wrapped.
    return m_realEx->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                    pPresentationParameters, pFullscreenDisplayMode, ppReturnedDeviceInterface);
}

HRESULT STDMETHODCALLTYPE D3D9Proxy::GetAdapterLUID(UINT Adapter, LUID* pLUID) {
    return m_realEx ? m_realEx->GetAdapterLUID(Adapter, pLUID) : D3DERR_INVALIDCALL;
}

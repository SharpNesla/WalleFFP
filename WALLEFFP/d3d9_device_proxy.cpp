#include "d3d9_device_proxy.h"

#include <cmath>
#include <cstring>
#include <cstdio>

#include "window_hook.h"

static D3DMATRIX TransposeMatrix(const D3DMATRIX& a) {
    D3DMATRIX t{};
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            t.m[r][c] = a.m[c][r];
        }
    }
    return t;
}

struct ProjInfo {
    float zn = 0.0f;
    float zf = 0.0f;
    bool valid = false;
};

static bool IsFiniteMatrix(const D3DMATRIX& m) {
    const float* f = reinterpret_cast<const float*>(&m);
    for (int i = 0; i < 16; i++) {
        if (!std::isfinite(f[i])) return false;
    }
    return true;
}

static float AbsSumMatrix(const D3DMATRIX& m) {
    const float* f = reinterpret_cast<const float*>(&m);
    float s = 0.f;
    for (int i = 0; i < 16; i++) s += std::fabs(f[i]);
    return s;
}

// Try to extract near/far from common D3D-style perspective projections.
// Handles both LH (m[2][3] ~= +1) and RH (m[2][3] ~= -1) in row-major layout.
static ProjInfo ExtractNearFarRowMajor(const D3DMATRIX& p) {
    ProjInfo out{};

    if (!IsFiniteMatrix(p)) return out;

    const float m33 = p.m[2][2];
    const float m34 = p.m[2][3];
    const float m43 = p.m[3][2];
    const float m44 = p.m[3][3];

    if (std::fabs(m44) > 1e-2f) return out;
    if (std::fabs(std::fabs(m34) - 1.0f) > 1e-2f) return out;
    if (std::fabs(m33) < 1e-6f) return out;

    // LH: m34=+1, m33=zf/(zf-zn), m43=-zn*zf/(zf-zn)
    if (m34 > 0.0f) {
        const float zn = -m43 / m33;
        const float denom = (m33 - 1.0f);
        if (zn > 0.0f && std::fabs(denom) > 1e-6f) {
            const float zf = (m33 * zn) / denom;
            if (zf > zn) {
                out.zn = zn;
                out.zf = zf;
                out.valid = true;
                return out;
            }
        }
    }

    // RH: m34=-1, m33=zf/(zn-zf), m43=zn*zf/(zn-zf)
    if (m34 < 0.0f) {
        const float zn = -m43 / m33;
        const float denom = (m33 + 1.0f);
        if (zn > 0.0f && std::fabs(denom) > 1e-6f) {
            const float zf = (m33 * zn) / denom;
            if (zf > zn) {
                out.zn = zn;
                out.zf = zf;
                out.valid = true;
                return out;
            }
        }
    }

    return out;
}

static float ScoreProjectionCandidate(const D3DMATRIX& p, ProjInfo* infoOut) {
    if (infoOut) *infoOut = {};
    if (!IsFiniteMatrix(p)) return -1.0f;

    const float absSum = AbsSumMatrix(p);
    if (absSum < 1e-3f) return -1.0f;

    ProjInfo info = ExtractNearFarRowMajor(p);
    if (infoOut) *infoOut = info;
    if (!info.valid) return 0.0f;

    // Prefer farther far-plane and sane ranges.
    if (!(info.zn > 0.0f && info.zf > info.zn)) return 0.0f;
    if (info.zn > 10.0f) return 0.25f;      // unlikely: near too big
    if (info.zf < 5.0f) return 0.1f;         // matches the "wall" symptom
    if (info.zf > 1e7f) return 0.25f;        // unlikely huge

    // Scale score gently by far-plane distance.
    const float zf = (info.zf > 1.0f) ? info.zf : 1.0f;
    return 1.0f + std::log10(zf);
}

static bool IsWorldishDrawGate(DWORD zEnable, DWORD zWriteEnable, DWORD alphaBlendEnable, DWORD colorWriteEnable) {
    const bool zOn = (zEnable != D3DZB_FALSE);
    const bool zWrite = (zWriteEnable != FALSE);
    const bool noBlend = (alphaBlendEnable == FALSE);
    (void)colorWriteEnable; // allow depth-only prepass (COLORWRITE=0)
    return zOn && zWrite && noBlend;
}

D3D9DeviceProxy::D3D9DeviceProxy(IDirect3DDevice9* real) : m_real(real) {
    if (m_real) {
        m_real->AddRef();
        m_real->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&m_realEx));
    }

    {
        char buf[64] = {};
        DWORD n = GetEnvironmentVariableA("WALLEFFP_FORCE_PRIMARY_RT", buf, static_cast<DWORD>(sizeof(buf)));
        if (n > 0) m_forcePrimaryRT = (buf[0] != '0');
    }
    {
        char buf[64] = {};
        DWORD n = GetEnvironmentVariableA("WALLEFFP_FORCE_PRIMARY_RT_AGGRESSIVE", buf, static_cast<DWORD>(sizeof(buf)));
        if (n > 0) m_forcePrimaryRTAggressive = (buf[0] != '0');
    }

    ProxyAddRemixRef();
    UpdateBackbufferDesc();
}

D3D9DeviceProxy::~D3D9DeviceProxy() {
    if (m_cachedBackbuffer) {
        m_cachedBackbuffer->Release();
        m_cachedBackbuffer = nullptr;
    }
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

bool D3D9DeviceProxy::Overlaps(UINT start, UINT count, UINT a, UINT b) {
    const UINT end = start + count; // exclusive
    return !(end <= a || start >= b);
}

bool D3D9DeviceProxy::PublishCameraFromVsConstsIfValid() {
    if (!m_real || !m_haveVsConsts) return false;

    auto load = [&](UINT base) {
        D3DMATRIX m{};
        m._11 = m_vsConsts[base + 0][0];
        m._12 = m_vsConsts[base + 0][1];
        m._13 = m_vsConsts[base + 0][2];
        m._14 = m_vsConsts[base + 0][3];
        m._21 = m_vsConsts[base + 1][0];
        m._22 = m_vsConsts[base + 1][1];
        m._23 = m_vsConsts[base + 1][2];
        m._24 = m_vsConsts[base + 1][3];
        m._31 = m_vsConsts[base + 2][0];
        m._32 = m_vsConsts[base + 2][1];
        m._33 = m_vsConsts[base + 2][2];
        m._34 = m_vsConsts[base + 2][3];
        m._41 = m_vsConsts[base + 3][0];
        m._42 = m_vsConsts[base + 3][1];
        m._43 = m_vsConsts[base + 3][2];
        m._44 = m_vsConsts[base + 3][3];
        return m;
    };

    const D3DMATRIX viewRaw = load(5);
    const D3DMATRIX projRaw = load(10);

    ProjInfo infoRaw{};
    ProjInfo infoT{};
    const float scoreRaw = ScoreProjectionCandidate(projRaw, &infoRaw);
    const D3DMATRIX projT = TransposeMatrix(projRaw);
    const float scoreT = ScoreProjectionCandidate(projT, &infoT);

    const bool needsTranspose = (scoreT > scoreRaw);
    const D3DMATRIX view = needsTranspose ? TransposeMatrix(viewRaw) : viewRaw;
    const D3DMATRIX proj = needsTranspose ? projT : projRaw;

    if (!IsFiniteMatrix(view) || !IsFiniteMatrix(proj)) return false;
    if (AbsSumMatrix(view) < 1e-3f || AbsSumMatrix(proj) < 1e-3f) return false;

    // Reject projections that still imply the "wall" far-plane.
    ProjInfo chosenInfo = needsTranspose ? infoT : infoRaw;
    if (!chosenInfo.valid) return false;
    if (chosenInfo.zf < 5.0f) return false;

    m_real->SetTransform(D3DTS_VIEW, &view);
    m_real->SetTransform(D3DTS_PROJECTION, &proj);

    m_lastView = view;
    m_lastProj = proj;
    m_haveLastCamera = true;

    if (needsTranspose) {
        OutputDebugStringA("[WALLEFFP] Projection candidate: transposed\n");
    }

    if (m_dbgCameraPublishes <= 5 || (m_dbgCameraPublishes % 300 == 0)) {
        char buf[192];
        std::snprintf(
            buf,
            sizeof(buf),
            "[WALLEFFP] Camera proj zn=%.4f zf=%.4f (scoreRaw=%.2f scoreT=%.2f)\n",
            chosenInfo.zn,
            chosenInfo.zf,
            scoreRaw,
            scoreT);
        OutputDebugStringA(buf);
    }
    return true;
}

void D3D9DeviceProxy::TryPublishCameraOncePerScene() {
    static const bool publishCamera = ProxyEnvFlag("WALLEFFP_PUBLISH_CAMERA", true);
    if (!publishCamera) return;
    if (!m_inScene) return;
    if (m_publishedThisScene) return;
    if (!m_haveVsConsts) return;

    if (!m_rt0IsPrimary) return;
    if (!IsWorldishDrawGate(m_rsZEnable, m_rsZWriteEnable, m_rsAlphaBlendEnable, m_rsColorWriteEnable)) return;

    bool published = false;
    if (m_dirtyView && m_dirtyProj) published = PublishCameraFromVsConstsIfValid();
    if (!published && m_haveLastCamera) {
        m_real->SetTransform(D3DTS_VIEW, &m_lastView);
        m_real->SetTransform(D3DTS_PROJECTION, &m_lastProj);
        published = true;
    }

    if (published) {
        m_publishedThisScene = true;
        m_dirtyView = m_dirtyProj = m_dirtyWorld = false;

        m_dbgCameraPublishes++;
        if ((m_dbgCameraPublishes <= 10) || (m_dbgCameraPublishes % 300 == 0)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "[WALLEFFP] Camera published (%u)\n", m_dbgCameraPublishes);
            OutputDebugStringA(buf);
        }
    }
}

// Per-frame RT0 usage, dumped every kFrameStatsPeriod frames: which targets the
// game draws into, how many draws each gets and on which the camera was published.
void D3D9DeviceProxy::NoteFrameRt0(IDirect3DSurface9* rt) {
    for (UINT i = 0; i < m_frameRtCount; i++) {
        if (m_frameRts[i].surface == rt) {
            m_frameRtCur = static_cast<int>(i);
            return;
        }
    }
    if (m_frameRtCount == kMaxFrameRts) {
        m_frameRtCur = -1;
        return;
    }
    m_frameRts[m_frameRtCount] = {rt, 0, 0};
    m_frameRtCur = static_cast<int>(m_frameRtCount++);
}

void D3D9DeviceProxy::NoteFrameDraw() {
    if (m_frameRtCur < 0) return;
    m_frameRts[m_frameRtCur].draws++;
    if (m_publishedThisScene) m_frameRts[m_frameRtCur].cameraDraws++;
}

void D3D9DeviceProxy::EndFrameStats() {
    if ((m_frameNo++ % kFrameStatsPeriod) == 0) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "[WALLEFFP] frame %u: %u RT0 targets, bb=%ux%u fmt=%u, cameraPublishes=%u\n",
                      m_frameNo, m_frameRtCount, m_bbWidth, m_bbHeight, static_cast<unsigned>(m_bbFormat), m_dbgCameraPublishes);
        OutputDebugStringA(buf);
        for (UINT i = 0; i < m_frameRtCount; i++) {
            const FrameRt& f = m_frameRts[i];
            D3DSURFACE_DESC d{};
            if (f.surface) f.surface->GetDesc(&d);
            std::snprintf(buf, sizeof(buf), "[WALLEFFP]   rt%u %p %s %ux%u fmt=%u draws=%u cameraDraws=%u\n",
                          i, static_cast<void*>(f.surface), f.surface == m_cachedBackbuffer ? "BACKBUFFER" : "offscreen",
                          d.Width, d.Height, static_cast<unsigned>(d.Format), f.draws, f.cameraDraws);
            OutputDebugStringA(buf);
        }
    }
    m_frameRtCount = 0;
    m_frameRtCur = -1;
}

void D3D9DeviceProxy::ReinstallActivateAppFilter() {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (SUCCEEDED(m_real->GetCreationParameters(&cp))) InstallActivateAppFilter(cp.hFocusWindow);
}

// D3D9 refuses Reset/ResetEx (D3DERR_INVALIDCALL) while any swap chain buffer
// is still referenced, so the cached backbuffer must be dropped first.
void D3D9DeviceProxy::ReleaseCachedBackbuffer() {
    if (m_cachedBackbuffer) {
        m_cachedBackbuffer->Release();
        m_cachedBackbuffer = nullptr;
    }
}

void D3D9DeviceProxy::UpdateBackbufferDesc() {
    if (!m_real) return;

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(m_real->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;

    D3DSURFACE_DESC d{};
    if (SUCCEEDED(bb->GetDesc(&d))) {
        m_bbWidth = d.Width;
        m_bbHeight = d.Height;
        m_bbFormat = d.Format;
    }

    ReleaseCachedBackbuffer();
    m_cachedBackbuffer = bb; // keep ref; dropped again before Reset/ResetEx
}

bool D3D9DeviceProxy::IsLikelyMainSceneRT(const D3DSURFACE_DESC& d) const {
    if (!m_bbWidth || !m_bbHeight) return false;
    if (d.Width != m_bbWidth || d.Height != m_bbHeight) return false;

    switch (d.Format) {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
            return true;
        default:
            return false;
    }
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;

    // Only claim IDirect3DDevice9Ex when the real device is one: D3DX and games
    // switch resource paths on this query, and our Ex methods fail without m_realEx.
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9) || (riid == __uuidof(IDirect3DDevice9Ex) && m_realEx)) {
        *ppvObj = static_cast<IDirect3DDevice9Ex*>(this);
        AddRef();
        return S_OK;
    }

    return m_real ? m_real->QueryInterface(riid, ppvObj) : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D3D9DeviceProxy::AddRef() { return ++m_refCount; }
ULONG STDMETHODCALLTYPE D3D9DeviceProxy::Release() {
    ULONG ref = --m_refCount;
    if (ref == 0) delete this;
    return ref;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::TestCooperativeLevel() { return m_real->TestCooperativeLevel(); }
UINT STDMETHODCALLTYPE D3D9DeviceProxy::GetAvailableTextureMem() { return m_real->GetAvailableTextureMem(); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::EvictManagedResources() { return m_real->EvictManagedResources(); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetDirect3D(IDirect3D9** ppD3D9) { return m_real->GetDirect3D(ppD3D9); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetDeviceCaps(D3DCAPS9* pCaps) { return m_real->GetDeviceCaps(pCaps); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) { return m_real->GetDisplayMode(iSwapChain, pMode); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParameters) { return m_real->GetCreationParameters(pParameters); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) { return m_real->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap); }
void STDMETHODCALLTYPE D3D9DeviceProxy::SetCursorPosition(int X, int Y, DWORD Flags) { m_real->SetCursorPosition(X, Y, Flags); }
BOOL STDMETHODCALLTYPE D3D9DeviceProxy::ShowCursor(BOOL bShow) { return m_real->ShowCursor(bShow); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain) { return m_real->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) { return m_real->GetSwapChain(iSwapChain, pSwapChain); }
UINT STDMETHODCALLTYPE D3D9DeviceProxy::GetNumberOfSwapChains() { return m_real->GetNumberOfSwapChains(); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) {
    ReleaseCachedBackbuffer();
    const HRESULT hr = m_real->Reset(pPresentationParameters);
    if (SUCCEEDED(hr)) UpdateBackbufferDesc();
    ReinstallActivateAppFilter();
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::Present(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) {
    EndFrameStats();
    return m_real->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) { return m_real->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) { return m_real->GetRasterStatus(iSwapChain, pRasterStatus); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetDialogBoxMode(BOOL bEnableDialogs) { return m_real->SetDialogBoxMode(bEnableDialogs); }
void STDMETHODCALLTYPE D3D9DeviceProxy::SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP* pRamp) { m_real->SetGammaRamp(iSwapChain, Flags, pRamp); }
void STDMETHODCALLTYPE D3D9DeviceProxy::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) { m_real->GetGammaRamp(iSwapChain, pRamp); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) { return m_real->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) { return m_real->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) { return m_real->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) { return m_real->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) { return m_real->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) { return m_real->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) { return m_real->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::UpdateSurface(IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, const POINT* pDestPoint) { return m_real->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) { return m_real->UpdateTexture(pSourceTexture, pDestinationTexture); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) { return m_real->GetRenderTargetData(pRenderTarget, pDestSurface); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) { return m_real->GetFrontBufferData(iSwapChain, pDestSurface); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::StretchRect(IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect, IDirect3DSurface9* pDestSurface, const RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter) { return m_real->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::ColorFill(IDirect3DSurface9* pSurface, const RECT* pRect, D3DCOLOR color) { return m_real->ColorFill(pSurface, pRect, color); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) { return m_real->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) {
    if (RenderTargetIndex == 0) {
        m_rt0IsPrimary = (pRenderTarget && pRenderTarget == m_cachedBackbuffer);
        NoteFrameRt0(pRenderTarget);
    }

    if (m_forcePrimaryRT && RenderTargetIndex == 0 && pRenderTarget && m_cachedBackbuffer) {
        if (pRenderTarget == m_cachedBackbuffer) {
            m_rt0IsPrimary = true;
            return m_real->SetRenderTarget(0, pRenderTarget);
        }

        if (m_forcePrimaryRTAggressive) {
            const HRESULT hr = m_real->SetRenderTarget(0, m_cachedBackbuffer);
            if (SUCCEEDED(hr)) {
                m_rt0IsPrimary = true;
                m_dbgRtOverrides++;
                if ((m_dbgRtOverrides <= 10) || (m_dbgRtOverrides % 300 == 0)) {
                    OutputDebugStringA("[WALLEFFP] Overrode RT0 -> backbuffer (aggressive)\n");
                }
            }
            return hr;
        }

        D3DSURFACE_DESC d{};
        if (SUCCEEDED(pRenderTarget->GetDesc(&d)) && IsLikelyMainSceneRT(d)) {
            const HRESULT hr = m_real->SetRenderTarget(0, m_cachedBackbuffer);
            if (SUCCEEDED(hr)) {
                m_rt0IsPrimary = true;
                m_dbgRtOverrides++;
                if ((m_dbgRtOverrides <= 10) || (m_dbgRtOverrides % 300 == 0)) {
                    char buf[160];
                    std::snprintf(
                        buf,
                        sizeof(buf),
                        "[WALLEFFP] Overrode RT0 -> backbuffer (%u), rt fmt=%u %ux%u\n",
                        m_dbgRtOverrides,
                        static_cast<unsigned>(d.Format),
                        static_cast<unsigned>(d.Width),
                        static_cast<unsigned>(d.Height));
                    OutputDebugStringA(buf);
                }
            }
            return hr;
        }
    }

    return m_real->SetRenderTarget(RenderTargetIndex, pRenderTarget);
}
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) { return m_real->GetRenderTarget(RenderTargetIndex, ppRenderTarget); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) { return m_real->SetDepthStencilSurface(pNewZStencil); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) { return m_real->GetDepthStencilSurface(ppZStencilSurface); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::BeginScene() {
    m_inScene = true;
    m_publishedThisScene = false;

    const HRESULT hr = m_real->BeginScene();
    if (SUCCEEDED(hr) && m_forcePrimaryRT && m_cachedBackbuffer) {
        m_real->SetRenderTarget(0, m_cachedBackbuffer);
        m_rt0IsPrimary = true;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::EndScene() {
    m_inScene = false;
    return m_real->EndScene();
}
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::Clear(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) { return m_real->Clear(Count, pRects, Flags, Color, Z, Stencil); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) { return m_real->SetTransform(State, pMatrix); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) { return m_real->GetTransform(State, pMatrix); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) { return m_real->MultiplyTransform(State, pMatrix); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetViewport(const D3DVIEWPORT9* pViewport) { return m_real->SetViewport(pViewport); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetViewport(D3DVIEWPORT9* pViewport) { return m_real->GetViewport(pViewport); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetMaterial(const D3DMATERIAL9* pMaterial) { return m_real->SetMaterial(pMaterial); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetMaterial(D3DMATERIAL9* pMaterial) { return m_real->GetMaterial(pMaterial); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetLight(DWORD Index, const D3DLIGHT9* pLight) { return m_real->SetLight(Index, pLight); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetLight(DWORD Index, D3DLIGHT9* pLight) { return m_real->GetLight(Index, pLight); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::LightEnable(DWORD Index, BOOL Enable) { return m_real->LightEnable(Index, Enable); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetLightEnable(DWORD Index, BOOL* pEnable) { return m_real->GetLightEnable(Index, pEnable); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetClipPlane(DWORD Index, const float* pPlane) { return m_real->SetClipPlane(Index, pPlane); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetClipPlane(DWORD Index, float* pPlane) { return m_real->GetClipPlane(Index, pPlane); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
    switch (State) {
        case D3DRS_ZENABLE:
            m_rsZEnable = Value;
            break;
        case D3DRS_ZWRITEENABLE:
            m_rsZWriteEnable = Value;
            break;
        case D3DRS_ALPHABLENDENABLE:
            m_rsAlphaBlendEnable = Value;
            break;
        case D3DRS_COLORWRITEENABLE:
            m_rsColorWriteEnable = Value;
            break;
        default:
            break;
    }
    return m_real->SetRenderState(State, Value);
}
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) { return m_real->GetRenderState(State, pValue); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) { return m_real->CreateStateBlock(Type, ppSB); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::BeginStateBlock() { return m_real->BeginStateBlock(); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::EndStateBlock(IDirect3DStateBlock9** ppSB) { return m_real->EndStateBlock(ppSB); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetClipStatus(const D3DCLIPSTATUS9* pClipStatus) { return m_real->SetClipStatus(pClipStatus); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetClipStatus(D3DCLIPSTATUS9* pClipStatus) { return m_real->GetClipStatus(pClipStatus); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) { return m_real->GetTexture(Stage, ppTexture); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) { return m_real->SetTexture(Stage, pTexture); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) { return m_real->GetTextureStageState(Stage, Type, pValue); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) { return m_real->SetTextureStageState(Stage, Type, Value); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) { return m_real->GetSamplerState(Sampler, Type, pValue); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) { return m_real->SetSamplerState(Sampler, Type, Value); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::ValidateDevice(DWORD* pNumPasses) { return m_real->ValidateDevice(pNumPasses); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries) { return m_real->SetPaletteEntries(PaletteNumber, pEntries); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) { return m_real->GetPaletteEntries(PaletteNumber, pEntries); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetCurrentTexturePalette(UINT PaletteNumber) { return m_real->SetCurrentTexturePalette(PaletteNumber); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetCurrentTexturePalette(UINT* PaletteNumber) { return m_real->GetCurrentTexturePalette(PaletteNumber); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetScissorRect(const RECT* pRect) { return m_real->SetScissorRect(pRect); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetScissorRect(RECT* pRect) { return m_real->GetScissorRect(pRect); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetSoftwareVertexProcessing(BOOL bSoftware) { return m_real->SetSoftwareVertexProcessing(bSoftware); }
BOOL STDMETHODCALLTYPE D3D9DeviceProxy::GetSoftwareVertexProcessing() { return m_real->GetSoftwareVertexProcessing(); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetNPatchMode(float nSegments) { return m_real->SetNPatchMode(nSegments); }
float STDMETHODCALLTYPE D3D9DeviceProxy::GetNPatchMode() { return m_real->GetNPatchMode(); }

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
    TryPublishCameraOncePerScene();
    NoteFrameDraw();
    return m_real->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) {
    TryPublishCameraOncePerScene();
    NoteFrameDraw();
    return m_real->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) { return m_real->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) { return m_real->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) { return m_real->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateVertexDeclaration(const D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) { return m_real->CreateVertexDeclaration(pVertexElements, ppDecl); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) { return m_real->SetVertexDeclaration(pDecl); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) { return m_real->GetVertexDeclaration(ppDecl); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetFVF(DWORD FVF) { return m_real->SetFVF(FVF); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetFVF(DWORD* pFVF) { return m_real->GetFVF(pFVF); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateVertexShader(const DWORD* pFunction, IDirect3DVertexShader9** ppShader) { return m_real->CreateVertexShader(pFunction, ppShader); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetVertexShader(IDirect3DVertexShader9* pShader) { return m_real->SetVertexShader(pShader); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetVertexShader(IDirect3DVertexShader9** ppShader) { return m_real->GetVertexShader(ppShader); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetVertexShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) {
    if (pConstantData && StartRegister < 256 && Vector4fCount <= 256 - StartRegister) {
        std::memcpy(&m_vsConsts[StartRegister][0], pConstantData, sizeof(float) * 4 * Vector4fCount);
        m_haveVsConsts = true;

        if (Overlaps(StartRegister, Vector4fCount, 5, 9)) m_dirtyView = true;
        if (Overlaps(StartRegister, Vector4fCount, 10, 14)) m_dirtyProj = true;
        if (Overlaps(StartRegister, Vector4fCount, 14, 18)) m_dirtyWorld = true;
    }

    return m_real->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetConvolutionMonoKernel(UINT width, UINT height, float* rows, float* columns) {
    return m_realEx ? m_realEx->SetConvolutionMonoKernel(width, height, rows, columns) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::ComposeRects(IDirect3DSurface9* pSrc, IDirect3DSurface9* pDst, IDirect3DVertexBuffer9* pSrcRectDescs, UINT NumRects, IDirect3DVertexBuffer9* pDstRectDescs, D3DCOMPOSERECTSOP Operation, INT Xoffset, INT Yoffset) {
    return m_realEx ? m_realEx->ComposeRects(pSrc, pDst, pSrcRectDescs, NumRects, pDstRectDescs, Operation, Xoffset, Yoffset) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::PresentEx(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion, DWORD dwFlags) {
    return m_realEx ? m_realEx->PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags) : Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetGPUThreadPriority(INT* pPriority) {
    return m_realEx ? m_realEx->GetGPUThreadPriority(pPriority) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetGPUThreadPriority(INT Priority) {
    return m_realEx ? m_realEx->SetGPUThreadPriority(Priority) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::WaitForVBlank(UINT iSwapChain) {
    return m_realEx ? m_realEx->WaitForVBlank(iSwapChain) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CheckResourceResidency(IDirect3DResource9** pResourceArray, UINT32 NumResources) {
    return m_realEx ? m_realEx->CheckResourceResidency(pResourceArray, NumResources) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetMaximumFrameLatency(UINT MaxLatency) {
    return m_realEx ? m_realEx->SetMaximumFrameLatency(MaxLatency) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetMaximumFrameLatency(UINT* pMaxLatency) {
    return m_realEx ? m_realEx->GetMaximumFrameLatency(pMaxLatency) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CheckDeviceState(HWND hDestinationWindow) {
    return m_realEx ? m_realEx->CheckDeviceState(hDestinationWindow) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateRenderTargetEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) {
    return m_realEx ? m_realEx->CreateRenderTargetEx(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle, Usage)
                    : CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) {
    return m_realEx ? m_realEx->CreateOffscreenPlainSurfaceEx(Width, Height, Format, Pool, ppSurface, pSharedHandle, Usage)
                    : CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateDepthStencilSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) {
    return m_realEx ? m_realEx->CreateDepthStencilSurfaceEx(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle, Usage)
                    : CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::ResetEx(D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode) {
    if (!m_realEx) return D3DERR_INVALIDCALL;
    ReleaseCachedBackbuffer();
    const HRESULT hr = m_realEx->ResetEx(pPresentationParameters, pFullscreenDisplayMode);
    if (SUCCEEDED(hr)) UpdateBackbufferDesc();
    ReinstallActivateAppFilter();
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation) {
    return m_realEx ? m_realEx->GetDisplayModeEx(iSwapChain, pMode, pRotation) : D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) { return m_real->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetVertexShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) { return m_real->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) { return m_real->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetVertexShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) { return m_real->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) { return m_real->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) { return m_real->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride) { return m_real->GetStreamSource(StreamNumber, ppStreamData, pOffsetInBytes, pStride); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetStreamSourceFreq(UINT StreamNumber, UINT Divider) { return m_real->SetStreamSourceFreq(StreamNumber, Divider); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetStreamSourceFreq(UINT StreamNumber, UINT* Divider) { return m_real->GetStreamSourceFreq(StreamNumber, Divider); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetIndices(IDirect3DIndexBuffer9* pIndexData) { return m_real->SetIndices(pIndexData); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetIndices(IDirect3DIndexBuffer9** ppIndexData) { return m_real->GetIndices(ppIndexData); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreatePixelShader(const DWORD* pFunction, IDirect3DPixelShader9** ppShader) { return m_real->CreatePixelShader(pFunction, ppShader); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetPixelShader(IDirect3DPixelShader9* pShader) { return m_real->SetPixelShader(pShader); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetPixelShader(IDirect3DPixelShader9** ppShader) { return m_real->GetPixelShader(ppShader); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetPixelShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) { return m_real->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) { return m_real->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetPixelShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) { return m_real->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) { return m_real->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::SetPixelShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) { return m_real->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) { return m_real->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::DrawRectPatch(UINT Handle, const float* pNumSegs, const D3DRECTPATCH_INFO* pRectPatchInfo) { return m_real->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::DrawTriPatch(UINT Handle, const float* pNumSegs, const D3DTRIPATCH_INFO* pTriPatchInfo) { return m_real->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::DeletePatch(UINT Handle) { return m_real->DeletePatch(Handle); }
HRESULT STDMETHODCALLTYPE D3D9DeviceProxy::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) { return m_real->CreateQuery(Type, ppQuery); }

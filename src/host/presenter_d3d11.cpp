#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "presenter.h"

#include "chrome.h"
#include "log.h"
#include "mirror_layout.h"
#include "protocol.h"
#include "shaders.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct QuadCB {
    float l, t, r, b;
    float u0, v0, u1, v1;
    float cr, cg, cb, ca;
    float pad0, pad1, pad2, pad3;
};

struct TabGpu {
    ComPtr<ID3D11Texture2D> sharedTex;
    ComPtr<ID3D11Texture2D> copyTex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<IDXGIKeyedMutex> mutex;
    HANDLE handle = nullptr;
    bool hasFrame = false;
    int texW = 0;
    int texH = 0;
};

bool check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) {
        return true;
    }
    logf("%s failed: 0x%08lX", what, (unsigned long)hr);
    return false;
}

ComPtr<ID3DBlob> compileShader(const char* source, const char* entry, const char* profile) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL2;
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, profile, flags, 0, &shader, &errors);
    if (FAILED(hr)) {
        if (errors) {
            logf("%s", static_cast<const char*>(errors->GetBufferPointer()));
        }
        check(hr, entry);
        return nullptr;
    }
    return shader;
}

void toNdc(float x, float y, float w, float h, int windowW, int windowH, float& l, float& t, float& r, float& b) {
    l = (x / (float)windowW) * 2.f - 1.f;
    r = ((x + w) / (float)windowW) * 2.f - 1.f;
    t = 1.f - (y / (float)windowH) * 2.f;
    b = 1.f - ((y + h) / (float)windowH) * 2.f;
}

struct Box {
    float x, y, w, h;
};

Box letterbox(int windowW, int windowH) {
    int availW = windowW;
    int availH = std::max(1, windowH - kChromeHeight);
    float scale = std::min(availW / (float)kSurfaceWidth, availH / (float)kSurfaceHeight);
    Box box;
    box.w = kSurfaceWidth * scale;
    box.h = kSurfaceHeight * scale;
    box.x = (availW - box.w) * 0.5f;
    box.y = (float)kChromeHeight + (availH - box.h) * 0.5f;
    return box;
}

}  // namespace

struct Presenter::Impl {
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> quadCb;
    ComPtr<ID3D11SamplerState> linear;
    ComPtr<ID3D11SamplerState> point;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11Texture2D> whiteTex;
    ComPtr<ID3D11ShaderResourceView> whiteSrv;
    ComPtr<ID3D11Texture2D> chromeTex;
    ComPtr<ID3D11ShaderResourceView> chromeSrv;
    std::vector<uint8_t> chromePixels;
    TabGpu tabs[kMaxRenderers];
    int width = 0;
    int height = 0;
    int chromeWidth = 0;

    void drawQuad(float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                  float r, float g, float b, float a, ID3D11ShaderResourceView* srv, ID3D11SamplerState* sampler) {
        if (width < 1 || height < 1 || w <= 0.f || h <= 0.f) {
            return;
        }
        QuadCB cb{};
        toNdc(x, y, w, h, width, height, cb.l, cb.t, cb.r, cb.b);
        cb.u0 = u0;
        cb.v0 = v0;
        cb.u1 = u1;
        cb.v1 = v1;
        cb.cr = r;
        cb.cg = g;
        cb.cb = b;
        cb.ca = a;
        context->UpdateSubresource(quadCb.Get(), 0, nullptr, &cb, 0, 0);
        context->PSSetShaderResources(0, 1, &srv);
        context->PSSetSamplers(0, 1, &sampler);
        context->Draw(6, 0);
    }

    bool ensureTarget(int w, int h) {
        if (w == width && h == height && rtv) {
            return true;
        }
        ID3D11RenderTargetView* unbound = nullptr;
        context->OMSetRenderTargets(1, &unbound, nullptr);
        ID3D11ShaderResourceView* unboundSrv = nullptr;
        context->PSSetShaderResources(0, 1, &unboundSrv);
        rtv.Reset();
        if (!check(swap->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers")) {
            return false;
        }
        ComPtr<ID3D11Texture2D> back;
        if (!check(swap->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer")) {
            return false;
        }
        if (!check(device->CreateRenderTargetView(back.Get(), nullptr, &rtv), "CreateRenderTargetView")) {
            return false;
        }
        width = w;
        height = h;
        return true;
    }

    void uploadChrome(const HostStatus& status) {
        if (status.windowWidth < 1) {
            return;
        }
        if (chromeWidth != status.windowWidth) {
            chromeTex.Reset();
            chromeSrv.Reset();
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = (UINT)status.windowWidth;
            desc.Height = (UINT)kChromeHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (!check(device->CreateTexture2D(&desc, nullptr, &chromeTex), "chrome texture")) {
                return;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = desc.Format;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            if (!check(device->CreateShaderResourceView(chromeTex.Get(), &sv, &chromeSrv), "chrome srv")) {
                return;
            }
            chromeWidth = status.windowWidth;
            chromePixels.resize((size_t)chromeWidth * kChromeHeight * 4);
        }
        ChromeInfo info;
        info.width = chromeWidth;
        info.height = kChromeHeight;
        info.hover = status.hover;
        for (int i = 0; i < kMaxRenderers; ++i) {
            info.listed[i] = status.listed[i];
            info.selected[i] = status.selected[i];
            std::memcpy(info.label[i], status.label[i], sizeof(info.label[i]));
        }
        paintChrome(chromePixels.data(), chromeWidth * 4, info);
        context->UpdateSubresource(chromeTex.Get(), 0, nullptr, chromePixels.data(), (UINT)chromeWidth * 4, 0);
    }
};

Presenter::Presenter() : impl_(new Impl) {}

Presenter::~Presenter() {
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool Presenter::init(void* nativeWindow) {
    impl_->hwnd = static_cast<HWND>(nativeWindow);
    RECT client{};
    GetClientRect(impl_->hwnd, &client);
    int width = std::max(1L, client.right - client.left);
    int height = std::max(1L, client.bottom - client.top);

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (!check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &level, 1, D3D11_SDK_VERSION,
                                 &impl_->device, nullptr, &impl_->context),
               "D3D11CreateDevice")) {
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (!check(impl_->device.As(&dxgiDevice), "IDXGIDevice")) {
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (!check(dxgiDevice->GetAdapter(&adapter), "GetAdapter")) {
        return false;
    }
    ComPtr<IDXGIFactory2> factory;
    if (!check(adapter->GetParent(IID_PPV_ARGS(&factory)), "IDXGIFactory2")) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (!check(factory->CreateSwapChainForHwnd(impl_->device.Get(), impl_->hwnd, &desc, nullptr, nullptr, &impl_->swap),
               "CreateSwapChainForHwnd")) {
        return false;
    }
    factory->MakeWindowAssociation(impl_->hwnd, DXGI_MWA_NO_ALT_ENTER);

    auto vsBlob = compileShader(kPresentShader, "vs_quad", "vs_5_0");
    auto psBlob = compileShader(kPresentShader, "ps_quad", "ps_5_0");
    if (!vsBlob || !psBlob) {
        return false;
    }
    if (!check(impl_->device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &impl_->vs),
               "CreateVertexShader") ||
        !check(impl_->device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &impl_->ps),
               "CreatePixelShader")) {
        return false;
    }

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(QuadCB);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (!check(impl_->device->CreateBuffer(&cbDesc, nullptr, &impl_->quadCb), "quad constant buffer")) {
        return false;
    }

    D3D11_SAMPLER_DESC samp{};
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (!check(impl_->device->CreateSamplerState(&samp, &impl_->linear), "linear sampler")) {
        return false;
    }
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (!check(impl_->device->CreateSamplerState(&samp, &impl_->point), "point sampler")) {
        return false;
    }

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (!check(impl_->device->CreateRasterizerState(&raster, &impl_->raster), "rasterizer")) {
        return false;
    }
    D3D11_DEPTH_STENCIL_DESC depth{};
    if (!check(impl_->device->CreateDepthStencilState(&depth, &impl_->depth), "depth")) {
        return false;
    }

    uint32_t white = 0xFFFFFFFFu;
    D3D11_TEXTURE2D_DESC one{};
    one.Width = 1;
    one.Height = 1;
    one.MipLevels = 1;
    one.ArraySize = 1;
    one.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    one.SampleDesc.Count = 1;
    one.Usage = D3D11_USAGE_DEFAULT;
    one.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA oneData{};
    oneData.pSysMem = &white;
    oneData.SysMemPitch = 4;
    if (!check(impl_->device->CreateTexture2D(&one, &oneData, &impl_->whiteTex), "white texture")) {
        return false;
    }
    if (!check(impl_->device->CreateShaderResourceView(impl_->whiteTex.Get(), nullptr, &impl_->whiteSrv), "white srv")) {
        return false;
    }

    impl_->width = width;
    impl_->height = height;
    return impl_->ensureTarget(width, height);
}

void Presenter::releaseSurface(int tab) {
    if (!impl_ || tab < 0 || tab >= kMaxRenderers) {
        return;
    }
    TabGpu& gpu = impl_->tabs[tab];
    if (impl_->context) {
        ID3D11ShaderResourceView* unbound = nullptr;
        impl_->context->PSSetShaderResources(0, 1, &unbound);
    }
    if (gpu.handle) {
        CloseHandle(gpu.handle);
        gpu.handle = nullptr;
    }
    gpu.sharedTex.Reset();
    gpu.copyTex.Reset();
    gpu.srv.Reset();
    gpu.mutex.Reset();
    gpu.hasFrame = false;
    gpu.texW = 0;
    gpu.texH = 0;
}

bool Presenter::createSurface(int tab, uint32_t ownerPid, uint32_t generation, int width, int height, uint64_t* tokenOut) {
    if (tokenOut) {
        *tokenOut = 0;
    }
    if (tab < 0 || tab >= kMaxRenderers || !impl_->device || width < 1 || height < 1) {
        return false;
    }
    releaseSurface(tab);
    TabGpu& gpu = impl_->tabs[tab];

    ComPtr<ID3D11Device1> device1;
    if (!check(impl_->device.As(&device1), "ID3D11Device1")) {
        return false;
    }
    wchar_t name[160];
    sharedSurfaceName(name, 160, ownerPid, generation);
    HRESULT opened = E_FAIL;
    for (int attempt = 0; attempt < 20; ++attempt) {
        opened = device1->OpenSharedResourceByName(name, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&gpu.sharedTex));
        if (SUCCEEDED(opened)) {
            break;
        }
        Sleep(15);
    }
    if (!check(opened, "OpenSharedResourceByName")) {
        logf("surface name was %ls", name);
        return false;
    }
    if (!check(gpu.sharedTex.As(&gpu.mutex), "IDXGIKeyedMutex")) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (!check(impl_->device->CreateTexture2D(&desc, nullptr, &gpu.copyTex), "copy texture")) {
        return false;
    }
    if (!check(impl_->device->CreateShaderResourceView(gpu.copyTex.Get(), nullptr, &gpu.srv), "copy srv")) {
        return false;
    }
    gpu.texW = width;
    gpu.texH = height;
    logf("shared surface tab %d  %ls", tab, name);
    return true;
}

void Presenter::pumpSurfaces() {
    if (!impl_->context) {
        return;
    }
    bool copied = false;
    for (int i = 0; i < kMaxRenderers; ++i) {
        TabGpu& gpu = impl_->tabs[i];
        if (!gpu.mutex || !gpu.copyTex) {
            continue;
        }
        // S_OK only. WAIT_TIMEOUT is a successful HRESULT and must not be treated as acquired.
        HRESULT hr = gpu.mutex->AcquireSync(1, 0);
        if (hr != S_OK) {
            continue;
        }
        impl_->context->CopyResource(gpu.copyTex.Get(), gpu.sharedTex.Get());
        gpu.mutex->ReleaseSync(0);
        gpu.hasFrame = true;
        copied = true;
    }
    if (copied) {
        impl_->context->Flush();
    }
}

void Presenter::render(const HostStatus& status) {
    if (!impl_->swap || status.windowWidth < 2 || status.windowHeight < 2) {
        return;
    }
    if (!impl_->ensureTarget(status.windowWidth, status.windowHeight)) {
        return;
    }
    impl_->uploadChrome(status);

    float clear[4] = {0.07f, 0.08f, 0.11f, 1.f};
    impl_->context->OMSetRenderTargets(1, impl_->rtv.GetAddressOf(), nullptr);
    impl_->context->ClearRenderTargetView(impl_->rtv.Get(), clear);
    D3D11_VIEWPORT viewport{};
    viewport.Width = (float)impl_->width;
    viewport.Height = (float)impl_->height;
    viewport.MaxDepth = 1.f;
    impl_->context->RSSetViewports(1, &viewport);
    impl_->context->RSSetState(impl_->raster.Get());
    impl_->context->OMSetDepthStencilState(impl_->depth.Get(), 0);
    impl_->context->IASetInputLayout(nullptr);
    impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    impl_->context->VSSetShader(impl_->vs.Get(), nullptr, 0);
    impl_->context->PSSetShader(impl_->ps.Get(), nullptr, 0);
    impl_->context->VSSetConstantBuffers(0, 1, impl_->quadCb.GetAddressOf());
    impl_->context->PSSetConstantBuffers(0, 1, impl_->quadCb.GetAddressOf());

    float chromeH = (float)std::min(kChromeHeight, impl_->height);
    impl_->drawQuad(0, 0, (float)impl_->width, chromeH, 0, 0, 1, 1, 1, 1, 1, 1, impl_->chromeSrv.Get(), impl_->point.Get());

    if (impl_->height > kChromeHeight) {
        bool selected[kMaxRenderers];
        bool alive[kMaxRenderers];
        for (int i = 0; i < kMaxRenderers; ++i) {
            selected[i] = status.selected[i] && status.listed[i];
            alive[i] = status.alive[i] && status.listed[i];
        }
        MirrorCell cells[kMaxRenderers];
        const int count = layoutMirrors(selected, alive, status.frameW, status.frameH, kMaxRenderers, impl_->width, impl_->height, cells,
                                        kMaxRenderers);
        for (int i = 0; i < count; ++i) {
            const MirrorCell& cell = cells[i];
            impl_->drawQuad(cell.x - 2.f, cell.y - 2.f, cell.w + 4.f, cell.h + 4.f, 0, 0, 1, 1, 0, 0, 0, 1, impl_->whiteSrv.Get(), impl_->point.Get());
            TabGpu& gpu = impl_->tabs[cell.slot];
            if (gpu.hasFrame && gpu.srv) {
                impl_->drawQuad(cell.x, cell.y, cell.w, cell.h, 0, 0, 1, 1, 1, 1, 1, 1, gpu.srv.Get(), impl_->linear.Get());
            }
        }
    }

    impl_->swap->Present(1, 0);
}

void Presenter::onRendererFrame(int, uint64_t, void (*)(int, uint64_t, void*), void*) {}

void Presenter::shutdown() {
    if (!impl_) {
        return;
    }
    if (impl_->context) {
        ID3D11RenderTargetView* unbound = nullptr;
        impl_->context->OMSetRenderTargets(1, &unbound, nullptr);
        impl_->context->ClearState();
        impl_->context->Flush();
    }
    for (TabGpu& gpu : impl_->tabs) {
        if (gpu.handle) {
            CloseHandle(gpu.handle);
            gpu.handle = nullptr;
        }
        gpu.mutex.Reset();
        gpu.srv.Reset();
        gpu.copyTex.Reset();
        gpu.sharedTex.Reset();
        gpu.hasFrame = false;
    }
    impl_->chromeSrv.Reset();
    impl_->chromeTex.Reset();
    impl_->whiteSrv.Reset();
    impl_->whiteTex.Reset();
    impl_->rtv.Reset();
    impl_->swap.Reset();
    impl_->context.Reset();
    impl_->device.Reset();
}

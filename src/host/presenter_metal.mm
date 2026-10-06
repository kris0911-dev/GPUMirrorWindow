#include "presenter.h"

#include "chrome.h"
#include "log.h"
#include "mirror_layout.h"
#include "protocol.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <IOSurface/IOSurface.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace {

const char* kPresentMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Quad {
    float4 rect;
    float4 uvRect;
    float4 tint;
    float4 unused;
};

struct VsOut {
    float4 pos [[position]];
    float2 uv;
};

constant float2 kCorners[6] = {
    float2(0, 0), float2(1, 0), float2(0, 1),
    float2(0, 1), float2(1, 0), float2(1, 1)
};

vertex VsOut vs_quad(uint id [[vertex_id]], constant Quad& quad [[buffer(0)]]) {
    float2 c = kCorners[id];
    VsOut o;
    o.pos = float4(mix(quad.rect.x, quad.rect.z, c.x), mix(quad.rect.y, quad.rect.w, c.y), 0.0, 1.0);
    o.uv = mix(quad.uvRect.xy, quad.uvRect.zw, c);
    return o;
}

fragment float4 ps_quad(VsOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler samp [[sampler(0)]], constant Quad& quad [[buffer(0)]]) {
    return tex.sample(samp, in.uv) * quad.tint;
}
)MSL";

struct QuadCB {
    float l, t, r, b;
    float u0, v0, u1, v1;
    float cr, cg, cb, ca;
    float pad0, pad1, pad2, pad3;
};

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

void toNdc(float x, float y, float w, float h, int windowW, int windowH, float& l, float& t, float& r, float& b) {
    l = (x / (float)windowW) * 2.f - 1.f;
    r = ((x + w) / (float)windowW) * 2.f - 1.f;
    t = 1.f - (y / (float)windowH) * 2.f;
    b = 1.f - ((y + h) / (float)windowH) * 2.f;
}

}  // namespace

struct Presenter::Impl {
    NSView* view = nil;
    CAMetalLayer* layer = nil;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLSamplerState> linear = nil;
    id<MTLSamplerState> point = nil;
    id<MTLTexture> white = nil;
    id<MTLTexture> chrome = nil;
    id<MTLCommandBuffer> last = nil;
    IOSurfaceRef surfaces[kMaxRenderers] = {};
    id<MTLTexture> shared[kMaxRenderers] = {};
    id<MTLTexture> copies[kMaxRenderers][2] = {};
    std::atomic<int> published[kMaxRenderers];
    int surfaceW[kMaxRenderers] = {};
    int surfaceH[kMaxRenderers] = {};
    std::vector<uint8_t> chromePixels;
    int chromeWidth = 0;

    Impl() {
        for (int i = 0; i < kMaxRenderers; ++i) {
            published[i].store(-1);
        }
    }

    void drawQuad(id<MTLRenderCommandEncoder> encoder, float x, float y, float w, float h, int windowW, int windowH,
                  id<MTLTexture> texture, id<MTLSamplerState> sampler, float r, float g, float b, float a) {
        if (!texture || w <= 0.f || h <= 0.f || windowW < 1 || windowH < 1) {
            return;
        }
        QuadCB cb{};
        toNdc(x, y, w, h, windowW, windowH, cb.l, cb.t, cb.r, cb.b);
        cb.u0 = 0;
        cb.v0 = 0;
        cb.u1 = 1;
        cb.v1 = 1;
        cb.cr = r;
        cb.cg = g;
        cb.cb = b;
        cb.ca = a;
        [encoder setVertexBytes:&cb length:sizeof(cb) atIndex:0];
        [encoder setFragmentBytes:&cb length:sizeof(cb) atIndex:0];
        [encoder setFragmentTexture:texture atIndex:0];
        [encoder setFragmentSamplerState:sampler atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    }
};

Presenter::Presenter() : impl_(new Impl) {}

Presenter::~Presenter() {
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool Presenter::init(void* nativeView) {
    impl_->view = (__bridge NSView*)nativeView;
    impl_->layer = (CAMetalLayer*)impl_->view.layer;
    if (![impl_->layer isKindOfClass:[CAMetalLayer class]]) {
        logf("the content view does not have a CAMetalLayer");
        return false;
    }
    impl_->device = MTLCreateSystemDefaultDevice();
    if (!impl_->device) {
        logf("Metal device was not created");
        return false;
    }
    impl_->layer.device = impl_->device;
    impl_->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    impl_->layer.framebufferOnly = YES;
    impl_->queue = [impl_->device newCommandQueue];

    NSError* error = nil;
    id<MTLLibrary> library = [impl_->device newLibraryWithSource:@(kPresentMsl) options:nil error:&error];
    if (!library) {
        logf("present shader failed: %s", error.localizedDescription.UTF8String ? error.localizedDescription.UTF8String : "unknown");
        return false;
    }
    MTLRenderPipelineDescriptor* pipelineDesc = [MTLRenderPipelineDescriptor new];
    pipelineDesc.vertexFunction = [library newFunctionWithName:@"vs_quad"];
    pipelineDesc.fragmentFunction = [library newFunctionWithName:@"ps_quad"];
    pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    impl_->pipeline = [impl_->device newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
    if (!impl_->pipeline) {
        logf("present pipeline failed: %s", error.localizedDescription.UTF8String ? error.localizedDescription.UTF8String : "unknown");
        return false;
    }

    MTLSamplerDescriptor* linearDesc = [MTLSamplerDescriptor new];
    linearDesc.minFilter = MTLSamplerMinMagFilterLinear;
    linearDesc.magFilter = MTLSamplerMinMagFilterLinear;
    impl_->linear = [impl_->device newSamplerStateWithDescriptor:linearDesc];
    MTLSamplerDescriptor* pointDesc = [MTLSamplerDescriptor new];
    pointDesc.minFilter = MTLSamplerMinMagFilterNearest;
    pointDesc.magFilter = MTLSamplerMinMagFilterNearest;
    impl_->point = [impl_->device newSamplerStateWithDescriptor:pointDesc];

    MTLTextureDescriptor* one = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
    one.usage = MTLTextureUsageShaderRead;
    one.storageMode = MTLStorageModeShared;
    impl_->white = [impl_->device newTextureWithDescriptor:one];
    uint32_t white = 0xFFFFFFFFu;
    [impl_->white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&white bytesPerRow:4];
    return true;
}

void Presenter::releaseSurface(int tab) {
    if (!impl_ || tab < 0 || tab >= kMaxRenderers) {
        return;
    }
    impl_->shared[tab] = nil;
    impl_->copies[tab][0] = nil;
    impl_->copies[tab][1] = nil;
    if (impl_->surfaces[tab]) {
        CFRelease(impl_->surfaces[tab]);
        impl_->surfaces[tab] = nullptr;
    }
    impl_->published[tab].store(-1);
    impl_->surfaceW[tab] = 0;
    impl_->surfaceH[tab] = 0;
}

bool Presenter::createSurface(int tab, uint32_t ownerPid, uint32_t generation, int width, int height, uint64_t* tokenOut) {
    (void)ownerPid;
    (void)generation;
    if (tokenOut) {
        *tokenOut = 0;
    }
    if (!impl_->device || tab < 0 || tab >= kMaxRenderers) {
        return false;
    }
    if (width < 1) {
        width = kSurfaceWidth;
    }
    if (height < 1) {
        height = kSurfaceHeight;
    }
    releaseSurface(tab);
    NSDictionary* props = @{
        (id)kIOSurfaceWidth : @(width),
        (id)kIOSurfaceHeight : @(height),
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfacePixelFormat : @(kCVPixelFormatType_32BGRA),
    };
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    if (!surface) {
        logf("IOSurfaceCreate failed for tab %d", tab);
        return false;
    }
    MTLTextureDescriptor* sharedDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                            width:(NSUInteger)width
                                                                                           height:(NSUInteger)height
                                                                                        mipmapped:NO];
    sharedDesc.usage = MTLTextureUsageShaderRead;
    sharedDesc.storageMode = MTLStorageModeShared;
    id<MTLTexture> shared = [impl_->device newTextureWithDescriptor:sharedDesc iosurface:surface plane:0];
    if (!shared) {
        CFRelease(surface);
        logf("host IOSurface texture failed for tab %d", tab);
        return false;
    }
    MTLTextureDescriptor* copyDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:(NSUInteger)width
                                                                                         height:(NSUInteger)height
                                                                                      mipmapped:NO];
    copyDesc.usage = MTLTextureUsageShaderRead;
    copyDesc.storageMode = MTLStorageModePrivate;
    impl_->surfaces[tab] = surface;
    impl_->shared[tab] = shared;
    impl_->copies[tab][0] = [impl_->device newTextureWithDescriptor:copyDesc];
    impl_->copies[tab][1] = [impl_->device newTextureWithDescriptor:copyDesc];
    impl_->published[tab].store(-1);
    impl_->surfaceW[tab] = width;
    impl_->surfaceH[tab] = height;
    if (tokenOut) {
        *tokenOut = IOSurfaceGetID(surface);
    }
    logf("shared surface tab %d  IOSurface %u", tab, (unsigned)IOSurfaceGetID(surface));
    return impl_->copies[tab][0] && impl_->copies[tab][1];
}

void Presenter::pumpSurfaces() {}

void Presenter::onRendererFrame(int tab, uint64_t frame, void (*ack)(int, uint64_t, void*), void* user) {
    if (!impl_->queue || tab < 0 || tab >= kMaxRenderers || !impl_->shared[tab]) {
        if (ack) {
            ack(tab, frame, user);
        }
        return;
    }
    const int slot = (int)(frame & 1ull);
    id<MTLCommandBuffer> buffer = [impl_->queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [buffer blitCommandEncoder];
    [blit copyFromTexture:impl_->shared[tab]
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake((NSUInteger)std::max(1, impl_->surfaceW[tab]), (NSUInteger)std::max(1, impl_->surfaceH[tab]), 1)
                toTexture:impl_->copies[tab][slot]
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    std::atomic<int>* published = &impl_->published[tab];
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        if (done.status == MTLCommandBufferStatusCompleted) {
            published->store(slot, std::memory_order_release);
        }
        if (ack) {
            ack(tab, frame, user);
        }
    }];
    impl_->last = buffer;
    [buffer commit];
}

void Presenter::render(const HostStatus& status) {
    if (!impl_->layer || !impl_->pipeline || status.windowWidth < 2 || status.windowHeight < 2) {
        return;
    }
    impl_->layer.contentsScale = impl_->view.window.backingScaleFactor;
    impl_->layer.drawableSize = CGSizeMake(status.windowWidth, status.windowHeight);
    id<CAMetalDrawable> drawable = [impl_->layer nextDrawable];
    if (!drawable) {
        return;
    }

    if (impl_->chromeWidth != status.windowWidth) {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:(NSUInteger)status.windowWidth
                                                                                         height:kChromeHeight
                                                                                      mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        impl_->chrome = [impl_->device newTextureWithDescriptor:desc];
        impl_->chromeWidth = status.windowWidth;
        impl_->chromePixels.resize((size_t)impl_->chromeWidth * kChromeHeight * 4);
    }
    ChromeInfo info;
    info.width = impl_->chromeWidth;
    info.height = kChromeHeight;
    info.hover = status.hover;
    for (int i = 0; i < kMaxRenderers; ++i) {
        info.listed[i] = status.listed[i];
        info.selected[i] = status.selected[i];
        std::memcpy(info.label[i], status.label[i], sizeof(info.label[i]));
    }
    paintChrome(impl_->chromePixels.data(), impl_->chromeWidth * 4, info);
    [impl_->chrome replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)impl_->chromeWidth, kChromeHeight)
                     mipmapLevel:0
                       withBytes:impl_->chromePixels.data()
                     bytesPerRow:(NSUInteger)impl_->chromeWidth * 4];

    id<MTLCommandBuffer> buffer = [impl_->queue commandBuffer];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.07, 0.08, 0.11, 1);
    id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:impl_->pipeline];
    MTLViewport viewport{0, 0, (double)status.windowWidth, (double)status.windowHeight, 0, 1};
    [encoder setViewport:viewport];

    float chromeH = (float)std::min(kChromeHeight, status.windowHeight);
    impl_->drawQuad(encoder, 0, 0, (float)status.windowWidth, chromeH, status.windowWidth, status.windowHeight, impl_->chrome, impl_->point, 1, 1, 1, 1);

    if (status.windowHeight > kChromeHeight) {
        bool selected[kMaxRenderers];
        bool alive[kMaxRenderers];
        for (int i = 0; i < kMaxRenderers; ++i) {
            selected[i] = status.selected[i] && status.listed[i];
            alive[i] = status.alive[i] && status.listed[i];
        }
        MirrorCell cells[kMaxRenderers];
        const int count = layoutMirrors(selected, alive, status.frameW, status.frameH, kMaxRenderers, status.windowWidth, status.windowHeight,
                                        cells, kMaxRenderers);
        for (int i = 0; i < count; ++i) {
            const MirrorCell& cell = cells[i];
            impl_->drawQuad(encoder, cell.x - 2.f, cell.y - 2.f, cell.w + 4.f, cell.h + 4.f, status.windowWidth, status.windowHeight, impl_->white, impl_->point, 0, 0, 0, 1);
            int slot = impl_->published[cell.slot].load(std::memory_order_acquire);
            if (slot >= 0) {
                impl_->drawQuad(encoder, cell.x, cell.y, cell.w, cell.h, status.windowWidth, status.windowHeight, impl_->copies[cell.slot][slot], impl_->linear, 1, 1, 1, 1);
            }
        }
    }
    [encoder endEncoding];
    [buffer presentDrawable:drawable];
    impl_->last = buffer;
    [buffer commit];
}

void Presenter::shutdown() {
    if (!impl_) {
        return;
    }
    if (impl_->last) {
        [impl_->last waitUntilCompleted];
        impl_->last = nil;
    }
    impl_->chrome = nil;
    impl_->white = nil;
    impl_->pipeline = nil;
    impl_->linear = nil;
    impl_->point = nil;
    for (int i = 0; i < kMaxRenderers; ++i) {
        impl_->shared[i] = nil;
        impl_->copies[i][0] = nil;
        impl_->copies[i][1] = nil;
        if (impl_->surfaces[i]) {
            CFRelease(impl_->surfaces[i]);
            impl_->surfaces[i] = nullptr;
        }
    }
    impl_->queue = nil;
    impl_->device = nil;
    impl_->layer = nil;
    impl_->view = nil;
}

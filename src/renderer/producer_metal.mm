#include "producer.h"

#include "log.h"
#include "protocol.h"
#include "scene_ui.h"

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>
#import <IOSurface/IOSurface.h>

#include <mach-o/dyld.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace {

std::string exeDirectory() {
    char path[1024];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) {
        return {};
    }
    char* slash = std::strrchr(path, '/');
    if (!slash) {
        return {};
    }
    slash[1] = 0;
    return path;
}

std::string assetFile(const char* name) {
    return exeDirectory() + "assets/" + name;
}

CGFloat cgBottom(float top, float height) {
    return (CGFloat)kSurfaceHeight - top - height;
}

CGRect cgRect(const UiRect& rect) {
    return CGRectMake(rect.x, cgBottom(rect.y, rect.h), rect.w, rect.h);
}

void setFill(CGContextRef ctx, CGFloat r, CGFloat g, CGFloat b, CGFloat a = 1) {
    CGContextSetRGBFillColor(ctx, r, g, b, a);
}

void setStroke(CGContextRef ctx, CGFloat r, CGFloat g, CGFloat b, CGFloat a = 1) {
    CGContextSetRGBStrokeColor(ctx, r, g, b, a);
}

void fillRect(CGContextRef ctx, CGRect rect, CGFloat r, CGFloat g, CGFloat b) {
    setFill(ctx, r, g, b);
    CGContextFillRect(ctx, rect);
}

void drawText(CGContextRef ctx, NSString* text, NSFont* font, NSColor* color, CGRect rect, bool center) {
    if (text.length == 0 || !font) {
        return;
    }
    NSDictionary* attributes = @{NSFontAttributeName : font, NSForegroundColorAttributeName : color};
    NSAttributedString* string = [[NSAttributedString alloc] initWithString:text attributes:attributes];
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)string);
    CGFloat ascent = 0;
    CGFloat descent = 0;
    double width = CTLineGetTypographicBounds(line, &ascent, &descent, nullptr);
    CGFloat x = rect.origin.x;
    if (center) {
        x += (rect.size.width - (CGFloat)width) * 0.5;
    }
    CGFloat y = rect.origin.y + (rect.size.height - (ascent + descent)) * 0.5 + descent;
    CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
    CGContextSetTextPosition(ctx, x, y);
    CTLineDraw(line, ctx);
    CFRelease(line);
}

NSString* clockText(double seconds) {
    if (seconds < 0) {
        seconds = 0;
    }
    int whole = (int)seconds;
    return [NSString stringWithFormat:@"%02d:%02d", whole / 60, whole % 60];
}

}  // namespace

struct Producer::Impl {
    IOSurfaceRef surface = nullptr;
    NSWindow* window = nil;
    NSView* view = nil;
    id windowDelegate = nil;
    CGContextRef bitmap = nullptr;
    SceneState state;
    int scene = 0;
    bool open = false;
    bool quit = false;
    bool videoFailed = false;
    bool photoFailed = false;
    bool haveFrame = false;
    double lastTime = -1;
    double frameStart = 0;
    double frameEnd = 0;
    AVAssetReader* reader = nil;
    AVAssetReaderTrackOutput* output = nil;
    NSURL* videoURL = nil;
    CGImageRef videoImage = nullptr;
    CGImageRef photo = nullptr;
    size_t videoW = 0;
    size_t videoH = 0;
    size_t photoW = 0;
    size_t photoH = 0;
    NSString* videoError = nil;
    NSString* photoError = nil;

    ~Impl() {
        if (videoImage) {
            CGImageRelease(videoImage);
            videoImage = nullptr;
        }
        if (photo) {
            CGImageRelease(photo);
            photo = nullptr;
        }
        if (bitmap) {
            CGContextRelease(bitmap);
            bitmap = nullptr;
        }
        if (surface) {
            CFRelease(surface);
            surface = nullptr;
        }
    }

    void apply(float x, float y, uint32_t action) { applyPointer(&state, scene, x, y, action); }

    bool openReader() {
        if (reader) {
            [reader cancelReading];
            reader = nil;
            output = nil;
        }
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:videoURL options:nil];
        AVAssetTrack* track = [[asset tracksWithMediaType:AVMediaTypeVideo] firstObject];
        if (!track) {
            videoError = @"The video has no picture";
            return false;
        }
        NSError* error = nil;
        reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
        if (!reader) {
            videoError = @"Could not open assets/flower.mp4";
            return false;
        }
        NSDictionary* settings = @{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
        output = [[AVAssetReaderTrackOutput alloc] initWithTrack:track outputSettings:settings];
        output.alwaysCopiesSampleData = YES;
        if (![reader canAddOutput:output]) {
            videoError = @"Could not decode the video";
            return false;
        }
        [reader addOutput:output];
        if (![reader startReading]) {
            videoError = @"Could not play the video";
            return false;
        }
        state.mediaDuration = CMTimeGetSeconds(asset.duration);
        haveFrame = false;
        return true;
    }

    bool readSample() {
        if (!output) {
            return false;
        }
        CMSampleBufferRef sample = [output copyNextSampleBuffer];
        if (!sample) {
            return false;
        }
        CVImageBufferRef buffer = CMSampleBufferGetImageBuffer(sample);
        if (!buffer) {
            CFRelease(sample);
            return true;
        }
        CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        videoW = CVPixelBufferGetWidth(buffer);
        videoH = CVPixelBufferGetHeight(buffer);
        void* base = CVPixelBufferGetBaseAddress(buffer);
        size_t stride = CVPixelBufferGetBytesPerRow(buffer);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGBitmapInfo info = kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst;
        CGContextRef raw = CGBitmapContextCreate(base, videoW, videoH, 8, stride, space, info);
        CGImageRef rawImage = raw ? CGBitmapContextCreateImage(raw) : nullptr;
        CGContextRef upright = CGBitmapContextCreate(nullptr, videoW, videoH, 8, videoW * 4, space, info);
        CGImageRef corrected = nullptr;
        if (upright && rawImage) {
            CGContextTranslateCTM(upright, 0, (CGFloat)videoH);
            CGContextScaleCTM(upright, 1, -1);
            CGContextDrawImage(upright, CGRectMake(0, 0, (CGFloat)videoW, (CGFloat)videoH), rawImage);
            corrected = CGBitmapContextCreateImage(upright);
        }
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        if (rawImage) {
            CGImageRelease(rawImage);
        }
        if (raw) {
            CGContextRelease(raw);
        }
        if (upright) {
            CGContextRelease(upright);
        }
        CGColorSpaceRelease(space);
        CMTime timestamp = CMSampleBufferGetPresentationTimeStamp(sample);
        CMTime duration = CMSampleBufferGetDuration(sample);
        CFRelease(sample);
        if (!corrected) {
            return false;
        }
        if (videoImage) {
            CGImageRelease(videoImage);
        }
        videoImage = corrected;
        frameStart = CMTimeGetSeconds(timestamp);
        double seconds = CMTimeGetSeconds(duration);
        if (!(seconds > 0)) {
            seconds = 1.0 / 30.0;
        }
        frameEnd = frameStart + seconds;
        haveFrame = true;
        return true;
    }

    void advanceVideo() {
        if (videoFailed || !reader) {
            return;
        }
        if (state.mediaDuration > 0.05 && state.mediaSeconds >= state.mediaDuration - 0.02) {
            state.mediaSeconds = 0;
            openReader();
        }
        for (int i = 0; i < 8; ++i) {
            if (haveFrame && state.mediaSeconds < frameEnd) {
                return;
            }
            if (!readSample()) {
                state.mediaSeconds = 0;
                openReader();
                readSample();
                return;
            }
        }
    }

    void drawImage(CGContextRef ctx, CGImageRef image, CGRect dest) {
        if (!image) {
            return;
        }
        CGContextSaveGState(ctx);
        CGContextClipToRect(ctx, dest);
        CGContextDrawImage(ctx, dest, image);
        CGContextRestoreGState(ctx);
    }

    CGRect fitted(CGRect well, size_t width, size_t height, bool actual) {
        if (width == 0 || height == 0) {
            return well;
        }
        if (actual) {
            return CGRectMake(well.origin.x + (well.size.width - (CGFloat)width) * 0.5,
                              well.origin.y + (well.size.height - (CGFloat)height) * 0.5, (CGFloat)width, (CGFloat)height);
        }
        CGFloat scale = std::min(well.size.width / (CGFloat)width, well.size.height / (CGFloat)height);
        CGFloat dw = (CGFloat)width * scale;
        CGFloat dh = (CGFloat)height * scale;
        return CGRectMake(well.origin.x + (well.size.width - dw) * 0.5, well.origin.y + (well.size.height - dh) * 0.5, dw, dh);
    }

    void drawButton(CGContextRef ctx, const UiRect& bounds, NSString* label, bool hot, bool down, bool primary) {
        CGRect rect = cgRect(bounds);
        CGPathRef path = CGPathCreateWithRoundedRect(rect, 6, 6, nullptr);
        CGContextAddPath(ctx, path);
        if (primary) {
            setFill(ctx, down ? 0.00 : hot ? 0.10 : 0.00, down ? 0.31 : hot ? 0.47 : 0.40, down ? 0.58 : hot ? 0.82 : 0.75);
        } else {
            setFill(ctx, down ? 0.90 : hot ? 0.96 : 1, down ? 0.91 : hot ? 0.97 : 1, down ? 0.93 : hot ? 0.98 : 1);
        }
        CGContextFillPath(ctx);
        if (!primary) {
            CGContextAddPath(ctx, path);
            setStroke(ctx, 0.78, 0.79, 0.82);
            CGContextSetLineWidth(ctx, 1);
            CGContextStrokePath(ctx);
        }
        CGPathRelease(path);
        NSColor* color = primary ? NSColor.whiteColor : [NSColor colorWithCalibratedWhite:0.13 alpha:1];
        drawText(ctx, label, [NSFont systemFontOfSize:16 weight:NSFontWeightSemibold], color, rect, true);
    }

    void paint() {
        CGContextRef ctx = bitmap;
        bool dark = scene == 0;
        fillRect(ctx, CGRectMake(0, 0, kSurfaceWidth, kSurfaceHeight), dark ? 0.09 : 0.95, dark ? 0.10 : 0.95, dark ? 0.12 : 0.96);
        NSColor* titleColor = dark ? NSColor.whiteColor : [NSColor colorWithCalibratedWhite:0.12 alpha:1];
        NSColor* subColor = [NSColor colorWithCalibratedWhite:dark ? 0.68 : 0.42 alpha:1];
        NSFont* titleFont = [NSFont systemFontOfSize:28 weight:NSFontWeightSemibold];
        NSFont* bodyFont = [NSFont systemFontOfSize:16 weight:NSFontWeightRegular];
        UiRect well = mediaWell();
        CGRect wellRect = cgRect(well);
        NSString* title = [NSString stringWithUTF8String:sceneTitle(scene)];
        drawText(ctx, title, titleFont, titleColor, CGRectMake(40, cgBottom(22, 36), 700, 36), false);
        if (scene == 0) {
            drawText(ctx, @"flower.mp4", bodyFont, subColor, CGRectMake(40, cgBottom(58, 28), 800, 28), false);
            fillRect(ctx, wellRect, 0.05, 0.05, 0.06);
            if (videoImage) {
                drawImage(ctx, videoImage, fitted(wellRect, videoW, videoH, false));
            } else {
                drawText(ctx, videoError ?: @"No video", bodyFont, NSColor.whiteColor,
                         CGRectMake(wellRect.origin.x + 24, wellRect.origin.y + 24, wellRect.size.width - 48, 32), false);
            }
            NSString* times = [NSString stringWithFormat:@"%@   /   %@", clockText(state.mediaSeconds), clockText(state.mediaDuration)];
            drawText(ctx, times, bodyFont, [NSColor colorWithCalibratedWhite:0.84 alpha:1], CGRectMake(210, cgBottom(656, 36), 220, 36), false);
            CGFloat trackX = 450;
            CGFloat trackW = 780;
            CGFloat trackY = cgBottom(668, 6);
            fillRect(ctx, CGRectMake(trackX, trackY, trackW, 6), 0.28, 0.30, 0.34);
            CGFloat fraction = 0;
            if (state.mediaDuration > 0.05) {
                fraction = (CGFloat)(state.mediaSeconds / state.mediaDuration);
            }
            if (fraction < 0) fraction = 0;
            if (fraction > 1) fraction = 1;
            fillRect(ctx, CGRectMake(trackX, trackY, trackW * fraction, 6), 0.20, 0.55, 0.95);
        } else if (scene == 1) {
            NSString* subtitle = photo ? [NSString stringWithFormat:@"photo.jpg    %zu × %zu", photoW, photoH] : @"photo.jpg";
            drawText(ctx, subtitle, bodyFont, subColor, CGRectMake(40, cgBottom(58, 28), 800, 28), false);
            fillRect(ctx, wellRect, 0.90, 0.91, 0.93);
            if (photo) {
                drawImage(ctx, photo, fitted(wellRect, photoW, photoH, state.actualSize));
            } else {
                drawText(ctx, photoError ?: @"No photo", bodyFont, [NSColor colorWithCalibratedWhite:0.35 alpha:1],
                         CGRectMake(wellRect.origin.x + 24, wellRect.origin.y + 24, wellRect.size.width - 48, 32), false);
            }
        } else {
            drawText(ctx, @"Click the buttons in this window. The mirror shows the same picture.", bodyFont, subColor,
                     CGRectMake(40, cgBottom(58, 28), 1100, 28), false);
            NSString* status = @"Idle";
            CGFloat dotR = 0.55, dotG = 0.57, dotB = 0.62;
            if (state.runState == 1) {
                status = @"Running";
                dotR = 0.13;
                dotG = 0.62;
                dotB = 0.36;
            } else if (state.runState == 2) {
                status = @"Stopped";
                dotR = 0.75;
                dotG = 0.22;
                dotB = 0.24;
            }
            CGRect card = CGRectMake(48, cgBottom(276, 154), 1184, 154);
            fillRect(ctx, card, 1, 1, 1);
            CGContextSetRGBStrokeColor(ctx, 0.86, 0.87, 0.90, 1);
            CGContextStrokeRect(ctx, card);
            setFill(ctx, dotR, dotG, dotB);
            CGContextFillEllipseInRect(ctx, CGRectMake(72, cgBottom(332, 16) , 16, 16));
            drawText(ctx, @"Status", [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold],
                     [NSColor colorWithCalibratedWhite:0.42 alpha:1], CGRectMake(108, cgBottom(300, 22), 300, 22), false);
            drawText(ctx, status, titleFont, [NSColor colorWithCalibratedWhite:0.12 alpha:1], CGRectMake(108, cgBottom(322, 40), 500, 40), false);
            drawText(ctx, [NSString stringWithFormat:@"Button presses:  %d", state.clicks], bodyFont,
                     [NSColor colorWithCalibratedWhite:0.30 alpha:1], CGRectMake(108, cgBottom(376, 28), 500, 28), false);
            drawText(ctx, @"Notes", [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold],
                     [NSColor colorWithCalibratedWhite:0.42 alpha:1], CGRectMake(48, cgBottom(456, 24), 300, 24), false);
            if (state.noteCount == 0) {
                drawText(ctx, @"No notes yet. Add note appends one.", bodyFont, [NSColor colorWithCalibratedWhite:0.48 alpha:1],
                         CGRectMake(48, cgBottom(488, 28), 800, 28), false);
            }
            for (int i = 0; i < state.noteCount; ++i) {
                NSString* line = [NSString stringWithUTF8String:state.notes[i]];
                drawText(ctx, line, bodyFont, [NSColor colorWithCalibratedWhite:0.13 alpha:1],
                         CGRectMake(48, cgBottom(488 + i * 32, 28), 800, 28), false);
            }
        }
        SceneButtons buttons{};
        layoutButtons(scene, &buttons);
        for (int i = 0; i < buttons.count; ++i) {
            NSString* label = [NSString stringWithUTF8String:buttonLabel(buttons.id[i], state)];
            bool hot = state.hot == buttons.id[i];
            bool down = state.pressed == buttons.id[i] && hot;
            drawButton(ctx, buttons.rect[i], label, hot, down, buttonPrimary(buttons.id[i], state));
        }
    }

    void publish() {
        if (!bitmap || !view) {
            return;
        }
        if (surface) {
            IOSurfaceLock(surface, 0, nullptr);
            auto* dest = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surface));
            size_t destStride = IOSurfaceGetBytesPerRow(surface);
            auto* src = static_cast<uint8_t*>(CGBitmapContextGetData(bitmap));
            size_t srcStride = CGBitmapContextGetBytesPerRow(bitmap);
            const int copyW = std::min((int)kSurfaceWidth, (int)IOSurfaceGetWidth(surface));
            const int copyH = std::min((int)kSurfaceHeight, (int)IOSurfaceGetHeight(surface));
            for (int y = 0; y < copyH; ++y) {
                std::memcpy(dest + (size_t)y * destStride, src + (size_t)(kSurfaceHeight - 1 - y) * srcStride, (size_t)copyW * 4);
            }
            IOSurfaceUnlock(surface, 0, nullptr);
        }
        CGImageRef image = CGBitmapContextCreateImage(bitmap);
        if (image) {
            [(id)view setPicture:image];
            CGImageRelease(image);
        }
    }
};

void rendererNoteClose(Producer::Impl* impl) {
    if (!impl) {
        return;
    }
    impl->quit = true;
    impl->window = nil;
}

void rendererNotePointer(Producer::Impl* impl, float x, float y, int action) {
    if (!impl) {
        return;
    }
    impl->apply(x, y, (uint32_t)action);
}

@interface MirrorContentView : NSView
@property(nonatomic, assign) CGImageRef picture;
@property(nonatomic, assign) Producer::Impl* owner;
@end

@implementation MirrorContentView
- (void)setPicture:(CGImageRef)picture {
    if (_picture == picture) {
        return;
    }
    if (_picture) {
        CGImageRelease(_picture);
    }
    _picture = picture ? CGImageRetain(picture) : nullptr;
    [self setNeedsDisplay:YES];
}
- (void)dealloc {
    if (_picture) {
        CGImageRelease(_picture);
        _picture = nullptr;
    }
}
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    [[NSColor colorWithCalibratedWhite:0.12 alpha:1] setFill];
    NSRectFill(self.bounds);
    if (!_picture) {
        return;
    }
    CGFloat boundsW = std::max<CGFloat>(1, self.bounds.size.width);
    CGFloat boundsH = std::max<CGFloat>(1, self.bounds.size.height);
    CGFloat scale = std::min(boundsW / (CGFloat)kSurfaceWidth, boundsH / (CGFloat)kSurfaceHeight);
    CGFloat pictureW = (CGFloat)kSurfaceWidth * scale;
    CGFloat pictureH = (CGFloat)kSurfaceHeight * scale;
    CGRect fitted = CGRectMake((boundsW - pictureW) * 0.5, (boundsH - pictureH) * 0.5, pictureW, pictureH);
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextDrawImage(ctx, fitted, _picture);
}
- (void)mapEvent:(NSEvent*)event toX:(float*)x y:(float*)y {
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    CGFloat boundsW = std::max<CGFloat>(1, self.bounds.size.width);
    CGFloat boundsH = std::max<CGFloat>(1, self.bounds.size.height);
    CGFloat scale = std::min(boundsW / (CGFloat)kSurfaceWidth, boundsH / (CGFloat)kSurfaceHeight);
    CGFloat pictureW = (CGFloat)kSurfaceWidth * scale;
    CGFloat pictureH = (CGFloat)kSurfaceHeight * scale;
    CGFloat originX = (boundsW - pictureW) * 0.5;
    CGFloat originY = (boundsH - pictureH) * 0.5;
    *x = (float)((point.x - originX) / pictureW * kSurfaceWidth);
    *y = (float)((originY + pictureH - point.y) / pictureH * kSurfaceHeight);
}
- (void)mouseMoved:(NSEvent*)event {
    float x = 0;
    float y = 0;
    [self mapEvent:event toX:&x y:&y];
    rendererNotePointer(_owner, x, y, kPointerMove);
}
- (void)mouseDragged:(NSEvent*)event {
    [self mouseMoved:event];
}
- (void)mouseDown:(NSEvent*)event {
    float x = 0;
    float y = 0;
    [self mapEvent:event toX:&x y:&y];
    rendererNotePointer(_owner, x, y, kPointerDown);
}
- (void)mouseUp:(NSEvent*)event {
    float x = 0;
    float y = 0;
    [self mapEvent:event toX:&x y:&y];
    rendererNotePointer(_owner, x, y, kPointerUp);
}
@end

@interface RendererWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) Producer::Impl* owner;
@end

@implementation RendererWindowDelegate
- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    rendererNoteClose(_owner);
}
@end

Producer::Producer() : impl_(new Impl) {}

Producer::~Producer() {
    close();
    delete impl_;
    impl_ = nullptr;
}

bool Producer::open(int scene) {
    close();
    impl_->scene = scene;
    NSApplication* app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    impl_->bitmap = CGBitmapContextCreate(nullptr, kSurfaceWidth, kSurfaceHeight, 8, kSurfaceWidth * 4, space,
                                          kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    CGColorSpaceRelease(space);
    if (!impl_->bitmap) {
        logf("bitmap context failed");
        return false;
    }

    NSScreen* screen = [NSScreen mainScreen];
    NSRect visible = screen.visibleFrame;
    CGFloat hostW = std::min(960.0, visible.size.width * 0.56);
    CGFloat contentW = std::min(640.0, visible.size.width - hostW - 48.0);
    if (contentW < 280) {
        contentW = 420;
    }
    CGFloat contentH = contentW * 9.0 / 16.0;
    CGFloat slot = (visible.size.height - 36.0) / 3.0;
    if (contentH > slot - 28) {
        contentH = std::max(160.0, slot - 28);
    }
    CGFloat x = visible.origin.x + 48 + (CGFloat)scene * 36;
    CGFloat y = visible.origin.y + visible.size.height - 640 - (CGFloat)scene * 36;
    NSRect frame = NSMakeRect(x, y, 960, 600);
    impl_->window = [[NSWindow alloc] initWithContentRect:frame
                                                 styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
    impl_->window.title = [NSString stringWithUTF8String:sceneTitle(impl_->scene)];
    MirrorContentView* view = [[MirrorContentView alloc] initWithFrame:NSMakeRect(0, 0, contentW, contentH)];
    view.owner = impl_;
    impl_->view = view;
    impl_->window.contentView = view;
    RendererWindowDelegate* delegate = [RendererWindowDelegate new];
    delegate.owner = impl_;
    impl_->windowDelegate = delegate;
    impl_->window.delegate = delegate;
    [impl_->window setAcceptsMouseMovedEvents:YES];

    if (impl_->scene == 0) {
        std::string path = assetFile("flower.mp4");
        impl_->videoURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        if (!impl_->openReader() || !impl_->readSample()) {
            impl_->videoFailed = true;
            if (!impl_->videoError) {
                impl_->videoError = @"Could not open assets/flower.mp4";
            }
            logf("video did not open");
        }
    } else if (impl_->scene == 1) {
        std::string path = assetFile("photo.jpg");
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
        impl_->photo = source ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
        if (source) {
            CFRelease(source);
        }
        if (!impl_->photo) {
            impl_->photoFailed = true;
            impl_->photoError = @"Could not open assets/photo.jpg";
            logf("photo did not open");
        } else {
            impl_->photoW = CGImageGetWidth(impl_->photo);
            impl_->photoH = CGImageGetHeight(impl_->photo);
        }
    }

    [impl_->window orderFront:nil];
    impl_->open = true;
    logf("renderer %s window is up", sceneTitle(impl_->scene));
    return true;
}

bool Producer::pump() {
    if (!impl_) {
        return false;
    }
    NSApplication* app = [NSApplication sharedApplication];
    NSDate* until = [NSDate date];
    while (NSEvent* event = [app nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES]) {
        [app sendEvent:event];
    }
    return impl_->window != nil && !impl_->quit;
}

bool Producer::draw(int scene, float timeSeconds) {
    if (!impl_->open || !impl_->bitmap) {
        return false;
    }
    impl_->scene = scene;
    if (impl_->lastTime < 0) {
        impl_->lastTime = timeSeconds;
    }
    double dt = (double)timeSeconds - impl_->lastTime;
    impl_->lastTime = timeSeconds;
    if (dt < 0 || dt > 0.25) {
        dt = 0;
    }
    if (scene == 0 && impl_->state.playing && !impl_->videoFailed) {
        impl_->state.mediaSeconds += dt;
        impl_->advanceVideo();
    }
    impl_->paint();
    impl_->publish();
    return true;
}

void Producer::pointer(int x, int y, int action) {
    if (!impl_ || !impl_->open) {
        return;
    }
    impl_->apply((float)x, (float)y, (uint32_t)action);
}

int Producer::width() const {
    return kSurfaceWidth;
}

int Producer::height() const {
    return kSurfaceHeight;
}

uint32_t Producer::generation() const {
    return impl_ && impl_->open ? 1u : 0u;
}

void Producer::attachSurface(uint64_t token) {
    if (!impl_ || token == 0 || impl_->surface) {
        return;
    }
    impl_->surface = IOSurfaceLookup((IOSurfaceID)token);
    if (!impl_->surface) {
        logf("IOSurfaceLookup %llu failed", (unsigned long long)token);
    }
}

void Producer::close() {
    if (!impl_) {
        return;
    }
    impl_->open = false;
    impl_->quit = true;
    if (impl_->window) {
        impl_->window.delegate = nil;
        [impl_->window close];
        impl_->window = nil;
    }
    impl_->view = nil;
    impl_->reader = nil;
    impl_->output = nil;
    delete impl_;
    impl_ = new Impl;
}

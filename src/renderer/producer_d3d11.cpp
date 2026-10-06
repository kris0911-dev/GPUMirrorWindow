#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "producer.h"

#include "log.h"
#include "scene_ui.h"
#include "win_layout.h"

#include <windowsx.h>
#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

bool check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) {
        return true;
    }
    logf("%s failed: 0x%08lX", what, (unsigned long)hr);
    return false;
}

std::wstring exeDirectory() {
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L"";
    }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) {
        return L"";
    }
    *(slash + 1) = 0;
    return path;
}

std::wstring assetFile(const wchar_t* name) {
    return exeDirectory() + L"assets\\" + name;
}

void toWide(const char* text, wchar_t* out, int count) {
    if (!out || count <= 0) {
        return;
    }
    out[0] = 0;
    if (!text) {
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out, count);
}

void formatClock(wchar_t* out, size_t count, double seconds) {
    if (seconds < 0) {
        seconds = 0;
    }
    const int whole = (int)seconds;
    swprintf_s(out, count, L"%02d:%02d", whole / 60, whole % 60);
}

D2D1_RECT_F rectF(const UiRect& rect) {
    return D2D1::RectF(rect.x, rect.y, rect.x + rect.w, rect.y + rect.h);
}

}  // namespace

struct Producer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> sharedTex;
    ComPtr<ID3D11Texture2D> uiTex;
    ComPtr<IDXGIKeyedMutex> mutex;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> uiTarget;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<IDWriteTextFormat> titleFont;
    ComPtr<IDWriteTextFormat> bodyFont;
    ComPtr<IDWriteTextFormat> buttonFont;
    ComPtr<IDWriteTextFormat> smallFont;
    ComPtr<ID2D1Bitmap1> photo;
    ComPtr<ID2D1Bitmap1> videoFrame;
    ComPtr<IMFSourceReader> reader;
    HANDLE sharedHandle = nullptr;
    HWND hwnd = nullptr;
    SceneState state;
    int scene = 0;
    int clientW = 0;
    int clientH = 0;
    int uiW = 0;
    int uiH = 0;
    int swapW = 0;
    int swapH = 0;
    uint32_t generation = 0;
    uint32_t pid = 0;
    float dpiScale = 1.f;
    bool open = false;
    bool quit = false;
    bool closing = false;
    bool comStarted = false;
    bool mfStarted = false;
    bool haveFrame = false;
    bool videoFailed = false;
    bool photoFailed = false;
    double lastTime = -1;
    double frameStart = 0;
    double frameEnd = 0;
    UINT videoW = 0;
    UINT videoH = 0;
    UINT photoW = 0;
    UINT photoH = 0;
    std::wstring videoError;
    std::wstring photoError;

    ~Impl() {
        closing = true;
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
        if (dc) {
            dc->SetTarget(nullptr);
        }
        if (sharedHandle) {
            CloseHandle(sharedHandle);
            sharedHandle = nullptr;
        }
        reader.Reset();
        if (mfStarted) {
            MFShutdown();
        }
        if (comStarted) {
            CoUninitialize();
        }
    }

    void apply(float x, float y, uint32_t action) {
        applyPointer(&state, scene, x, y, action, (float)std::max(1, uiW), (float)std::max(1, uiH), dpiScale);
    }

    void mapClient(int x, int y, float* sx, float* sy) const {
        *sx = (float)x;
        *sy = (float)y;
    }

    bool createUiTexture(ID3D11Texture2D** texture) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = (UINT)std::max(1, uiW);
        desc.Height = (UINT)std::max(1, uiH);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        return check(device->CreateTexture2D(&desc, nullptr, texture), "ui texture");
    }

    bool bindTarget(ID3D11Texture2D* texture, ID2D1Bitmap1** bitmap, bool target) {
        ComPtr<IDXGISurface> surface;
        if (!check(texture->QueryInterface(IID_PPV_ARGS(&surface)), "IDXGISurface")) {
            return false;
        }
        const UINT options = target ? (D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW) : D2D1_BITMAP_OPTIONS_NONE;
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            (D2D1_BITMAP_OPTIONS)options, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
        return check(dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, bitmap), target ? "ui target" : "window bitmap");
    }

    bool ensureFonts(float scale) {
        if (titleFont && dpiScale > scale - 0.01f && dpiScale < scale + 0.01f) {
            return true;
        }
        titleFont = makeFont(28.f * scale, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
        bodyFont = makeFont(16.f * scale, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_TEXT_ALIGNMENT_LEADING);
        buttonFont = makeFont(16.f * scale, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        smallFont = makeFont(13.f * scale, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
        if (!titleFont || !bodyFont || !buttonFont || !smallFont) {
            return false;
        }
        dpiScale = scale;
        return true;
    }

    ComPtr<IDWriteTextFormat> makeFont(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
        ComPtr<IDWriteTextFormat> format;
        if (!check(writeFactory->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                                  L"en-us", &format),
                   "CreateTextFormat")) {
            return nullptr;
        }
        format->SetTextAlignment(align);
        format->SetParagraphAlignment(align == DWRITE_TEXT_ALIGNMENT_CENTER ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
                                                                            : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return format;
    }

    void fill(const D2D1_RECT_F& rect, D2D1_COLOR_F color) {
        brush->SetColor(color);
        dc->FillRectangle(rect, brush.Get());
    }

    void text(const wchar_t* value, IDWriteTextFormat* font, const D2D1_RECT_F& rect, D2D1_COLOR_F color) {
        if (!value || !font) {
            return;
        }
        brush->SetColor(color);
        dc->DrawText(value, (UINT32)wcslen(value), font, rect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void drawButton(const UiRect& bounds, const wchar_t* label, bool hot, bool down, bool primary) {
        D2D1_ROUNDED_RECT round{rectF(bounds), 6.f, 6.f};
        D2D1_COLOR_F fillColor;
        D2D1_COLOR_F textColor;
        if (primary) {
            fillColor = down ? D2D1::ColorF(0.00f, 0.31f, 0.58f) : hot ? D2D1::ColorF(0.10f, 0.47f, 0.82f) : D2D1::ColorF(0.00f, 0.40f, 0.75f);
            textColor = D2D1::ColorF(1.f, 1.f, 1.f);
        } else {
            fillColor = down ? D2D1::ColorF(0.90f, 0.91f, 0.93f) : hot ? D2D1::ColorF(0.96f, 0.97f, 0.98f) : D2D1::ColorF(1.f, 1.f, 1.f);
            textColor = D2D1::ColorF(0.12f, 0.13f, 0.16f);
        }
        brush->SetColor(fillColor);
        dc->FillRoundedRectangle(round, brush.Get());
        if (!primary) {
            brush->SetColor(D2D1::ColorF(0.78f, 0.79f, 0.82f));
            dc->DrawRoundedRectangle(round, brush.Get(), 1.f);
        }
        text(label, buttonFont.Get(), rectF(bounds), textColor);
    }

    void drawButtons(const SceneButtons& buttons) {
        for (int i = 0; i < buttons.count; ++i) {
            wchar_t label[64];
            toWide(buttonLabel(buttons.id[i], state), label, 64);
            const bool hot = state.hot == buttons.id[i];
            const bool down = state.pressed == buttons.id[i] && hot;
            drawButton(buttons.rect[i], label, hot, down, buttonPrimary(buttons.id[i], state));
        }
    }

    void drawHeader(bool dark, const wchar_t* title, const wchar_t* subtitle, const ViewLayout& view) {
        const float s = view.scale;
        const D2D1_COLOR_F titleColor = dark ? D2D1::ColorF(0.96f, 0.97f, 0.98f) : D2D1::ColorF(0.11f, 0.12f, 0.14f);
        const D2D1_COLOR_F subColor = dark ? D2D1::ColorF(0.62f, 0.66f, 0.72f) : D2D1::ColorF(0.38f, 0.41f, 0.46f);
        text(title, titleFont.Get(), D2D1::RectF(20.f * s, 16.f * s, view.width - 20.f * s, 50.f * s), titleColor);
        text(subtitle, bodyFont.Get(), D2D1::RectF(20.f * s, 46.f * s, view.width - 20.f * s, 70.f * s), subColor);
    }

    bool loadPhoto(const std::wstring& path) {
        ComPtr<IWICImagingFactory> wic;
        if (!check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)), "WIC factory")) {
            photoError = L"Windows Imaging Component did not start";
            return false;
        }
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))) {
            photoError = L"Could not open assets\\photo.jpg";
            return false;
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converted;
        if (!check(decoder->GetFrame(0, &frame), "photo frame") || !check(wic->CreateFormatConverter(&converted), "photo converter")) {
            photoError = L"Could not decode the photo";
            return false;
        }
        if (!check(converted->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.f,
                                         WICBitmapPaletteTypeCustom),
                   "photo convert")) {
            photoError = L"Could not convert the photo";
            return false;
        }
        converted->GetSize(&photoW, &photoH);
        if (photoW == 0 || photoH == 0) {
            photoError = L"The photo is empty";
            return false;
        }
        std::vector<BYTE> pixels((size_t)photoW * photoH * 4);
        if (!check(converted->CopyPixels(nullptr, photoW * 4, (UINT)pixels.size(), pixels.data()), "photo pixels")) {
            photoError = L"Could not read the photo";
            return false;
        }
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (!check(dc->CreateBitmap(D2D1::SizeU(photoW, photoH), pixels.data(), photoW * 4, &props, &photo), "photo bitmap")) {
            photoError = L"Could not upload the photo";
            return false;
        }
        return true;
    }

    bool openVideo(const std::wstring& path) {
        ComPtr<IMFAttributes> attributes;
        if (!check(MFCreateAttributes(&attributes, 2), "MFCreateAttributes")) {
            videoError = L"Media Foundation did not start";
            return false;
        }
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader))) {
            videoError = L"Could not open assets\\flower.mp4";
            return false;
        }
        reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        ComPtr<IMFMediaType> output;
        if (!check(MFCreateMediaType(&output), "video output type")) {
            videoError = L"Could not describe the video";
            return false;
        }
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (!check(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, output.Get()), "video RGB32")) {
            videoError = L"Could not decode the video";
            return false;
        }
        ComPtr<IMFMediaType> current;
        if (SUCCEEDED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current))) {
            MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &videoW, &videoH);
        }
        PROPVARIANT duration{};
        if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration))) {
            if (duration.vt == VT_UI8) {
                state.mediaDuration = duration.uhVal.QuadPart / 10000000.0;
            } else if (duration.vt == VT_I8) {
                state.mediaDuration = duration.hVal.QuadPart / 10000000.0;
            }
        }
        PropVariantClear(&duration);
        if (videoW == 0 || videoH == 0) {
            videoError = L"The video has no picture";
            return false;
        }
        return readSample();
    }

    void seekToStart() {
        if (!reader) {
            return;
        }
        PROPVARIANT position{};
        position.vt = VT_I8;
        position.hVal.QuadPart = 0;
        reader->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);
        reader->Flush((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        haveFrame = false;
    }

    bool readSample() {
        if (!reader) {
            return false;
        }
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &timestamp, &sample);
        if (FAILED(hr)) {
            videoFailed = true;
            videoError = L"Reading the video failed";
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            return false;
        }
        if (!sample) {
            return true;
        }
        LONGLONG duration = 0;
        sample->GetSampleDuration(&duration);
        if (duration <= 0) {
            duration = 333333;
        }
        ComPtr<IMFMediaBuffer> buffer;
        if (!check(sample->ConvertToContiguousBuffer(&buffer), "video buffer")) {
            return false;
        }
        BYTE* data = nullptr;
        DWORD length = 0;
        if (!check(buffer->Lock(&data, nullptr, &length), "video lock")) {
            return false;
        }
        const UINT pitch = videoW * 4;
        std::vector<BYTE> packed;
        const BYTE* source = data;
        if (length >= (DWORD)videoH * pitch) {
            source = data;
        } else if (length >= pitch) {
            videoH = length / pitch;
        }
        if (!videoFrame) {
            D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
            if (!check(dc->CreateBitmap(D2D1::SizeU(videoW, videoH), source, pitch, &props, &videoFrame), "video bitmap")) {
                buffer->Unlock();
                videoError = L"Could not upload a video frame";
                return false;
            }
        } else {
            videoFrame->CopyFromMemory(nullptr, source, pitch);
        }
        buffer->Unlock();
        frameStart = timestamp / 10000000.0;
        frameEnd = (timestamp + duration) / 10000000.0;
        haveFrame = true;
        return true;
    }

    void advanceVideo() {
        if (!reader || videoFailed) {
            return;
        }
        if (state.mediaDuration > 0.05 && state.mediaSeconds >= state.mediaDuration - 0.02) {
            seekToStart();
            state.mediaSeconds = 0;
        }
        for (int i = 0; i < 16; ++i) {
            if (haveFrame && state.mediaSeconds < frameEnd) {
                return;
            }
            if (!readSample()) {
                seekToStart();
                state.mediaSeconds = 0;
                readSample();
                return;
            }
            if (!haveFrame) {
                continue;
            }
        }
    }

    void drawPhoto(const UiRect& well) {
        fill(rectF(well), D2D1::ColorF(0.90f, 0.91f, 0.93f));
        if (!photo) {
            text(photoError.empty() ? L"No photo" : photoError.c_str(), bodyFont.Get(),
                 D2D1::RectF(well.x + 24.f, well.y + well.h * 0.45f, well.x + well.w - 24.f, well.y + well.h),
                 D2D1::ColorF(0.35f, 0.37f, 0.42f));
            return;
        }
        dc->PushAxisAlignedClip(rectF(well), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        D2D1_RECT_F dest;
        if (!state.actualSize) {
            const float scale = std::min(well.w / (float)photoW, well.h / (float)photoH);
            const float dw = photoW * scale;
            const float dh = photoH * scale;
            const float dx = well.x + (well.w - dw) * 0.5f;
            const float dy = well.y + (well.h - dh) * 0.5f;
            dest = D2D1::RectF(dx, dy, dx + dw, dy + dh);
        } else {
            const float dx = well.x + (well.w - (float)photoW) * 0.5f;
            const float dy = well.y + (well.h - (float)photoH) * 0.5f;
            dest = D2D1::RectF(dx, dy, dx + (float)photoW, dy + (float)photoH);
        }
        dc->DrawBitmap(photo.Get(), &dest, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        dc->PopAxisAlignedClip();
    }

    void drawVideo(const UiRect& well) {
        fill(rectF(well), D2D1::ColorF(0.05f, 0.05f, 0.06f));
        if (!videoFrame) {
            text(videoError.empty() ? L"No video" : videoError.c_str(), bodyFont.Get(),
                 D2D1::RectF(well.x + 24.f, well.y + well.h * 0.45f, well.x + well.w - 24.f, well.y + well.h),
                 D2D1::ColorF(0.75f, 0.78f, 0.82f));
            return;
        }
        const float scale = std::min(well.w / (float)videoW, well.h / (float)videoH);
        const float dw = videoW * scale;
        const float dh = videoH * scale;
        const float dx = well.x + (well.w - dw) * 0.5f;
        const float dy = well.y + (well.h - dh) * 0.5f;
        const D2D1_RECT_F dest = D2D1::RectF(dx, dy, dx + dw, dy + dh);
        dc->DrawBitmap(videoFrame.Get(), &dest, 1.f, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
    }

    void drawTransport(const ViewLayout& view) {
        const float s = view.scale;
        wchar_t nowText[16];
        wchar_t endText[16];
        formatClock(nowText, 16, state.mediaSeconds);
        formatClock(endText, 16, state.mediaDuration);
        wchar_t label[40];
        swprintf_s(label, L"%s   /   %s", nowText, endText);
        const float textX = 20.f * s + 112.f * s + 16.f * s;
        const float textY = view.barTop + 16.f * s;
        text(label, bodyFont.Get(), D2D1::RectF(textX, textY, textX + 150.f * s, textY + 32.f * s), D2D1::ColorF(0.82f, 0.84f, 0.88f));

        const float trackX = textX + 150.f * s;
        const float trackW = std::max(40.f, view.width - trackX - 20.f * s);
        const float trackY = view.barTop + 28.f * s;
        fill(D2D1::RectF(trackX, trackY, trackX + trackW, trackY + 6.f * s), D2D1::ColorF(0.28f, 0.30f, 0.34f));
        float fraction = 0.f;
        if (state.mediaDuration > 0.05) {
            fraction = (float)(state.mediaSeconds / state.mediaDuration);
        }
        if (fraction < 0.f) {
            fraction = 0.f;
        }
        if (fraction > 1.f) {
            fraction = 1.f;
        }
        fill(D2D1::RectF(trackX, trackY, trackX + trackW * fraction, trackY + 6.f * s), D2D1::ColorF(0.20f, 0.55f, 0.95f));
    }

    void drawControls(const ViewLayout& view, const SceneButtons& buttons) {
        const float s = view.scale;
        float top = view.barTop;
        for (int i = 0; i < buttons.count; ++i) {
            top = std::max(top, buttons.rect[i].y + buttons.rect[i].h);
        }
        top += 20.f * s;
        const float left = 20.f * s;
        const float right = view.width - 20.f * s;
        const float cardBottom = top + 120.f * s;
        const wchar_t* status = L"Idle";
        D2D1_COLOR_F dot = D2D1::ColorF(0.55f, 0.57f, 0.62f);
        if (state.runState == 1) {
            status = L"Running";
            dot = D2D1::ColorF(0.13f, 0.62f, 0.36f);
        } else if (state.runState == 2) {
            status = L"Stopped";
            dot = D2D1::ColorF(0.75f, 0.22f, 0.24f);
        }
        fill(D2D1::RectF(left, top, right, cardBottom), D2D1::ColorF(1.f, 1.f, 1.f));
        brush->SetColor(D2D1::ColorF(0.86f, 0.87f, 0.90f));
        dc->DrawRectangle(D2D1::RectF(left, top, right, cardBottom), brush.Get(), 1.f);
        brush->SetColor(dot);
        D2D1_ELLIPSE ellipse{D2D1::Point2F(left + 28.f * s, top + 56.f * s), 8.f * s, 8.f * s};
        dc->FillEllipse(ellipse, brush.Get());
        text(L"Status", smallFont.Get(), D2D1::RectF(left + 52.f * s, top + 22.f * s, right - 16.f * s, top + 44.f * s),
             D2D1::ColorF(0.40f, 0.43f, 0.48f));
        text(status, titleFont.Get(), D2D1::RectF(left + 52.f * s, top + 44.f * s, right - 16.f * s, top + 84.f * s),
             D2D1::ColorF(0.10f, 0.11f, 0.14f));
        wchar_t presses[64];
        swprintf_s(presses, L"Button presses:  %d", state.clicks);
        text(presses, bodyFont.Get(), D2D1::RectF(left + 52.f * s, top + 86.f * s, right - 16.f * s, top + 112.f * s),
             D2D1::ColorF(0.28f, 0.30f, 0.35f));

        const float notesY = cardBottom + 24.f * s;
        text(L"Notes", smallFont.Get(), D2D1::RectF(left, notesY, right, notesY + 22.f * s), D2D1::ColorF(0.40f, 0.43f, 0.48f));
        if (state.noteCount == 0) {
            text(L"No notes yet. Add note appends one.", bodyFont.Get(),
                 D2D1::RectF(left, notesY + 28.f * s, right, notesY + 56.f * s), D2D1::ColorF(0.45f, 0.48f, 0.53f));
        }
        for (int i = 0; i < state.noteCount; ++i) {
            wchar_t line[64];
            toWide(state.notes[i], line, 64);
            const float y = notesY + 28.f * s + i * 32.f * s;
            text(line, bodyFont.Get(), D2D1::RectF(left, y, right, y + 30.f * s), D2D1::ColorF(0.12f, 0.13f, 0.16f));
        }
    }

    void paint(const ViewLayout& view, const SceneButtons& buttons) {
        dc->SetTarget(uiTarget.Get());
        dc->SetDpi(96.f, 96.f);
        dc->BeginDraw();
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        const bool dark = scene == 0;
        fill(D2D1::RectF(0.f, 0.f, view.width, view.height),
             dark ? D2D1::ColorF(0.09f, 0.10f, 0.12f) : D2D1::ColorF(0.95f, 0.95f, 0.96f));
        if (scene == 0) {
            drawHeader(true, L"Video", L"flower.mp4", view);
            drawVideo(view.well);
            drawTransport(view);
        } else if (scene == 1) {
            wchar_t subtitle[80];
            if (photo) {
                swprintf_s(subtitle, L"photo.jpg    %u × %u", photoW, photoH);
            } else {
                wcscpy_s(subtitle, L"photo.jpg");
            }
            drawHeader(false, L"Image", subtitle, view);
            drawPhoto(view.well);
        } else {
            drawHeader(false, L"Controls", L"Click the buttons. The mirror shows this window.", view);
            drawControls(view, buttons);
        }
        drawButtons(buttons);
        dc->EndDraw();
        dc->SetTarget(nullptr);
    }

    bool ensureSurface(int width, int height) {
        if (width < 1 || height < 1) {
            return false;
        }
        if (width == uiW && height == uiH && sharedTex && uiTex && uiTarget && mutex) {
            return true;
        }
        if (dc) {
            dc->SetTarget(nullptr);
        }
        uiTarget.Reset();
        uiTex.Reset();
        mutex.Reset();
        sharedTex.Reset();
        if (sharedHandle) {
            CloseHandle(sharedHandle);
            sharedHandle = nullptr;
        }
        uiW = width;
        uiH = height;
        generation += 1;

        D3D11_TEXTURE2D_DESC shared{};
        shared.Width = (UINT)uiW;
        shared.Height = (UINT)uiH;
        shared.MipLevels = 1;
        shared.ArraySize = 1;
        shared.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        shared.SampleDesc.Count = 1;
        shared.Usage = D3D11_USAGE_DEFAULT;
        shared.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        shared.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        if (!check(device->CreateTexture2D(&shared, nullptr, &sharedTex), "renderer shared texture")) {
            return false;
        }
        ComPtr<IDXGIResource1> resource;
        if (!check(sharedTex.As(&resource), "renderer IDXGIResource1")) {
            return false;
        }
        wchar_t name[160];
        sharedSurfaceName(name, 160, pid, generation);
        if (!check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, name, &sharedHandle),
                   "renderer CreateSharedHandle")) {
            return false;
        }
        if (!check(sharedTex.As(&mutex), "renderer keyed mutex")) {
            return false;
        }
        if (!createUiTexture(&uiTex) || !bindTarget(uiTex.Get(), &uiTarget, true)) {
            return false;
        }
        logf("renderer surface %dx%d gen %u %ls", uiW, uiH, generation, name);
        return true;
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            CREATESTRUCTW* created = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created->lpCreateParams));
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self || self->closing) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_SIZE: {
            self->clientW = LOWORD(lParam);
            self->clientH = HIWORD(lParam);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 640;
            info->ptMinTrackSize.y = 420;
            return 0;
        }
        case WM_DPICHANGED: {
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            float x = 0;
            float y = 0;
            self->mapClient(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &x, &y);
            self->apply(x, y, kPointerMove);
            return 0;
        }
        case WM_MOUSELEAVE:
            self->state.hot = kButtonNone;
            return 0;
        case WM_LBUTTONDOWN: {
            SetCapture(hwnd);
            float x = 0;
            float y = 0;
            self->mapClient(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &x, &y);
            self->apply(x, y, kPointerDown);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            float x = 0;
            float y = 0;
            self->mapClient(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &x, &y);
            self->apply(x, y, kPointerUp);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            self->hwnd = nullptr;
            self->quit = true;
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
};

Producer::Producer() : impl_(new Impl) {}

Producer::~Producer() {
    close();
    delete impl_;
    impl_ = nullptr;
}

bool Producer::open(int scene) {
    close();
    impl_->scene = scene;
    impl_->pid = GetCurrentProcessId();

    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    impl_->comStarted = com == S_OK || com == S_FALSE;
    if (!check(MFStartup(MF_VERSION), "MFStartup")) {
        return false;
    }
    impl_->mfStarted = true;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (!check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &level, 1, D3D11_SDK_VERSION, &impl_->device, nullptr,
                                 &impl_->context),
               "renderer D3D11CreateDevice")) {
        return false;
    }

    if (!check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&impl_->d2dFactory)), "D2D1CreateFactory")) {
        return false;
    }
    ComPtr<IDXGIDevice> dxgi;
    if (!check(impl_->device.As(&dxgi), "renderer DXGI device")) {
        return false;
    }
    if (!check(impl_->d2dFactory->CreateDevice(dxgi.Get(), &impl_->d2dDevice), "D2D device")) {
        return false;
    }
    if (!check(impl_->d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &impl_->dc), "D2D context")) {
        return false;
    }
    impl_->dc->SetDpi(96.f, 96.f);
    if (!check(impl_->dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f), &impl_->brush), "brush")) {
        return false;
    }
    if (!check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(impl_->writeFactory.GetAddressOf())),
               "DWriteCreateFactory")) {
        return false;
    }
    if (!impl_->ensureFonts(1.f)) {
        return false;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = Impl::proc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.lpszClassName = L"GPUMirrorRenderer";
    RegisterClassExW(&windowClass);

    const RECT work = mirrorWorkArea();
    int outerW = 0;
    int outerH = 0;
    clientToOuter(960, 600, &outerW, &outerH);
    const int x = work.left + 48 + scene * 40;
    const int y = work.top + 48 + scene * 36;
    wchar_t title[64];
    toWide(sceneTitle(scene), title, 64);
    impl_->hwnd = CreateWindowExW(0, windowClass.lpszClassName, title, WS_OVERLAPPEDWINDOW, x, y, outerW, outerH, nullptr, nullptr,
                                  instance, impl_);
    if (!impl_->hwnd) {
        logf("renderer CreateWindowEx failed: %lu", GetLastError());
        return false;
    }
    RECT client{};
    GetClientRect(impl_->hwnd, &client);
    impl_->clientW = std::max(1L, client.right - client.left);
    impl_->clientH = std::max(1L, client.bottom - client.top);

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (!check(impl_->device.As(&dxgiDevice), "swap DXGI") || !check(dxgiDevice->GetAdapter(&adapter), "swap adapter") ||
        !check(adapter->GetParent(IID_PPV_ARGS(&factory)), "swap factory")) {
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = (UINT)impl_->clientW;
    desc.Height = (UINT)impl_->clientH;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (!check(factory->CreateSwapChainForHwnd(impl_->device.Get(), impl_->hwnd, &desc, nullptr, nullptr, &impl_->swap),
               "renderer swap chain")) {
        return false;
    }
    factory->MakeWindowAssociation(impl_->hwnd, DXGI_MWA_NO_ALT_ENTER);
    impl_->swapW = impl_->clientW;
    impl_->swapH = impl_->clientH;

    if (impl_->scene == 0) {
        if (!impl_->openVideo(assetFile(L"flower.mp4"))) {
            impl_->videoFailed = true;
            logf("video did not open");
        }
    } else if (impl_->scene == 1) {
        if (!impl_->loadPhoto(assetFile(L"photo.jpg"))) {
            impl_->photoFailed = true;
            logf("photo did not open");
        }
    }

    ShowWindow(impl_->hwnd, SW_SHOWNORMAL);
    UpdateWindow(impl_->hwnd);
    impl_->open = true;
    logf("renderer %s window %p", sceneTitle(scene), impl_->hwnd);
    return true;
}

bool Producer::pump() {
    if (!impl_) {
        return false;
    }
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            impl_->quit = true;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return impl_->hwnd != nullptr && !impl_->quit;
}

bool Producer::draw(int scene, float timeSeconds) {
    if (!impl_->open || !impl_->dc || !impl_->hwnd) {
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

    RECT client{};
    GetClientRect(impl_->hwnd, &client);
    const int width = std::max(0L, client.right - client.left);
    const int height = std::max(0L, client.bottom - client.top);
    if (width < 2 || height < 2) {
        return true;
    }
    impl_->clientW = width;
    impl_->clientH = height;
    const UINT dpi = GetDpiForWindow(impl_->hwnd);
    const float scale = dpi > 0 ? (float)dpi / 96.f : 1.f;
    if (!impl_->ensureFonts(scale) || !impl_->ensureSurface(width, height)) {
        return false;
    }
    if (width != impl_->swapW || height != impl_->swapH) {
        if (!check(impl_->swap->ResizeBuffers(0, (UINT)width, (UINT)height, DXGI_FORMAT_UNKNOWN, 0), "renderer ResizeBuffers")) {
            return false;
        }
        impl_->swapW = width;
        impl_->swapH = height;
    }

    ViewLayout view{};
    SceneButtons buttons{};
    layoutScene(scene, (float)width, (float)height, impl_->dpiScale, &view, &buttons);
    impl_->paint(view, buttons);

    // The window is presented even when the host is not running. The keyed mutex
    // only gates the shared copy, and a missed key must not freeze this window.
    HRESULT hr = impl_->mutex->AcquireSync(0, 0);
    if (hr == S_OK) {
        impl_->context->CopyResource(impl_->sharedTex.Get(), impl_->uiTex.Get());
        impl_->mutex->ReleaseSync(1);
    }

    ComPtr<ID3D11Texture2D> back;
    if (!check(impl_->swap->GetBuffer(0, IID_PPV_ARGS(&back)), "renderer back buffer")) {
        return false;
    }
    impl_->context->CopyResource(back.Get(), impl_->uiTex.Get());
    impl_->context->Flush();
    impl_->swap->Present(1, 0);
    return true;
}

int Producer::width() const {
    return impl_ ? impl_->uiW : 0;
}

int Producer::height() const {
    return impl_ ? impl_->uiH : 0;
}

uint32_t Producer::generation() const {
    return impl_ ? impl_->generation : 0;
}

void Producer::attachSurface(uint64_t) {}

void Producer::pointer(int x, int y, int action) {
    if (!impl_ || !impl_->open) {
        return;
    }
    impl_->apply((float)x, (float)y, (uint32_t)action);
}

void Producer::close() {
    if (!impl_) {
        return;
    }
    impl_->open = false;
    impl_->closing = true;
    if (impl_->dc) {
        impl_->dc->SetTarget(nullptr);
    }
    if (impl_->context) {
        impl_->context->ClearState();
    }
    delete impl_;
    impl_ = new Impl;
}

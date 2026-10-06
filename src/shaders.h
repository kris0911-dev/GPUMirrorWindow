#pragma once

// Full-screen triangle for a renderer, and a quad shader for the host window.
// uv.y is 0 at the top of the surface.

inline constexpr const char* kSurfaceShader = R"HLSL(
cbuffer Frame : register(b0) {
    float time;
    float mode;
    float2 unused;
};

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut vs_surface(uint id : SV_VertexID) {
    float2 t = float2((id << 1) & 2, id & 2);
    VsOut o;
    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = t * 0.5;
    return o;
}

float segmentOn(float2 p, int digit) {
    int masks[10] = {119, 36, 93, 109, 46, 107, 123, 37, 127, 111};
    int index = digit;
    if (index < 0) index = 0;
    if (index > 9) index = 9;
    int mask = masks[index];
    float t = 0.18;
    float on = 0.0;
    if ((mask & 1) != 0 && p.y < t && p.x > 0.16 && p.x < 0.84) on = 1.0;
    if ((mask & 2) != 0 && p.x < t && p.y > 0.06 && p.y < 0.50) on = 1.0;
    if ((mask & 4) != 0 && p.x > 1.0 - t && p.y > 0.06 && p.y < 0.50) on = 1.0;
    if ((mask & 8) != 0 && abs(p.y - 0.50) < t * 0.45 && p.x > 0.16 && p.x < 0.84) on = 1.0;
    if ((mask & 16) != 0 && p.x < t && p.y > 0.50 && p.y < 0.94) on = 1.0;
    if ((mask & 32) != 0 && p.x > 1.0 - t && p.y > 0.50 && p.y < 0.94) on = 1.0;
    if ((mask & 64) != 0 && p.y > 1.0 - t && p.x > 0.16 && p.x < 0.84) on = 1.0;
    return on;
}

float timecode(float2 uv, float timeValue) {
    int centi = (int)(timeValue * 100.0);
    if (centi < 0) centi = 0;
    int secs = centi / 100;
    int mm = (secs / 60) % 100;
    int ss = secs % 60;
    int digits[4] = {mm / 10, mm % 10, ss / 10, ss % 10};
    float result = 0.0;
    [loop]
    for (int i = 0; i < 4; ++i) {
        float x0 = 0.040 + (float)i * 0.050 + (i >= 2 ? 0.022 : 0.0);
        float2 p = (uv - float2(x0, 0.860)) / float2(0.036, 0.100);
        if (p.x >= 0.0 && p.y >= 0.0 && p.x <= 1.0 && p.y <= 1.0) {
            result = max(result, segmentOn(p, digits[i]));
        }
    }
    float cx = 0.040 + 2.0 * 0.050 + 0.006;
    if (abs(uv.x - cx) < 0.004 && (abs(uv.y - 0.890) < 0.008 || abs(uv.y - 0.930) < 0.008)) {
        if (frac(timeValue * 2.0) > 0.25) result = 1.0;
    }
    return result;
}

float3 videoFrame(float2 uv, float timeValue) {
    float3 bars[8] = {
        float3(0.78, 0.78, 0.78),
        float3(0.78, 0.78, 0.10),
        float3(0.10, 0.78, 0.78),
        float3(0.10, 0.72, 0.18),
        float3(0.72, 0.12, 0.62),
        float3(0.78, 0.12, 0.12),
        float3(0.12, 0.16, 0.72),
        float3(0.08, 0.08, 0.08)
    };
    int idx = (int)floor(uv.x * 8.0);
    if (idx < 0) idx = 0;
    if (idx > 7) idx = 7;
    float3 rgb = bars[idx];
    if (uv.y > 0.67) rgb *= 0.72;

    if (uv.y > 0.18 && uv.y < 0.62) {
        float2 cell = floor(float2(uv.x * 20.0, (uv.y - 0.18) * 14.0));
        float n = frac(sin(dot(cell, float2(12.9898, 78.233)) + floor(timeValue * 12.0) * 0.17) * 43758.5453);
        float3 picture = lerp(float3(0.05, 0.08, 0.12), float3(0.95, 0.62, 0.28), n);
        float sweep = frac(timeValue * 0.35);
        if (uv.x > sweep && uv.x < sweep + 0.18) {
            picture += float3(0.15, 0.18, 0.22);
        }
        rgb = picture;
    }

    float head = frac(timeValue * 0.12);
    if (uv.y > 0.70 && uv.y < 0.78 && abs(uv.x - head) < 0.006) {
        rgb = float3(1.0, 1.0, 1.0);
    } else if (uv.y > 0.72 && uv.y < 0.76) {
        rgb = lerp(rgb, float3(0.02, 0.02, 0.02), 0.65);
        if (uv.x < head) rgb = lerp(rgb, float3(0.85, 0.15, 0.18), 0.85);
    }

    if (timecode(uv, timeValue) > 0.5) rgb = float3(0.95, 0.97, 0.92);

    float2 rec = uv - float2(0.94, 0.91);
    if (length(rec * float2(1.0, 1.6)) < 0.016) {
        rgb = frac(timeValue) > 0.5 ? float3(0.95, 0.12, 0.16) : float3(0.35, 0.05, 0.06);
    }
    return rgb;
}

float3 pageFrame(float2 uv, float timeValue) {
    float3 paper = float3(0.955, 0.941, 0.902);
    float3 rgb = paper;
    float scroll = frac(timeValue * 0.05) * 0.45;
    float y = uv.y + scroll;

    if (uv.y < 0.10) {
        rgb = float3(0.11, 0.14, 0.20);
        if (uv.x > 0.04 && uv.x < 0.62 && uv.y > 0.028 && uv.y < 0.072) {
            rgb = float3(0.22, 0.27, 0.36);
        }
        return rgb;
    }

    if (y > 0.16 && y < 0.23 && uv.x > 0.07 && uv.x < 0.58) {
        rgb = float3(0.16, 0.17, 0.20);
    }

    [loop]
    for (int i = 0; i < 7; ++i) {
        float rowY = 0.30 + (float)i * 0.08;
        float w = 0.28 + frac(sin((float)i * 4.7) * 19.1) * 0.38;
        if (y > rowY && y < rowY + 0.018 && uv.x > 0.07 && uv.x < 0.07 + w) {
            rgb = (i == 2) ? float3(0.16, 0.38, 0.72) : float3(0.45, 0.46, 0.48);
        }
    }

    float cardY = 0.48;
    if (abs(uv.x - 0.74) < 0.18 && abs(y - cardY) < 0.24) {
        float stripe = frac(uv.y * 10.0 + timeValue * 0.4);
        rgb = lerp(float3(0.18, 0.36, 0.58), float3(0.86, 0.48, 0.28), stripe);
        if (abs(uv.x - 0.74) > 0.165 || abs(y - cardY) > 0.225) rgb = float3(0.75, 0.73, 0.68);
    }

    float2 cursor = float2(0.22 + frac(timeValue * 0.07) * 0.3, 0.58);
    if (abs(uv.x - cursor.x) < 0.012 && abs(y - cursor.y) < 0.02) {
        rgb = float3(0.10, 0.35, 0.85);
    }
    return rgb;
}

float3 sceneFrame(float2 uv, float timeValue) {
    float2 p = uv * 2.0 - 1.0;
    p.x *= 1280.0 / 720.0;
    float glow = 0.0;
    [loop]
    for (int i = 0; i < 3; ++i) {
        float speed = 0.65 + (float)i * 0.22;
        float2 c = 0.62 * float2(cos(timeValue * speed + (float)i), sin(timeValue * (speed + 0.15) + (float)i * 2.1));
        glow += 0.045 / max(length(p - c), 0.02);
    }
    float grid = 0.0;
    if (frac(uv.x * 14.0) > 0.97 || frac(uv.y * 8.0) > 0.97) grid = 1.0;
    float3 rgb = float3(0.03, 0.05, 0.09);
    rgb += float3(0.15, 0.85, 0.78) * saturate(glow);
    rgb += float3(0.10, 0.16, 0.28) * grid;
    float ring = abs(length(p) - 0.35 - 0.08 * sin(timeValue * 1.4));
    if (ring < 0.015) rgb += float3(0.95, 0.85, 0.45);
    return saturate(rgb);
}

float4 ps_surface(VsOut i) : SV_Target {
    float3 rgb;
    if (mode < 0.5) rgb = videoFrame(i.uv, time);
    else if (mode < 1.5) rgb = pageFrame(i.uv, time);
    else rgb = sceneFrame(i.uv, time);
    return float4(rgb, 1.0);
}
)HLSL";

inline constexpr const char* kPresentShader = R"HLSL(
cbuffer Quad : register(b0) {
    float4 rect;
    float4 uvRect;
    float4 tint;
    float4 unused;
};

Texture2D surface : register(t0);
SamplerState samp : register(s0);

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float2 kCorners[6] = {
    float2(0, 0), float2(1, 0), float2(0, 1),
    float2(0, 1), float2(1, 0), float2(1, 1)
};

VsOut vs_quad(uint id : SV_VertexID) {
    float2 c = kCorners[id];
    VsOut o;
    o.pos = float4(lerp(rect.x, rect.z, c.x), lerp(rect.y, rect.w, c.y), 0.0, 1.0);
    o.uv = lerp(uvRect.xy, uvRect.zw, c);
    return o;
}

float4 ps_quad(VsOut i) : SV_Target {
    return surface.Sample(samp, i.uv) * tint;
}
)HLSL";

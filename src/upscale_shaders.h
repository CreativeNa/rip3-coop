// HLSL (ps_3_0) for the upscaler in rip3video.c, compiled at runtime with
// d3dcompiler_47.dll. Vertex positions are pre-transformed (XYZRHW), so only
// pixel shaders are needed.

/* Catmull-Rom bicubic upscale in 9 bilinear taps (after M. Pettineo).
   c0 = source size (w, h, 1/w, 1/h). s0 = source, linear filtering. */
static const char k_ps_catmull[] =
    "sampler2D src : register(s0);\n"
    "float4 size : register(c0);\n"
    "float4 tap(float2 p) { return tex2Dlod(src, float4(p, 0, 0)); }\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float2 sp = uv * size.xy;\n"
    "  float2 t1 = floor(sp - 0.5) + 0.5;\n"
    "  float2 f = sp - t1;\n"
    "  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
    "  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
    "  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
    "  float2 w3 = f * f * (-0.5 + 0.5 * f);\n"
    "  float2 w12 = w1 + w2;\n"
    "  float2 t0 = (t1 - 1.0) * size.zw;\n"
    "  float2 t3 = (t1 + 2.0) * size.zw;\n"
    "  float2 t12 = (t1 + w2 / w12) * size.zw;\n"
    "  float4 r = 0;\n"
    "  r += tap(float2(t0.x, t0.y)) * w0.x * w0.y;\n"
    "  r += tap(float2(t12.x, t0.y)) * w12.x * w0.y;\n"
    "  r += tap(float2(t3.x, t0.y)) * w3.x * w0.y;\n"
    "  r += tap(float2(t0.x, t12.y)) * w0.x * w12.y;\n"
    "  r += tap(float2(t12.x, t12.y)) * w12.x * w12.y;\n"
    "  r += tap(float2(t3.x, t12.y)) * w3.x * w12.y;\n"
    "  r += tap(float2(t0.x, t3.y)) * w0.x * w3.y;\n"
    "  r += tap(float2(t12.x, t3.y)) * w12.x * w3.y;\n"
    "  r += tap(float2(t3.x, t3.y)) * w3.x * w3.y;\n"
    "  return float4(saturate(r.rgb), 1);\n"
    "}\n";

/* AMD FidelityFX RCAS (robust contrast-adaptive sharpening) from FSR 1,
   ported to SM3. c0 = (1/w, 1/h, sharpening amount = exp2(-sharpness), 0).
   s0 = upscaled image, point filtering. */
static const char k_ps_rcas[] =
    "sampler2D img : register(s0);\n"
    "float4 px : register(c0);\n"
    "float3 tap(float2 p) { return tex2Dlod(img, float4(p, 0, 0)).rgb; }\n"
    "float max3(float3 v) { return max(v.r, max(v.g, v.b)); }\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float3 b = tap(uv + float2(0, -px.y));\n"
    "  float3 d = tap(uv + float2(-px.x, 0));\n"
    "  float3 e = tap(uv);\n"
    "  float3 f = tap(uv + float2(px.x, 0));\n"
    "  float3 h = tap(uv + float2(0, px.y));\n"
    "  float3 mn4 = min(min(b, d), min(f, h));\n"
    "  float3 mx4 = max(max(b, d), max(f, h));\n"
    "  float3 hitMin = min(mn4, e) / (4.0 * mx4 + 1e-5);\n"
    "  float3 hitMax = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0 - 1e-5);\n"
    "  float3 lobeRGB = max(-hitMin, hitMax);\n"
    "  float lobe = max(-0.1875, min(max3(lobeRGB), 0.0)) * px.z;\n"
    "  float3 c = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);\n"
    "  return float4(saturate(c), 1);\n"
    "}\n";

/* "Sharp bilinear": nearest-neighbour look without shimmering, for a pixel-art feel.
   c0 = source size (w, h, 1/w, 1/h), c1.x = scale factor. s0 = source, linear. */
static const char k_ps_sharp[] =
    "sampler2D src : register(s0);\n"
    "float4 size : register(c0);\n"
    "float4 scale : register(c1);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float2 t = uv * size.xy;\n"
    "  float2 ti = floor(t);\n"
    "  float2 dist = (t - ti) - 0.5;\n"
    "  float range = 0.5 - 0.5 / scale.x;\n"
    "  float2 f = (dist - clamp(dist, -range, range)) * scale.x + 0.5;\n"
    "  return float4(tex2Dlod(src, float4((ti + f) * size.zw, 0, 0)).rgb, 1);\n"
    "}\n";

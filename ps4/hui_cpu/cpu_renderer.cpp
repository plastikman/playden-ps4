// Playden PS4 port - Hui's renderer on the CPU: textures, the draw-list shapes, backdrops and glass.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Each shape is hui's gl_batch.cpp fragment shader evaluated per output pixel, over the quad its
// vertex shader would cover, with the same straight-alpha "over" blend. Output is 0x00RRGGBB.

#include "gfx/renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace hui::gfx
{

namespace cpu
{

namespace
{

struct Store
{
    std::mutex mutex;
    std::unordered_map<std::uint32_t, Texture> textures;
    std::uint32_t next = 1;
    std::uint64_t bytes = 0;
};

Store &store()
{
    static Store s;
    return s;
}

Surface &present_target()
{
    static Surface surface;
    return surface;
}

std::uint64_t &present_ns()
{
    static std::uint64_t ns = 0;
    return ns;
}

bool &present_skipped()
{
    static bool skipped = false;
    return skipped;
}

} // namespace

void set_present_surface(const Surface &surface)
{
    present_target() = surface;
}

const Surface &present_surface()
{
    return present_target();
}

std::uint64_t last_present_ns()
{
    return present_ns();
}

bool last_present_skipped()
{
    return present_skipped();
}

void note_present(std::uint64_t ns, bool skipped)
{
    present_ns() = ns;
    present_skipped() = skipped;
}

std::uint32_t create_texture(int width, int height, int channels, const std::uint8_t *bytes)
{
    if (width <= 0 || height <= 0 || (channels != 1 && channels != 4))
        return 0;
    Store &s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    // Never hand out a font handle (0xf000000x) or 0.
    if (s.next >= kFontHandleBase)
        s.next = 1;
    const std::uint32_t name = s.next++;
    Texture &texture = s.textures[name];
    texture.width = width;
    texture.height = height;
    texture.channels = channels;
    const std::size_t size = static_cast<std::size_t>(width) * height * channels;
    if (bytes != nullptr)
        texture.data.assign(bytes, bytes + size);
    else
        texture.data.assign(size, 0);
    s.bytes += size;
    return name;
}

Texture *find_texture(std::uint32_t name)
{
    if (name == 0)
        return nullptr;
    Store &s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    const auto found = s.textures.find(name);
    return found == s.textures.end() ? nullptr : &found->second;
}

void delete_textures(GLsizei count, const GLuint *textures)
{
    Store &s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (GLsizei i = 0; textures != nullptr && i < count; ++i)
    {
        const auto found = s.textures.find(textures[i]);
        if (found == s.textures.end())
            continue;
        s.bytes -= found->second.data.size();
        s.textures.erase(found);
    }
}

std::uint64_t texture_bytes()
{
    Store &s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.bytes;
}

} // namespace cpu

namespace
{

struct Rgba
{
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline float smoothstep(float e0, float e1, float x)
{
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

inline float sign_of(float v)
{
    return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f);
}

inline Rgba mix4(const float *a, const float *b, float t)
{
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t,
            a[3] + (b[3] - a[3]) * t};
}

inline Rgba mixc(const Rgba &a, const Rgba &b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

float round_box(float px, float py, float bx, float by, float r)
{
    const float qx = std::fabs(px) - bx + r;
    const float qy = std::fabs(py) - by + r;
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

float triangle_sdf(float px, float py, float p0x, float p0y, float p1x, float p1y, float p2x, float p2y)
{
    const float e0x = p1x - p0x, e0y = p1y - p0y, e1x = p2x - p1x, e1y = p2y - p1y;
    const float e2x = p0x - p2x, e2y = p0y - p2y;
    const float v0x = px - p0x, v0y = py - p0y, v1x = px - p1x, v1y = py - p1y;
    const float v2x = px - p2x, v2y = py - p2y;
    const float t0 = clamp01((v0x * e0x + v0y * e0y) / (e0x * e0x + e0y * e0y));
    const float t1 = clamp01((v1x * e1x + v1y * e1y) / (e1x * e1x + e1y * e1y));
    const float t2 = clamp01((v2x * e2x + v2y * e2y) / (e2x * e2x + e2y * e2y));
    const float q0x = v0x - e0x * t0, q0y = v0y - e0y * t0;
    const float q1x = v1x - e1x * t1, q1y = v1y - e1y * t1;
    const float q2x = v2x - e2x * t2, q2y = v2y - e2y * t2;
    const float s = sign_of(e0x * e2y - e0y * e2x);
    float dx = q0x * q0x + q0y * q0y, dy = s * (v0x * e0y - v0y * e0x);
    dx = std::min(dx, q1x * q1x + q1y * q1y);
    dy = std::min(dy, s * (v1x * e1y - v1y * e1x));
    dx = std::min(dx, q2x * q2x + q2y * q2y);
    dy = std::min(dy, s * (v2x * e2y - v2y * e2x));
    return -std::sqrt(dx) * sign_of(dy);
}

float star5(float px, float py, float r, float rf)
{
    const float k1x = 0.809016994375f, k1y = -0.587785252292f;
    const float k2x = -k1x, k2y = k1y;
    px = std::fabs(px);
    float d = std::max(k1x * px + k1y * py, 0.0f);
    px -= 2.0f * d * k1x;
    py -= 2.0f * d * k1y;
    d = std::max(k2x * px + k2y * py, 0.0f);
    px -= 2.0f * d * k2x;
    py -= 2.0f * d * k2y;
    px = std::fabs(px);
    py -= r;
    const float bax = rf * -k1y, bay = rf * k1x - 1.0f;
    const float h = std::clamp((px * bax + py * bay) / (bax * bax + bay * bay), 0.0f, r);
    const float lx = px - bax * h, ly = py - bay * h;
    return std::sqrt(lx * lx + ly * ly) * sign_of(py * bax - px * bay);
}

// GL_LINEAR with GL_CLAMP_TO_EDGE: texel centres at (i + 0.5) / size.
Rgba sample(const cpu::Texture &t, float u, float v)
{
    const float fx = u * static_cast<float>(t.width) - 0.5f;
    const float fy = v * static_cast<float>(t.height) - 0.5f;
    const float flx = std::floor(fx), fly = std::floor(fy);
    const float tx = fx - flx, ty = fy - fly;
    const int x0 = std::clamp(static_cast<int>(flx), 0, t.width - 1);
    const int y0 = std::clamp(static_cast<int>(fly), 0, t.height - 1);
    const int x1 = std::clamp(static_cast<int>(flx) + 1, 0, t.width - 1);
    const int y1 = std::clamp(static_cast<int>(fly) + 1, 0, t.height - 1);
    const std::uint8_t *d = t.data.data();
    const int c = t.channels;
    const auto at = [&](int x, int y, int k) {
        return static_cast<float>(d[(static_cast<std::size_t>(y) * t.width + x) * c + k]);
    };
    Rgba out;
    float *channel[4] = {&out.r, &out.g, &out.b, &out.a};
    for (int k = 0; k < c; ++k)
    {
        const float top = at(x0, y0, k) + (at(x1, y0, k) - at(x0, y0, k)) * tx;
        const float bottom = at(x0, y1, k) + (at(x1, y1, k) - at(x0, y1, k)) * tx;
        *channel[k] = (top + (bottom - top) * ty) * (1.0f / 255.0f);
    }
    if (c == 1)
        out.a = 1.0f;
    return out;
}

inline void blend(std::uint32_t &dst, const Rgba &src)
{
    float a = src.a;
    if (!(a > 0.0f))
        return;
    if (a > 1.0f)
        a = 1.0f;
    const float keep = 1.0f - a;
    const float dr = static_cast<float>((dst >> 16) & 0xffu);
    const float dg = static_cast<float>((dst >> 8) & 0xffu);
    const float db = static_cast<float>(dst & 0xffu);
    const auto channel = [](float v) {
        return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
    };
    dst = (channel(clamp01(src.r) * 255.0f * a + dr * keep) << 16) |
          (channel(clamp01(src.g) * 255.0f * a + dg * keep) << 8) |
          channel(clamp01(src.b) * 255.0f * a + db * keep);
}

struct Clip
{
    int x0, y0, x1, y1;
};

// One colour over n pixels: 8.8 fixed point, or a plain fill when opaque.
void blend_span(std::uint32_t *p, int n, const Rgba &c)
{
    const float a = clamp01(c.a);
    if (n <= 0 || a <= 0.0f)
        return;
    const auto byte = [](float v) { return static_cast<std::uint32_t>(clamp01(v) * 255.0f + 0.5f); };
    if (a >= 0.999f)
    {
        std::fill(p, p + n, (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b));
        return;
    }
    const std::uint32_t ia = static_cast<std::uint32_t>(a * 256.0f + 0.5f), keep = 256u - ia;
    const std::uint32_t sr = byte(c.r) * ia, sg = byte(c.g) * ia, sb = byte(c.b) * ia;
    for (int i = 0; i < n; ++i)
    {
        const std::uint32_t d = p[i];
        p[i] = (((sr + ((d >> 16) & 0xffu) * keep) >> 8) << 16) | (((sg + ((d >> 8) & 0xffu) * keep) >> 8) << 8) |
               ((sb + (d & 0xffu) * keep) >> 8);
    }
}

// An RGBA texture along one row: v fixed, u = u0 + du * i; bilinear (GL_LINEAR, clamp to edge),
// tinted by fill, blended over p.
void image_span(std::uint32_t *p, int n, const cpu::Texture &t, float u0, float du, float v, const Rgba &fill)
{
    if (t.channels != 4 || n <= 0)
        return;
    const float fy = v * static_cast<float>(t.height) - 0.5f;
    const float fly = std::floor(fy);
    const std::uint32_t wy = static_cast<std::uint32_t>((fy - fly) * 256.0f + 0.5f);
    const int ya = std::clamp(static_cast<int>(fly), 0, t.height - 1);
    const int yb = std::clamp(static_cast<int>(fly) + 1, 0, t.height - 1);
    const std::uint8_t *ra = t.data.data() + static_cast<std::size_t>(ya) * t.width * 4;
    const std::uint8_t *rb = t.data.data() + static_cast<std::size_t>(yb) * t.width * 4;
    const float tw = static_cast<float>(t.width);
    const std::uint32_t fr = static_cast<std::uint32_t>(clamp01(fill.r) * 256.0f + 0.5f);
    const std::uint32_t fg = static_cast<std::uint32_t>(clamp01(fill.g) * 256.0f + 0.5f);
    const std::uint32_t fb = static_cast<std::uint32_t>(clamp01(fill.b) * 256.0f + 0.5f);
    const std::uint32_t fa = static_cast<std::uint32_t>(clamp01(fill.a) * 256.0f + 0.5f);
    for (int i = 0; i < n; ++i)
    {
        const float fx = (u0 + du * static_cast<float>(i)) * tw - 0.5f;
        const float flx = std::floor(fx);
        const std::uint32_t wx = static_cast<std::uint32_t>((fx - flx) * 256.0f + 0.5f);
        const int xa = std::clamp(static_cast<int>(flx), 0, t.width - 1) * 4;
        const int xb = std::clamp(static_cast<int>(flx) + 1, 0, t.width - 1) * 4;
        std::uint32_t texel[4];
        for (int k = 0; k < 4; ++k)
        {
            const std::uint32_t top = ra[xa + k] * (256u - wx) + ra[xb + k] * wx;
            const std::uint32_t bottom = rb[xa + k] * (256u - wx) + rb[xb + k] * wx;
            texel[k] = (top * (256u - wy) + bottom * wy) >> 16; // 0..255
        }
        const std::uint32_t a = (texel[3] * fa) >> 8; // 0..255
        if (a == 0)
            continue;
        const std::uint32_t r = (texel[0] * fr) >> 8, g = (texel[1] * fg) >> 8, b = (texel[2] * fb) >> 8;
        if (a >= 255)
        {
            p[i] = (r << 16) | (g << 8) | b;
            continue;
        }
        const std::uint32_t ia = a + (a >> 7), keep = 256u - ia, d = p[i];
        p[i] = (((r * ia + ((d >> 16) & 0xffu) * keep) >> 8) << 16) |
               (((g * ia + ((d >> 8) & 0xffu) * keep) >> 8) << 8) | ((b * ia + (d & 0xffu) * keep) >> 8);
    }
}

#ifdef HUI_CPU_STATS
} // namespace
// Per shape: instances, pixels evaluated, nanoseconds. Read by tools/preview_host.
std::uint64_t hui_cpu_stats[10][3];
namespace
{
#endif

// One instance: the vertex shader's quad, then the fragment shader per pixel inside it.
void draw_instance_body(const Instance &in, const cpu::Texture *run_texture, const cpu::Texture *const *fonts,
                        const Viewport &vp, const cpu::Surface &s, const Clip &clip);

void draw_instance(const Instance &in, const cpu::Texture *run_texture, const cpu::Texture *const *fonts,
                   const Viewport &vp, const cpu::Surface &s, const Clip &clip)
{
#ifdef HUI_CPU_STATS
    const auto started = std::chrono::steady_clock::now();
    draw_instance_body(in, run_texture, fonts, vp, s, clip);
    const int shape = std::min(9, static_cast<int>(in.params[3] + 0.5f));
    hui_cpu_stats[shape][0] += 1;
    hui_cpu_stats[shape][1] += static_cast<std::uint64_t>(std::max(0.0f, in.rect[2] * vp.scale) * std::max(0.0f, in.rect[3] * vp.scale));
    hui_cpu_stats[shape][2] += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count());
#else
    draw_instance_body(in, run_texture, fonts, vp, s, clip);
#endif
}

void draw_instance_body(const Instance &in, const cpu::Texture *run_texture, const cpu::Texture *const *fonts,
                        const Viewport &vp, const cpu::Surface &s, const Clip &clip)
{
    const int shape = static_cast<int>(in.params[3] + 0.5f);
    const float w = in.rect[2], h = in.rect[3];
    const float scale = vp.scale;
    const float px = 1.0f / scale;
    const float pad = (shape == 1 || shape == 3) ? 0.0f : in.params[2] + px;
    const bool box = shape == 0 || shape == 2 || shape == 5;
    const float angle = box ? in.extra[0] : 0.0f;
    const float hw = 0.5f * w, hh = 0.5f * h;
    const float cx = in.rect[0] + hw, cy = in.rect[1] + hh;
    const float qx = hw + pad, qy = hh + pad; // the quad's half size
    float cs = 1.0f, sn = 0.0f, ex = qx, ey = qy;
    if (angle != 0.0f)
    {
        cs = std::cos(angle);
        sn = std::sin(angle);
        ex = std::fabs(cs) * qx + std::fabs(sn) * qy;
        ey = std::fabs(sn) * qx + std::fabs(cs) * qy;
    }
    int x0 = static_cast<int>(std::floor((cx - ex) * scale + vp.offset_x));
    int x1 = static_cast<int>(std::ceil((cx + ex) * scale + vp.offset_x));
    int y0 = static_cast<int>(std::floor((cy - ey) * scale + vp.offset_y));
    int y1 = static_cast<int>(std::ceil((cy + ey) * scale + vp.offset_y));
    x0 = std::max(x0, clip.x0);
    y0 = std::max(y0, clip.y0);
    x1 = std::min(x1, clip.x1);
    y1 = std::min(y1, clip.y1);
    if (x0 >= x1 || y0 >= y1)
        return;

    const float radius = std::min(in.params[0], std::min(hw, hh));
    const float qw = 2.0f * qx, qh = 2.0f * qy;
    const bool horizontal = shape == 0 && in.extra[1] > 0.5f;
    const cpu::Texture *glyph_texture = run_texture;
    if (shape == 1)
    {
        const int slot = static_cast<int>(in.params[2] + 0.5f);
        if (slot >= 1 && slot <= static_cast<int>(kFontSlots))
            glyph_texture = fonts[slot - 1];
    }
    if ((shape == 1 || shape == 3) && glyph_texture == nullptr)
        return;
    // Arc constants.
    const float rb = 0.5f * in.params[1];
    const float ra = std::min(hw, hh) - rb;
    const float half_sweep = 0.5f * in.extra[0];
    const float mid = in.params[0] + half_sweep;
    const float cm = std::cos(mid), sm = std::sin(mid);
    const float scx = std::sin(half_sweep), scy = std::cos(half_sweep);

    // The solid core: inside it every pixel has coverage 1 (and no border), so a row's core is one
    // colour (shapes 0 and 2, unrotated, vertical gradient) or a plain texture lookup (3). margin is
    // how far inside the edge that starts; the corner radius bounds the rows that have a core.
    float margin = -1.0f, corner = radius;
    if (angle == 0.0f)
    {
        if (shape == 0 && !horizontal)
            margin = std::max(in.params[1], 0.0f) + 0.5f * px;
        else if (shape == 2 && in.params[2] > 0.0f)
            margin = in.params[2];
        else if (shape == 3)
        {
            margin = in.params[0] > 0.0f ? 0.5f * px : 0.0f;
            corner = in.params[0] > 0.0f ? radius : 0.0f;
        }
    }

    for (int y = y0; y < y1; ++y)
    {
        std::uint32_t *row = s.pixels + static_cast<std::size_t>(y) * s.stride;
        const float vy = ((static_cast<float>(y) + 0.5f) - vp.offset_y) / scale - cy;
        const auto shade = [&](int x) {
            const float vx = ((static_cast<float>(x) + 0.5f) - vp.offset_x) / scale - cx;
            // Into the quad's unrotated frame (centred), as v_local.
            const float lx = angle != 0.0f ? cs * vx + sn * vy : vx;
            const float ly = angle != 0.0f ? -sn * vx + cs * vy : vy;
            if (std::fabs(lx) > qx || std::fabs(ly) > qy)
                return;
            const float t = horizontal ? clamp01((lx + hw) / std::max(w, 1e-3f))
                                       : clamp01((ly + hh) / std::max(h, 1e-3f));
            const Rgba fill = mix4(in.color_top, in.color_bottom, t);
            Rgba color;
            if (shape == 0 || shape == 8)
            {
                float d;
                if (shape == 0)
                    d = round_box(lx, ly, hw, hh, radius);
                else
                {
                    const float cut = std::min(in.params[0], std::min(hw, hh));
                    const float ax = std::fabs(lx) - hw, ay = std::fabs(ly) - hh;
                    const float ox = std::max(ax, 0.0f), oy = std::max(ay, 0.0f);
                    const float boxd = std::sqrt(ox * ox + oy * oy) + std::min(std::max(ax, ay), 0.0f);
                    d = std::max(boxd, (ax + ay + cut) * 0.70710678f);
                }
                color = fill;
                if (in.params[1] > 0.0f)
                {
                    const float inner = clamp01(0.5f - (d + in.params[1]) / px);
                    const Rgba border{in.border_color[0], in.border_color[1], in.border_color[2],
                                      in.border_color[3]};
                    color = mixc(border, fill, inner);
                }
                color.a *= clamp01(0.5f - d / px);
            }
            else if (shape == 1 || shape == 3)
            {
                const float u = in.extra[0] + (in.extra[2] - in.extra[0]) * ((lx + qx) / qw);
                const float v = in.extra[1] + (in.extra[3] - in.extra[1]) * ((ly + qy) / qh);
                if (shape == 1)
                {
                    const float sdf = sample(*glyph_texture, u, v).r;
                    const float distance = (sdf - 0.5f) * 2.0f * in.params[0];
                    color = {fill.r, fill.g, fill.b, fill.a * clamp01(distance / px + 0.5f)};
                }
                else
                {
                    const Rgba texel = sample(*glyph_texture, u, v);
                    color = {texel.r * fill.r, texel.g * fill.g, texel.b * fill.b, texel.a * fill.a};
                    if (in.params[0] > 0.0f)
                        color.a *= clamp01(0.5f - round_box(lx, ly, hw, hh, radius) / px);
                }
            }
            else if (shape == 2)
            {
                const float d = round_box(lx, ly, hw, hh, radius);
                color = {fill.r, fill.g, fill.b,
                         fill.a * (1.0f - smoothstep(-in.params[2], in.params[2], d))};
            }
            else if (shape == 4)
            {
                const float pax = vx + cx - in.extra[0], pay = vy + cy - in.extra[1];
                const float bax = in.extra[2] - in.extra[0], bay = in.extra[3] - in.extra[1];
                const float hh2 = clamp01((pax * bax + pay * bay) / std::max(bax * bax + bay * bay, 1e-6f));
                const float dx = pax - bax * hh2, dy = pay - bay * hh2;
                const float d = std::sqrt(dx * dx + dy * dy) - 0.5f * in.params[1];
                color = {fill.r, fill.g, fill.b, fill.a * clamp01(0.5f - d / px)};
            }
            else if (shape == 7)
            {
                float apx = lx, apy = -ly;
                const float rx = std::fabs(apx * cm - apy * sm);
                const float ry = apx * sm + apy * cm;
                apx = rx;
                apy = ry;
                const float ring = std::fabs(std::sqrt(apx * apx + apy * apy) - ra);
                float d;
                if (in.extra[1] > 0.5f)
                    d = std::max(ring - rb, apx * scy - apy * scx);
                else
                {
                    const float ex2 = apx - scx * ra, ey2 = apy - scy * ra;
                    d = ((scy * apx > scx * apy) ? std::sqrt(ex2 * ex2 + ey2 * ey2) : ring) - rb;
                }
                color = {fill.r, fill.g, fill.b, fill.a * clamp01(0.5f - d / px)};
            }
            else if (shape == 6)
            {
                float d = star5(lx, -ly, std::min(hw, hh), 0.45f);
                if (in.params[1] > 0.0f)
                    d = std::fabs(d + 0.5f * in.params[1]) - 0.5f * in.params[1];
                color = {fill.r, fill.g, fill.b, fill.a * clamp01(0.5f - d / px)};
            }
            else
            {
                float d = triangle_sdf(lx, ly, 0.0f, -hh, -hw, hh, hw, hh);
                if (in.params[1] > 0.0f)
                    d = std::fabs(d + 0.5f * in.params[1]) - 0.5f * in.params[1];
                color = {fill.r, fill.g, fill.b, fill.a * clamp01(0.5f - d / px)};
            }
            blend(row[x], color);
        };
        int sx0 = x1, sx1 = x1;
        if (margin >= 0.0f && std::fabs(vy) <= hh - std::max(margin, corner) && std::fabs(vy) <= qy)
        {
            const float inner = hw - margin;
            if (inner > 0.0f)
            {
                sx0 = std::max(x0, static_cast<int>(std::ceil((cx - inner) * scale + vp.offset_x - 0.5f)));
                sx1 = std::min(x1, static_cast<int>(std::floor((cx + inner) * scale + vp.offset_x - 0.5f)) + 1);
                if (sx0 >= sx1)
                    sx0 = sx1 = x1;
            }
        }
        for (int x = x0; x < sx0; ++x)
            shade(x);
        if (sx0 < sx1)
        {
            const float t = clamp01((vy + hh) / std::max(h, 1e-3f));
            const Rgba fill = mix4(in.color_top, in.color_bottom, t);
            if (shape == 3)
            {
                const float lx0 = ((static_cast<float>(sx0) + 0.5f) - vp.offset_x) / scale - cx;
                const float u0 = in.extra[0] + (in.extra[2] - in.extra[0]) * ((lx0 + qx) / qw);
                const float du = (in.extra[2] - in.extra[0]) / (qw * scale);
                const float v = in.extra[1] + (in.extra[3] - in.extra[1]) * ((vy + qy) / qh);
                image_span(row + sx0, sx1 - sx0, *glyph_texture, u0, du, v, fill);
            }
            else
                blend_span(row + sx0, sx1 - sx0, fill);
        }
        for (int x = sx1; x < x1; ++x)
            shade(x);
    }
}

// A filled triangle of a polygon mesh: per-vertex colour, pixel centres inside (no anti-aliasing,
// as GL draws it without multisampling).
void draw_triangle(const MeshVertex &a, const MeshVertex &b, const MeshVertex &c, const Viewport &vp,
                   const cpu::Surface &s, const Clip &clip)
{
    const float ax = a.x * vp.scale + vp.offset_x, ay = a.y * vp.scale + vp.offset_y;
    const float bx = b.x * vp.scale + vp.offset_x, by = b.y * vp.scale + vp.offset_y;
    const float cx = c.x * vp.scale + vp.offset_x, cy = c.y * vp.scale + vp.offset_y;
    const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (std::fabs(area) < 1e-6f)
        return;
    const int x0 = std::max(clip.x0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
    const int x1 = std::min(clip.x1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
    const int y0 = std::max(clip.y0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
    const int y1 = std::min(clip.y1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
    for (int y = y0; y < y1; ++y)
    {
        std::uint32_t *row = s.pixels + static_cast<std::size_t>(y) * s.stride;
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = x0; x < x1; ++x)
        {
            const float pxf = static_cast<float>(x) + 0.5f;
            const float w0 = ((bx - pxf) * (cy - py) - (by - py) * (cx - pxf)) / area;
            const float w1 = ((cx - pxf) * (ay - py) - (cy - py) * (ax - pxf)) / area;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                continue;
            const Rgba color{a.r * w0 + b.r * w1 + c.r * w2, a.g * w0 + b.g * w1 + c.g * w2,
                             a.b * w0 + b.b * w1 + c.b * w2, a.a * w0 + b.a * w1 + c.a * w2};
            blend(row[x], color);
        }
    }
}

} // namespace

namespace
{

// ---- worker threads: the frame in horizontal bands ----------------------------------------------

class Pool
{
  public:
    // Lives as long as the process: its workers wait on it and are never joined.
    static Pool &get()
    {
        static Pool *pool = new Pool;
        return *pool;
    }

    void set_threads(int count)
    {
        count = std::clamp(count, 1, 16);
        std::lock_guard<std::mutex> lock(mutex_);
        wanted_ = count;
    }

    int threads()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return wanted_;
    }

    // job(band, bands) for every band; the caller runs band 0. Returns when all have finished.
    void run(int bands, const std::function<void(int, int)> &job)
    {
        if (bands <= 1)
        {
            job(0, 1);
            return;
        }
        {
            std::unique_lock<std::mutex> lock(mutex_);
            while (static_cast<int>(workers_.size()) < bands - 1)
            {
                const int index = static_cast<int>(workers_.size()) + 1;
                workers_.emplace_back([this, index] { work(index); });
            }
            job_ = &job;
            bands_ = bands;
            pending_ = bands - 1;
            ++generation_;
        }
        start_.notify_all();
        job(0, bands);
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return pending_ == 0; });
        job_ = nullptr;
    }

  private:
    void work(int index)
    {
        std::uint64_t seen = 0;
        for (;;)
        {
            const std::function<void(int, int)> *job;
            int bands;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                start_.wait(lock, [&] { return generation_ != seen; });
                seen = generation_;
                job = job_;
                bands = bands_;
            }
            if (job != nullptr && index < bands)
                (*job)(index, bands);
            std::lock_guard<std::mutex> lock(mutex_);
            if (index < bands && --pending_ == 0)
                done_.notify_one();
        }
    }

    std::mutex mutex_;
    std::condition_variable start_, done_;
    std::vector<std::thread> workers_;
    const std::function<void(int, int)> *job_ = nullptr;
    int bands_ = 0, pending_ = 0, wanted_ = 1;
    std::uint64_t generation_ = 0;
};

inline void band_rows(int height, int band, int bands, int *y0, int *y1)
{
    *y0 = static_cast<int>(static_cast<long long>(height) * band / bands);
    *y1 = static_cast<int>(static_cast<long long>(height) * (band + 1) / bands);
}

} // namespace

namespace cpu
{
void set_threads(int count)
{
    Pool::get().set_threads(count);
}

void parallel_rows(int height, void (*job)(void *context, int row0, int row1), void *context)
{
    Pool &pool = Pool::get();
    const int bands = std::min(pool.threads(), std::max(1, height / 16));
    pool.run(bands, [&](int band, int count) {
        int row0, row1;
        band_rows(height, band, count, &row0, &row1);
        job(context, row0, row1);
    });
}
} // namespace cpu

std::uint32_t GlBatch::create_font_texture(const Font &font)
{
    const std::uint32_t texture =
        cpu::create_texture(font.atlas_width(), font.atlas_height(), 1, font.atlas().data());
    if (texture != 0 && font_count_ < kFontSlots)
    {
        font_textures_[font_count_] = texture;
        ++font_count_;
        return kFontHandleBase | font_count_;
    }
    return texture;
}

std::uint32_t GlBatch::create_texture(int width, int height, const std::uint8_t *rgba)
{
    return cpu::create_texture(width, height, 4, rgba);
}

void GlBatch::draw(const DrawList &list, const Viewport &viewport, int surface_width, int surface_height)
{
    cpu::Surface s = target_;
    s.width = std::min(surface_width, s.width);
    s.height = std::min(surface_height, s.height);
    draw_rows(list, viewport, s, 0, s.height);
    draw_calls_ = list.runs().size();
}

void GlBatch::draw_rows(const DrawList &list, const Viewport &viewport, const cpu::Surface &s, int row0,
                        int row1) const
{
    if (s.pixels == nullptr)
        return;
    // The font slots as the GL backend binds them: unit k holds font k, or the last one.
    const cpu::Texture *fonts[kFontSlots] = {};
    for (std::uint32_t slot = 0; slot < kFontSlots; ++slot)
        fonts[slot] = font_count_ > 0 ? cpu::find_texture(font_textures_[std::min(slot, font_count_ - 1)])
                                      : nullptr;
    const auto &instances = list.instances();
    const auto &mesh = list.mesh_vertices();
    for (const Run &run : list.runs())
    {
        if (run.count == 0)
            continue;
        Clip clip{0, std::max(0, row0), s.width, std::min(s.height, row1)};
        if (run.clipped)
        {
            clip.x0 = std::max(clip.x0, static_cast<int>(std::floor(run.clip.x * viewport.scale + viewport.offset_x)));
            clip.y0 = std::max(clip.y0, static_cast<int>(std::floor(run.clip.y * viewport.scale + viewport.offset_y)));
            clip.x1 = std::min(clip.x1, static_cast<int>(std::ceil((run.clip.x + run.clip.w) * viewport.scale +
                                                                   viewport.offset_x)));
            clip.y1 = std::min(clip.y1, static_cast<int>(std::ceil((run.clip.y + run.clip.h) * viewport.scale +
                                                                   viewport.offset_y)));
        }
        if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1)
            continue;
        if (run.mesh)
        {
            for (std::uint32_t i = run.first; i + 2 < run.first + run.count && i + 2 < mesh.size(); i += 3)
                draw_triangle(mesh[i], mesh[i + 1], mesh[i + 2], viewport, s, clip);
        }
        else
        {
            const cpu::Texture *texture = cpu::find_texture(run.texture);
            for (std::uint32_t i = run.first; i < run.first + run.count && i < instances.size(); ++i)
                draw_instance(instances[i], texture, fonts, viewport, s, clip);
        }
    }
}

void Backdrop::draw(const cpu::Surface &target, const BackdropSpec &spec, float opacity)
{
    draw_rows(target, spec, opacity, 0, target.height);
}

void Backdrop::draw_rows(const cpu::Surface &target, const BackdropSpec &spec, float opacity, int row0,
                         int row1) const
{
    if (target.pixels == nullptr || spec.mode == BackdropMode::none)
        return;
    const float aspect = static_cast<float>(target.width) / static_cast<float>(target.height);
    const bool gradient = spec.mode == BackdropMode::gradient;
    const bool highlight = gradient && spec.params[2] != 0.0f;
    for (int y = std::max(0, row0); y < std::min(target.height, row1); ++y)
    {
        std::uint32_t *row = target.pixels + static_cast<std::size_t>(y) * target.stride;
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(target.height);
        const float t = gradient ? smoothstep(0.0f, 1.0f, v) : 0.0f;
        const Color &c0 = spec.colors[0], &c1 = spec.colors[1], &c2 = spec.colors[2];
        const Rgba base{c0.r + (c1.r - c0.r) * t, c0.g + (c1.g - c0.g) * t, c0.b + (c1.b - c0.b) * t, opacity};
        if (!highlight)
        {
            blend_span(row, target.width, base);
            continue;
        }
        for (int x = 0; x < target.width; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(target.width);
            const float dx = (u - spec.params[0]) * aspect, dy = v - spec.params[1];
            const float glow = spec.params[2] * std::exp(-(dx * dx + dy * dy) * 3.0f);
            const Rgba color{base.r + c2.r * glow, base.g + c2.g * glow, base.b + c2.b * glow, opacity};
            if (opacity >= 1.0f)
                row[x] = 0;
            blend(row[x], color);
        }
    }
}

namespace
{

constexpr int kGlassWidth = 480;
constexpr int kGlassHeight = 270;

// hui's blur pass (five bilinear taps along one axis at 0, +-1.384615 and +-3.230769 steps) as the
// integer kernel it amounts to, in 16.16 weights; clamp to edge.
struct Kernel
{
    int offset[10];
    std::uint32_t weight[10];
    int taps = 0;
};

Kernel blur_kernel(float step)
{
    float weights[64] = {};
    const int centre = 32;
    const auto add = [&](float position, float weight) {
        const float fl = std::floor(position);
        const float t = position - fl;
        weights[centre + static_cast<int>(fl)] += weight * (1.0f - t);
        weights[centre + static_cast<int>(fl) + 1] += weight * t;
    };
    add(0.0f, 0.227027f);
    for (const float direction : {1.0f, -1.0f})
    {
        add(1.384615f * step * direction, 0.316216f);
        add(3.230769f * step * direction, 0.070270f);
    }
    Kernel kernel;
    for (int i = 0; i < 64 && kernel.taps < 10; ++i)
        if (weights[i] > 1e-6f)
        {
            kernel.offset[kernel.taps] = i - centre;
            kernel.weight[kernel.taps] = static_cast<std::uint32_t>(weights[i] * 65536.0f + 0.5f);
            ++kernel.taps;
        }
    return kernel;
}

void blur_rows(const std::uint32_t *source, std::uint32_t *target, bool horizontal, const Kernel &kernel, int row0,
               int row1)
{
    const int width = kGlassWidth, height = kGlassHeight;
    for (int y = row0; y < row1; ++y)
        for (int x = 0; x < width; ++x)
        {
            std::uint32_t r = 0, g = 0, b = 0;
            for (int k = 0; k < kernel.taps; ++k)
            {
                const int sx = horizontal ? std::clamp(x + kernel.offset[k], 0, width - 1) : x;
                const int sy = horizontal ? y : std::clamp(y + kernel.offset[k], 0, height - 1);
                const std::uint32_t p = source[static_cast<std::size_t>(sy) * width + sx];
                r += ((p >> 16) & 0xffu) * kernel.weight[k];
                g += ((p >> 8) & 0xffu) * kernel.weight[k];
                b += (p & 0xffu) * kernel.weight[k];
            }
            const auto channel = [](std::uint32_t v) { return std::min<std::uint32_t>((v + 32768u) >> 16, 255u); };
            target[static_cast<std::size_t>(y) * width + x] = (channel(r) << 16) | (channel(g) << 8) | channel(b);
        }
}

// FNV-1a over a frame's layers: an unchanged frame is not painted again.
struct Hash
{
    std::uint64_t value = 1469598103934665603ull;
    void bytes(const void *data, std::size_t size)
    {
        const auto *p = static_cast<const unsigned char *>(data);
        for (std::size_t i = 0; i < size; ++i)
            value = (value ^ p[i]) * 1099511628211ull;
    }
    template <typename T>
    void add(const T &v)
    {
        bytes(&v, sizeof(v));
    }
};

} // namespace

bool Renderer::init()
{
    glass_pixels_.assign(static_cast<std::size_t>(kGlassWidth) * kGlassHeight, 0);
    glass_scratch_.assign(glass_pixels_.size(), 0);
    glass_texture_ = cpu::create_texture(kGlassWidth, kGlassHeight, 4, nullptr);
    return glass_texture_ != 0;
}

void Renderer::begin()
{
    layers_.clear();
}

void Renderer::backdrop(const BackdropSpec &spec, float opacity)
{
    if (spec.mode == BackdropMode::none || opacity <= 0.0f)
        return;
    Layer layer;
    layer.kind = Kind::backdrop;
    layer.spec = spec;
    layer.opacity = opacity;
    layers_.push_back(layer);
}

void Renderer::draw(const DrawList &list)
{
    if (list.empty())
        return;
    Layer layer;
    layer.kind = Kind::list;
    layer.list = &list;
    layers_.push_back(layer);
}

void Renderer::glass()
{
    Layer layer;
    layer.kind = Kind::glass;
    layers_.push_back(layer);
}

void Renderer::play(const Layer &layer, const cpu::Surface &target, float virtual_width, float virtual_height,
                    int row0, int row1)
{
    if (layer.kind == Kind::backdrop)
        backdrop_.draw_rows(target, layer.spec, layer.opacity, row0, row1);
    else if (layer.kind == Kind::list)
        batch_.draw_rows(*layer.list, fit_viewport(target.width, target.height, virtual_width, virtual_height),
                         target, row0, row1);
}

// Layers [first, last) over target, in bands on the worker threads.
void Renderer::play_range(std::size_t first, std::size_t last, const cpu::Surface &target, float virtual_width,
                          float virtual_height)
{
    if (first >= last)
        return;
    Pool &pool = Pool::get();
    const int bands = std::min(pool.threads(), std::max(1, target.height / 32));
    pool.run(bands, [&](int band, int count) {
        int row0, row1;
        band_rows(target.height, band, count, &row0, &row1);
        for (std::size_t i = first; i < last; ++i)
            play(layers_[i], target, virtual_width, virtual_height, row0, row1);
    });
    for (std::size_t i = first; i < last; ++i)
        if (layers_[i].kind == Kind::list)
            draw_calls_ += layers_[i].list->runs().size();
        else if (layers_[i].kind == Kind::backdrop)
            ++draw_calls_;
}

void Renderer::capture_glass(std::size_t layers, float virtual_width, float virtual_height)
{
    std::fill(glass_pixels_.begin(), glass_pixels_.end(), 0u);
    const cpu::Surface small{glass_pixels_.data(), kGlassWidth, kGlassHeight, kGlassWidth};
    play_range(0, layers, small, virtual_width, virtual_height);
    Pool &pool = Pool::get();
    const int bands = std::min(pool.threads(), kGlassHeight / 16);
    for (const float step : {1.0f, 2.2f})
    {
        const Kernel kernel = blur_kernel(step);
        for (const bool horizontal : {true, false})
        {
            const std::uint32_t *source = horizontal ? glass_pixels_.data() : glass_scratch_.data();
            std::uint32_t *target = horizontal ? glass_scratch_.data() : glass_pixels_.data();
            pool.run(bands, [&](int band, int count) {
                int row0, row1;
                band_rows(kGlassHeight, band, count, &row0, &row1);
                blur_rows(source, target, horizontal, kernel, row0, row1);
            });
            ++draw_calls_;
        }
    }
    // The glass texture is a GL framebuffer's: bottom row first (DrawList::glass flips its v).
    if (cpu::Texture *texture = cpu::find_texture(glass_texture_))
        for (int y = 0; y < kGlassHeight; ++y)
        {
            const std::uint32_t *source =
                glass_pixels_.data() + static_cast<std::size_t>(kGlassHeight - 1 - y) * kGlassWidth;
            std::uint8_t *out = texture->data.data() + static_cast<std::size_t>(y) * kGlassWidth * 4;
            for (int x = 0; x < kGlassWidth; ++x)
            {
                out[x * 4 + 0] = static_cast<std::uint8_t>(source[x] >> 16);
                out[x * 4 + 1] = static_cast<std::uint8_t>(source[x] >> 8);
                out[x * 4 + 2] = static_cast<std::uint8_t>(source[x]);
                out[x * 4 + 3] = 255;
            }
        }
}

void Renderer::present(GLuint, int width, int height, float virtual_width, float virtual_height)
{
    draw_calls_ = 0;
    instances_ = 0;
    const cpu::Surface &surface = cpu::present_surface();
    if (surface.pixels == nullptr)
        return;
    const auto started = std::chrono::steady_clock::now();
    cpu::Surface target = surface;
    target.width = std::min(width, surface.width);
    target.height = std::min(height, surface.height);

    // The same layers over the same surface paint the same pixels: keep the last frame.
    Hash hash;
    hash.add(target.pixels);
    hash.add(target.width);
    hash.add(target.height);
    hash.add(virtual_width);
    hash.add(virtual_height);
    for (const Layer &layer : layers_)
    {
        hash.add(layer.kind);
        if (layer.kind == Kind::backdrop)
        {
            BackdropSpec spec = layer.spec;
            if (spec.mode == BackdropMode::gradient)
                spec.time = 0.0f; // this renderer's gradient has no moving noise
            hash.add(spec.mode);
            hash.bytes(spec.colors, sizeof(spec.colors));
            hash.bytes(spec.params, sizeof(spec.params));
            hash.add(spec.time);
            hash.add(layer.opacity);
        }
        else if (layer.kind == Kind::list)
        {
            const auto &instances = layer.list->instances();
            const auto &mesh = layer.list->mesh_vertices();
            hash.bytes(instances.data(), instances.size() * sizeof(Instance));
            hash.bytes(mesh.data(), mesh.size() * sizeof(MeshVertex));
            for (const Run &run : layer.list->runs())
            {
                hash.add(run.first);
                hash.add(run.count);
                hash.add(run.texture);
                hash.add(run.clipped);
                hash.add(run.mesh);
                hash.bytes(&run.clip, sizeof(run.clip));
            }
            instances_ += instances.size();
        }
    }
    if (hash.value == last_hash_)
    {
        cpu::note_present(0, true);
        return;
    }

    std::size_t first = 0;
    for (std::size_t i = 0; i <= layers_.size(); ++i)
    {
        if (i < layers_.size() && layers_[i].kind != Kind::glass)
            continue;
        play_range(first, i, target, virtual_width, virtual_height);
        if (i < layers_.size())
            capture_glass(i, virtual_width, virtual_height);
        first = i + 1;
    }
    last_hash_ = hash.value;
    cpu::note_present(static_cast<std::uint64_t>(
                          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                              .count()),
                      false);
}

} // namespace hui::gfx

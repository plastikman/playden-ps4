// Playden PS4 port - the CPU renderer's target and texture store.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <vector>

namespace hui::gfx::cpu
{

// 0x00RRGGBB pixels, the PS4 VideoOut's BGRX bytes; stride in pixels.
struct Surface
{
    std::uint32_t *pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
};

// A texture as GL would hold it: rows from the first uploaded one, 1 (R8) or 4 (RGBA8) channels,
// straight alpha.
struct Texture
{
    int width = 0;
    int height = 0;
    int channels = 4;
    std::vector<std::uint8_t> data;
};

// The surface Renderer::present paints: Playden's Render owns its Renderer, so the host names the
// target here before each draw. And how long the last present took, in nanoseconds.
void set_present_surface(const Surface &surface);
const Surface &present_surface();
std::uint64_t last_present_ns();
// Whether the last present kept the previous frame (its layers had not changed).
bool last_present_skipped();
void note_present(std::uint64_t ns, bool skipped);
// Worker threads the renderer paints with (bands of rows); 1 paints on the caller only.
void set_threads(int count);
// job(row0, row1) over [0, height) in bands on those threads; returns when all are done.
void parallel_rows(int height, void (*job)(void *context, int row0, int row1), void *context);

std::uint32_t create_texture(int width, int height, int channels, const std::uint8_t *bytes);
Texture *find_texture(std::uint32_t name);
std::uint64_t texture_bytes();

} // namespace hui::gfx::cpu

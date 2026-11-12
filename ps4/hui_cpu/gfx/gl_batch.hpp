// Playden PS4 port - Hui's draw-list batch, drawn by the CPU.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Same public interface as hui's OpenGL GlBatch; every shape is the fragment shader of
// gl_batch.cpp evaluated per pixel, with straight-alpha "over" blending.
#pragma once
#include "gfx/draw_list.hpp"
#include "gfx/font.hpp"
#include "gfx/gl.hpp"
#include "gfx/surface.hpp"
#include <cstddef>
#include <cstdint>

namespace hui::gfx
{

class GlBatch
{
  public:
    GlBatch() = default;
    GlBatch(const GlBatch &) = delete;
    GlBatch &operator=(const GlBatch &) = delete;
    bool init() { return true; }
    void release() {}
    std::uint32_t create_font_texture(const Font &font);
    std::uint32_t create_texture(int width, int height, const std::uint8_t *rgba);
    // Draws into target() of the given size.
    void draw(const DrawList &list, const Viewport &viewport, int surface_width, int surface_height);
    // CPU: rows [row0, row1) of surface only; safe to call from several threads at once.
    void draw_rows(const DrawList &list, const Viewport &viewport, const cpu::Surface &surface, int row0,
                   int row1) const;
    std::size_t last_draw_calls() const { return draw_calls_; }

    // CPU: where draw() paints.
    void set_target(const cpu::Surface &surface) { target_ = surface; }
    const cpu::Surface &target() const { return target_; }

  private:
    cpu::Surface target_;
    std::size_t draw_calls_ = 0;
    std::uint32_t font_textures_[kFontSlots] = {};
    std::uint32_t font_count_ = 0;
};

} // namespace hui::gfx

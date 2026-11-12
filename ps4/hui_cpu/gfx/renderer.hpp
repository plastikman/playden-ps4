// Playden PS4 port - Hui's frame composition (backdrops, draw lists, frosted glass) on the CPU.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Same public interface as hui's OpenGL Renderer. present() paints into cpu::present_surface(),
// which the host sets; the framebuffer name is ignored. glass() replays the layers queued so far into
// a 480x270 copy and blurs it twice, as hui's renderer does, into glass_texture().
#pragma once
#include "gfx/backdrop.hpp"
#include "gfx/draw_list.hpp"
#include "gfx/gl_batch.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hui::gfx
{

class Renderer
{
  public:
    Renderer() = default;
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    bool init();
    void release() {}
    GlBatch &batch() { return batch_; }
    Backdrop &backdrops() { return backdrop_; }
    std::uint32_t glass_texture() const { return glass_texture_; }

    void begin();
    void backdrop(const BackdropSpec &spec, float opacity = 1.0f);
    void draw(const DrawList &list);
    void glass();
    void present(GLuint framebuffer, int width, int height, float virtual_width = kVirtualWidth,
                 float virtual_height = kVirtualHeight);
    std::size_t last_draw_calls() const { return draw_calls_; }
    std::size_t last_instances() const { return instances_; }

  private:
    enum class Kind : std::uint8_t { backdrop, list, glass };
    struct Layer
    {
        Kind kind = Kind::list;
        BackdropSpec spec;
        float opacity = 1.0f;
        const DrawList *list = nullptr;
    };
    void play(const Layer &layer, const cpu::Surface &target, float virtual_width, float virtual_height, int row0,
              int row1);
    void play_range(std::size_t first, std::size_t last, const cpu::Surface &target, float virtual_width,
                    float virtual_height);
    void capture_glass(std::size_t layers, float virtual_width, float virtual_height);

    GlBatch batch_;
    Backdrop backdrop_;
    std::vector<std::uint32_t> glass_pixels_;
    std::vector<std::uint32_t> glass_scratch_;
    std::uint32_t glass_texture_ = 0;
    std::vector<Layer> layers_;
    std::size_t draw_calls_ = 0;
    std::size_t instances_ = 0;
    std::uint64_t last_hash_ = 0;
};

} // namespace hui::gfx

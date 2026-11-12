// Playden PS4 port - Hui's backdrops, drawn by the CPU.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gfx/backdrop_spec.hpp"
#include "gfx/surface.hpp"

namespace hui::gfx
{

// The gradient mode (Playden's theme) as hui's shader paints it; other modes paint their first
// colour.
class Backdrop
{
  public:
    Backdrop() = default;
    Backdrop(const Backdrop &) = delete;
    Backdrop &operator=(const Backdrop &) = delete;
    bool init() { return true; }
    void release() {}
    void draw(const cpu::Surface &target, const BackdropSpec &spec, float opacity = 1.0f);
    void draw_rows(const cpu::Surface &target, const BackdropSpec &spec, float opacity, int row0, int row1) const;
};

} // namespace hui::gfx

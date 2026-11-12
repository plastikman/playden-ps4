// Playden PS4 port - Playden's own UI (src/homebrew) on the console, drawn by the CPU renderer.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A measurement build, not the launcher: with /data/playden-ui-preview (FTP can create it; the title's
// own folder is private to it) or /data/PLDN10001/ui-preview present, main.c hands the
// display to this loop instead of the native shell. It runs the UI on its simulation fixtures (no
// Steam, no installs), drives it with the controller, and logs how long frames take and how much
// memory the UI holds. The file may say "WIDTHxHEIGHT THREADS" (default 1280x720 6).

#include "gfx/surface.hpp"
#include "homebrew/render.hpp"
#include "homebrew_live.hpp"
#include "homebrew/simulation.hpp"
#include "platform_ps4/game_maintenance.hpp"
#include "platform_ps4/pad.hpp"
#include "playden_heap.h"
#include "runtime/game_settings.h"

#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>
#include <dirent.h>
#include <system_error>
#include <filesystem>
#include <cerrno>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

extern "C" void playden_ps4_log_text(const char *line);

namespace
{

constexpr const char *kMarkers[] = {"/data/playden-ui-preview", "/data/PLDN10001/ui-preview"};
constexpr const char *kAssets = "/app0/homebrew/assets";
constexpr const char *kFixtures = "/app0/homebrew/fixtures";
constexpr const char *kProfile = "/data/PLDN10001/ui-preview-profile.json";

void logf(const char *format, ...) __attribute__((format(printf, 1, 2)));
void logf(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    playden_ps4_log_text(line);
}

std::uint64_t now_us()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000u;
}

const char *action_name(hui::Action action)
{
    switch (action)
    {
    case hui::Action::confirm: return "cross";
    case hui::Action::back: return "circle";
    case hui::Action::north: return "triangle";
    case hui::Action::west: return "square";
    case hui::Action::page_prev: return "l1";
    case hui::Action::page_next: return "r1";
    case hui::Action::jump_prev: return "l2";
    case hui::Action::jump_next: return "r2";
    case hui::Action::menu: return "options";
    case hui::Action::touch: return "search";
    default: return nullptr;
    }
}

// As Playden's desktop host feeds its controller to the simulation.
void apply_input(playden::homebrew::Simulation &simulation, const hui::InputFrame &frame)
{
    switch (frame.nav)
    {
    case hui::Direction::up: simulation.action("up", frame.nav_repeat); break;
    case hui::Direction::down: simulation.action("down", frame.nav_repeat); break;
    case hui::Direction::left: simulation.action("left", frame.nav_repeat); break;
    case hui::Direction::right: simulation.action("right", frame.nav_repeat); break;
    case hui::Direction::none: break;
    }
    for (unsigned index = static_cast<unsigned>(hui::Action::confirm); index < static_cast<unsigned>(hui::Action::count);
         ++index)
    {
        const auto action = static_cast<hui::Action>(index);
        if (frame.is_pressed(action))
            if (const char *name = action_name(action))
            {
                simulation.action(name);
                break;
            }
    }
}

// The rendered frame (width x height) into a 1920x1080 VideoOut buffer: bilinear in 8.8 fixed point,
// the column taps computed once, rows split over the renderer's threads.
struct Scaler
{
    const std::uint32_t *source = nullptr;
    std::uint32_t *frame = nullptr;
    int width = 0, height = 0, frame_width = 0, frame_height = 0;
    std::vector<int> x0, x1;
    std::vector<std::uint32_t> wx;

    void prepare(int w, int h, int fw, int fh)
    {
        width = w;
        height = h;
        frame_width = fw;
        frame_height = fh;
        x0.resize(fw);
        x1.resize(fw);
        wx.resize(fw);
        for (int x = 0; x < fw; ++x)
        {
            const float fx = (static_cast<float>(x) + 0.5f) * w / fw - 0.5f;
            const int a = std::clamp(static_cast<int>(std::floor(fx)), 0, w - 1);
            x0[x] = a;
            x1[x] = std::min(a + 1, w - 1);
            wx[x] = static_cast<std::uint32_t>(std::clamp(fx - static_cast<float>(a), 0.0f, 1.0f) * 256.0f);
        }
    }

    static void rows(void *context, int row0, int row1)
    {
        const Scaler &s = *static_cast<const Scaler *>(context);
        for (int y = row0; y < row1; ++y)
        {
            const float fy = (static_cast<float>(y) + 0.5f) * s.height / s.frame_height - 0.5f;
            const int ya = std::clamp(static_cast<int>(std::floor(fy)), 0, s.height - 1);
            const int yb = std::min(ya + 1, s.height - 1);
            const std::uint32_t wy = static_cast<std::uint32_t>(std::clamp(fy - static_cast<float>(ya), 0.0f, 1.0f) * 256.0f);
            const std::uint32_t *a = s.source + static_cast<std::size_t>(ya) * s.width;
            const std::uint32_t *b = s.source + static_cast<std::size_t>(yb) * s.width;
            std::uint32_t *out = s.frame + static_cast<std::size_t>(y) * s.frame_width;
            for (int x = 0; x < s.frame_width; ++x)
            {
                const std::uint32_t w1 = s.wx[x], w0 = 256u - w1;
                const std::uint32_t p00 = a[s.x0[x]], p01 = a[s.x1[x]], p10 = b[s.x0[x]], p11 = b[s.x1[x]];
                // Red and blue together, then green: two lanes of 8.8 per 32-bit word.
                const std::uint32_t rb_top = ((p00 & 0xff00ffu) * w0 + (p01 & 0xff00ffu) * w1) >> 8 & 0xff00ffu;
                const std::uint32_t rb_bottom = ((p10 & 0xff00ffu) * w0 + (p11 & 0xff00ffu) * w1) >> 8 & 0xff00ffu;
                const std::uint32_t g_top = ((p00 & 0xff00u) * w0 + (p01 & 0xff00u) * w1) >> 8 & 0xff00u;
                const std::uint32_t g_bottom = ((p10 & 0xff00u) * w0 + (p11 & 0xff00u) * w1) >> 8 & 0xff00u;
                const std::uint32_t rb = (rb_top * (256u - wy) + rb_bottom * wy) >> 8 & 0xff00ffu;
                const std::uint32_t g = (g_top * (256u - wy) + g_bottom * wy) >> 8 & 0xff00u;
                out[x] = rb | g;
            }
        }
    }

    void run(const std::uint32_t *src, std::uint32_t *dst)
    {
        source = src;
        frame = dst;
        if (width == frame_width && height == frame_height)
        {
            std::memcpy(dst, src, static_cast<std::size_t>(width) * height * 4);
            return;
        }
        hui::gfx::cpu::parallel_rows(frame_height, &Scaler::rows, this);
    }
};

} // namespace

static FILE *open_marker()
{
    for (const char *path : kMarkers)
        if (FILE *marker = std::fopen(path, "rb"))
            return marker;
    return nullptr;
}

extern "C" int playden_ps4_preview_wanted(void)
{
    FILE *marker = open_marker();
    if (marker == nullptr)
        return 0;
    std::fclose(marker);
    return 1;
}

// Runs until the title is closed. buffers: the registered VideoOut buffers, frame_width x frame_height.
extern "C" void playden_ps4_preview_run(int video, void **buffers, int buffer_count, int frame_width, int frame_height)
{
    int width = 1280, height = 720, threads = 6;
    bool live = false;
    if (FILE *marker = open_marker())
    {
        int w = 0, h = 0, t = 0;
        char mode[16] = {};
        const int read = std::fscanf(marker, "%dx%d %d %15s", &w, &h, &t, mode);
        live = read == 4 && std::strcmp(mode, "live") == 0;
        if (read >= 2 && w >= 320 && h >= 180 && w <= frame_width && h <= frame_height)
        {
            width = w;
            height = h;
        }
        if (read >= 3 && t >= 1 && t <= 16)
            threads = t;
        std::fclose(marker);
    }
    hui::gfx::cpu::set_threads(threads);
    logf("PLDN10001 preview: Playden UI on the CPU renderer, %dx%d, %d threads, %s", width, height, threads,
         live ? "live services" : "fixtures /app0/homebrew/fixtures");

    // Live: the desktop host's live mode with the PS4's paths (ps4/homebrew_live.cpp), no runtime kit yet.
    playden::homebrew::Simulation simulation(live ? std::filesystem::path{} : std::filesystem::path(kFixtures),
                                             live ? std::filesystem::path{} : std::filesystem::path(kProfile),
                                             live ? playden::homebrew::ModelMode::Live
                                                  : playden::homebrew::ModelMode::Simulation);
    {
        std::vector<playden::runtime::SettingsPreset> presets;
        std::vector<playden::runtime::GameRecipe> recipes;
        std::string reason;
        playden::runtime::KitCapabilities kit;
        kit.dxvk_config = !live;  // as the desktop host: live mode has no runtime kit yet
        simulation.SetKitCapabilities(kit);
        if (playden::runtime::LoadSettingsData(std::string(kAssets) + "/runtime", &presets, &recipes, &reason))
            simulation.SetSettingsData(std::move(presets), std::move(recipes));
        else
            logf("PLDN10001 preview: settings data %s", reason.c_str());
    }
    playden::ps4::LiveServices services;
    if (live && !services.Start(simulation))
        logf("PLDN10001 preview: live services failed to start");
    playden::homebrew::Render render(kAssets, live ? services.artwork() : nullptr);

    // The frame stays in its own memory between frames: an unchanged frame is not painted again.
    const std::size_t bytes = (static_cast<std::size_t>(width) * height * 4 + 0xffffu) & ~static_cast<std::size_t>(0xffffu);
    void *memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED)
    {
        logf("PLDN10001 preview: no memory for a %dx%d frame", width, height);
        return;
    }
    auto *pixels = static_cast<std::uint32_t *>(memory);
    hui::gfx::cpu::set_present_surface({pixels, width, height, width});

    Scaler scaler;
    scaler.prepare(width, height, frame_width, frame_height);
    // Flip pacing: a buffer is written only once the previous flip has completed (it may still be on
    // screen until then), and a flip is submitted only for a newly painted frame. Without the wait the
    // loop spun at 1,700-3,300 iterations a second (02.35) and could write a buffer VideoOut was showing.
    OrbisKernelEqueue flips = 0;
    const bool flip_events = sceKernelCreateEqueue(&flips, "playden-ui-flip") >= 0 &&
                             sceVideoOutAddFlipEvent(flips, video, nullptr) >= 0;
    if (!flip_events)
        logf("PLDN10001 preview: no flip events, pacing by sleep only");
    const auto wait_flip_done = [&] {
        for (int tries = 0; tries < 8 && sceVideoOutIsFlipPending(video) > 0; ++tries)
        {
            if (!flip_events)
            {
                usleep(2000);
                continue;
            }
            OrbisKernelEvent event{};
            int count = 0;
            OrbisKernelUseconds timeout = 20000;
            sceKernelWaitEqueue(flips, &event, 1, &count, &timeout);
        }
    };
    constexpr std::uint64_t kFrameUs = 16667;  // the 60 Hz display
    std::uint64_t painted_serial = 0, shown_serial = 0;

    hui::ps4::Pad pad;
    const bool pad_open = pad.open();
    logf("PLDN10001 preview: controller %s", pad_open ? "open" : "unavailable");
    hui::InputTracker input;

    int current = 0;
    std::uint64_t last = now_us(), window_start = last;
    unsigned frames = 0, painted = 0, flipped = 0;
    double draw_ms = 0.0, raster_ms = 0.0, worst_ms = 0.0, copy_ms = 0.0;
    for (;;)
    {
        const std::uint64_t now = now_us();
        hui::PadSample samples[16];
        const std::size_t count = pad.read(std::span<hui::PadSample>(samples, 16));
        apply_input(simulation, input.update(std::span<const hui::PadSample>(samples, count), now));

        const float dt = std::clamp(static_cast<float>(now - last) / 1e6f, 0.0f, 0.25f);
        last = now;
        if (live)
            services.Tick(simulation);
        const std::uint64_t draw_start = now_us();
        render.draw(simulation, width, height, dt);
        const double ms = static_cast<double>(now_us() - draw_start) / 1000.0;
        ++frames;
        if (!hui::gfx::cpu::last_present_skipped())
        {
            ++painted;
            ++painted_serial;
            draw_ms += ms;
            raster_ms += static_cast<double>(hui::gfx::cpu::last_present_ns()) / 1e6;
            worst_ms = std::max(worst_ms, ms);
        }

        if (painted_serial != shown_serial)
        {
            wait_flip_done();
            const std::uint64_t copy_start = now_us();
            current = (current + 1) % buffer_count;
            scaler.run(pixels, static_cast<std::uint32_t *>(buffers[current]));
            copy_ms += static_cast<double>(now_us() - copy_start) / 1000.0;
            sceVideoOutSubmitFlip(video, current, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0);
            shown_serial = painted_serial;
            ++flipped;
        }
        // Nothing faster than the display: the rest of this frame's time is the CPU's.
        const std::uint64_t spent = now_us() - now;
        if (spent < kFrameUs)
            usleep(static_cast<useconds_t>(kFrameUs - spent));
        pad.tick(dt);

        if (now - window_start >= 2000000u)
        {
            PlaydenHeapStats heap{};
            playden_heap_stats(&heap);
            const double seconds = static_cast<double>(now - window_start) / 1e6;
            logf("PLDN10001 preview: %.1f loops/s, painted %u, flipped %u, paint %.1f ms (raster %.1f, worst %.1f), "
                 "copy %.1f ms, heap %.1f MB (peak %.1f), textures %.1f MB",
                 frames / seconds, painted, flipped, painted ? draw_ms / painted : 0.0,
                 painted ? raster_ms / painted : 0.0, worst_ms, flipped ? copy_ms / flipped : 0.0,
                 static_cast<double>(heap.live_bytes) / 1048576.0, static_cast<double>(heap.peak_live_bytes) / 1048576.0,
                 static_cast<double>(hui::gfx::cpu::texture_bytes()) / 1048576.0);
            window_start = now;
            frames = painted = flipped = 0;
            draw_ms = raster_ms = worst_ms = copy_ms = 0.0;
        }
    }
}

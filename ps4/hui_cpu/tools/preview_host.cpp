// Playden PS4 port - renders Playden's UI (src/homebrew) with the CPU renderer on a desktop,
// one PNG per fixture scenario, and times each frame. Checks the renderer before a console run.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: preview_host FIXTURES ASSETS OUT [WIDTH HEIGHT [ACTIONS...]]
#include "gfx/surface.hpp"
#include "homebrew/render.hpp"
#include "homebrew/simulation.hpp"
#include "runtime/game_settings.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef HUI_CPU_STATS
namespace hui::gfx
{
extern std::uint64_t hui_cpu_stats[10][3];
}
using hui::gfx::hui_cpu_stats;
#endif

namespace
{

bool write_png(const std::filesystem::path &path, const std::vector<std::uint32_t> &pixels, int width, int height)
{
    std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * height * 3);
    for (std::size_t i = 0; i < pixels.size(); ++i)
    {
        rgb[i * 3 + 0] = static_cast<unsigned char>(pixels[i] >> 16);
        rgb[i * 3 + 1] = static_cast<unsigned char>(pixels[i] >> 8);
        rgb[i * 3 + 2] = static_cast<unsigned char>(pixels[i]);
    }
    return stbi_write_png(path.string().c_str(), width, height, 3, rgb.data(), width * 3) != 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s FIXTURES ASSETS OUT [WIDTH HEIGHT [SCENARIO|ACTION...]]\n", argv[0]);
        return 2;
    }
    const std::filesystem::path fixtures = argv[1], assets = argv[2], out = argv[3];
    const int width = argc > 5 ? std::atoi(argv[4]) : 1920, height = argc > 5 ? std::atoi(argv[5]) : 1080;
    std::filesystem::create_directories(out);
    playden::homebrew::Simulation simulation(fixtures, out / "profile.json", playden::homebrew::ModelMode::Simulation);
    {
        std::vector<playden::runtime::SettingsPreset> presets;
        std::vector<playden::runtime::GameRecipe> recipes;
        std::string reason;
        playden::runtime::KitCapabilities kit;
        kit.dxvk_config = true;
        simulation.SetKitCapabilities(kit);
        if (playden::runtime::LoadSettingsData((assets / "runtime").string(), &presets, &recipes, &reason))
            simulation.SetSettingsData(std::move(presets), std::move(recipes));
        else
            std::fprintf(stderr, "settings data: %s\n", reason.c_str());
    }
    if (const char *threads = std::getenv("HUI_THREADS"))
        hui::gfx::cpu::set_threads(std::atoi(threads));
    playden::homebrew::Render render(assets, nullptr);
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
    hui::gfx::cpu::set_present_surface({pixels.data(), width, height, width});

    std::vector<std::string> steps;
    for (int i = 6; i < argc; ++i)
        steps.emplace_back(argv[i]);
    if (steps.empty())
        steps = {"scenario:default", "right", "right", "right", "right", "scenario:default", "confirm",
                 "scenario:signed-out", "scenario:stress", "scenario:install-ready", "scenario:paused"};
    int index = 0;
    for (const std::string &step : steps)
    {
        bool ok = step.rfind("scenario:", 0) == 0 ? simulation.scenario(step.substr(9)) : simulation.action(step);
        double total_ms = 0.0, raster_ms = 0.0, worst_ms = 0.0;
        int painted = 0, skipped = 0;
        constexpr int kFrames = 20; // the first frames settle the motion
        for (int frame = 0; frame < kFrames; ++frame)
        {
            const auto started = std::chrono::steady_clock::now();
            render.draw(simulation, width, height, 1.0F / 60.0F);
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            if (hui::gfx::cpu::last_present_skipped())
            {
                ++skipped;
                continue;
            }
            ++painted;
            total_ms += ms;
            raster_ms += static_cast<double>(hui::gfx::cpu::last_present_ns()) / 1e6;
            worst_ms = std::max(worst_ms, ms);
        }
        char name[96];
        std::snprintf(name, sizeof(name), "%02d-%s.png", index++, step.c_str() + (step.rfind("scenario:", 0) == 0 ? 9 : 0));
        write_png(out / name, pixels, width, height);
#ifdef HUI_CPU_STATS
        for (int shape = 0; shape < 10; ++shape)
            if (hui_cpu_stats[shape][0])
                std::printf("    shape %d: %llu instances %.1f Mpx %.1f ms\n", shape,
                            (unsigned long long)hui_cpu_stats[shape][0] / std::max(painted, 1),
                            hui_cpu_stats[shape][1] / 1e6 / std::max(painted, 1),
                            hui_cpu_stats[shape][2] / 1e6 / std::max(painted, 1));
        std::memset(hui_cpu_stats, 0, sizeof(hui_cpu_stats));
#endif
        std::printf("%-26s ok=%d painted=%d skipped=%d frame=%.2fms raster=%.2fms worst=%.2fms textures=%.1fMB\n",
                    step.c_str(), ok ? 1 : 0, painted, skipped, painted ? total_ms / painted : 0.0,
                    painted ? raster_ms / painted : 0.0, worst_ms,
                    static_cast<double>(hui::gfx::cpu::texture_bytes()) / 1048576.0);
    }
    return 0;
}

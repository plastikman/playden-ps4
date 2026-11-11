// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui.hpp"

#include "gfx/draw_list.hpp"
#include "gfx/font.hpp"
#include "ui/glyphs.hpp"
#include "homebrew/qr.hpp"
#include "homebrew/artwork.hpp"
#include "homebrew/artwork_cache.hpp"
#include "steam/backend/http_client.h"
#include "library/store.h"
// The PS4 PRX loader in this runtime does not provide working dynamic TLS
// resolution to a worker thread. Artwork has exactly one decoder worker, so
// stb_image's process-global load flags are safe and avoid its TLS access.
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <sys/stat.h>
#include <string>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <atomic>
#include <memory>
#include <vector>

namespace playden::homebrew {
std::string CoverUrl(const playden::GameId &game);
}

extern "C" std::size_t playden_ps4_library_count();
extern "C" const char *playden_ps4_library_title(std::size_t index);
extern "C" const char *playden_ps4_library_title_by_app_id(std::uint32_t app_id);
extern "C" std::size_t playden_ps4_library_visible_count(int filter);
extern "C" const char *playden_ps4_library_visible_title(int filter, std::size_t index);
extern "C" std::uint32_t playden_ps4_library_visible_app_id(int filter, std::size_t index);
extern "C" int playden_ps4_library_is_favorite(std::uint32_t app_id);
extern "C" int playden_ps4_library_update_available(std::uint32_t app_id);
extern "C" {
#include "native/pw_update_ps4.h"
}
extern "C" int playden_ps4_library_is_hidden(std::uint32_t app_id);
extern "C" const char *playden_ps4_library_search_text();
extern "C" const char *playden_ps4_library_sort_label();
extern "C" std::size_t playden_ps4_library_collection_count();
extern "C" const char *playden_ps4_library_collection_name(std::size_t index);
extern "C" std::size_t playden_ps4_library_collection_size(std::size_t index);
extern "C" int playden_ps4_library_in_collection(std::uint32_t app_id, std::size_t index);
extern "C" int playden_ps4_maintenance_state(void);
extern "C" int playden_ps4_library_is_family(std::uint32_t app_id);
extern "C" const char *playden_ps4_auth_prompt();
extern "C" const char *playden_ps4_auth_account_name();
extern "C" int playden_ps4_auth_guard_code_wanted();
// Settings' Steam account row: main.c arms the sign-out with a first Cross.
extern "C" int playden_ps4_signout_armed;
// Delete (page 3): main.c's stage (0 choosing, 1 Cloud sync, 2-3 prefix backup and removal) and why it stopped.
extern "C" int playden_ps4_delete_stage;
extern "C" int playden_ps4_launching;
extern "C" int playden_ps4_download_fastest();
extern "C" char playden_ps4_delete_note[160];
extern "C" const char *playden_ps4_maintenance_message(void);
extern "C" const char *playden_ps4_maintenance_backup_label(std::uint32_t app_id);
// Maintenance (page 10): main.c arms the prefix reset with a first Cross.
extern "C" int playden_ps4_maintenance_armed;
// The game the collection picker (page 9) is for; main.c sets it when the picker opens.
extern "C" std::uint32_t playden_ps4_collection_app_id;
// The Library tab's views (Home uses 0: the installed list, unsearched); main.c has the same rule.
extern "C" int playden_ps4_home_shelf;
extern "C" std::size_t playden_ps4_home_shelf_count();
extern "C" const char *playden_ps4_home_shelf_title(std::size_t shelf);
extern "C" int playden_ps4_library_collection_pinned(std::size_t index);
static int library_filter(int tab, int row) { return tab == 0 ? 3000 + playden_ps4_home_shelf : 1000 + row; }
extern "C" const char *playden_ps4_library_detail();
extern "C" int playden_ps4_game_details_state();
extern "C" int playden_ps4_game_details_installed();
extern "C" int playden_ps4_game_details_incomplete();
extern "C" int playden_ps4_game_details_update();
extern "C" const char *playden_ps4_install_sheet_value(int row);
extern "C" const char *playden_ps4_install_sheet_note();
extern "C" int playden_ps4_game_details_launch_supported();
extern "C" std::uint32_t playden_ps4_game_details_app_id();
extern "C" void playden_ps4_game_uninstall_start(std::uint32_t app_id);
extern "C" int playden_ps4_game_uninstall_state();
extern "C" std::uint32_t playden_ps4_game_uninstall_app_id();
extern "C" const char *playden_ps4_game_uninstall_message();
extern "C" std::uint32_t playden_ps4_game_details_app_id();
extern "C" const char *playden_ps4_game_details_message();
extern "C" int playden_ps4_game_store_details_ready();
extern "C" int playden_ps4_game_store_details_available();
extern "C" const char *playden_ps4_game_store_description();
extern "C" const char *playden_ps4_game_store_genres();
extern "C" const char *playden_ps4_game_store_developers();
extern "C" const char *playden_ps4_game_store_publishers();
extern "C" const char *playden_ps4_game_store_categories();
extern "C" const char *playden_ps4_game_store_release_date();
extern "C" const char *playden_ps4_game_store_metacritic_score();
extern "C" const char *playden_ps4_game_store_controller_support();
extern "C" int playden_ps4_game_store_windows_supported();
extern "C" int playden_ps4_game_install_state();
extern "C" const char *playden_ps4_game_install_message();
extern "C" std::uint32_t playden_ps4_queue_app_id();
extern "C" int playden_ps4_queue_state();
extern "C" std::size_t playden_ps4_queue_waiting_count();
extern "C" std::uint32_t playden_ps4_queue_waiting_app(std::size_t index);
extern "C" std::size_t playden_ps4_update_count();
extern "C" const char *playden_ps4_queue_message();
extern "C" int playden_ps4_saves_state();
extern "C" std::uint32_t playden_ps4_saves_app_id();
extern "C" const char *playden_ps4_saves_message();
extern "C" std::uint64_t playden_ps4_saves_progress_done();
extern "C" std::uint64_t playden_ps4_saves_progress_total();
extern "C" const char *playden_ps4_log_receiver_ip();
extern "C" const char *playden_ps4_log_receiver_status();
extern "C" int playden_ps4_logging_enabled();
extern "C" const char *playden_ps4_wine_debug_level();
extern "C" int playden_ps4_wine_debug_enabled();
extern "C" int playden_ps4_wait_watchdog_enabled();
extern "C" int playden_ps4_gpu_hang_dump_enabled();
extern "C" int playden_ps4_download_connections();
extern "C" const char *playden_ps4_content_language();
extern "C" std::size_t playden_ps4_controls_count();
extern "C" const char *playden_ps4_controls_label(std::size_t row);
extern "C" const char *playden_ps4_controls_value(std::size_t row);
extern "C" int playden_ps4_controls_custom(std::size_t row);
extern "C" const char *playden_ps4_controls_status();
extern "C" std::size_t playden_ps4_overrides_open(std::size_t row);
extern "C" const char *playden_ps4_overrides_label(std::size_t item);
extern "C" int playden_ps4_overrides_checked(std::size_t item);
extern "C" const char *playden_ps4_overrides_value();

namespace
{
constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr const char *kFontPath = "/app0/assets/inter-regular.huifont";
constexpr std::size_t kMaxArtworkDownloadBytes = 4 * 1024 * 1024;

std::string ArtworkCachePath(const playden::homebrew::ArtworkKey &key)
{
    const std::uint32_t app = key.game_id.steam_app().value_or(0);
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : key.source)
    {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    char path[128]{};
    std::snprintf(path, sizeof(path), "/data/PLDN10001/artwork/%u-%016llx.img",
                  app, static_cast<unsigned long long>(hash));
    return path;
}

std::vector<std::uint8_t> ReadArtworkCache(const std::string &path)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return {};
    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        std::fclose(file);
        return {};
    }
    const long length = std::ftell(file);
    if (length <= 0 || static_cast<unsigned long>(length) > kMaxArtworkDownloadBytes ||
        std::fseek(file, 0, SEEK_SET) != 0)
    {
        std::fclose(file);
        return {};
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size()) return {};
    return bytes;
}

void WriteArtworkCache(const std::string &path, const std::vector<std::uint8_t> &bytes)
{
    (void)::mkdir("/data/PLDN10001/artwork", 0777);
    const std::string temporary = path + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) return;
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() &&
                         std::fflush(file) == 0;
    std::fclose(file);
    if (written)
        (void)std::rename(temporary.c_str(), path.c_str());
    else
        (void)std::remove(temporary.c_str());
}

std::optional<playden::homebrew::ArtworkImage> DecodeArtwork(const std::vector<std::uint8_t> &bytes)
{
    if (bytes.empty() || bytes.size() > kMaxArtworkDownloadBytes) return std::nullopt;
    int width = 0, height = 0, channels = 0;
    stbi_uc *decoded = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                             &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0 || width > 1920 || height > 1920 ||
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4 >
            playden::homebrew::kMaxArtworkImageBytes)
    {
        stbi_image_free(decoded);
        return std::nullopt;
    }
    playden::homebrew::ArtworkImage image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.rgba.assign(decoded, decoded + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(decoded);
    return image;
}

class Ps4ArtworkBackend final : public playden::homebrew::ArtworkBackend
{
  public:
    std::optional<playden::homebrew::ArtworkImage> Load(
        const playden::homebrew::ArtworkKey &key, const std::atomic<bool> &cancel) override
    {
        if (key.source.rfind("https://cdn.cloudflare.steamstatic.com/steam/apps/", 0) != 0)
            return std::nullopt;
        const std::string cache_path = ArtworkCachePath(key);
        auto cached = ReadArtworkCache(cache_path);
        if (!cached.empty())
        {
            if (auto image = DecodeArtwork(cached)) return image;
            (void)std::remove(cache_path.c_str());
        }
        if (cancel.load()) return std::nullopt;
        playden::steam::HttpClient http("/app0/assets/cacert.pem");
        playden::steam::HttpRequest request;
        request.url = key.source;
        request.timeout_ms = 15000;
        request.max_response_bytes = kMaxArtworkDownloadBytes;
        const auto response = http.Perform(request, cancel);
        if (!response.ok() || response.status_code != 200 || response.body.empty())
            return std::nullopt;
        std::vector<std::uint8_t> bytes(response.body.begin(), response.body.end());
        auto image = DecodeArtwork(bytes);
        if (image) WriteArtworkCache(cache_path, bytes);
        return image;
    }
};

class CpuArtworkTextures final : public playden::homebrew::TextureBackend
{
  public:
    std::uint32_t Upload(const playden::homebrew::ArtworkImage &image) override
    {
        if (image.width == 0 || image.height == 0 || image.rgba.empty()) return 0;
        const std::uint32_t id = next_id_++;
        images_.emplace(id, image);
        uploaded_ = true;
        return id;
    }
    void Release(std::uint32_t id) override { images_.erase(id); }
    bool ConsumeUpload()
    {
        if (!uploaded_) return false;
        uploaded_ = false;
        return true;
    }
    const playden::homebrew::ArtworkImage *Find(std::uint32_t id) const
    {
        const auto found = images_.find(id);
        return found == images_.end() ? nullptr : &found->second;
    }
  private:
    std::uint32_t next_id_ = 1;
    bool uploaded_ = false;
    std::unordered_map<std::uint32_t, playden::homebrew::ArtworkImage> images_;
};

struct Ps4Artwork
{
    Ps4ArtworkBackend backend;
    playden::homebrew::ArtworkService service{backend};
    CpuArtworkTextures textures;
    playden::homebrew::LiveArtworkCache cache{service, textures};
};

Ps4Artwork &artwork()
{
    static Ps4Artwork instance;
    return instance;
}

bool load_font(hui::gfx::Font &font)
{
    FILE *file = std::fopen(kFontPath, "rb");
    if (file == nullptr)
        return false;
    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        std::fclose(file);
        return false;
    }
    const long length = std::ftell(file);
    if (length <= 0 || length > 4000000 || std::fseek(file, 0, SEEK_SET) != 0)
    {
        std::fclose(file);
        return false;
    }
    std::string bytes(static_cast<std::size_t>(length), '\0');
    const bool read = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    return read && font.load(bytes);
}

void blend(std::uint32_t *frame, int x, int y, float r, float g, float b, float a)
{
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight || a <= 0.0F)
        return;
    a = std::clamp(a, 0.0F, 1.0F);
    std::uint32_t &pixel = frame[y * kWidth + x];
    if (a >= 0.999F)
    {
        pixel = (static_cast<std::uint32_t>(std::clamp(r * 255.0F, 0.0F, 255.0F)) << 16) |
                (static_cast<std::uint32_t>(std::clamp(g * 255.0F, 0.0F, 255.0F)) << 8) |
                static_cast<std::uint32_t>(std::clamp(b * 255.0F, 0.0F, 255.0F));
        return;
    }
    const float inverse = 1.0F - a;
    const float old_r = static_cast<float>((pixel >> 16) & 0xffu);
    const float old_g = static_cast<float>((pixel >> 8) & 0xffu);
    const float old_b = static_cast<float>(pixel & 0xffu);
    const auto channel = [inverse, a](float old, float value) {
        return static_cast<std::uint32_t>(std::clamp(old * inverse + value * 255.0F * a, 0.0F, 255.0F));
    };
    pixel = (channel(old_r, r) << 16) | (channel(old_g, g) << 8) | channel(old_b, b);
}

float hint_text(hui::gfx::DrawList &list, const hui::gfx::Font &font, const char *value,
                float x, float baseline, float size, hui::gfx::Color color)
{
    using namespace hui::gfx;
    const std::string text(value != nullptr ? value : "");
    float cursor_x = x;
    std::size_t cursor = 0;
    std::size_t scan = 0;
    static const hui::ui::Fonts empty_fonts{};
    const hui::ui::GlyphStyle glyph_style = hui::ui::GlyphStyle::mono(
        color, Color::rgb(0x292521, 0.98F));
    while (scan < text.size())
    {
        std::size_t word_start = scan;
        while (word_start < text.size() && text[word_start] == ' ')
            ++word_start;
        if (word_start == text.size())
            break;
        std::size_t word_end = word_start;
        while (word_end < text.size() && text[word_end] != ' ')
            ++word_end;
        scan = word_end;
        const std::string_view word(text.data() + word_start, word_end - word_start);
        const bool is_cross = word == "CROSS" || word == "Cross";
        const bool is_circle = word == "CIRCLE" || word == "Circle";
        const bool is_square = word == "SQUARE" || word == "Square";
        const bool is_options = word == "OPTIONS" || word == "Options";
        const bool is_triangle = word == "TRIANGLE" || word == "Triangle";
        const bool is_l1 = word == "L1";
        const bool is_r1 = word == "R1";
        if (!is_cross && !is_circle && !is_square && !is_options && !is_triangle && !is_l1 && !is_r1)
            continue;

        const std::string_view prefix(text.data() + cursor, word_start - cursor);
        if (!prefix.empty())
        {
            list.text(font, 0, prefix, cursor_x, baseline, size, color);
            cursor_x += font.measure(prefix, size);
        }
        // The hints are viewed from across the room on a TV. At text-sized
        // glyphs the face symbols blur into plain dots, so give the controller
        // artwork a little more room than the label height.
        const float diameter = std::min(size * 1.8F, 32.0F);
        if (is_l1 || is_r1)
        {
            // The library's bumper glyph draws its label from hui::ui::Fonts, which hint_text does
            // not have (an empty set crashed 01.94): draw the key and its label with our font.
            const float h = diameter * 0.74F;
            const float w = diameter * 1.25F;
            const float top = baseline - size * 0.46F - h * 0.5F;
            list.bordered_rect({cursor_x, top, w, h}, h * 0.34F, Color::rgb(0x292521, 0.98F), 1.5F, color);
            const char *name = is_l1 ? "L1" : "R1";
            const float text_size = size * 0.8F;
            list.text(font, 0, name, cursor_x + (w - font.measure(name, text_size)) * 0.5F,
                      top + h * 0.5F + text_size * 0.36F, text_size, color);
            cursor_x += w + size * 0.18F;
            cursor = word_end;
            continue;
        }
        if (is_triangle)
        {
            // The CPU rasterizer has no triangle shape, so the library's triangle face draws only
            // its round body: add the symbol as three lines, which it does draw.
            hui::ui::draw_button(list, empty_fonts, glyph_style, hui::ui::Button::triangle,
                                 cursor_x, baseline - size * 0.46F, diameter);
            const float cx = cursor_x + diameter * 0.5F;
            const float cy = baseline - size * 0.46F;
            const float r = diameter * 0.26F;
            const float stroke = std::max(1.5F, diameter * 0.075F);
            const float top_y = cy - r, base_y = cy + r * 0.62F, half = r * 1.0F;
            list.line(cx, top_y, cx - half, base_y, stroke, color);
            list.line(cx - half, base_y, cx + half, base_y, stroke, color);
            list.line(cx + half, base_y, cx, top_y, stroke, color);
            cursor_x += diameter + size * 0.18F;
            cursor = word_end;
            continue;
        }
        hui::ui::draw_button(list, empty_fonts, glyph_style,
                             is_cross ? hui::ui::Button::cross :
                             is_circle ? hui::ui::Button::circle :
                             is_square ? hui::ui::Button::square :
                             is_triangle ? hui::ui::Button::triangle :
                             is_l1 ? hui::ui::Button::l1 :
                             is_r1 ? hui::ui::Button::r1 : hui::ui::Button::options,
                             cursor_x, baseline - size * 0.46F, diameter);
        cursor_x += diameter + size * 0.18F;
        cursor = word_end;
    }
    if (cursor < text.size())
    {
        const std::string_view suffix(text.data() + cursor, text.size() - cursor);
        list.text(font, 0, suffix, cursor_x, baseline, size, color);
        cursor_x += font.measure(suffix, size);
    }
    return cursor_x;
}

void raster(hui::gfx::DrawList &list, const hui::gfx::Font &font,
            const CpuArtworkTextures &textures, std::uint32_t *frame)
{
    for (const hui::gfx::Run &run : list.runs())
    {
        if (run.mesh)
            continue;
        const auto &instances = list.instances();
        for (std::uint32_t index = run.first; index < run.first + run.count; ++index)
        {
            const hui::gfx::Instance &instance = instances[index];
            const auto shape = static_cast<int>(instance.params[3] + 0.5F);
            const int left = std::max(0, static_cast<int>(std::floor(instance.rect[0])));
            const int top = std::max(0, static_cast<int>(std::floor(instance.rect[1])));
            const int right = std::min(kWidth, static_cast<int>(std::ceil(instance.rect[0] + instance.rect[2])));
            const int bottom = std::min(kHeight, static_cast<int>(std::ceil(instance.rect[1] + instance.rect[3])));
            if (right <= left || bottom <= top)
                continue;
            const auto &atlas = font.atlas();
            for (int y = top; y < bottom; ++y)
            {
                for (int x = left; x < right; ++x)
                {
                    float coverage = 1.0F;
                    const float local_x = static_cast<float>(x) + 0.5F - instance.rect[0];
                    const float local_y = static_cast<float>(y) + 0.5F - instance.rect[1];
                    if (shape == 0)
                    {
                        const float radius = std::min(instance.params[0],
                            std::min(instance.rect[2], instance.rect[3]) * 0.5F);
                        if (radius <= 0.0F)
                        {
                            const float t = std::clamp(local_y / instance.rect[3], 0.0F, 1.0F);
                            blend(frame, x, y,
                                  instance.color_top[0] + (instance.color_bottom[0] - instance.color_top[0]) * t,
                                  instance.color_top[1] + (instance.color_bottom[1] - instance.color_top[1]) * t,
                                  instance.color_top[2] + (instance.color_bottom[2] - instance.color_top[2]) * t,
                                  instance.color_top[3] + (instance.color_bottom[3] - instance.color_top[3]) * t);
                            continue;
                        }
                        if (instance.params[1] <= 0.0F && local_x >= radius &&
                            local_x <= instance.rect[2] - radius && local_y >= radius &&
                            local_y <= instance.rect[3] - radius)
                        {
                            const float t = std::clamp(local_y / instance.rect[3], 0.0F, 1.0F);
                            blend(frame, x, y,
                                  instance.color_top[0] + (instance.color_bottom[0] - instance.color_top[0]) * t,
                                  instance.color_top[1] + (instance.color_bottom[1] - instance.color_top[1]) * t,
                                  instance.color_top[2] + (instance.color_bottom[2] - instance.color_top[2]) * t,
                                  instance.color_top[3] + (instance.color_bottom[3] - instance.color_top[3]) * t);
                            continue;
                        }
                        const float dx = std::fabs(local_x - instance.rect[2] * 0.5F) -
                                         (instance.rect[2] * 0.5F - radius);
                        const float dy = std::fabs(local_y - instance.rect[3] * 0.5F) -
                                         (instance.rect[3] * 0.5F - radius);
                        const float outside = std::sqrt(std::max(dx, 0.0F) * std::max(dx, 0.0F) +
                                                         std::max(dy, 0.0F) * std::max(dy, 0.0F));
                        const float distance = outside + std::min(std::max(dx, dy), 0.0F) - radius;
                        coverage = std::clamp(0.5F - distance, 0.0F, 1.0F);
                        const float t = std::clamp(local_y / instance.rect[3], 0.0F, 1.0F);
                        float r = instance.color_top[0] + (instance.color_bottom[0] - instance.color_top[0]) * t;
                        float g = instance.color_top[1] + (instance.color_bottom[1] - instance.color_top[1]) * t;
                        float b = instance.color_top[2] + (instance.color_bottom[2] - instance.color_top[2]) * t;
                        float a = instance.color_top[3] + (instance.color_bottom[3] - instance.color_top[3]) * t;
                        if (instance.params[1] > 0.0F)
                        {
                            const float inner = std::clamp(0.5F - (distance + instance.params[1]), 0.0F, 1.0F);
                            r = instance.border_color[0] + (r - instance.border_color[0]) * inner;
                            g = instance.border_color[1] + (g - instance.border_color[1]) * inner;
                            b = instance.border_color[2] + (b - instance.border_color[2]) * inner;
                            a = instance.border_color[3] + (a - instance.border_color[3]) * inner;
                        }
                        blend(frame, x, y, r, g, b, a * coverage);
                        continue;
                    }
                    else if (shape == 1)
                    {
                        if (atlas.empty() || font.atlas_width() == 0 || font.atlas_height() == 0)
                            continue;
                        const float u = instance.extra[0] + (local_x / instance.rect[2]) *
                                        (instance.extra[2] - instance.extra[0]);
                        const float v = instance.extra[1] + (local_y / instance.rect[3]) *
                                        (instance.extra[3] - instance.extra[1]);
                        const float fx = u * font.atlas_width() - 0.5F;
                        const float fy = v * font.atlas_height() - 0.5F;
                        const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0,
                                                  static_cast<int>(font.atlas_width()) - 1);
                        const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0,
                                                  static_cast<int>(font.atlas_height()) - 1);
                        const int x1 = std::min(x0 + 1, static_cast<int>(font.atlas_width()) - 1);
                        const int y1 = std::min(y0 + 1, static_cast<int>(font.atlas_height()) - 1);
                        const float tx = std::clamp(fx - std::floor(fx), 0.0F, 1.0F);
                        const float ty = std::clamp(fy - std::floor(fy), 0.0F, 1.0F);
                        const auto at = [&atlas, &font](int ax, int ay) {
                            return static_cast<float>(atlas[static_cast<std::size_t>(ay) *
                                font.atlas_width() + ax]) / 255.0F;
                        };
                        const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
                        const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
                        const float sdf = top + (bottom - top) * ty;
                        const float distance = (sdf - 0.5F) * 2.0F * instance.params[0];
                        coverage = std::clamp(distance + 0.5F, 0.0F, 1.0F);
                    }
                    else if (shape == 3)
                    {
                        const playden::homebrew::ArtworkImage *image = textures.Find(run.texture);
                        if (!image) continue;
                        const float u = instance.extra[0] + (local_x / instance.rect[2]) *
                                        (instance.extra[2] - instance.extra[0]);
                        const float v = instance.extra[1] + (local_y / instance.rect[3]) *
                                        (instance.extra[3] - instance.extra[1]);
                        const int ix = std::clamp(static_cast<int>(u * image->width), 0,
                                                  static_cast<int>(image->width) - 1);
                        const int iy = std::clamp(static_cast<int>(v * image->height), 0,
                                                  static_cast<int>(image->height) - 1);
                        if (instance.params[0] > 0.0F)
                        {
                            const float radius = std::min(instance.params[0],
                                std::min(instance.rect[2], instance.rect[3]) * 0.5F);
                            if (local_x < radius || local_x > instance.rect[2] - radius ||
                                local_y < radius || local_y > instance.rect[3] - radius)
                            {
                                const float dx = std::fabs(local_x - instance.rect[2] * 0.5F) -
                                                 (instance.rect[2] * 0.5F - radius);
                                const float dy = std::fabs(local_y - instance.rect[3] * 0.5F) -
                                                 (instance.rect[3] * 0.5F - radius);
                                const float outside = std::sqrt(std::max(dx, 0.0F) * std::max(dx, 0.0F) +
                                                                 std::max(dy, 0.0F) * std::max(dy, 0.0F));
                                const float distance = outside + std::min(std::max(dx, dy), 0.0F) - radius;
                                coverage = std::clamp(0.5F - distance, 0.0F, 1.0F);
                            }
                        }
                        const std::size_t offset = (static_cast<std::size_t>(iy) * image->width +
                                                    static_cast<std::size_t>(ix)) * 4;
                        blend(frame, x, y,
                              static_cast<float>(image->rgba[offset]) / 255.0F * instance.color_top[0],
                              static_cast<float>(image->rgba[offset + 1]) / 255.0F * instance.color_top[1],
                              static_cast<float>(image->rgba[offset + 2]) / 255.0F * instance.color_top[2],
                              static_cast<float>(image->rgba[offset + 3]) / 255.0F * instance.color_top[3] * coverage);
                        continue;
                    }
                    else if (shape == 4)
                    {
                        const float px = static_cast<float>(x) + 0.5F - instance.extra[0];
                        const float py = static_cast<float>(y) + 0.5F - instance.extra[1];
                        const float bx = instance.extra[2] - instance.extra[0];
                        const float by = instance.extra[3] - instance.extra[1];
                        const float h = std::clamp((px * bx + py * by) /
                                                   std::max(bx * bx + by * by, 1e-6F), 0.0F, 1.0F);
                        const float dx = px - bx * h;
                        const float dy = py - by * h;
                        const float distance = std::sqrt(dx * dx + dy * dy) - instance.params[1] * 0.5F;
                        coverage = std::clamp(0.5F - distance, 0.0F, 1.0F);
                    }
                    else
                        continue;
                    const float t = std::clamp(local_y / instance.rect[3], 0.0F, 1.0F);
                    blend(frame, x, y,
                          instance.color_top[0] + (instance.color_bottom[0] - instance.color_top[0]) * t,
                          instance.color_top[1] + (instance.color_bottom[1] - instance.color_top[1]) * t,
                          instance.color_top[2] + (instance.color_bottom[2] - instance.color_top[2]) * t,
                          (instance.color_top[3] + (instance.color_bottom[3] - instance.color_top[3]) * t) * coverage);
                }
            }
        }
    }
}

void draw(std::uint32_t *frame, int tab, int selected, int page, int controller_ready,
          int auth_state, int game_focus, int game_selected, int delete_choice,
          const char *challenge_url, const char *auth_failure, const char *runtime_status)
{
    static hui::gfx::Font font;
    static bool font_checked = false;
    static bool font_ready = false;
    if (!font_checked)
    {
        font_checked = true;
        font_ready = load_font(font);
    }
    hui::gfx::DrawList list;
    using namespace hui::gfx;
    const Color bg = Color::rgb(0x0e0d0c);
    const Color panel = Color::rgb(0x16140f);
    const Color ink = Color::rgb(0xf3efe9);
    const Color secondary = Color::rgb(0xa8a29e);
    const Color accent = Color::rgb(0xf0863a);
    auto &art = artwork();
    const bool steam_connected = auth_state == 6 || auth_state == 7;
    art.cache.BeginFrame(steam_connected ? 1 : 0);
    // Direct row fills are substantially faster than sending the 1080p base
    // color through the per-pixel shape blender on every controller move.
    for (int y = 0; y < kHeight; ++y)
    {
        const float t = static_cast<float>(y) / static_cast<float>(kHeight - 1);
        const auto channel = [t](int top, int bottom) {
            return static_cast<std::uint32_t>(static_cast<float>(top) +
                (static_cast<float>(bottom - top) * t));
        };
        const std::uint32_t color = (channel(0x17, 0x0e) << 16) |
                                    (channel(0x13, 0x0d) << 8) | channel(0x0f, 0x0c);
        std::fill_n(frame + y * kWidth, kWidth, color);
    }
    list.rounded_rect({80, 52, 1760, 84}, 14, panel);
    const char *tabs[] = {"HOME", "LIBRARY", "QUEUE", "SAVES", "SETTINGS"};
    for (int i = 0; i < 5; ++i)
    {
        const float x = 98.0F + static_cast<float>(i) * 204.0F;
        if (i == tab)
            list.bordered_rect({x, 66, 184, 54}, 9, Color::rgb(0xf0863a, 0.12F), 1.5F,
                               Color::rgb(0xf0863a, 0.80F));
        if (font_ready)
            list.text(font, 0, tabs[i], x + 18, 101, 20,
                      i == tab ? ink : secondary);
    }
    if (font_ready)
        list.text(font, 0, "PLAYDEN FOR PS4", 1470, 103, 17, secondary);

    if (page == 0 && tab == 1)
    {
        if (font_ready)
        {
            list.text(font, 0, "LIBRARY", 96, 198, 38, ink);
            list.text(font, 0, controller_ready ? "DUALSHOCK 4 CONNECTED" : "CONNECT A DUALSHOCK 4",
                      98, 232, 16, secondary);
        }
        // The four views, then one row per collection; counts follow the search.
        const int collection_count = static_cast<int>(playden_ps4_library_collection_count());
        const int filter_count = 4 + collection_count;
        const int connect_index = filter_count;
        const std::size_t visible_count = selected >= 0 && selected < filter_count
            ? playden_ps4_library_visible_count(1000 + selected) : 0;
        const std::string installed_detail = std::to_string(visible_count) +
            (visible_count == 1 ? " game" : " games");
        constexpr int kMenuRows = 9;
        const int first_row = std::max(0, std::min(selected, filter_count - 1) - (kMenuRows - 1));
        for (int i = first_row; i < filter_count && i < first_row + kMenuRows; ++i)
        {
            const char *names[] = {"Installed", "All games", "Favorites", "Hidden"};
            const std::string name = i < 4 ? names[i] : playden_ps4_library_collection_name(static_cast<std::size_t>(i - 4));
            const std::string count = std::to_string(playden_ps4_library_visible_count(1000 + i));
            const float y = 282.0F + static_cast<float>(i - first_row) * 68.0F;
            const Rect row{96, y, 316, 58};
            if (i == selected)
                list.bordered_rect(row, 8, Color::rgb(0xf3efe9, 0.08F), 1.0F,
                                   Color::rgb(0xf0863a, 0.78F));
            else
                list.rounded_rect(row, 8, bg);
            if (font_ready)
            {
                list.text(font, 0, font.fit(name, 22, 230), 116, y + 36, 22,
                          i == selected ? ink : secondary);
                list.text(font, 0, count, 390, y + 36, 18, secondary, Align::right);
                if (i == 3 && collection_count > 0)
                    list.rounded_rect({116, y + 63, 274, 1}, 0, Color::rgb(0xf3efe9, 0.12F));
            }
        }
        list.rounded_rect({510, 272, 1304, 690}, 16, panel);
        const std::string search = playden_ps4_library_search_text();
        if (font_ready && selected < filter_count)
        {
            std::string order = std::string("SORT  ") + playden_ps4_library_sort_label();
            if (!search.empty()) order = "SEARCH  \"" + search + "\"     " + order;
            list.text(font, 0, font.fit(order, 17, 760), 1780, 320, 17, search.empty() ? secondary : accent,
                      Align::right);
        }
        if (visible_count == 0)
        {
            list.rounded_rect({574, 344, 44, 4}, 2, accent);
            if (font_ready)
            {
                const bool steam_connected = auth_state == 6 || auth_state == 7;
                const bool show_connect = !steam_connected && selected == connect_index;
                const bool searched = !search.empty() && selected < filter_count;
                const char *empty = auth_state == 5 ? "LOADING STEAM LIBRARY" :
                    searched ? "NO MATCHES" :
                    selected >= 4 && selected < filter_count ? "NO GAMES IN THIS COLLECTION" :
                    selected == 0 ? "NO INSTALLED GAMES" :
                    selected == 2 ? "NO FAVORITES YET" :
                    selected == 3 ? "NO HIDDEN GAMES" :
                    selected == connect_index ? "STEAM NOT CONNECTED" : "NO GAMES HERE";
                list.text(font, 0, empty, 1162, 472, 42, ink, Align::center);
                const char *detail = searched ? "Nothing here matches the search. Triangle changes it." :
                    selected >= 4 && selected < filter_count ? "Open Actions on a game and add it to this collection." :
                    selected != 1 && auth_state == 6 ?
                    (selected == 0 ? "Games you install on this console will appear here." :
                     selected == 2 ? "Press Square on a game to add it to Favorites." :
                     selected == 3 ? "Hidden games appear here. Open Actions on a game to hide it." :
                                     "Steam is connected for this session.") :
                    playden_ps4_library_detail();
                const char *empty_detail = detail[0] ? detail : "Connect Steam or import a local game to fill your library.";
                list.text(font, 0, empty_detail, 1162, 520, 22, secondary, Align::center);
                if (show_connect)
                {
                    list.bordered_rect({914, 600, 496, 72}, 9, bg, 2.0F,
                                       Color::rgb(0xf3efe9, 0.25F));
                    list.text(font, 0, "CONNECT STEAM", 1162, 644, 24, ink, Align::center);
                }
                hint_text(list, font, selected == connect_index && show_connect ? "CROSS  CONNECT     CIRCLE  BACK" :
                          "UP / DOWN  MOVE     TRIANGLE  SEARCH     R1  SORT     CIRCLE  BACK",
                          96, 970, 17, secondary);
            }
        }
        else if (font_ready)
        {
            const char *library_detail = selected == 0 ? installed_detail.c_str()
                : runtime_status != nullptr && runtime_status[0] != '\0'
                    ? runtime_status : playden_ps4_library_detail();
            list.text(font, 0, library_detail, 574, 320, 19,
                      runtime_status != nullptr && runtime_status[0] != '\0'
                          ? Color::rgb(0xffc477) : secondary);
            constexpr std::size_t kPageSize = 12;
            const std::size_t first = (static_cast<std::size_t>(std::max(game_selected, 0)) / kPageSize) * kPageSize;
            const std::size_t visible = std::min(kPageSize, visible_count > first ? visible_count - first : 0);
            for (std::size_t i = 0; i < visible; ++i)
            {
                const std::size_t game_index = first + i;
                const float x = 548.0F + static_cast<float>(i % 6) * 204.0F;
                const float y = 354.0F + static_cast<float>(i / 6) * 326.0F;
                const bool focused = game_focus && game_index == static_cast<std::size_t>(game_selected);
                const Rect cover{x, y, 180.0F, 270.0F};
                const std::uint32_t app_id = playden_ps4_library_visible_app_id(1000 + selected, game_index);
                const std::string title = playden_ps4_library_visible_title(1000 + selected, game_index);
                const auto game_id = playden::GameId::Steam(app_id);
                const std::string url = playden::homebrew::CoverUrl(game_id);
                const std::uint32_t texture = art.cache.TextureFor(game_id, url);
                list.rounded_rect({cover.x - 3.0F, cover.y - 3.0F, cover.w + 6.0F, cover.h + 6.0F},
                                  9.0F, focused ? Color::rgb(0xf0863a, 0.96F) : Color::rgb(0x292521));
                list.rounded_rect(cover, 7.0F, Color::rgb(0x292521));
                if (texture != 0)
                    list.image(texture, cover, kFullUv, Color::rgb(0xffffff), 7.0F);
                else
                {
                    list.gradient_rect(cover, 7.0F, Color::rgb(0x34302b), Color::rgb(0x1a1816));
                    list.rounded_rect({cover.x + 69.0F, cover.y + 128.0F, 42.0F, 4.0F}, 2.0F,
                                      Color::rgb(0xf0863a, 0.78F));
                }
                list.gradient_rect({cover.x, cover.y + cover.h - 58.0F, cover.w, 58.0F}, 7.0F,
                                   Color::rgb(0x0a0908, 0.0F), Color::rgb(0x0a0908, 0.90F));
                list.text(font, 0, font.fit(title, 16.0F, cover.w - 20.0F), cover.x + 10.0F,
                          cover.y + cover.h - 14.0F, 16.0F, ink);
                if (playden_ps4_library_is_favorite(app_id))
                    list.rounded_rect({cover.x + cover.w - 24.0F, cover.y + 8.0F, 14.0F, 14.0F},
                                      7.0F, accent);
                // Shared by a Steam family member: a label in the cover's top corner.
                if (playden_ps4_library_is_family(app_id) && font_ready)
                {
                    list.rounded_rect({cover.x + 6.0F, cover.y + 6.0F, 72.0F, 24.0F}, 6.0F, Color::rgb(0x0e0d0c, 0.82F));
                    list.text(font, 0, "FAMILY", cover.x + 42.0F, cover.y + 24.0F, 14, ink, Align::center);
                }
                // A newer version on Steam: a label across the cover's foot.
                if (playden_ps4_library_update_available(app_id) && font_ready)
                {
                    list.rounded_rect({cover.x + 6.0F, cover.y + cover.h - 32.0F, cover.w - 12.0F, 26.0F}, 6.0F,
                                      Color::rgb(0x0e0d0c, 0.82F));
                    list.text(font, 0, "UPDATE", cover.x + cover.w * 0.5F, cover.y + cover.h - 13.0F, 15, accent,
                              Align::center);
                }
                if (focused)
                    list.bordered_rect({cover.x - 4.0F, cover.y - 4.0F, cover.w + 8.0F, cover.h + 8.0F},
                                       10.0F, Color::rgb(0x000000, 0.0F), 3.0F, accent);
            }
            hint_text(list, font, game_focus ? "ARROWS  SELECT     CROSS  DETAILS     SQUARE  FAVORITE     TRIANGLE  SEARCH     "
                                               "R1  SORT     OPTIONS     CIRCLE  BACK" :
                      "CROSS  BROWSE GAMES     UP / DOWN  VIEWS     TRIANGLE  SEARCH     R1  SORT",
                      96, 970, 17, secondary);
        }
    }
    else if (page == 2)
    {
        list.rounded_rect({180, 180, 1560, 730}, 18, panel);
        if (font_ready)
        {
            list.text(font, 0, "GAME DETAILS", 260, 276, 22, accent);
            const std::uint32_t detail_app_id = playden_ps4_game_details_app_id();
            const char *detail_title = playden_ps4_library_title_by_app_id(detail_app_id);
            list.text(font, 0, detail_title[0] ? detail_title :
                      playden_ps4_library_visible_title(library_filter(tab, selected),
                                                        static_cast<std::size_t>(game_selected)),
                      530, 356, 44, ink);
            const Rect poster{260, 322, 220, 330};
            const playden::GameId detail_game = playden::GameId::Steam(detail_app_id);
            const std::uint32_t cover_texture = art.cache.TextureFor(
                detail_game, playden::homebrew::CoverUrl(detail_game));
            list.rounded_rect(poster, 8, Color::rgb(0x292521));
            if (cover_texture != 0)
                list.image(cover_texture, poster, kFullUv, Color::rgb(0xffffff), 8.0F);
            else
                list.gradient_rect(poster, 8, Color::rgb(0x34302b), Color::rgb(0x1a1816));
            const int details_state = playden_ps4_game_details_state();
            const bool installed = playden_ps4_game_details_installed() != 0;
            const bool incomplete = playden_ps4_game_details_incomplete() != 0;
            const bool can_launch = installed && playden_ps4_game_details_launch_supported();
            const char *details = playden_ps4_game_details_message();
            const int install_state = playden_ps4_game_install_state();
            const char *install = playden_ps4_game_install_message();
            const char *action_state = details_state != 2
                ? (details[0] ? details : "Checking Steam install options…")
                : install_state == 3 ? install
                : install_state == 1 ? install
                : install_state == 4 ? "QUEUED  ·  STARTS AFTER THE CURRENT DOWNLOAD"
                : incomplete ? "INCOMPLETE  ·  CROSS TO RESUME"
                : !installed ? "NOT INSTALLED  ·  CROSS TO INSTALL"
                : playden_ps4_game_details_update() ? "UPDATE AVAILABLE  ·  CROSS TO UPDATE"
                : can_launch && playden_ps4_game_details_app_id() == 220
                    ? "INSTALLED  ·  CROSS TO PLAY"
                : can_launch ? "INSTALLED  ·  CROSS TO TRY LAUNCH"
                : "INSTALLED  ·  NO USABLE WINDOWS LAUNCH";
            const Color action_color = install_state == 3 ? Color::rgb(0xff9c84) : accent;
            list.rounded_rect({260, 674, 220, 56}, 8, Color::rgb(0xf0863a, 0.12F));
            list.text(font, 0, incomplete ? "INCOMPLETE" : installed && playden_ps4_game_details_update() ? "UPDATE"
                      : installed ? "INSTALLED" : "NOT INSTALLED",
                      370, 709, 18, installed ? accent : incomplete ? Color::rgb(0xffc164) : secondary,
                      Align::center);
            hint_text(list, font, action_state, 530, 405, 21, action_color);
            // Maintenance has no other sign on this page: name its button under the badge.
            if (details_state == 2 && (installed || incomplete) && install_state != 1)
                hint_text(list, font, "OPTIONS  MAINTENANCE", 262, 770, 15, secondary);

            const char *developers = playden_ps4_game_store_developers();
            const char *publishers = playden_ps4_game_store_publishers();
            const char *release_date = playden_ps4_game_store_release_date();
            const char *metacritic = playden_ps4_game_store_metacritic_score();
            const char *genres = playden_ps4_game_store_genres();
            const char *categories = playden_ps4_game_store_categories();
            if (developers[0])
                list.text(font, 0, font.fit(std::string("DEVELOPER  ") + developers, 17, 1160),
                          530, 447, 17, secondary);
            if (publishers[0])
                list.text(font, 0, font.fit(std::string("PUBLISHER  ") + publishers, 17, 1160),
                          530, 475, 17, secondary);
            std::string release_line;
            if (release_date[0]) release_line = std::string("RELEASED  ") + release_date;
            if (metacritic[0]) {
                if (!release_line.empty()) release_line += "     ·     ";
                release_line += std::string("METACRITIC  ") + metacritic + "/100";
            }
            if (!release_line.empty())
                list.text(font, 0, font.fit(release_line, 17, 1160), 530, 503, 17, secondary);
            if (!playden_ps4_game_store_details_ready())
                list.text(font, 0, "LOADING STEAM STORE DETAILS…", 530, 552, 17, secondary);
            else if (!playden_ps4_game_store_details_available())
                list.text(font, 0, "Steam store details are unavailable.", 530, 552, 18, secondary);
            else
            {
                const char *description = playden_ps4_game_store_description();
                if (description[0])
                {
                    list.text(font, 0, "ABOUT THIS GAME", 530, 550, 17, accent);
                    const auto lines = font.wrap(description, 19.0F, 1160.0F);
                    const std::size_t line_count = std::min<std::size_t>(3, lines.size());
                    for (std::size_t line = 0; line < line_count; ++line)
                        list.text(font, 0, lines[line], 530, 580.0F + static_cast<float>(line) * 25.0F,
                                  19, secondary);
                }
                if (genres[0])
                    list.text(font, 0, font.fit(std::string("GENRES  ") + genres, 17, 1160),
                              530, 665, 17, secondary);
                if (categories[0])
                    list.text(font, 0, font.fit(std::string("FEATURES  ") + categories, 17, 1160),
                              530, 693, 17, secondary);
                const char *controller = playden_ps4_game_store_controller_support();
                std::string support = "WINDOWS  ";
                support += playden_ps4_game_store_windows_supported() ? "SUPPORTED" : "NOT LISTED";
                if (controller[0]) support += std::string("     ·     CONTROLLER  ") + controller;
                list.text(font, 0, font.fit(support, 17, 1160), 530, 721, 17, secondary);
            }
            list.text(font, 0, font.fit(std::string("STEAM APP ID  ") + std::to_string(detail_app_id), 15, 1160),
                      530, 755, 15, secondary);
            const int uninstall_state = playden_ps4_game_uninstall_state();
            const bool uninstall_for_game = playden_ps4_game_uninstall_app_id() ==
                                            playden_ps4_game_details_app_id();
            if (details_state == 2 && details[0])
                list.text(font, 0, font.fit(details, 15, 1160), 530, 780, 15, secondary);
            if (uninstall_for_game && uninstall_state != 0)
                list.text(font, 0, playden_ps4_game_uninstall_message(), 530, 808, 17,
                          uninstall_state == 3 ? Color::rgb(0xff9c84) : Color::rgb(0xffc477));
            if (runtime_status != nullptr && runtime_status[0] != '\0')
                list.text(font, 0, runtime_status, 530, 835, 17, Color::rgb(0xffc477));
            const char *detail_hint = details_state != 2 ? "CIRCLE  BACK TO LIBRARY" :
                uninstall_for_game && uninstall_state == 1 ? "REMOVING GAME FILES…" :
                install_state == 1 ? "INSTALL IN PROGRESS     LEFT / RIGHT  SECTIONS" :
                install_state == 4 ? "QUEUED: SEE THE QUEUE TAB     CIRCLE  BACK TO LIBRARY" :
                !installed ? "CROSS  INSTALL     CIRCLE  BACK TO LIBRARY" :
                installed && playden_ps4_game_details_update()
                    ? "CROSS  UPDATE     TRIANGLE  SETTINGS     OPTIONS  MAINTENANCE     SQUARE  DELETE     CIRCLE  BACK" :
                can_launch ? (playden_ps4_game_details_app_id() == 220
                                  ? "CROSS  PLAY     TRIANGLE  SETTINGS     OPTIONS  MAINTENANCE     SQUARE  DELETE     CIRCLE  BACK"
                                  : "CROSS  TRY LAUNCH     TRIANGLE  SETTINGS     OPTIONS  MAINTENANCE     SQUARE  DELETE     CIRCLE  BACK") :
                "TRIANGLE  SETTINGS     OPTIONS  MAINTENANCE     SQUARE  DELETE     CIRCLE  BACK TO LIBRARY";
            hint_text(list, font, detail_hint, 96, 970, 17, secondary);
        }
    }
    else if (page == 3)
    {
        list.rounded_rect({300, 210, 1320, 650}, 18, panel);
        if (font_ready)
        {
            const char *title = playden_ps4_library_title_by_app_id(playden_ps4_game_details_app_id());
            list.text(font, 0, "DELETE GAME", 390, 296, 24, accent);
            list.text(font, 0, font.fit(title, 40, 1140), 390, 360, 40, ink);
            const bool signed_in = auth_state == 6 || auth_state == 7;
            list.text(font, 0, signed_in ? "Steam Cloud saves are synced first."
                                         : "Not signed in: Steam Cloud saves are not synced first.",
                      390, 408, 19, secondary);
            list.text(font, 0, "Saves a game keeps in its own folder are deleted with it.", 390, 440, 19,
                      secondary);
            const char *choices[] = {"DELETE GAME, KEEP ITS WINE PREFIX AND SAVES",
                                     "DELETE GAME AND ITS WINE PREFIX (SAVES BACKED UP FIRST)", "KEEP GAME"};
            for (int i = 0; i < 3; ++i)
            {
                const Rect row{390, 478.0F + static_cast<float>(i) * 72.0F, 1140, 58};
                if (delete_choice == i)
                    list.bordered_rect(row, 8, Color::rgb(0xf0863a, 0.16F), 1.5F, accent);
                else
                    list.rounded_rect(row, 8, bg);
                list.text(font, 0, choices[i], 416, 515.0F + static_cast<float>(i) * 72.0F,
                          20, delete_choice == i ? ink : secondary);
            }
            const int stage = playden_ps4_delete_stage;
            const char *status = stage == 1 ? "Syncing Steam Cloud saves before deleting…"
                : stage >= 2 ? playden_ps4_maintenance_message() : playden_ps4_delete_note;
            if (status[0])
                list.text(font, 0, font.fit(status, 19, 1140), 390, 726, 19,
                          stage == 0 ? Color::rgb(0xffc477) : accent);
            hint_text(list, font, stage == 3 ? "REMOVING THE WINE PREFIX…" : stage == 1 ? "CIRCLE  CANCEL" :
                                  "UP / DOWN  CHOOSE     CROSS  CONFIRM     CIRCLE  CANCEL",
                      96, 970, 17, secondary);
        }
    }
    else if (page == 5)
    {
        list.rounded_rect({180, 180, 1560, 730}, 18, panel);
        if (font_ready)
        {
            const std::uint32_t app_id = playden_ps4_game_details_app_id();
            const char *title = playden_ps4_library_title_by_app_id(app_id);
            list.text(font, 0, "GAME SETTINGS", 260, 262, 22, accent);
            list.text(font, 0, font.fit(title[0] ? title : "Selected game", 34, 1300), 260, 314, 34, ink);
            const int rows = static_cast<int>(playden_ps4_controls_count());
            constexpr int kVisible = 8;
            const int first = std::max(0, std::min(delete_choice - kVisible / 2, rows - kVisible));
            for (int i = 0; i < kVisible && first + i < rows; ++i)
            {
                const int row = first + i;
                const float y = 352.0F + static_cast<float>(i) * 58.0F;
                const Rect box{260, y, 1400, 50};
                if (row == delete_choice)
                    list.bordered_rect(box, 8, Color::rgb(0xf0863a, 0.16F), 1.5F, accent);
                else
                    list.rounded_rect(box, 8, bg);
                list.text(font, 0, playden_ps4_controls_label(static_cast<std::size_t>(row)), 286, y + 33, 20,
                          row == delete_choice ? ink : secondary);
                const bool custom = playden_ps4_controls_custom(static_cast<std::size_t>(row)) != 0;
                std::string value = playden_ps4_controls_value(static_cast<std::size_t>(row));
                if (row == delete_choice) value = "<  " + value + "  >";
                list.text(font, 0, font.fit(value, 20, 640), 980, y + 33, 20, custom ? accent : ink);
            }
            if (rows > kVisible)
                list.text(font, 0, std::to_string(delete_choice + 1) + " / " + std::to_string(rows), 1560, 314, 17,
                          secondary);
            list.text(font, 0, font.fit(playden_ps4_controls_status(), 17, 1400), 260, 845, 17,
                      Color::rgb(0xffc477));
            if (playden_ps4_game_details_state() < 2)
                list.text(font, 0, "Checking the game with Steam…", 260, 880, 17, secondary);
            hint_text(list, font,
                      rows > 0 ? "UP / DOWN  CHOOSE     LEFT / RIGHT  CHANGE     L1 / R1  JUMP     SQUARE  DEFAULT     "
                                 "CROSS  REFRESH     CIRCLE  BACK"
                               : "CROSS  REFRESH     CIRCLE  BACK",
                      96, 970, 17, secondary);
        }
    }
    else if (page == 6)
    {
        list.rounded_rect({180, 180, 1560, 730}, 18, panel);
        if (font_ready)
        {
            const std::uint32_t app_id = playden_ps4_game_details_app_id();
            const char *title = playden_ps4_library_title_by_app_id(app_id);
            list.text(font, 0, "LIBRARY OVERRIDES", 260, 262, 22, accent);
            list.text(font, 0, font.fit(title[0] ? title : "Selected game", 34, 1300), 260, 314, 34, ink);
            // Choices, then "Type your own"; the count comes from the open picker (main.c keeps it).
            int choices = 0;
            while (choices < 64 && std::strcmp(playden_ps4_overrides_label(static_cast<std::size_t>(choices)),
                                                "Type your own…") != 0)
                ++choices;
            const int rows = choices + 1;
            constexpr int kVisible = 8;
            const int first = std::max(0, std::min(delete_choice - kVisible / 2, rows - kVisible));
            for (int i = 0; i < kVisible && first + i < rows; ++i)
            {
                const int row = first + i;
                const float y = 352.0F + static_cast<float>(i) * 58.0F;
                const Rect box{260, y, 1400, 50};
                if (row == delete_choice)
                    list.bordered_rect(box, 8, Color::rgb(0xf0863a, 0.16F), 1.5F, accent);
                else
                    list.rounded_rect(box, 8, bg);
                const bool custom_row = row == choices;
                const bool on = !custom_row && playden_ps4_overrides_checked(static_cast<std::size_t>(row)) != 0;
                std::string label = custom_row ? "Type your own…"
                                               : std::string(on ? "[x]  " : "[  ]  ") +
                                                     playden_ps4_overrides_label(static_cast<std::size_t>(row));
                list.text(font, 0, font.fit(label, 20, 1340), 286, y + 33, 20,
                          on ? accent : row == delete_choice ? ink : secondary);
            }
            list.text(font, 0, font.fit(std::string("Now: ") + playden_ps4_overrides_value(), 17, 1400), 260, 845,
                      17, secondary);
            list.text(font, 0, font.fit(playden_ps4_controls_status(), 17, 1400), 260, 875, 17,
                      Color::rgb(0xffc477));
            hint_text(list, font, "UP / DOWN  CHOOSE     CROSS  ON / OFF     CIRCLE  BACK", 96, 970, 17, secondary);
        }
    }
    else if (page == 8)
    {
        // The install sheet.
        list.rounded_rect({300, 190, 1320, 700}, 18, panel);
        if (font_ready)
        {
            const std::uint32_t app_id = playden_ps4_game_details_app_id();
            const char *title = playden_ps4_library_title_by_app_id(app_id);
            list.text(font, 0, "INSTALL", 390, 276, 24, accent);
            list.text(font, 0, font.fit(title[0] ? title : "Selected game", 40, 1140), 390, 344, 40, ink);
            const char *labels[] = {"Version", "Language"};
            for (int i = 0; i < 2; ++i)
            {
                const float y = 400.0F + static_cast<float>(i) * 70.0F;
                const Rect row{390, y, 1140, 56};
                if (delete_choice == i)
                    list.bordered_rect(row, 8, Color::rgb(0xf0863a, 0.16F), 1.5F, accent);
                else
                    list.rounded_rect(row, 8, bg);
                list.text(font, 0, labels[i], 416, y + 37, 21, delete_choice == i ? ink : secondary);
                std::string value = playden_ps4_install_sheet_value(i);
                if (delete_choice == i) value = "<  " + value + "  >";
                list.text(font, 0, font.fit(value, 21, 820), 680, y + 37, 21, ink);
            }
            list.text(font, 0, std::string("Download  ") + playden_ps4_install_sheet_value(2), 416, 600, 19,
                      secondary);
            list.text(font, 0, std::string("On the PS4  ") + playden_ps4_install_sheet_value(3), 416, 634, 19,
                      secondary);
            const Rect go{390, 670, 1140, 60};
            if (delete_choice == 2)
                list.bordered_rect(go, 8, Color::rgb(0xf0863a, 0.16F), 1.5F, accent);
            else
                list.rounded_rect(go, 8, bg);
            list.text(font, 0, "INSTALL", 416, 709, 21, delete_choice == 2 ? ink : secondary);
            const char *note = playden_ps4_install_sheet_note();
            if (note[0]) list.text(font, 0, font.fit(note, 17, 1140), 390, 780, 17, Color::rgb(0xffc477));
            else
                list.text(font, 0, "Sizes are Steam's estimate for the game's own files; DLC is added as owned.", 390,
                          780, 17, secondary);
            hint_text(list, font,
                      delete_choice < 2 ? "UP / DOWN  CHOOSE     LEFT / RIGHT  CHANGE     CIRCLE  BACK"
                                        : "UP / DOWN  CHOOSE     CROSS  INSTALL     CIRCLE  BACK",
                      96, 970, 17, secondary);
        }
    }
    else if (page == 7)
    {
        // Steam Cloud before play.
        list.rounded_rect({300, 210, 1320, 650}, 18, panel);
        if (font_ready)
        {
            const std::uint32_t app_id = playden_ps4_game_details_app_id();
            const char *title = playden_ps4_library_title_by_app_id(app_id);
            const int state = playden_ps4_saves_state();
            list.text(font, 0, playden_ps4_launching ? "STARTING" : "STEAM CLOUD SAVES", 390, 306, 24, accent);
            list.text(font, 0, font.fit(title[0] ? title : "Selected game", 40, 1140), 390, 378, 40, ink);
            if (playden_ps4_launching)
            {
                list.text(font, 0, "Setting up Steam and Windows for this game…", 390, 438, 21, secondary);
                list.text(font, 0, "The first launch after an install or a prefix reset copies Windows: about a minute.",
                          390, 590, 19, secondary);
            }
            else
                list.text(font, 0, font.fit(playden_ps4_saves_message(), 21, 1140), 390, 438, 21, secondary);
            if (playden_ps4_launching) {}
            else if (state == 3)
            {
                const char *choices[] = {"USE THE CLOUD SAVES, THEN PLAY", "USE THE PS4 SAVES, THEN PLAY",
                                         "PLAY WITHOUT SYNCING", "CANCEL"};
                for (int i = 0; i < 4; ++i)
                {
                    const Rect row{390, 500.0F + static_cast<float>(i) * 70.0F, 1140, 56};
                    if (delete_choice == i)
                        list.bordered_rect(row, 8, Color::rgb(0xf0863a, 0.16F), 1.5F, accent);
                    else
                        list.rounded_rect(row, 8, bg);
                    list.text(font, 0, choices[i], 416, 537.0F + static_cast<float>(i) * 70.0F, 21,
                              delete_choice == i ? ink : secondary);
                }
                hint_text(list, font, "UP / DOWN  CHOOSE     CROSS  CONFIRM     CIRCLE  BACK", 96, 970, 17, secondary);
            }
            else
            {
                const std::uint64_t total = playden_ps4_saves_progress_total();
                const std::uint64_t done = playden_ps4_saves_progress_done();
                const float fraction = total ? static_cast<float>(done) / static_cast<float>(total) : 0.0F;
                list.rounded_rect({390, 520, 1140, 14}, 7, bg);
                if (total) list.rounded_rect({390, 520, 1140.0F * std::min(fraction, 1.0F), 14}, 7, accent);
                list.text(font, 0, "The game starts when the saves agree.", 390, 590, 19, secondary);
                hint_text(list, font, "CIRCLE  CANCEL", 96, 970, 17, secondary);
            }
        }
    }
    else if (page == 4)
    {
        const int filter = library_filter(tab, selected);
        const std::uint32_t app_id = playden_ps4_library_visible_app_id(
            filter, static_cast<std::size_t>(std::max(game_selected, 0)));
        const char *title = playden_ps4_library_visible_title(
            filter, static_cast<std::size_t>(std::max(game_selected, 0)));
        list.rounded_rect({480, 250, 960, 570}, 16, panel);
        if (font_ready)
        {
            list.text(font, 0, "GAME ACTIONS", 560, 328, 24, accent);
            list.text(font, 0, title[0] ? title : "Selected game", 560, 382, 34, ink);
            const char *choices[] = {
                playden_ps4_library_is_favorite(app_id) ? "REMOVE FROM FAVORITES" : "ADD TO FAVORITES",
                playden_ps4_library_is_hidden(app_id) ? "UNHIDE GAME" : "HIDE GAME",
                "ADD TO COLLECTION…",
                "CANCEL"
            };
            for (int i = 0; i < 4; ++i)
            {
                const Rect row{560, 438.0F + static_cast<float>(i) * 76.0F, 800, 58};
                if (delete_choice == i)
                    list.bordered_rect(row, 8, Color::rgb(0xf0863a, 0.16F), 1.5F, accent);
                else
                    list.rounded_rect(row, 8, bg);
                list.text(font, 0, choices[i], 586, 476.0F + static_cast<float>(i) * 76.0F,
                          21, delete_choice == i ? ink : secondary);
            }
            hint_text(list, font, "UP / DOWN  CHOOSE     CROSS  SELECT     CIRCLE  BACK",
                      96, 970, 17, secondary);
        }
    }
    else if (page == 10)
    {
        // Maintenance for the game on the details screen.
        const std::uint32_t app_id = playden_ps4_game_details_app_id();
        list.rounded_rect({420, 190, 1080, 720}, 16, panel);
        if (font_ready)
        {
            const char *title = playden_ps4_library_title_by_app_id(app_id);
            list.text(font, 0, "MAINTENANCE", 500, 268, 24, accent);
            list.text(font, 0, font.fit(title[0] ? title : "Selected game", 34, 920), 500, 322, 34, ink);
            const char *choices[] = {"CHECK AND REPAIR FILES", "BACK UP SAVES", "RESTORE LAST BACKUP",
                                     playden_ps4_maintenance_armed ? "PRESS CROSS AGAIN TO RESET THE PREFIX"
                                                                   : "RESET WINE PREFIX", "CLEAR SHADER CACHE", "CLOSE"};
            const char *notes[] = {"Hashes every file; downloads only what is missing or changed.",
                                   "Copies the Wine prefix's user folders (Documents, AppData, Saved Games).",
                                   "Copies the newest backup back over the prefix's user folders.",
                                   "Backs up, then rebuilds Windows and the registry at the next launch. Saves stay.",
                                   "This game's DXVK cache and the shared GPU shader cache. They rebuild as you play.",
                                   ""};
            for (int i = 0; i < 6; ++i)
            {
                const float y = 362.0F + static_cast<float>(i) * 68.0F;
                const Rect row{500, y, 920, 60};
                if (delete_choice == i)
                    list.bordered_rect(row, 8, Color::rgb(0xf0863a, 0.16F), 1.5F, accent);
                else
                    list.rounded_rect(row, 8, bg);
                list.text(font, 0, choices[i], 526, y + (notes[i][0] ? 26.0F : 38.0F), 21,
                          i == 3 && playden_ps4_maintenance_armed ? accent : delete_choice == i ? ink : secondary);
                if (notes[i][0]) list.text(font, 0, notes[i], 526, y + 49, 15, secondary);
            }
            const int state = playden_ps4_maintenance_state();
            const char *message = playden_ps4_maintenance_message();
            if (state != 0 && message[0])
                list.text(font, 0, font.fit(message, 18, 920), 500, 778, 18,
                          state == 3 ? Color::rgb(0xffc477) : state == 2 ? accent : secondary);
            const char *backup = playden_ps4_maintenance_backup_label(app_id);
            list.text(font, 0, backup[0] ? backup : "No save backup yet.", 500, 812, 16, secondary);
            hint_text(list, font, state == 1 ? "WORKING…     CIRCLE  BACK" : "UP / DOWN  CHOOSE     CROSS  RUN     CIRCLE  BACK",
                      96, 970, 17, secondary);
        }
    }
    else if (page == 9)
    {
        // Collections for one game: tick to add or remove; the last row makes a new one.
        const std::uint32_t app_id = playden_ps4_collection_app_id;
        list.rounded_rect({480, 200, 960, 700}, 16, panel);
        if (font_ready)
        {
            const char *title = playden_ps4_library_title_by_app_id(app_id);
            list.text(font, 0, "COLLECTIONS", 560, 278, 24, accent);
            list.text(font, 0, font.fit(title[0] ? title : "Selected game", 34, 800), 560, 332, 34, ink);
            const int count = static_cast<int>(playden_ps4_library_collection_count());
            const int rows = count + 1;
            constexpr int kVisible = 6;
            const int first = std::max(0, std::min(delete_choice - kVisible / 2, rows - kVisible));
            for (int i = 0; i < kVisible && first + i < rows; ++i)
            {
                const int row = first + i;
                const float y = 380.0F + static_cast<float>(i) * 70.0F;
                const Rect box{560, y, 800, 56};
                if (row == delete_choice)
                    list.bordered_rect(box, 8, Color::rgb(0xf0863a, 0.16F), 1.5F, accent);
                else
                    list.rounded_rect(box, 8, bg);
                std::string label;
                bool on = false;
                if (row == count) label = "NEW COLLECTION…";
                else {
                    on = playden_ps4_library_in_collection(app_id, static_cast<std::size_t>(row)) != 0;
                    label = std::string(on ? "[x]  " : "[  ]  ") +
                            playden_ps4_library_collection_name(static_cast<std::size_t>(row));
                }
                list.text(font, 0, font.fit(label, 21, 640), 586, y + 36, 21,
                          on ? accent : row == delete_choice ? ink : secondary);
                if (row < count)
                {
                    const bool pinned = playden_ps4_library_collection_pinned(static_cast<std::size_t>(row)) != 0;
                    list.text(font, 0, (pinned ? std::string("ON HOME  ·  ") : std::string()) +
                                           std::to_string(playden_ps4_library_collection_size(static_cast<std::size_t>(row))),
                              1336, y + 36, 18, pinned ? accent : secondary, Align::right);
                }
            }
            if (count == 0)
                list.text(font, 0, "No collections yet. Make one, then add games to it.", 560, 850, 18, secondary);
            hint_text(list, font,
                      delete_choice < count ? "UP / DOWN  CHOOSE     CROSS  ADD / REMOVE     TRIANGLE  PIN TO HOME     SQUARE  DELETE     CIRCLE  BACK"
                                            : "UP / DOWN  CHOOSE     CROSS  NEW COLLECTION     CIRCLE  BACK",
                      96, 970, 17, secondary);
        }
    }
    else if (page == 0 && tab == 2)
    {
        list.rounded_rect({510, 200, 1304, 700}, 16, panel);
        if (font_ready)
        {
            const int state = playden_ps4_queue_state();
            const std::uint32_t app = playden_ps4_queue_app_id();
            const int waiting = static_cast<int>(playden_ps4_queue_waiting_count());
            const std::size_t updates = playden_ps4_update_count();
            const char *status = state == 1 ? "DOWNLOADING" :
                                 state == 2 ? "LAST DOWNLOAD COMPLETE" :
                                 state == 3 ? "LAST DOWNLOAD NEEDS ATTENTION" : "DOWNLOAD QUEUE";
            list.text(font, 0, status, 590, 262, 22, accent);
            // Row 0: the running (or last) job.
            {
                const Rect row{590, 284, 1144, 112};
                if (selected == 0 && state == 1)
                    list.bordered_rect(row, 9, Color::rgb(0xf0863a, 0.12F), 1.5F, accent);
                else
                    list.rounded_rect(row, 9, bg);
                const char *title = app ? playden_ps4_library_title_by_app_id(app) : "";
                list.text(font, 0, font.fit(title[0] ? title : "Nothing downloading", 24, 1100), 614, 326, 24, ink);
                const char *message = playden_ps4_queue_message();
                list.text(font, 0, font.fit(state == 0 ? "No download has been started this session." : message, 18, 1100),
                          614, 368, 18, secondary);
            }
            list.text(font, 0, waiting > 0 ? "WAITING" : "WAITING  ·  NONE", 590, 440, 20, secondary);
            constexpr int kVisible = 5;
            const int first = std::max(0, std::min(selected - 1 - kVisible / 2, waiting - kVisible));
            for (int i = 0; i < kVisible && first + i < waiting; ++i)
            {
                const int index = first + i;
                const float y = 456.0F + static_cast<float>(i) * 62.0F;
                const Rect row{590, y, 1144, 52};
                if (selected == index + 1)
                    list.bordered_rect(row, 8, Color::rgb(0xf0863a, 0.12F), 1.5F, accent);
                else
                    list.rounded_rect(row, 8, bg);
                const char *title = playden_ps4_library_title_by_app_id(
                    playden_ps4_queue_waiting_app(static_cast<std::size_t>(index)));
                list.text(font, 0, std::to_string(index + 1) + ".  " + (title[0] ? title : "Game"), 614, y + 34, 20,
                          selected == index + 1 ? ink : secondary);
            }
            if (updates > 0)
            {
                const Rect row{590, 786, 1144, 56};
                if (selected == waiting + 1)
                    list.bordered_rect(row, 8, Color::rgb(0xf0863a, 0.16F), 1.5F, accent);
                else
                    list.rounded_rect(row, 8, bg);
                list.text(font, 0, "UPDATE ALL  ·  " + std::to_string(updates) + (updates == 1 ? " GAME" : " GAMES"),
                          614, 822, 21, accent);
            }
            hint_text(list, font,
                      selected == 0 && state == 1 ? "UP / DOWN  CHOOSE     CROSS  CANCEL DOWNLOAD     LEFT / RIGHT  SECTIONS" :
                      selected >= 1 && selected <= waiting ? "UP / DOWN  CHOOSE     CROSS  REMOVE FROM QUEUE     LEFT / RIGHT  SECTIONS" :
                      selected == waiting + 1 && updates > 0 ? "UP / DOWN  CHOOSE     CROSS  QUEUE EVERY UPDATE     LEFT / RIGHT  SECTIONS" :
                      "UP / DOWN  CHOOSE     LEFT / RIGHT  SECTIONS",
                      96, 970, 17, secondary);
        }
    }
    else if (page == 0 && tab == 3)
    {
        list.rounded_rect({510, 272, 1304, 542}, 16, panel);
        if (font_ready)
        {
            const int state = playden_ps4_saves_state();
            const int installed_count = static_cast<int>(playden_ps4_library_visible_count(0));
            const std::uint32_t selected_app = installed_count > selected
                ? playden_ps4_library_visible_app_id(0, static_cast<std::size_t>(selected)) : 0;
            const bool operation_active = state == 1 || state == 3;
            const std::uint32_t saves_app = operation_active ? playden_ps4_saves_app_id() : selected_app;
            const bool selection_changed = !operation_active && playden_ps4_saves_app_id() != 0 &&
                                           saves_app != playden_ps4_saves_app_id();
            std::string game_title = installed_count > 0 ? "Select an installed game" : "No installed games";
            for (int i = 0; i < installed_count; ++i)
                if (playden_ps4_library_visible_app_id(0, static_cast<std::size_t>(i)) == saves_app)
                    game_title = playden_ps4_library_visible_title(0, static_cast<std::size_t>(i));
            list.text(font, 0, "STEAM CLOUD SAVES", 590, 350, 22, accent);
            const Rect target{590, 374, 660, 58};
            if (state != 3)
                list.bordered_rect(target, 8, Color::rgb(0xf0863a, 0.12F), 1.5F, accent);
            else
                list.rounded_rect(target, 8, bg);
            list.text(font, 0, game_title.c_str(), 614, 411, 22, ink);
            const char *save_message = installed_count == 0 ? "Install a game before checking Cloud saves." :
                selection_changed ? "Press Cross to check this game's Steam Cloud saves." :
                                    playden_ps4_saves_message();
            list.text(font, 0, save_message, 590, 462, 21, secondary);
            if (state == 3)
            {
                const char *choices[] = {"CHECK AGAIN", "USE LOCAL SAVES", "USE STEAM CLOUD SAVES"};
                for (int i = 0; i < 3; ++i)
                {
                    const Rect row{590, 492.0F + static_cast<float>(i) * 68.0F, 660, 54};
                    if (selected == i)
                        list.bordered_rect(row, 8, Color::rgb(0xf0863a, 0.12F), 1.5F, accent);
                    else
                        list.rounded_rect(row, 8, bg);
                    list.text(font, 0, choices[i], 614, 527.0F + static_cast<float>(i) * 68.0F,
                              20, selected == i ? ink : secondary);
                }
                list.text(font, 0, "Selected saves are backed up before changes.", 590, 712, 19, secondary);
            }
            else
            {
                list.bordered_rect({590, 520, 440, 64}, 9, Color::rgb(0xf0863a, 0.14F),
                                   1.8F, accent);
                const char *action = state == 1 ? "CHECKING / SYNCING…" : "CHECK AND SYNC";
                list.text(font, 0, action, 810, 560, 22, ink, Align::center);
                if (state == 1)
                {
                    const std::uint64_t done = playden_ps4_saves_progress_done();
                    const std::uint64_t total = playden_ps4_saves_progress_total();
                    if (total != 0)
                    {
                        const int percent = static_cast<int>(std::min<std::uint64_t>(100, done * 100 / total));
                        list.rounded_rect({590, 630, 900, 20}, 8, bg);
                        if (percent > 0)
                            list.rounded_rect({590, 630, std::max(12.0F, 900.0F * percent / 100.0F), 20}, 8, accent);
                        const auto mb = [](std::uint64_t bytes) { return bytes / (1024 * 1024); };
                        const std::string progress = "Downloaded " + std::to_string(mb(done)) + " / " +
                            std::to_string(mb(total)) + " MB  (" + std::to_string(percent) + "%)";
                        list.text(font, 0, progress.c_str(), 590, 684, 20, secondary);
                    }
                    else
                    {
                        list.text(font, 0, "Comparing local saves with Steam Cloud…", 590, 650, 20, secondary);
                    }
                }
            }
            hint_text(list, font, state == 3 ? "UP / DOWN  CHOOSE     CROSS  APPLY     LEFT / RIGHT  SECTIONS" :
                      state == 1 ? "LEFT / RIGHT  SECTIONS" :
                      installed_count > 1 ? "UP / DOWN  GAME     CROSS  CHECK     LEFT / RIGHT  SECTIONS" :
                                           "CROSS  CHECK / SYNC     LEFT / RIGHT  SECTIONS",
                      96, 970, 17, secondary);
        }
    }
    else if (page == 0 && tab == 4)
    {
        list.rounded_rect({510, 180, 1304, 730}, 16, panel);
        if (font_ready)
        {
            list.text(font, 0, "DOWNLOAD & RUNTIME SETTINGS", 590, 242, 24, accent);
            const char *labels[] = {"Content language", "Remote diagnostic logging",
                                    "Wine log level", "Wait watchdog traces", "Full GPU hang report",
                                    "Log receiver IP", "Playden update", "Steam account", "Download server"};
            PwUpdateDownload update{};
            pw_update_ps4_download_status(&update);
            char update_value[64];
            switch (update.state)
            {
            case PW_UPDATE_DOWNLOAD_READY:
                std::snprintf(update_value, sizeof(update_value), "%s · %llu MB", update.tag, update.total >> 20);
                break;
            case PW_UPDATE_DOWNLOAD_RUNNING:
                std::snprintf(update_value, sizeof(update_value), "%u%%",
                              update.total ? static_cast<unsigned>(update.done * 100 / update.total) : 0U);
                break;
            case PW_UPDATE_DOWNLOAD_DONE: std::snprintf(update_value, sizeof(update_value), "In /data/pkg"); break;
            case PW_UPDATE_DOWNLOAD_FAILED: std::snprintf(update_value, sizeof(update_value), "Failed: retry"); break;
            default:
                std::snprintf(update_value, sizeof(update_value), "%s", pw_update_ps4_notice() ? "Available" : "Up to date");
                break;
            }
            const char *debug_level = playden_ps4_wine_debug_level();
            const std::string values[] = {
                playden_ps4_content_language(),
                playden_ps4_logging_enabled() ? "ON" : "OFF",
                !debug_level || !debug_level[0] || std::string_view(debug_level) == "profile"
                    ? "PROFILE" : std::string_view(debug_level) == "quiet" ? "QUIET" : "DETAILED",
                playden_ps4_wait_watchdog_enabled() ? "ON" : "OFF",
                playden_ps4_gpu_hang_dump_enabled() ? "ON" : "OFF",
                playden_ps4_log_receiver_ip(), update_value,
                auth_state != 6 && auth_state != 7 ? std::string("Not signed in")
                : playden_ps4_signout_armed ? std::string("CROSS AGAIN TO SIGN OUT")
                : playden_ps4_auth_account_name()[0] ? std::string(playden_ps4_auth_account_name())
                                                     : std::string("Signed in"),
                playden_ps4_download_fastest() ? "FASTEST" : "STEAM'S ORDER"};
            for (int i = 0; i < 9; ++i)
            {
                const Rect row{590, 258.0F + static_cast<float>(i) * 62.0F, 900, 52};
                if (selected == i)
                    list.bordered_rect(row, 8, Color::rgb(0xf0863a, 0.12F), 1.5F, accent);
                else
                    list.rounded_rect(row, 8, bg);
                list.text(font, 0, labels[i], 614, 292.0F + static_cast<float>(i) * 62.0F,
                          22, selected == i ? ink : secondary);
                list.text(font, 0, values[i].c_str(), 1450, 292.0F + static_cast<float>(i) * 62.0F,
                          22, accent, Align::right);
            }
            hint_text(list, font,
                      selected == 6 ? (update.state == PW_UPDATE_DOWNLOAD_RUNNING ? "CROSS cancels the download." :
                                       update.state == PW_UPDATE_DOWNLOAD_DONE ? "Install it with GoldHEN's package installer." :
                                       update.state == PW_UPDATE_DOWNLOAD_READY || update.state == PW_UPDATE_DOWNLOAD_FAILED
                                           ? "CROSS downloads it to /data/pkg, checked against GitHub." :
                                       "Checked once at start on github.com/plastikman/playden-ps4.") :
                      selected == 8 ? "Fastest tests Steam's servers when a download starts and keeps the quickest." :
                      selected == 5 ? "CROSS opens the system keyboard. Saved IP applies when Wine starts." :
                      selected == 7 ? (auth_state == 6 || auth_state == 7
                                           ? "CROSS signs out. Installed games and saves stay; sign in again for any account."
                                           : "CROSS opens Steam sign-in: QR code, or account name and password.") :
                      "CROSS changes the selected setting. Runtime settings apply when Wine starts.",
                      590, 840, 17, secondary);
            const char *receiver_status = playden_ps4_log_receiver_status();
            list.text(font, 0, selected == 6 ? (update.detail[0] ? update.detail : "") :
                      selected == 5 && receiver_status ? receiver_status : "",
                      590, 866, 17, accent);
            list.text(font, 0, "Settings are saved in this title's settings.conf.", 590, 892, 17, secondary);
            hint_text(list, font, selected == 5 ? "UP / DOWN  SELECT     CROSS  EDIT IP / OPEN OSK     LEFT / RIGHT  SECTIONS" :
                      "UP / DOWN  SELECT     CROSS  CHANGE     LEFT / RIGHT  SECTIONS",
                      96, 970, 17, secondary);
        }
    }
    else if (page == 0)
    {
        const std::size_t shelf_count = playden_ps4_home_shelf_count();
        const std::size_t installed_count = shelf_count;
        const int queue_state = playden_ps4_queue_state();
        if (font_ready)
        {
            if (shelf_count == 0)
            {
                list.text(font, 0, "HOME", 96, 250, 24, accent);
                list.rounded_rect({96, 278, 1728, 360}, 16, panel);
                list.text(font, 0, "NOTHING HERE YET", 150, 425, 34, ink);
                list.text(font, 0, "Games you play, install or favorite show up here. Press Cross to browse the Library.",
                          150, 474, 21, secondary);
            }
            else
            {
                // Two shelves at a time; the focused one stays on screen.
                constexpr std::size_t kShelfCovers = 9;
                const std::size_t focus_shelf = game_focus
                    ? std::min<std::size_t>(static_cast<std::size_t>(std::max(playden_ps4_home_shelf, 0)), shelf_count - 1) : 0;
                const std::size_t first_shelf = focus_shelf > 0 ? focus_shelf - 1 : 0;
                for (std::size_t s = first_shelf; s < shelf_count && s < first_shelf + 2; ++s)
                {
                    const float top = 236.0F + static_cast<float>(s - first_shelf) * 352.0F;
                    const int filter = 3000 + static_cast<int>(s);
                    const std::size_t count = playden_ps4_library_visible_count(filter);
                    const bool shelf_focused = game_focus && s == focus_shelf;
                    std::string heading = playden_ps4_home_shelf_title(s);
                    if (heading == "DOWNLOADING" && queue_state == 1)
                        heading += std::string("  ·  ") + playden_ps4_queue_message();
                    list.text(font, 0, font.fit(heading, 22, 1728), 96, top, 22, shelf_focused ? accent : secondary);
                    const std::size_t selected_index = shelf_focused
                        ? static_cast<std::size_t>(std::max(game_selected, 0)) : 0;
                    const std::size_t first = selected_index >= kShelfCovers ? selected_index - kShelfCovers + 1 : 0;
                    const std::size_t visible = std::min(kShelfCovers, count - std::min(first, count));
                    for (std::size_t i = 0; i < visible; ++i)
                    {
                        const std::size_t index = first + i;
                        const float x = 96.0F + static_cast<float>(i) * 192.0F;
                        const Rect cover{x, top + 22, 172, 242};
                        const std::uint32_t app_id = playden_ps4_library_visible_app_id(filter, index);
                        const std::string title = playden_ps4_library_visible_title(filter, index);
                        const auto game_id = playden::GameId::Steam(app_id);
                        const std::uint32_t texture = art.cache.TextureFor(
                            game_id, playden::homebrew::CoverUrl(game_id));
                        const bool focused = shelf_focused && index == selected_index;
                        list.rounded_rect({cover.x - 4, cover.y - 4, cover.w + 8, cover.h + 8}, 11,
                                          focused ? accent : Color::rgb(0x292521));
                        list.rounded_rect(cover, 7, Color::rgb(0x292521));
                        if (texture != 0)
                            list.image(texture, cover, kFullUv, Color::rgb(0xffffff), 7);
                        else
                        {
                            list.gradient_rect(cover, 7, Color::rgb(0x34302b), Color::rgb(0x1a1816));
                            list.rounded_rect({cover.x + 62, cover.y + 119, 48, 4}, 2, accent);
                        }
                        list.gradient_rect({cover.x, cover.y + cover.h - 52, cover.w, 52}, 7,
                                           Color::rgb(0x0a0908, 0.0F), Color::rgb(0x0a0908, 0.90F));
                        list.text(font, 0, font.fit(title, 15, cover.w - 16), cover.x + 8,
                                  cover.y + cover.h - 12, 15, ink);
                        if (focused)
                            list.bordered_rect({cover.x - 5, cover.y - 5, cover.w + 10, cover.h + 10},
                                               12, Color::rgb(0, 0.0F), 3, accent);
                    }
                }
            }
            if (game_focus && installed_count > 0)
                hint_text(list, font, "ARROWS  SELECT     CROSS  OPEN     SQUARE  FAVORITE     OPTIONS     CIRCLE  BACK",
                          96, 970, 17, secondary);
            else
                hint_text(list, font, installed_count > 0
                              ? "CROSS  BROWSE HOME     LEFT / RIGHT  SECTIONS"
                              : "CROSS  BROWSE LIBRARY     LEFT / RIGHT  SECTIONS",
                          96, 970, 17, secondary);
        }
    }
    else
    {
        list.rounded_rect({180, 180, 1560, 640}, 18, panel);
        if (font_ready)
        {
            list.text(font, 0, "STEAM SIGN-IN", 260, 334, 42, ink);
            const char *prompt = playden_ps4_auth_prompt();
            const char *message = (auth_state == 1 || auth_state == 8) && prompt[0] ? prompt :
                                  auth_state == 1 ? "Contacting Steam securely…" :
                                  auth_state == 2 ? "Scan this code with the Steam mobile app." :
                                  auth_state == 3 ? "Steam sign-in complete for this session." :
                                  auth_state == 5 ? "Loading your Steam library…" :
                                  auth_state == 4 ? (auth_failure != nullptr && auth_failure[0] != '\0'
                                                        ? auth_failure
                                                        : "Sign-in failed. Check the console network.") :
                                                    "Press Circle to return to your library.";
            hint_text(list, font, message, 260, 390, 23, secondary);
            if (auth_state == 3)
                list.text(font, 0, "Steam is signed in. Loading your library; saved sign-in is a later step.",
                          260, 432, 20, secondary);
            if (auth_state == 2 && challenge_url != nullptr && challenge_url[0] != '\0')
            {
                const playden::homebrew::QrMatrix matrix =
                    playden::homebrew::encode_qr(challenge_url);
                const auto qr = playden::homebrew::qr_layout(matrix, 464);
                if (qr.module_pixels > 0)
                {
                    constexpr int x = 1180, y = 262;
                    const int left = x + qr.offset_pixels;
                    const int top = y + qr.offset_pixels;
                    list.rounded_rect({static_cast<float>(x), static_cast<float>(y), 464, 464}, 6,
                                      Color::rgb(0xffffff));
                    for (int row = 0; row < matrix.size(); ++row)
                        for (int col = 0; col < matrix.size(); ++col)
                            if (matrix.at(col, row))
                                list.rounded_rect({static_cast<float>(left + (col + 4) * qr.module_pixels),
                                                   static_cast<float>(top + (row + 4) * qr.module_pixels),
                                                   static_cast<float>(qr.module_pixels),
                                                   static_cast<float>(qr.module_pixels)},
                                                  0, Color::rgb(0x111111));
                }
            }
            if (auth_state == 2 || auth_state == 4 || auth_state == 0)
                list.text(font, 0, "No phone handy? Press Square to sign in with your account name and password.",
                          260, 470, 19, secondary);
            hint_text(list, font,
                      playden_ps4_auth_guard_code_wanted() ? "TRIANGLE  TYPE CODE     CIRCLE  CANCEL" :
                      auth_state == 8 || auth_state == 1 ? "CIRCLE  CANCEL" : "SQUARE  ACCOUNT NAME AND PASSWORD     CIRCLE  BACK",
                      96, 970, 17, secondary);
        }
    }
    art.cache.EndFrame();
    raster(list, font, art.textures, frame);
}
} // namespace

extern "C" void playden_ps4_draw_ui(std::uint32_t *frame, int tab, int selected, int page,
                                    int controller_ready, int auth_state, int game_focus,
                                    int game_selected, int delete_choice, const char *challenge_url,
                                    const char *auth_failure, const char *runtime_status)
{
    if (frame != nullptr)
        draw(frame, tab, selected, page, controller_ready, auth_state, game_focus,
             game_selected, delete_choice, challenge_url, auth_failure, runtime_status);
}

extern "C" int playden_ps4_ui_poll_artwork(void)
{
    auto &art = artwork();
    art.cache.EndFrame();
    return art.textures.ConsumeUpload() ? 1 : 0;
}

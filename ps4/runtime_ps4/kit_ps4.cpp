// Playden PS4 port - the bundled Orbis-Win as Playden's runtime kit.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// On the PS5 Playden admits an installed prospero-win kit by its manifest (runtime/kit_manifest.cpp),
// whose layout is the PS5 kit's: a title shell with its helper per variant, a runtime tree with the
// PS5's native modules. The PS4 package carries Orbis-Win itself and its build writes a smaller
// description of it (scripts/ps4-kit-view.py): the files Playden's package installer copies per game
// (the template prefix into the game's bottle, a few small files into its title folder), with their
// sizes, digests and modes. That is read here into the same VerifiedKitView.

#include "runtime_ps4/kit_ps4.hpp"

#include "runtime/artifact_hash.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <sys/stat.h>

namespace playden::ps4 {
namespace {

namespace json = playden::steam::json;

std::optional<std::string> ReadAll(const char *path, std::size_t limit)
{
    FILE *file = std::fopen(path, "rb");
    if (!file)
        return std::nullopt;
    std::string bytes;
    char chunk[65536];
    for (std::size_t count; (count = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) {
        bytes.append(chunk, count);
        if (bytes.size() > limit) {
            std::fclose(file);
            return std::nullopt;
        }
    }
    std::fclose(file);
    return bytes;
}

const json::Value *Text(const json::Value &object, const char *key)
{
    const auto *value = object.Find(key);
    return value && value->is_string() && !value->text().empty() ? value : nullptr;
}

bool IsDirectory(const std::string &path)
{
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool SafeRelative(const std::string &text)
{
    if (text.empty() || text.find('\\') != std::string::npos)
        return false;
    const std::filesystem::path path(text);
    if (path.is_absolute() || path.root_name() != std::filesystem::path{} || path.generic_string() != text)
        return false;
    for (const auto &part : path)
        if (part.empty() || part == "." || part == "..")
            return false;
    return true;
}

bool HexDigest(const std::string &text)
{
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](unsigned char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
    });
}

}  // namespace

std::optional<playden::homebrew::RuntimeKit> AdmitPs4Kit(std::string *reason)
{
    const auto fail = [&](const std::string &why) -> std::optional<playden::homebrew::RuntimeKit> {
        if (reason)
            *reason = "The bundled runtime is not usable: " + why;
        return std::nullopt;
    };
    const auto bytes = ReadAll(kKitView, 4 * 1024 * 1024);
    if (!bytes)
        return fail("its description is missing.");
    json::Limits limits;
    limits.max_bytes = 4 * 1024 * 1024;
    limits.max_values = 64 * 1024;
    const auto parsed = json::Parse(*bytes, limits);
    if (!parsed || !parsed->is_object())
        return fail("its description is not readable.");
    const auto &view_json = *parsed;
    const auto *schema = Text(view_json, "schema");
    const auto *kit_id = Text(view_json, "kit_id");
    const auto *files_sha = Text(view_json, "files_sha256");
    const auto *runtime = view_json.Find("runtime");
    const auto *bottle = view_json.Find("template");
    const auto *recipe = view_json.Find("recipe");
    const auto *variants = view_json.Find("variants");
    const auto *files = view_json.Find("files");
    const auto *capabilities = view_json.Find("capabilities");
    if (!schema || schema->text() != "playden-ps4-kit-view/1" || !kit_id || !files_sha || !runtime ||
        !runtime->is_object() || !bottle || !bottle->is_object() || !recipe || !recipe->is_object() || !variants ||
        !variants->is_array() || !files || !files->is_array() || !capabilities || !capabilities->is_array())
        return fail("its description is incomplete.");

    playden::runtime::VerifiedKitView view;
    view.root = kKitRoot;
    view.kit_id = kit_id->text();
    view.files_sha256 = files_sha->text();
    const auto *bundle = Text(*runtime, "bundle_id");
    const auto *revision = Text(*runtime, "artifact_revision");
    const auto *runtime_path = Text(*runtime, "path");
    const auto *template_path = Text(*bottle, "path");
    const auto *drive_table = Text(*bottle, "drive_table");
    const auto *recipe_id = Text(*recipe, "id");
    const auto *recipe_version = Text(*recipe, "version");
    const auto *recipe_path = Text(*recipe, "path");
    const auto *recipe_sha = Text(*recipe, "sha256");
    if (!bundle || !revision || !runtime_path || !template_path || !drive_table || !recipe_id || !recipe_version ||
        !recipe_path || !recipe_sha)
        return fail("its runtime, template or recipe is not named.");
    view.runtime = {bundle->text(), revision->text(), runtime_path->text()};
    view.template_path = template_path->text();
    view.template_drive_table = drive_table->text();
    view.recipe = {recipe_id->text(), recipe_version->text(), recipe_path->text(), recipe_sha->text()};
    for (const auto &item : variants->array()) {
        const auto *id = item.is_object() ? Text(item, "id") : nullptr;
        const auto *title = item.is_object() ? Text(item, "title_id") : nullptr;
        const auto *path = item.is_object() ? Text(item, "path") : nullptr;
        const auto *any = item.is_object() ? item.Find("any_title") : nullptr;
        if (!id || !title || !path)
            return fail("a title variant is malformed.");
        view.variants.push_back({id->text(), title->text(), path->text(), any && any->is_bool() && any->boolean()});
    }
    for (const auto &item : files->array()) {
        const auto *path = item.is_object() ? Text(item, "path") : nullptr;
        const auto *sha = item.is_object() ? Text(item, "sha256") : nullptr;
        const auto *size = item.is_object() ? item.Find("size") : nullptr;
        const auto *mode = item.is_object() ? item.Find("mode") : nullptr;
        const auto parsed_size = size ? size->AsUint64() : std::nullopt;
        const auto parsed_mode = mode ? mode->AsUint64() : std::nullopt;
        if (!path || !sha || !parsed_size || !parsed_mode || *parsed_mode > 07777)
            return fail("its file list is malformed.");
        view.files.push_back({path->text(), *parsed_size, sha->text(), static_cast<unsigned>(*parsed_mode)});
    }
    if (view.files.empty() || view.variants.empty())
        return fail("it lists no files or title variants.");
    if (!IsDirectory(std::string(kKitRoot) + "/" + view.template_path))
        return fail("its template prefix is missing.");

    // Match the PS5 kit admission contract: the manifest's hashes identify its version; startup
    // checks sizes and modes without rehashing hundreds of megabytes of bundled Wine files.
    // The package installer checks the source again before each copy.
    std::set<std::string> paths;
    std::string inventory;
    for (const auto &file : view.files) {
        if (!SafeRelative(file.path) || !HexDigest(file.sha256) || !paths.insert(file.path).second)
            return fail("its file inventory contains an invalid path or digest.");
        const auto source = std::filesystem::path(kKitRoot) / file.path;
        struct stat info {};
        if (::lstat(source.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
            static_cast<std::uint64_t>(info.st_size) != file.size ||
            (static_cast<unsigned>(info.st_mode) & 07777u) != file.mode)
            return fail("a bundled runtime file differs from its description: " + file.path);
        char mode[8];
        std::snprintf(mode, sizeof(mode), "%04o", file.mode);
        inventory += file.path + "\t" + std::to_string(file.size) + "\t" + file.sha256 + "\t" + mode + "\n";
    }
    if (playden::runtime::HashArtifactBytes(inventory) != view.files_sha256)
        return fail("its file inventory digest does not match.");

    playden::homebrew::RuntimeKit kit;
    kit.view = std::move(view);
    for (const auto &item : capabilities->array())
        if (item.is_string())
            kit.capabilities.push_back(item.text());
    const auto view_sha256 = playden::runtime::HashArtifactFile(kKitView);
    if (!view_sha256)
        return fail("its description could not be hashed.");
    kit.manifest_sha256 = *view_sha256;
    kit.expected_manifest_sha256 = *view_sha256;
    kit.manifest_path = kKitView;
    kit.prospero_win_revision = revision->text();
    return kit;
}

}  // namespace playden::ps4

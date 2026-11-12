/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_title_publication_native.h"
#if defined(PLAYDEN_PLATFORM_PS4) && PLAYDEN_PLATFORM_PS4
#define PW_MANAGED_TITLE_ROOT "/data/PLDN10001"
#else
#define PW_MANAGED_TITLE_ROOT "/data/homebrew"
#endif

#include "../src/pw_profile_catalog.h"
#include "../src/pw_publication_sha256.h"
#include "../src/pw_title_publication.h"
#include "../src/pw_wine_launch.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

enum { USER_REG_MAX = 32 * 1024 * 1024 };

typedef struct FileIdentity {
    uint64_t device, inode;
    int64_t size, modified, modified_nsec, changed, changed_nsec;
    mode_t mode;
    uint64_t links;
    uint32_t uid;
} FileIdentity;

static int failure(char *error, size_t capacity, const char *message)
{
    if (error && capacity) {
        size_t length = strlen(message);
        if (length >= capacity) length = capacity - 1;
        memcpy(error, message, length); error[length] = 0;
    }
    return PW_TITLE_PUBLICATION_ADMISSION_INVALID;
}

static int ascii_equal(const char *left, const char *right)
{
    while (*left && *right) {
        unsigned a = (unsigned char)*left++, b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return !*left && !*right;
}

static int canonical(const char *value)
{
    const unsigned char *p = (const unsigned char *)value;
    if (!p || *p != '/' || p[1] == '/') return 0;
    while (*p) {
        const unsigned char *start;
        if (*p < 0x20 || *p > 0x7e || *p == '\\') return 0;
        if (*p++ != '/') continue;
        start = p;
        while (*p && *p != '/') {
            if (*p < 0x20 || *p > 0x7e || *p == '\\') return 0;
            ++p;
        }
        if (p == start || (p - start == 1 && start[0] == '.') ||
            (p - start == 2 && start[0] == '.' && start[1] == '.')) return 0;
    }
    return 1;
}

static int host_root_valid(const char *root)
{
    size_t length;
    if (!root || !root[0]) return 1;
    length = strlen(root);
    return canonical(root) && (length == 1 || root[length - 1] != '/');
}

static int mapped_path(const PwTitlePublicationPlatform *platform, const char *console,
                       char *output, size_t capacity)
{
    const char *root;
    size_t a, b;
    if (!platform || !canonical(console) || !host_root_valid(platform->host_root)) return -1;
    root = platform->host_root && platform->host_root[0] ? platform->host_root : "";
    a = strlen(root); b = strlen(console);
    if (a + b + 1 > capacity) return -1;
    memcpy(output, root, a); memcpy(output + a, console, b + 1);
    return 0;
}

static FileIdentity identity(const struct stat *info)
{
    FileIdentity result;
    result.device = (uint64_t)info->st_dev; result.inode = (uint64_t)info->st_ino;
    result.size = (int64_t)info->st_size;
#if defined(__APPLE__)
    result.modified = (int64_t)info->st_mtimespec.tv_sec;
    result.modified_nsec = (int64_t)info->st_mtimespec.tv_nsec;
    result.changed = (int64_t)info->st_ctimespec.tv_sec;
    result.changed_nsec = (int64_t)info->st_ctimespec.tv_nsec;
#else
    result.modified = (int64_t)info->st_mtim.tv_sec;
    result.modified_nsec = (int64_t)info->st_mtim.tv_nsec;
    result.changed = (int64_t)info->st_ctim.tv_sec;
    result.changed_nsec = (int64_t)info->st_ctim.tv_nsec;
#endif
    result.mode = info->st_mode; result.links = (uint64_t)info->st_nlink;
    result.uid = (uint32_t)info->st_uid;
    return result;
}

static int same_identity(FileIdentity a, FileIdentity b)
{
    return a.device == b.device && a.inode == b.inode && a.size == b.size &&
           a.modified == b.modified && a.modified_nsec == b.modified_nsec &&
           a.changed == b.changed && a.changed_nsec == b.changed_nsec && a.mode == b.mode &&
           a.links == b.links && a.uid == b.uid;
}

static int no_symlink_ancestors(const char *path)
{
    char copy[PW_TITLE_PUBLICATION_NATIVE_PATH * 2];
    size_t length = strlen(path);
    if (!length || length >= sizeof(copy)) return 0;
    memcpy(copy, path, length + 1);
    for (char *p = copy + 1; ; ++p) {
        struct stat info;
        if (*p && *p != '/') continue;
        char saved = *p; *p = 0;
        if (lstat(copy, &info) || S_ISLNK(info.st_mode) || (!saved && !S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))) {
            *p = saved; return 0;
        }
        *p = saved;
        if (!saved) break;
    }
    return 1;
}

static int checked_directory(const PwTitlePublicationPlatform *platform,
                             const char *console, mode_t wanted, FileIdentity *output)
{
    char path[PW_TITLE_PUBLICATION_NATIVE_PATH * 2]; struct stat info;
    if (mapped_path(platform, console, path, sizeof(path)) || !no_symlink_ancestors(path) ||
        lstat(path, &info) || !S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode) ||
        (info.st_mode & 07777) != wanted || info.st_uid != (uid_t)platform->owner_uid) return 0;
    if (output) *output = identity(&info);
    return 1;
}

/* unowned: the game folder's filesystem records no owner or permissions
 * (exFAT), so any owner and mode are accepted; type and symlinks still count. */
static int checked_approved_directory(const PwTitlePublicationPlatform *platform,
                                      const char *console, int unowned, FileIdentity *output)
{
    char path[PW_TITLE_PUBLICATION_NATIVE_PATH * 2]; struct stat info; mode_t mode;
    if (mapped_path(platform, console, path, sizeof(path)) || !no_symlink_ancestors(path) ||
        lstat(path, &info) || !S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode) ||
        (!unowned && info.st_uid != (uid_t)platform->owner_uid)) return 0;
    mode = info.st_mode & 07777;
    if (!unowned && mode != 0700 && mode != 0755) return 0;
    if (output) *output = identity(&info);
    return 1;
}

static int directory_unchanged(const PwTitlePublicationPlatform *platform,
                               const char *console, mode_t wanted, FileIdentity before)
{
    FileIdentity after;
    return checked_directory(platform, console, wanted, &after) && same_identity(before, after);
}

/* read_regular file policies: private files are 0600; legacy ones may be 0644;
 * a file in an unowned game folder may have any owner and mode. */
enum { READ_PRIVATE = 0, READ_LEGACY = 1, READ_GAME_UNOWNED = 2 };

static int admitted_mode(mode_t actual, int legacy)
{
    mode_t permission = actual & 07777;
    return legacy == READ_GAME_UNOWNED || permission == 0600 || (legacy && permission == 0644);
}

static uint8_t *read_regular(const PwTitlePublicationPlatform *platform, const char *console,
                             size_t maximum, int legacy_mode, size_t *length)
{
    char path[PW_TITLE_PUBLICATION_NATIVE_PATH * 2]; struct stat before, opened, after, post;
    uint8_t *bytes = NULL; size_t used = 0; int fd = -1;
    if (!length || mapped_path(platform, console, path, sizeof(path)) ||
        !no_symlink_ancestors(path) || lstat(path, &before) || !S_ISREG(before.st_mode) ||
        S_ISLNK(before.st_mode) ||
        (legacy_mode != READ_GAME_UNOWNED && before.st_uid != (uid_t)platform->owner_uid) ||
        before.st_nlink != 1 || !admitted_mode(before.st_mode, legacy_mode) ||
        before.st_size < 0 || (uint64_t)before.st_size > maximum) return NULL;
    fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &opened) || !same_identity(identity(&before), identity(&opened))) goto done;
    bytes = malloc((size_t)before.st_size + 1u);
    if (!bytes) goto done;
    while (used < (size_t)before.st_size) {
        ssize_t count = read(fd, bytes + used, (size_t)before.st_size - used);
        if (count <= 0) goto done;
        used += (size_t)count;
    }
    {
        uint8_t extra;
        if (read(fd, &extra, 1) != 0) goto done;
    }
    if (fstat(fd, &after) || lstat(path, &post) ||
        !same_identity(identity(&opened), identity(&after)) ||
        !same_identity(identity(&opened), identity(&post))) goto done;
    bytes[used] = 0; *length = used; close(fd); return bytes;
done:
    if (fd >= 0) close(fd); free(bytes); return NULL;
}

static int digest_hex(const uint8_t *bytes, size_t length, char output[65])
{
    static const char digits[] = "0123456789abcdef"; uint8_t raw[32];
    if (pw_publication_sha256_bytes(bytes, length, raw)) return -1;
    for (size_t i = 0; i < sizeof(raw); ++i) {
        output[i * 2] = digits[raw[i] >> 4]; output[i * 2 + 1] = digits[raw[i] & 15];
    }
    output[64] = 0; return 0;
}

static int title_id_valid(const char *text)
{
    if (!text || strlen(text) != 9) return 0;
    for (size_t i = 0; i < 9; ++i)
        if (i < 4 ? text[i] < 'A' || text[i] > 'Z' : text[i] < '0' || text[i] > '9') return 0;
    return 1;
}

void pw_title_publication_lease_init(PwTitlePublicationLease *lease)
{
    if (!lease) return;
    memset(lease, 0, sizeof(*lease)); lease->lock_fd = -1;
}

void pw_title_publication_close(PwTitlePublicationLease *lease)
{
    if (!lease) return;
    if (lease->lock_fd >= 0) close(lease->lock_fd);
    pw_title_publication_lease_init(lease);
}

int pw_title_publication_descriptor_parse(const uint8_t *bytes, size_t length,
                                          const char *expected_title_id,
                                          PwTitlePublicationDescriptor *descriptor,
                                          char *error, size_t error_size)
{
    PwTitlePublicationDescriptor candidate;
    if (!bytes || !descriptor || !title_id_valid(expected_title_id) ||
        !length || length > sizeof(candidate.bytes))
        return failure(error, error_size, "invalid pre-grant descriptor");
    memset(&candidate, 0, sizeof(candidate));
    if (pw_title_launch_parse((const char *)bytes, length, expected_title_id,
                              &candidate.launch, error, error_size) != PW_OK)
        return PW_TITLE_PUBLICATION_ADMISSION_INVALID;
    if (snprintf(candidate.canonical_path, sizeof(candidate.canonical_path),
                 PW_MANAGED_TITLE_ROOT "/%s/game-launch.json", expected_title_id) <= 0)
        return failure(error, error_size, "descriptor path overflow");
    memcpy(candidate.bytes, bytes, length); candidate.length = length;
    *descriptor = candidate; return PW_OK;
}

int pw_title_publication_read_descriptor(const char *canonical_path,
                                         const char *expected_title_id,
                                         const PwTitlePublicationPlatform *platform,
                                         PwTitlePublicationDescriptor *descriptor,
                                         char *error, size_t error_size)
{
    char expected[PW_TITLE_PUBLICATION_NATIVE_PATH]; size_t length = 0; uint8_t *bytes;
    if (!canonical_path || !platform || !descriptor || !title_id_valid(expected_title_id) ||
        snprintf(expected, sizeof(expected), PW_MANAGED_TITLE_ROOT "/%s/game-launch.json", expected_title_id) <= 0 ||
        strcmp(canonical_path, expected)) return failure(error, error_size, "noncanonical descriptor path");
    bytes = read_regular(platform, canonical_path, PW_TITLE_LAUNCH_MAX_BYTES, 0, &length);
    if (!bytes) return failure(error, error_size, "descriptor read refused");
    int result = pw_title_publication_descriptor_parse(bytes, length, expected_title_id,
                                                       descriptor, error, error_size);
    free(bytes); return result;
}

static int append_path(char *output, size_t capacity, const char *base, const char *tail)
{
    int count = snprintf(output, capacity, "%s/%s", base, tail);
    return count > 0 && (size_t)count < capacity && canonical(output) ? 0 : -1;
}

static int append_identifier_path(char *output, size_t capacity, const char *base,
                                  const char *middle, const char *identifier, const char *suffix)
{
    for (const char *p = identifier; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' )) return -1;
    int count = snprintf(output, capacity, "%s/%s%s%s", base, middle, identifier, suffix);
    return count > 0 && (size_t)count < capacity && canonical(output) ? 0 : -1;
}

static int receipt_matches(const PwTitlePublication *r, const PwTitleLaunch *l)
{
    return
        !strcmp(r->install_id, l->install_id) && !strcmp(r->title_id, l->title_id) &&
        !strcmp(r->runtime_bundle_id, l->runtime_bundle_id) &&
        !strcmp(r->recipe_id, l->recipe_id) && !strcmp(r->recipe_version, l->recipe_version) &&
        r->settings_revision == l->settings_revision;
}

static int metadata_matches(const PwTitlePublicationMetadata *m,
                            const PwTitlePublication *r, const PwTitleLaunch *l)
{
    return (m->schema_version == 2 || m->schema_version == 3) && !strcmp(m->install_id, l->install_id) &&
        !strcmp(m->title_id, l->title_id) && !strcmp(m->data_root, l->data_root) &&
        !strcmp(m->profile_id, l->default_profile_id) &&
        !strcmp(m->runtime_bundle_id, l->runtime_bundle_id) &&
        !strcmp(m->recipe_id, l->recipe_id) && !strcmp(m->recipe_version, l->recipe_version) &&
        !strcmp(m->kit_id, r->kit_id) && m->settings_revision == l->settings_revision &&
        /* A game found on a USB drive is on exFAT, which records no owners. */
        (l->schema_version != 3 || m->game_root_unowned);
}

/* Where a USB game is. The token is the mount without its number. */
enum { USB_MOUNTS = 8 };
static const char usb_token[] = "/mnt/usb";

/* A token path on the drive that was found; any other path as it is. */
static int game_path(const char *token_path, const char *mount, char *output, size_t capacity)
{
    int used = mount[0] ? snprintf(output, capacity, "%s%s", mount, token_path + sizeof(usb_token) - 1)
                        : snprintf(output, capacity, "%s", token_path);
    return used < 0 || (size_t)used >= capacity ? -1 : 0;
}

/* Which drive holds the program: 0 with the mount, or DRIVE_MISSING or
 * DRIVE_AMBIGUOUS. Only a regular file with no link on the way to it counts. */
static int find_game_mount(const PwTitlePublicationPlatform *platform, const char *executable,
                           char mount[PW_TITLE_PUBLICATION_MOUNT], char *error, size_t error_size)
{
    char found[USB_MOUNTS * PW_TITLE_PUBLICATION_MOUNT] = ""; unsigned matches = 0;
    for (unsigned number = 0; number < USB_MOUNTS; ++number) {
        char candidate[PW_TITLE_PUBLICATION_MOUNT], console[1024], path[2048]; struct stat info;
        snprintf(candidate, sizeof(candidate), "%s%c", usb_token, (char)('0' + number));
        if (game_path(executable, candidate, console, sizeof(console)) ||
            mapped_path(platform, console, path, sizeof(path)) || !no_symlink_ancestors(path) ||
            lstat(path, &info) || !S_ISREG(info.st_mode)) continue;
        if (!matches++) strcpy(mount, candidate);
        strcat(found, " "); strcat(found, candidate);
    }
    if (matches == 1) return PW_OK;
    if (error && error_size) {
        if (matches) snprintf(error, error_size, "game is on more than one USB drive:%s", found);
        else snprintf(error, error_size, "game is on none of %s0 to %s%d", usb_token, usb_token, USB_MOUNTS - 1);
    }
    mount[0] = 0;
    return matches ? PW_TITLE_PUBLICATION_ADMISSION_DRIVE_AMBIGUOUS : PW_TITLE_PUBLICATION_ADMISSION_DRIVE_MISSING;
}

static int hex_digit(unsigned value);

static int wine_space(unsigned value)
{
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
           value == '\v' || value == '\f';
}

/* ASCII identity projection of retained Wine 490f6d5 server/unicode.c
 * parse_strW. Non-ASCII codepoints remain a sentinel and therefore cannot
 * alias an owned ASCII key. */
typedef struct WineString {
    uint16_t units[1200];
    size_t length;
} WineString;

static int wine_parse_string(const uint8_t *begin, const uint8_t *end, unsigned delimiter,
                             WineString *output, const uint8_t **after)
{
    size_t used = 0;
    while (begin < end && *begin != delimiter) {
        unsigned value = *begin++; int normal = 1;
        if (value == '\\') {
            if (begin == end) return -1;
            value = *begin++;
            if (value == 'a') { value = '\a'; normal = 0; }
            else if (value == 'b') { value = '\b'; normal = 0; }
            else if (value == 'e') { value = 0x1b; normal = 0; }
            else if (value == 'f') { value = '\f'; normal = 0; }
            else if (value == 'n') { value = '\n'; normal = 0; }
            else if (value == 'r') { value = '\r'; normal = 0; }
            else if (value == 't') { value = '\t'; normal = 0; }
            else if (value == 'v') { value = '\v'; normal = 0; }
            else if (value == 'x') {
                unsigned code = 0;
                unsigned digits = 0;
                while (begin < end && digits < 4) {
                    int digit = hex_digit(*begin);
                    if (digit < 0) break;
                    ++begin; ++digits;
                    code = code * 16u + (unsigned)digit;
                }
                value = digits ? code : 'x';
                normal = 0;
            } else if (value >= '0' && value <= '7') {
                unsigned code = value - '0', digits = 1;
                while (begin < end && digits < 3 && *begin >= '0' && *begin <= '7') {
                    code = code * 8u + (*begin++ - '0'); ++digits;
                }
                value = code;
                normal = 0;
            } /* Wine drops the slash for every other escape. */
        }
        if (normal && value >= 0x80) {
            static const uint8_t lengths[128] = {
                [0x42 ... 0x5f] = 1, [0x60 ... 0x6f] = 2, [0x70 ... 0x74] = 3,
            };
            static const uint8_t masks[4] = { 0x7f, 0x1f, 0x0f, 0x07 };
            static const uint32_t minimum[4] = { 0, 0x80, 0x800, 0x10000 };
            unsigned extra = lengths[value - 0x80], decoded = value & masks[extra];
            int valid = extra != 0;
            for (unsigned i = extra; valid && i; --i) {
                if (begin == end || ((*begin ^ 0x80u) >= 0x40u)) valid = 0;
                else decoded = (decoded << 6) | (*begin++ ^ 0x80u);
            }
            if (!valid || decoded < minimum[extra] || decoded > 0x10ffff) continue;
            if (decoded > 0xffff) {
                decoded -= 0x10000;
                if (used + 2 > sizeof(output->units) / sizeof(output->units[0])) return -1;
                output->units[used++] = (uint16_t)(0xd800 | (decoded >> 10));
                output->units[used++] = (uint16_t)(0xdc00 | (decoded & 0x3ff));
                continue;
            }
            value = decoded;
        }
        if (used == sizeof(output->units) / sizeof(output->units[0])) return -1;
        output->units[used++] = (uint16_t)value;
    }
    if (begin == end || *begin != delimiter) return -1;
    output->length = used; if (after) *after = begin + 1; return 0;
}

static int wine_equal_ascii(const WineString *decoded, const char *ascii)
{
    size_t length = strlen(ascii);
    if (decoded->length != length) return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned left = decoded->units[i], right = (unsigned char)ascii[i];
        if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
        if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
        if (left != right) return 0;
    }
    return 1;
}

static int wine_equal_ascii_sensitive(const WineString *decoded, const char *ascii)
{
    size_t length = strlen(ascii);
    if (decoded->length != length) return 0;
    for (size_t i = 0; i < length; ++i)
        if (decoded->units[i] != (unsigned char)ascii[i]) return 0;
    return 1;
}

static int wine_root_path(const WineString *decoded)
{
    if (!decoded->length || !decoded->units[0]) return 1;
    for (size_t i = 0; i < decoded->length; ++i)
        if (decoded->units[i] != '\\') return 0;
    return 1;
}

static int wine_path_equal_ascii(const WineString *decoded, const char *ascii)
{
    WineString canonical = *decoded;
    if (canonical.length && !canonical.units[0]) return 0;
    if (canonical.length && canonical.units[canonical.length - 1] == '\\') --canonical.length;
    return wine_equal_ascii(&canonical, ascii);
}

static int wine_path_ambiguous_ascii(const WineString *decoded, const char *ascii)
{
    WineString canonical = *decoded; size_t trailing = 0;
    if (canonical.length && !canonical.units[0]) return 0;
    while (canonical.length && canonical.units[canonical.length - 1] == '\\') {
        --canonical.length; ++trailing;
    }
    return trailing > 1 && wine_equal_ascii(&canonical, ascii);
}

typedef struct RegistryFound {
    unsigned sections;
    unsigned assignments;
    PwTitlePublicationRegistryKind kind;
    WineString string_value;
    uint32_t dword;
} RegistryFound;

static int hex_digit(unsigned value)
{
    if (value >= '0' && value <= '9') return (int)(value - '0');
    if (value >= 'a' && value <= 'f') return (int)(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return (int)(value - 'A' + 10);
    return -1;
}

static int registry_semantics(const uint8_t *bytes, size_t length,
                              const PwTitlePublicationMetadata *metadata,
                              const PwGameProfile *profile)
{
    char exe[PW_APP_PATH_CAPACITY], owned0[1200]; const char *base;
    RegistryFound found[PW_TITLE_PUBLICATION_MAX_REGISTRY]; int current = -1, owned_path = 0;
    if (metadata->registry_count != 2 || memchr(bytes, 0, length)) return 0;
    base = strrchr(profile->app.executable, '\\'); base = base ? base + 1 : profile->app.executable;
    if (!*base || strchr(base, '/') || strlen(base) >= sizeof(exe)) return 0;
    strcpy(exe, base);
    if (snprintf(owned0, sizeof(owned0), "Software\\Wine\\AppDefaults\\%s", exe) <= 0) return 0;
    memset(found, 0, sizeof(found));
    for (unsigned i = 0; i < 2; ++i) {
        int first = ascii_equal(metadata->registry[i].key, owned0) && ascii_equal(metadata->registry[i].name, "Version");
        int second = ascii_equal(metadata->registry[i].key, "Control Panel\\Desktop") && ascii_equal(metadata->registry[i].name, "LogPixels");
        if ((!first && !second) || (first && i && ascii_equal(metadata->registry[0].key, owned0)) ||
            (second && i && ascii_equal(metadata->registry[0].key, "Control Panel\\Desktop"))) return 0;
    }
    size_t cursor = 0, header_end = 0, header_length;
    static const char header[] = "WINE REGISTRY Version 2";
    while (header_end < length && bytes[header_end] != '\n') ++header_end;
    header_length = header_end;
    if (header_length && bytes[header_length - 1] == '\r') --header_length;
    if (header_length != sizeof(header) - 1 || memcmp(bytes, header, sizeof(header) - 1)) return 0;
    cursor = header_end < length ? header_end + 1 : header_end;
    while (cursor < length) {
        size_t start = cursor, end; WineString decoded; const uint8_t *after;
        while (cursor < length && bytes[cursor] != '\n') ++cursor;
        end = cursor; if (end > start && bytes[end - 1] == '\r') --end;
        if (cursor < length) ++cursor;
        while (start < end && wine_space(bytes[start])) ++start;
        if (start == end || bytes[start] == ';') continue;
        if (bytes[start] == '[') {
            if (wine_parse_string(bytes + start + 1, bytes + end, ']', &decoded,
                                  &after)) { current = -1; owned_path = 0; continue; }
            if (wine_path_ambiguous_ascii(&decoded, "Software") ||
                wine_path_ambiguous_ascii(&decoded, "Software\\Wine") ||
                wine_path_ambiguous_ascii(&decoded, "Software\\Wine\\AppDefaults") ||
                wine_path_ambiguous_ascii(&decoded, owned0) ||
                wine_path_ambiguous_ascii(&decoded, "Control Panel") ||
                wine_path_ambiguous_ascii(&decoded, "Control Panel\\Desktop")) return 0;
            current = -1; owned_path = wine_root_path(&decoded) ||
                wine_path_equal_ascii(&decoded, "Software") ||
                wine_path_equal_ascii(&decoded, "Software\\Wine") ||
                wine_path_equal_ascii(&decoded, "Software\\Wine\\AppDefaults") ||
                wine_path_equal_ascii(&decoded, owned0) ||
                wine_path_equal_ascii(&decoded, "Control Panel") ||
                wine_path_equal_ascii(&decoded, "Control Panel\\Desktop");
            for (unsigned i = 0; i < 2; ++i) if (wine_path_equal_ascii(&decoded, metadata->registry[i].key)) {
                if (++found[i].sections != 1) return 0;
                current = (int)i;
            }
            continue;
        }
        if (owned_path && end - start >= 5 && !memcmp(bytes + start, "#link", 5)) return 0;
        if (bytes[start] == '#') continue;
        if (current < 0 || bytes[start] != '"') continue;
        const uint8_t *limit = bytes + end;
        if (wine_parse_string(bytes + start + 1, limit, '"', &decoded, &after) ||
            !wine_equal_ascii(&decoded, metadata->registry[current].name)) continue;
        while (after < limit && wine_space(*after)) ++after;
        if (++found[current].assignments != 1 || after == limit || *after++ != '=') return 0;
        while (after < limit && wine_space(*after)) ++after;
        const uint8_t *value = after;
        if ((size_t)(limit - value) >= 6 && !memcmp(value, "dword:", 6)) {
            uint32_t number = 0; value += 6;
            if (limit - value != 8) return 0;
            for (unsigned n = 0; n < 8; ++n) { int digit = hex_digit(value[n]); if (digit < 0) return 0; number = number * 16u + (unsigned)digit; }
            found[current].kind = PW_TITLE_PUBLICATION_REGISTRY_DWORD; found[current].dword = number;
        } else if (value < limit && *value == '"') {
            if (wine_parse_string(value + 1, limit, '"', &found[current].string_value,
                                  &after)) return 0;
            while (after < limit && wine_space(*after)) ++after;
            if (after != limit) return 0;
            found[current].kind = PW_TITLE_PUBLICATION_REGISTRY_STRING;
        } else return 0;
    }
    for (unsigned i = 0; i < 2; ++i) {
        const PwTitlePublicationRegistry *expected = &metadata->registry[i];
        if (found[i].sections > 1 || found[i].assignments > 1) return 0;
        if (expected->kind == PW_TITLE_PUBLICATION_REGISTRY_ABSENT) {
            if (found[i].assignments) return 0;
        } else if (!found[i].assignments || found[i].kind != expected->kind ||
                   (expected->kind == PW_TITLE_PUBLICATION_REGISTRY_STRING &&
                    !wine_equal_ascii_sensitive(&found[i].string_value, expected->string_value)) ||
                   (expected->kind == PW_TITLE_PUBLICATION_REGISTRY_DWORD &&
                    found[i].dword != expected->dword)) return 0;
    }
    return 1;
}

static int private_tree(const PwTitlePublicationPlatform *platform, const char *root,
                        FileIdentity identities[7], char paths[7][PW_TITLE_PUBLICATION_NATIVE_PATH])
{
    static const char *suffix[] = { "", "library", "library/profiles", "library/input",
                                    "library/prefixes", "library/prefixes/game", ".playden-control" };
    for (unsigned i = 0; i < 7; ++i) {
        if ((suffix[i][0] ? append_path(paths[i], PW_TITLE_PUBLICATION_NATIVE_PATH, root, suffix[i]) :
                           (strlen(root) < PW_TITLE_PUBLICATION_NATIVE_PATH ? (strcpy(paths[i], root), 0) : -1)) ||
            !checked_directory(platform, paths[i], 0700, &identities[i])) return 0;
    }
    return 1;
}

static int private_tree_unchanged(const PwTitlePublicationPlatform *platform,
                                  FileIdentity identities[7], char paths[7][PW_TITLE_PUBLICATION_NATIVE_PATH])
{
    for (unsigned i = 0; i < 7; ++i)
        if (!directory_unchanged(platform, paths[i], 0700, identities[i])) return 0;
    return 1;
}

static int verify_publication(const PwTitlePublicationDescriptor *before,
                              const PwTitlePublicationPlatform *platform,
                              PwTitlePublicationLease *lease,
                              char *error, size_t error_size,
                              int borrowed_fd, const uint8_t *expected_receipt,
                              size_t expected_receipt_length, const char *metadata_digest)
{
    char control[1024], lock_path[1024], receipt_path[1024], metadata_path[1024];
    char profile_path[1024], catalog_path[1024], input_path[1024], prefix_path[1024], user_reg[1024];
    char host_lock[2048], digest[65], expected_catalog[128], working[1024], executable[1024];
    char game_mount[PW_TITLE_PUBLICATION_MOUNT] = "", working_real[1024], dxvk_real[1100];
    char expected_dxvk[1100];
    const char *game_root = NULL;
    FileIdentity directories[7], control_before, control_after, lock_before, lock_opened, lock_after;
    char directory_paths[7][PW_TITLE_PUBLICATION_NATIVE_PATH]; struct stat info;
    PwTitlePublicationDescriptor reread; PwTitlePublication receipt; PwTitlePublicationMetadata metadata;
    PwGameProfile profile; PwProfileCatalog catalog; PwGameInput selected;
    uint8_t *bytes = NULL; size_t length = 0; int lock_fd = -1, result = PW_TITLE_PUBLICATION_ADMISSION_INVALID;
    int has_selected = 0, stage = 0; FileIdentity working_before, working_after;
    if (!lease) return failure(error, error_size, "missing lease");
    pw_title_publication_lease_init(lease);
    pw_game_input_init(&selected);
    if (error && error_size) error[0] = 0;
    if (!before || !platform || (borrowed_fd < 0 && !platform->try_lock_shared) || !canonical(before->launch.data_root) ||
        !private_tree(platform, before->launch.data_root, directories, directory_paths) ||
        append_path(control, sizeof(control), before->launch.data_root, ".playden-control") ||
        append_path(lock_path, sizeof(lock_path), control, "publication.lock") ||
        append_path(receipt_path, sizeof(receipt_path), control, "publication-state.v1"))
        return failure(error, error_size, "invalid publication roots");
    if (!checked_directory(platform, control, 0700, &control_before) ||
        mapped_path(platform, lock_path, host_lock, sizeof(host_lock)) || !no_symlink_ancestors(host_lock) ||
        lstat(host_lock, &info) || !S_ISREG(info.st_mode) || S_ISLNK(info.st_mode) ||
        info.st_uid != (uid_t)platform->owner_uid || info.st_nlink != 1 || (info.st_mode & 07777) != 0600)
        return failure(error, error_size, "publication lock refused");
    lock_before = identity(&info);
    lock_fd = borrowed_fd >= 0 ? borrowed_fd : open(host_lock, O_RDONLY | O_NOFOLLOW);
    if (lock_fd < 0 || fstat(lock_fd, &info) || !same_identity(lock_before, identity(&info))) goto done;
    lock_opened = identity(&info); errno = 0;
    if (borrowed_fd < 0 && platform->try_lock_shared(platform->context, lock_fd)) {
        result = (errno == EWOULDBLOCK || errno == EAGAIN) ? PW_TITLE_PUBLICATION_ADMISSION_BUSY : PW_TITLE_PUBLICATION_ADMISSION_INVALID;
        if (result == PW_TITLE_PUBLICATION_ADMISSION_BUSY && error && error_size)
            snprintf(error, error_size, "publication is busy");
        goto done;
    }
    if (fstat(lock_fd, &info)) goto done; lock_after = identity(&info);
    if (!same_identity(lock_opened, lock_after) || lstat(host_lock, &info) ||
        !same_identity(lock_opened, identity(&info)) ||
        !checked_directory(platform, control, 0700, &control_after) ||
        !same_identity(control_before, control_after)) goto done;
    if (pw_title_publication_read_descriptor(before->canonical_path, before->launch.title_id,
          platform, &reread, error, error_size) != PW_OK || reread.length != before->length ||
        memcmp(reread.bytes, before->bytes, before->length)) goto done;
    stage = 1;

    bytes = read_regular(platform, receipt_path, PW_TITLE_PUBLICATION_MAX_BYTES, 0, &length);
    if (!bytes || pw_title_publication_parse(bytes, length, &receipt) != PW_OK ||
        !receipt_matches(&receipt, &before->launch) ||
        (borrowed_fd < 0 && receipt.phase != PW_TITLE_PUBLICATION_COMMITTED) ||
        (borrowed_fd >= 0 && (length != expected_receipt_length || memcmp(bytes, expected_receipt, length))) ||
        (borrowed_fd >= 0 && receipt.phase == PW_TITLE_PUBLICATION_COMMITTED &&
         strcmp(receipt.publication_sha256, metadata_digest))) goto done;
    free(bytes); bytes = NULL;
    stage = 2;
    if (append_path(metadata_path, sizeof(metadata_path), before->launch.data_root,
                    "library/profiles/.playden-publication")) goto done;
    bytes = read_regular(platform, metadata_path, PW_TITLE_PUBLICATION_METADATA_MAX_BYTES, 0, &length);
    if (!bytes || digest_hex(bytes, length, digest) || strcmp(digest, borrowed_fd >= 0 ? metadata_digest : receipt.publication_sha256) ||
        pw_title_publication_metadata_parse(bytes, length, &metadata) != PW_OK ||
        !metadata_matches(&metadata, &receipt, &before->launch)) goto done;
    free(bytes); bytes = NULL;
    stage = 3;

    if (append_identifier_path(profile_path, sizeof(profile_path), before->launch.data_root,
          "library/profiles/", before->launch.default_profile_id, ".profile") ||
        append_path(catalog_path, sizeof(catalog_path), before->launch.data_root, "library/profiles/profiles.lst")) goto done;
    bytes = read_regular(platform, profile_path, PW_APP_PROFILE_MAX_BYTES, 0, &length);
    if (!bytes || digest_hex(bytes, length, digest) || strcmp(digest, metadata.profile_sha256) ||
        pw_game_profile_parse(bytes, length, &profile) != PW_OK ||
        strcmp(profile.app.id, before->launch.default_profile_id) ||
        strcmp(profile.app.runtime, before->launch.runtime_bundle_id)) goto done;
    free(bytes); bytes = NULL;
    stage = 4;
    if (digest_hex(before->bytes, before->length, digest) || strcmp(digest, metadata.descriptor_sha256)) goto done;
    bytes = read_regular(platform, catalog_path, 4096, 0, &length);
    if (!bytes || digest_hex(bytes, length, digest) || strcmp(digest, metadata.catalog_sha256) ||
        pw_profile_catalog_parse(bytes, length, &catalog) != PW_OK || catalog.count != 1 ||
        snprintf(expected_catalog, sizeof(expected_catalog), "%s.profile", before->launch.default_profile_id) <= 0 ||
        strcmp(catalog.names[0], expected_catalog)) goto done;
    free(bytes); bytes = NULL;
    stage = 5;

    for (unsigned i = 0; i < metadata.input_count; ++i) {
        char input_directory[1024];
        if (append_path(input_directory, sizeof(input_directory), before->launch.data_root, "library/input") ||
            append_path(input_path, sizeof(input_path), input_directory, metadata.inputs[i].name)) goto done;
        bytes = read_regular(platform, input_path, PW_APP_PROFILE_MAX_BYTES, 0, &length);
        PwGameInput parsed; pw_game_input_init(&parsed);
        if (!bytes || digest_hex(bytes, length, digest) || strcmp(digest, metadata.inputs[i].sha256) ||
            pw_game_input_parse(bytes, length, &parsed) != PW_OK) goto done;
        free(bytes); bytes = NULL;
        if (profile.input.preset[0]) {
            char wanted[80];
            if (snprintf(wanted, sizeof(wanted), "%s.input", profile.input.preset) <= 0) goto done;
            if (ascii_equal(wanted, metadata.inputs[i].name)) {
                if (has_selected || strcmp(wanted, metadata.inputs[i].name)) goto done;
                selected = parsed; has_selected = 1;
            }
        }
    }
    if ((profile.input.preset[0] && !has_selected) || (!profile.input.preset[0] && has_selected)) goto done;
    stage = 6;
    game_root = reread.launch.game_root;

    if (append_identifier_path(prefix_path, sizeof(prefix_path), before->launch.data_root,
          "library/prefixes/", profile.app.prefix, "") || strcmp(profile.app.prefix, "game") ||
        pw_wine_launch_host_dir(prefix_path, profile.app.working_directory, working, sizeof(working)) ||
        pw_wine_launch_host_dir(prefix_path, profile.app.executable, executable, sizeof(executable)) ||
        !canonical(working) || !canonical(executable) ||
        !canonical(game_root) ||
        (strcmp(working, game_root) &&
         (strncmp(working, game_root, strlen(game_root)) || working[strlen(game_root)] != '/')) ||
        strncmp(executable, game_root, strlen(game_root)) ||
        (executable[strlen(game_root)] != '/' && executable[strlen(game_root)] != 0)) goto done;
    /* Everything compared above and below is the published token form. Only
     * what touches the game's files goes to the drive. */
    if (reread.launch.schema_version == 3) {
        if (platform->known_game_mount) {
            if (strncmp(platform->known_game_mount, usb_token, sizeof(usb_token) - 1) ||
                platform->known_game_mount[sizeof(usb_token) - 1] < '0' ||
                platform->known_game_mount[sizeof(usb_token) - 1] >= '0' + USB_MOUNTS ||
                platform->known_game_mount[sizeof(usb_token)]) goto done;
            strcpy(game_mount, platform->known_game_mount);
        } else {
            int found = find_game_mount(platform, executable, game_mount, error, error_size);
            if (found != PW_OK) { result = found; goto done; }
        }
    }
    if (game_path(working, game_mount, working_real, sizeof(working_real)) ||
        !checked_approved_directory(platform, working_real, (int)metadata.game_root_unowned,
                                    &working_before)) goto done;
    if (append_path(expected_dxvk, sizeof(expected_dxvk), working, "dxvk.conf") ||
        strcmp(expected_dxvk, metadata.dxvk_path) ||
        game_path(expected_dxvk, game_mount, dxvk_real, sizeof(dxvk_real))) goto done;
    if (metadata.dxvk_present) {
        bytes = read_regular(platform, dxvk_real, 1024 * 1024,
                             metadata.game_root_unowned ? READ_GAME_UNOWNED : READ_LEGACY, &length);
        if (!bytes || digest_hex(bytes, length, digest) || strcmp(digest, metadata.dxvk_sha256)) goto done;
        free(bytes); bytes = NULL;
    } else {
        char mapped[2048];
        if (mapped_path(platform, dxvk_real, mapped, sizeof(mapped))) goto done;
        errno = 0; if (!lstat(mapped, &info) || errno != ENOENT) goto done;
    }
    if (append_path(user_reg, sizeof(user_reg), prefix_path, "user.reg")) goto done;
    stage = 7;
    bytes = read_regular(platform, user_reg, USER_REG_MAX, 1, &length);
    if (!bytes || !registry_semantics(bytes, length, &metadata, &profile)) goto done;
    free(bytes); bytes = NULL;
    stage = 8;
    if (!checked_approved_directory(platform, working_real, (int)metadata.game_root_unowned,
                                    &working_after) ||
        !same_identity(working_before, working_after) ||
        !private_tree_unchanged(platform, directories, directory_paths)) goto done;
    lease->lock_fd = borrowed_fd >= 0 ? -1 : lock_fd; lease->lock_device = lock_opened.device; lease->lock_inode = lock_opened.inode;
    lease->profile = profile; lease->preset = selected; lease->has_preset = has_selected;
    memcpy(lease->game_mount, game_mount, sizeof(lease->game_mount));
    lock_fd = -1; result = PW_OK;
done:
    free(bytes); if (lock_fd >= 0 && borrowed_fd < 0) close(lock_fd);
    if (result == PW_TITLE_PUBLICATION_ADMISSION_INVALID)
        if (error && error_size) snprintf(error, error_size, "publication admission refused at stage %d", stage);
    return result;
}

int pw_title_publication_open(const PwTitlePublicationDescriptor *before,
                              const PwTitlePublicationPlatform *platform,
                              PwTitlePublicationLease *lease,
                              char *error, size_t error_size)
{
    return verify_publication(before, platform, lease, error, error_size, -1, NULL, 0, NULL);
}

/* Every "Z:\\mnt\\usb\\" in text gets the drive's number. The letter's case doesn't
 * matter, as it doesn't when admission reads the path. */
static int use_mount_in(char *text, size_t capacity, char number)
{
    static const char token[] = ":\\mnt\\usb\\";
    size_t length = strlen(text);
    for (char *at = strstr(text, token); at; at = strstr(at, token)) {
        const int letter = at > text && (at[-1] == 'Z' || at[-1] == 'z');
        at += sizeof(token) - 2;  /* the separator after "usb" */
        if (!letter) continue;
        if (length + 1 >= capacity) return -1;
        memmove(at + 1, at, strlen(at) + 1);
        *at = number; at += 2; ++length;
    }
    return 0;
}

int pw_title_publication_use_game_mount(PwGameProfile *profile, const char *mount,
                                        char *error, size_t error_size)
{
    size_t token = sizeof(usb_token) - 1;
    if (!profile || !mount) return failure(error, error_size, "invalid game mount");
    if (!mount[0]) return PW_OK;
    if (strncmp(mount, usb_token, token) || mount[token] < '0' || mount[token] >= '0' + USB_MOUNTS ||
        mount[token + 1]) return failure(error, error_size, "invalid game mount");
    if (use_mount_in(profile->app.executable, sizeof(profile->app.executable), mount[token]) ||
        use_mount_in(profile->app.working_directory, sizeof(profile->app.working_directory), mount[token]) ||
        use_mount_in(profile->app.arguments, sizeof(profile->app.arguments), mount[token]))
        return failure(error, error_size, "game path too long for its drive");
    for (size_t i = 0; i < profile->environment_count; ++i)
        if (use_mount_in(profile->environment[i].value, sizeof(profile->environment[i].value), mount[token]))
            return failure(error, error_size, "game path too long for its drive");
    return PW_OK;
}

int pw_title_publication_verify_locked(
    const PwTitlePublicationDescriptor *descriptor,
    const PwTitlePublicationPlatform *platform, int borrowed_lock_fd,
    const uint8_t *expected_receipt_bytes, size_t expected_receipt_length,
    const char expected_metadata_sha256[65], PwTitlePublicationContents *contents,
    char *error, size_t error_size)
{
    PwTitlePublication parsed; PwTitlePublicationLease verified;
    if (borrowed_lock_fd < 0 || !contents || !expected_receipt_bytes ||
        !expected_metadata_sha256 || !expected_receipt_length ||
        expected_receipt_length > PW_TITLE_PUBLICATION_MAX_BYTES ||
        pw_title_publication_parse(expected_receipt_bytes, expected_receipt_length, &parsed) != PW_OK)
        return failure(error, error_size, "invalid locked verification contract");
    for (unsigned i = 0; i < 64; ++i) {
        unsigned ch = (unsigned char)expected_metadata_sha256[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return failure(error, error_size, "invalid locked metadata digest");
    }
    if (expected_metadata_sha256[64]) return failure(error, error_size, "invalid locked metadata digest");
    int rc = verify_publication(descriptor, platform, &verified, error, error_size,
        borrowed_lock_fd, expected_receipt_bytes, expected_receipt_length, expected_metadata_sha256);
    if (rc == PW_OK) {
        contents->profile = verified.profile; contents->preset = verified.preset;
        contents->has_preset = verified.has_preset;
        memcpy(contents->game_mount, verified.game_mount, sizeof(contents->game_mount));
    }
    return rc;
}

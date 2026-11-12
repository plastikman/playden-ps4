/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_update_check.h"

#include <stdio.h>
#include <string.h>

int pw_update_parse_version(const char *text, unsigned v[3])
{
    if (!text || !v) return -1;
    /* An optional name before the version, as a repository that releases more than one title
     * needs ("ps4-v0.1.0"): letters or digits, then one '-'. */
    for (const char *p = text; p < text + 16 && *p; p++) {
        if (*p == '-') { text = p + 1; break; }
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) break;
    }
    if (*text == 'v' || *text == 'V') text++;
    for (int part = 0; part < 3; part++) {
        unsigned long value = 0;
        int digits = 0;

        while (*text >= '0' && *text <= '9') {
            value = value * 10u + (unsigned long)(*text++ - '0');
            if (++digits > 6) return -1;
        }
        if (!digits) return -1;
        v[part] = (unsigned)value;
        if (part < 2 && *text++ != '.') return -1;
    }
    return *text ? -1 : 0;
}

int pw_update_is_newer(const char *current, const char *latest)
{
    unsigned have[3], offer[3];

    if (pw_update_parse_version(current, have) || pw_update_parse_version(latest, offer)) return -1;
    for (int i = 0; i < 3; i++)
        if (offer[i] != have[i]) return offer[i] > have[i];
    return 0;
}

/* The value after "key": within json, skipping whitespace; NULL if absent. */
static const char *value_of(const char *json, size_t length, const char *key)
{
    const size_t key_length = strlen(key);

    for (size_t i = 0; i + key_length + 2 <= length; i++) {
        if (json[i] != '"' || memcmp(json + i + 1, key, key_length) || json[i + 1 + key_length] != '"')
            continue;
        size_t j = i + key_length + 2;
        while (j < length && (json[j] == ' ' || json[j] == '\t' || json[j] == '\r' || json[j] == '\n')) j++;
        if (j >= length || json[j] != ':') continue;
        j++;
        while (j < length && (json[j] == ' ' || json[j] == '\t' || json[j] == '\r' || json[j] == '\n')) j++;
        return j < length ? json + j : NULL;
    }
    return NULL;
}

static int is_true(const char *json, size_t length, const char *key)
{
    const char *value = value_of(json, length, key);
    return value && (size_t)(json + length - value) >= 4 && !memcmp(value, "true", 4);
}

int pw_update_release_tag(const char *json, size_t length, char *tag, size_t capacity)
{
    const char *value;
    size_t n = 0;

    if (!json || !tag || !capacity) return -1;
    tag[0] = 0;
    if (is_true(json, length, "draft") || is_true(json, length, "prerelease")) return -1;
    if (!(value = value_of(json, length, "tag_name")) || *value != '"') return -1;
    for (value++; value < json + length && *value != '"'; value++) {
        if (*value == '\\' || n + 1 >= capacity) return -1;
        tag[n++] = *value;
    }
    if (value >= json + length) { tag[0] = 0; return -1; }
    tag[n] = 0;
    return n ? 0 : -1;
}

/* Past the JSON value starting at p (a string, object, array, or a bare
 * word or number); NULL when it runs off the end. */
static const char *skip_value(const char *p, const char *end)
{
    int depth = 0;

    if (p >= end) return NULL;
    if (*p != '"' && *p != '{' && *p != '[') {
        while (p < end && *p != ',' && *p != '}' && *p != ']') p++;
        return p;
    }
    for (; p < end; p++) {
        if (*p == '"') {
            for (p++; p < end && *p != '"'; p++)
                if (*p == '\\') p++;
            if (p >= end) return NULL;
            if (!depth) return p + 1;
        } else if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (--depth == 0) return p + 1;
        }
    }
    return NULL;
}

static const char *skip_space(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')) p++;
    return p;
}

/* The value of key among the members of the object at [p, end) -- its own
 * members, not those of objects inside it; NULL if absent. */
static const char *member(const char *p, const char *end, const char *key)
{
    const size_t key_length = strlen(key);

    if (p >= end || *p != '{') return NULL;
    for (p = skip_space(p + 1, end); p < end && *p == '"';) {
        const char *name = p + 1, *after = skip_value(p, end), *value;
        if (!after) return NULL;
        value = after;
        while (value < end && (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n')) value++;
        if (value >= end || *value != ':') return NULL;
        value++;
        while (value < end && (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n')) value++;
        if ((size_t)(after - 1 - name) == key_length && !memcmp(name, key, key_length)) return value;
        if (!(p = skip_value(value, end))) return NULL;
        p = skip_space(p, end);
    }
    return NULL;
}

/* A plain JSON string (no escapes) at p into out. 0, or -1. */
static int string_at(const char *p, const char *end, char *out, size_t capacity)
{
    size_t n = 0;

    if (!p || p >= end || *p != '"') return -1;
    for (p++; p < end && *p != '"'; p++) {
        if (*p == '\\' || n + 1 >= capacity) return -1;
        out[n++] = *p;
    }
    if (p >= end) return -1;
    out[n] = 0;
    return 0;
}

int pw_update_release_asset(const char *json, size_t length, const char *name, PwUpdateAsset *asset)
{
    const char *end = json + length, *p;

    if (!json || !name || !asset) return -1;
    memset(asset, 0, sizeof(*asset));
    if (*skip_space(json, end) != '{') return -1;
    if (!(p = member(skip_space(json, end), end, "assets")) || *p != '[') return -1;
    for (p = skip_space(p + 1, end); p < end && *p == '{';) {
        const char *next = skip_value(p, end), *value;
        char text[128];
        if (!next) return -1;
        if (!string_at(member(p, next, "name"), next, text, sizeof(text)) && !strcmp(text, name)) {
            char digest[80];
            unsigned long long size = 0;
            snprintf(asset->name, sizeof(asset->name), "%s", text);
            if (string_at(member(p, next, "browser_download_url"), next, asset->url, sizeof(asset->url)) ||
                strncmp(asset->url, "https://", 8)) return -1;
            if (!(value = member(p, next, "size")) || *value < '0' || *value > '9') return -1;
            for (; value < next && *value >= '0' && *value <= '9'; value++) size = size * 10u + (unsigned)(*value - '0');
            asset->size = size;
            if (string_at(member(p, next, "url"), next, asset->api_url, sizeof(asset->api_url)) ||
                strncmp(asset->api_url, "https://api.github.com/", 23))
                asset->api_url[0] = 0;
            if (string_at(member(p, next, "digest"), next, digest, sizeof(digest)) ||
                strncmp(digest, "sha256:", 7) || strlen(digest + 7) != 64) return -1;
            for (int i = 0; i < 64; i++) {
                char c = digest[7 + i];
                if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return -1;
                asset->sha256[i] = c;
            }
            return size ? 0 : -1;
        }
        p = skip_space(next, end);
    }
    return -1;
}

int pw_update_notice(const char *current, const char *json, size_t length, const char *site,
                     char *out, size_t capacity)
{
    char tag[32];
    int newer;

    if (!out || !capacity) return -1;
    out[0] = 0;
    if (pw_update_release_tag(json, length, tag, sizeof(tag))) return -1;
    if ((newer = pw_update_is_newer(current, tag)) <= 0) return newer;
    if (snprintf(out, capacity, "%s available: %s", tag, site ? site : "") >= (int)capacity) {
        out[0] = 0;
        return -1;
    }
    return 1;
}

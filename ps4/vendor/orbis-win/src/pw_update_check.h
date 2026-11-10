/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_UPDATE_CHECK_H
#define PW_UPDATE_CHECK_H
/*
 * Whether a newer stable release exists, from GitHub's answer to
 *
 *     GET https://api.github.com/repos/<owner>/<repo>/releases/latest
 *
 * which names the newest release that is neither a draft nor a pre-release,
 * as JSON with a "tag_name" (e.g. "v0.0.3"). This file only reads that answer
 * and compares versions; fetching it is the platform's job (pw_update_ps4.c),
 * so all of this is testable on the host.
 *
 * Versions are MAJOR.MINOR.PATCH, optionally with a leading 'v'. Anything
 * else -- a pre-release suffix ("0.0.3-beta.1"), a missing part, junk -- is
 * not a version, and an answer that is not a version never produces a notice.
 */
#include <stddef.h>

/* 0 and the three parts in v[], or -1. */
int pw_update_parse_version(const char *text, unsigned v[3]);

/* 1 if latest is a newer version than current, 0 if not, -1 if either is not
 * a version. */
int pw_update_is_newer(const char *current, const char *latest);

/* Copies the "tag_name" string of a release JSON into tag (NUL-terminated).
 * 0, or -1 when there is none, it does not fit, or the release says it is a
 * draft or a pre-release. */
int pw_update_release_tag(const char *json, size_t length, char *tag, size_t capacity);

/* The launcher's one line: 1 and "<tag> available: <site>" in out
 * when the release in json is newer than current, 0 when it is not (out is
 * then ""), -1 when the answer or current cannot be read. */
int pw_update_notice(const char *current, const char *json, size_t length, const char *site,
                     char *out, size_t capacity);

/* A release's downloadable file, from its "assets" list. */
typedef struct PwUpdateAsset {
    char name[128];
    char url[512];                    /* browser_download_url */
    /* The asset's API address ("url", https://api.github.com/...), which
     * serves the file with Accept: application/octet-stream. Its redirect
     * carries 2 KB of headers where github.com's carries 5.6 KB, past
     * libSceHttp's default limit (01.56). Empty when absent. */
    char api_url[256];
    unsigned long long size;
    char sha256[65];                  /* lower-case hex, from "digest": "sha256:..." */
} PwUpdateAsset;

/* Finds the asset called name in a release JSON. 0 and *asset filled, or -1
 * when there is none, or it lacks a URL, a size or a SHA-256 digest (GitHub
 * gives one for every asset uploaded since mid-2025): nothing is downloaded
 * that cannot be checked. */
int pw_update_release_asset(const char *json, size_t length, const char *name, PwUpdateAsset *asset);

#endif

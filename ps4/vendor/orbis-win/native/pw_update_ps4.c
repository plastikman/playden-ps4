/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_update_ps4.h"
#include "../src/pw_update_check.h"

#include "../src/pw_sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef PW_RELEASE_VERSION
#define PW_RELEASE_VERSION ""
#endif
/* The release feed, the site its notice names and the HTTP agent; a title that ships this updater for
 * its own releases (Playden) sets them at build time. */
#ifndef PW_UPDATE_URL
#define PW_UPDATE_URL "https://api.github.com/repos/plastikman/orbis-win/releases/latest"
#endif
#ifndef PW_UPDATE_SITE
#define PW_UPDATE_SITE "github.com/plastikman/orbis-win"
#endif
#ifndef PW_UPDATE_AGENT
#define PW_UPDATE_AGENT "orbis-win"
#endif
#define PW_UPDATE_BODY_MAX (64u * 1024u)    /* tag_name is near the top; more is not needed */
#define PW_UPDATE_TIMEOUT_US (8u * 1000u * 1000u)
/* The release's package, as every release names it, and where it goes:
 * GoldHEN's installer lists /data/pkg. */
#ifndef PW_UPDATE_ASSET
#define PW_UPDATE_ASSET "IV0000-GLDS10043_00-GLDS100430000000.pkg"
#endif
#define PW_UPDATE_PKG_DIR "/data/pkg"
#define PW_UPDATE_CHUNK (256u * 1024u)
#define PW_UPDATE_REDIRECTS 5

int sceSysmoduleLoadModuleInternal(uint32_t id);
int sceSysmoduleGetModuleHandleInternal(uint32_t id, int32_t *handle);
int sceKernelDlsym(int32_t handle, const char *symbol, void **address);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int id);

/* libSceHttp and libSceSsl are system modules a title loads at run time; their
 * functions are looked up by name in the loaded module (as pw_hid_ps5.c does
 * for the keyboard), so the launcher imports nothing from them and still
 * starts on a console where loading them fails. The server certificate is
 * always verified (libSceHttp's default); if this console's root store cannot
 * verify GitHub's chain, the check fails, says so in the log, and the launcher
 * shows nothing. */
enum { MODULE_HTTP = 0x8000000Au, MODULE_SSL = 0x8000000Bu };
enum { HTTP_VERSION_1_1 = 2, HTTP_METHOD_GET = 0, HTTP_HEADER_OVERWRITE = 0 };

static struct {
    int (*ssl_init)(size_t pool);
    int (*ssl_term)(int ctx);
    int (*init)(int net_pool, int ssl_ctx, size_t pool);
    int (*term)(int ctx);
    int (*create_template)(int ctx, const char *agent, int version, int auto_proxy);
    int (*delete_template)(int id);
    int (*resolve_timeout)(int id, uint32_t usec);
    int (*connect_timeout)(int id, uint32_t usec);
    int (*recv_timeout)(int id, uint32_t usec);
    int (*create_connection)(int template_id, const char *url, int keep_alive);
    int (*delete_connection)(int id);
    int (*create_request)(int connection, int method, const char *url, uint64_t content_length);
    int (*delete_request)(int id);
    int (*add_header)(int id, const char *name, const char *value, int mode);
    int (*send)(int id, const void *data, size_t size);
    int (*status)(int id, int *code);
    int (*read)(int id, void *data, uint32_t size);
    /* Optional: following GitHub's redirect to its file host by hand when
     * libSceHttp does not (and turning its own following off then). */
    int (*auto_redirect)(int id, int enable);
    int (*all_headers)(int id, char **headers, size_t *size);
    /* github.com answers a release download with a redirect whose headers
     * (security policy, cookies) pass libSceHttp's default limit: send then
     * fails with 0x80431073, SCE_HTTP_ERROR_TOO_LARGE_RESPONSE_HEADER
     * (01.56, 2026-11-09). */
    int (*header_max)(int id, size_t size);
} http;

/* The newer release's package, once the check found one, and the download. */
static PwUpdateAsset asset;
static char asset_tag[32];
static pthread_mutex_t download_lock = PTHREAD_MUTEX_INITIALIZER;
static PwUpdateDownload download;     /* under download_lock */
static volatile int download_cancel;

static char notice[128];
static volatile int notice_ready;
static void (*log_line)(const char *line);

static void note(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *format, ...)
{
    char line[192];
    __builtin_va_list args;

    if (!log_line) return;
    __builtin_va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    __builtin_va_end(args);
    log_line(line);
}

static int module(uint32_t id)
{
    int32_t handle = -1;
    int status = sceSysmoduleLoadModuleInternal(id);

    if (sceSysmoduleGetModuleHandleInternal(id, &handle) || handle < 0) {
        note("update check: module %#x load=%#x, no handle", (unsigned)id, (unsigned)status);
        return -1;
    }
    return handle;
}

static int resolve(int handle, const char *name, void *slot)
{
    void *address = NULL;

    if (sceKernelDlsym(handle, name, &address) || !address) {
        note("update check: %s not found", name);
        return -1;
    }
    memcpy(slot, &address, sizeof(address));
    return 0;
}

/* The optional functions; their absence only changes how redirects go. */
static int bind_optional(int lib)
{
    void *address = NULL;

    http.auto_redirect = NULL;
    http.all_headers = NULL;
    http.header_max = NULL;
    if (!sceKernelDlsym(lib, "sceHttpSetResponseHeaderMaxSize", &address) && address)
        memcpy(&http.header_max, &address, sizeof(address));
    address = NULL;
    if (!sceKernelDlsym(lib, "sceHttpSetAutoRedirect", &address) && address)
        memcpy(&http.auto_redirect, &address, sizeof(address));
    address = NULL;
    if (!sceKernelDlsym(lib, "sceHttpGetAllResponseHeaders", &address) && address)
        memcpy(&http.all_headers, &address, sizeof(address));
    return 0;
}

static int bind_http(void)
{
    int ssl = module(MODULE_SSL), lib = module(MODULE_HTTP);

    if (ssl < 0 || lib < 0) return -1;
    return resolve(ssl, "sceSslInit", &http.ssl_init) | resolve(ssl, "sceSslTerm", &http.ssl_term) |
           resolve(lib, "sceHttpInit", &http.init) | resolve(lib, "sceHttpTerm", &http.term) |
           resolve(lib, "sceHttpCreateTemplate", &http.create_template) |
           resolve(lib, "sceHttpDeleteTemplate", &http.delete_template) |
           resolve(lib, "sceHttpSetResolveTimeOut", &http.resolve_timeout) |
           resolve(lib, "sceHttpSetConnectTimeOut", &http.connect_timeout) |
           resolve(lib, "sceHttpSetRecvTimeOut", &http.recv_timeout) |
           resolve(lib, "sceHttpCreateConnectionWithURL", &http.create_connection) |
           resolve(lib, "sceHttpDeleteConnection", &http.delete_connection) |
           resolve(lib, "sceHttpCreateRequestWithURL", &http.create_request) |
           resolve(lib, "sceHttpDeleteRequest", &http.delete_request) |
           resolve(lib, "sceHttpAddRequestHeader", &http.add_header) |
           resolve(lib, "sceHttpSendRequest", &http.send) |
           resolve(lib, "sceHttpGetStatusCode", &http.status) |
           resolve(lib, "sceHttpReadData", &http.read) ? -1 : bind_optional(lib);
}

/* One GET of the latest release. The body length, or -1 (logged). */
static long get_release(int ctx, char *body, size_t capacity)
{
    int template_id, connection = -1, request = -1, code = 0, status;
    long length = -1;
    size_t used = 0;

    template_id = http.create_template(ctx, PW_UPDATE_AGENT "-update-check/" PW_RELEASE_VERSION, HTTP_VERSION_1_1, 1);
    if (template_id < 0) { note("update check: template %#x", (unsigned)template_id); return -1; }
    http.resolve_timeout(template_id, PW_UPDATE_TIMEOUT_US);
    http.connect_timeout(template_id, PW_UPDATE_TIMEOUT_US);
    http.recv_timeout(template_id, PW_UPDATE_TIMEOUT_US);
    if ((connection = http.create_connection(template_id, PW_UPDATE_URL, 0)) < 0) {
        note("update check: connection %#x", (unsigned)connection);
        goto done;
    }
    if ((request = http.create_request(connection, HTTP_METHOD_GET, PW_UPDATE_URL, 0)) < 0) {
        note("update check: request %#x", (unsigned)request);
        goto done;
    }
    http.add_header(request, "Accept", "application/vnd.github+json", HTTP_HEADER_OVERWRITE);
    if ((status = http.send(request, NULL, 0)) < 0) {
        /* No network, DNS, a timeout, or a certificate this console cannot
         * verify (libSceHttp's SSL errors are 0x80435xxx). */
        note("update check: send %#x", (unsigned)status);
        goto done;
    }
    if (http.status(request, &code) < 0 || code != 200) {
        note("update check: HTTP %d", code);
        goto done;
    }
    for (;;) {
        int n = http.read(request, body + used, (uint32_t)(capacity - 1 - used));
        if (n < 0) { note("update check: read %#x", (unsigned)n); break; }
        if (n == 0 || (used += (size_t)n) >= capacity - 1) break;
    }
    body[used] = 0;
    length = (long)used;
done:
    if (request >= 0) http.delete_request(request);
    if (connection >= 0) http.delete_connection(connection);
    http.delete_template(template_id);
    return length;
}

static void *fetch(void *unused)
{
    int net_pool = -1, ssl_ctx = -1, ctx = -1;
    char *body = MAP_FAILED;
    long length;

    (void)unused;
    if (bind_http()) return NULL;
    if ((net_pool = sceNetPoolCreate("pw-update-check", 16 * 1024, 0)) < 0) {
        note("update check: net pool %#x", (unsigned)net_pool);
        goto done;
    }
    if ((ssl_ctx = http.ssl_init(256 * 1024)) < 0) { note("update check: ssl %#x", (unsigned)ssl_ctx); goto done; }
    if ((ctx = http.init(net_pool, ssl_ctx, 64 * 1024)) < 0) { note("update check: http %#x", (unsigned)ctx); goto done; }
    body = mmap(NULL, PW_UPDATE_BODY_MAX, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (body == MAP_FAILED) goto done;
    if ((length = get_release(ctx, body, PW_UPDATE_BODY_MAX)) >= 0) {
        char line[sizeof(notice)];
        int result = pw_update_notice(PW_RELEASE_VERSION, body, (size_t)length, PW_UPDATE_SITE, line, sizeof(line));
        /* A newer release with its package: offer the download, and say
         * where in the title bar. */
        if (result == 1 && !pw_update_release_asset(body, (size_t)length, PW_UPDATE_ASSET, &asset) &&
            !pw_update_release_tag(body, (size_t)length, asset_tag, sizeof(asset_tag))) {
            snprintf(line, sizeof(line), "%s available: Triangle, then Download update", asset_tag);
            pthread_mutex_lock(&download_lock);
            download.state = PW_UPDATE_DOWNLOAD_READY;
            download.total = asset.size;
            snprintf(download.tag, sizeof(download.tag), "%s", asset_tag);
            pthread_mutex_unlock(&download_lock);
            note("update check: %s is %llu bytes, sha256 %.16s...", asset.name, asset.size, asset.sha256);
        } else if (result == 1) {
            note("update check: the release has no %s with a digest; no download offered", PW_UPDATE_ASSET);
        }
        if (result == 1) {
            memcpy(notice, line, sizeof(notice));
            __atomic_store_n(&notice_ready, 1, __ATOMIC_RELEASE);
        }
        note("update check: running %s, %s", PW_RELEASE_VERSION,
             result == 1 ? line : result == 0 ? "up to date" : "answer not understood");
    }
done:
    if (body != MAP_FAILED) munmap(body, PW_UPDATE_BODY_MAX);
    if (ctx >= 0) http.term(ctx);
    if (ssl_ctx >= 0) http.ssl_term(ssl_ctx);
    if (net_pool >= 0) sceNetPoolDestroy(net_pool);
    return NULL;
}

void pw_update_ps4_start(const char *root, void (*log)(const char *line))
{
    static int started;
    unsigned version[3];
    char marker[160];
    pthread_t thread;
    pthread_attr_t attr;

    if (started++) return;
    log_line = log;
    if (pw_update_parse_version(PW_RELEASE_VERSION, version)) {
        note("update check: off, this build has no release version");
        return;
    }
    snprintf(marker, sizeof(marker), "%s/no-update-check", root ? root : "");
    if (root && !access(marker, F_OK)) {
        note("update check: off (%s)", marker);
        return;
    }
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, fetch, NULL)) note("update check: no thread");
    pthread_attr_destroy(&attr);
}

const char *pw_update_ps4_notice(void)
{
    return __atomic_load_n(&notice_ready, __ATOMIC_ACQUIRE) ? notice : NULL;
}

/* ---- the download ------------------------------------------------------ */

static void download_set(PwUpdateDownloadState state, unsigned long long done, const char *detail)
{
    pthread_mutex_lock(&download_lock);
    download.state = state;
    download.done = done;
    snprintf(download.detail, sizeof(download.detail), "%s", detail ? detail : "");
    pthread_mutex_unlock(&download_lock);
}

/* "Location:" from a redirect's headers into out; 0, or -1. */
static int location(int request, char *out, size_t capacity)
{
    char *headers = NULL, *p;
    size_t size = 0, n = 0;

    if (!http.all_headers || http.all_headers(request, &headers, &size) < 0 || !headers) return -1;
    for (p = headers; p < headers + size; p++) {
        if ((p == headers || p[-1] == '\n') && (size_t)(headers + size - p) > 9 && !strncasecmp(p, "location:", 9)) {
            for (p += 9; p < headers + size && (*p == ' ' || *p == '\t'); p++) {}
            while (p < headers + size && *p != '\r' && *p != '\n' && n + 1 < capacity) out[n++] = *p++;
            out[n] = 0;
            return n ? 0 : -1;
        }
    }
    return -1;
}

/* One GET of url, following redirects; the open request, or -1 (logged,
 * with the reason in download.detail). */
static int open_download(int template_id, const char *first_url, int *connection_out)
{
    static char url[2048];             /* GitHub's signed file-host address is ~1 KB */
    int connection = -1, request = -1, code = 0, status;

    snprintf(url, sizeof(url), "%s", first_url);
    for (int hop = 0; hop <= PW_UPDATE_REDIRECTS; hop++) {
        char detail[96];
        if ((connection = http.create_connection(template_id, url, 0)) < 0 ||
            (request = http.create_request(connection, HTTP_METHOD_GET, url, 0)) < 0) {
            snprintf(detail, sizeof(detail), "Connection failed (%#x)", (unsigned)(connection < 0 ? connection : request));
            break;
        }
        /* The API address serves the file itself only when asked for one. */
        http.add_header(request, "Accept", "application/octet-stream", HTTP_HEADER_OVERWRITE);
        if ((status = http.send(request, NULL, 0)) < 0) {
            snprintf(detail, sizeof(detail), "No answer from %.40s (%#x)", url + 8, (unsigned)status);
            note("update download: send %#x to %s", (unsigned)status, url);
            download_set(PW_UPDATE_DOWNLOAD_FAILED, 0, detail);
            goto fail;
        }
        http.status(request, &code);
        note("update download: HTTP %d from %.80s", code, url);
        if (code == 200) {
            *connection_out = connection;
            return request;
        }
        if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) &&
            !location(request, url, sizeof(url)) && !strncmp(url, "https://", 8)) {
            http.delete_request(request);
            http.delete_connection(connection);
            request = connection = -1;
            continue;
        }
        snprintf(detail, sizeof(detail), "Server said HTTP %d", code);
        download_set(PW_UPDATE_DOWNLOAD_FAILED, 0, detail);
        goto fail;
    }
    download_set(PW_UPDATE_DOWNLOAD_FAILED, 0, "Could not reach the download");
fail:
    if (request >= 0) http.delete_request(request);
    if (connection >= 0) http.delete_connection(connection);
    return -1;
}

static void *download_run(void *unused)
{
    char part[192], final[192], hex[65], detail[96];
    int net_pool = -1, ssl_ctx = -1, ctx = -1, template_id = -1, connection = -1, request = -1, fd = -1;
    unsigned long long done = 0, last_note = 0;
    uint8_t *buffer = MAP_FAILED, digest[32];
    PwSha256 sha;

    (void)unused;
    snprintf(final, sizeof(final), PW_UPDATE_PKG_DIR "/%s", asset.name);
    snprintf(part, sizeof(part), "%s.part", final);
    note("update download: %s -> %s (%llu bytes)", asset.api_url[0] ? asset.api_url : asset.url, final, asset.size);
    if (bind_http()) { download_set(PW_UPDATE_DOWNLOAD_FAILED, 0, "The console's HTTPS is unavailable"); return NULL; }
    if ((net_pool = sceNetPoolCreate("pw-update-download", 16 * 1024, 0)) < 0 ||
        (ssl_ctx = http.ssl_init(256 * 1024)) < 0 || (ctx = http.init(net_pool, ssl_ctx, 256 * 1024)) < 0 ||
        (template_id = http.create_template(ctx, PW_UPDATE_AGENT "-update/" PW_RELEASE_VERSION, HTTP_VERSION_1_1, 1)) < 0) {
        download_set(PW_UPDATE_DOWNLOAD_FAILED, 0, "Could not start the console's HTTPS");
        goto done;
    }
    http.resolve_timeout(template_id, PW_UPDATE_TIMEOUT_US);
    http.connect_timeout(template_id, PW_UPDATE_TIMEOUT_US);
    http.recv_timeout(template_id, 4u * PW_UPDATE_TIMEOUT_US);
    /* Redirects are followed here, where they are logged; libSceHttp's own
     * following is turned off when it can be. */
    if (http.auto_redirect) http.auto_redirect(template_id, 0);
    if (http.header_max) {
        int rc = http.header_max(template_id, 64 * 1024);
        note("update download: response header limit 64 KiB rc=%#x", (unsigned)rc);
    } else {
        note("update download: sceHttpSetResponseHeaderMaxSize not found");
    }
    if ((request = open_download(template_id, asset.api_url[0] ? asset.api_url : asset.url, &connection)) < 0)
        goto done;
    if ((buffer = mmap(NULL, PW_UPDATE_CHUNK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)) == MAP_FAILED) {
        download_set(PW_UPDATE_DOWNLOAD_FAILED, 0, "Out of memory");
        goto done;
    }
    mkdir(PW_UPDATE_PKG_DIR, 0777);
    if ((fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
        snprintf(detail, sizeof(detail), "Cannot write %s (errno %d)", PW_UPDATE_PKG_DIR, errno);
        note("update download: open %s errno=%d", part, errno);
        download_set(PW_UPDATE_DOWNLOAD_FAILED, 0, detail);
        goto done;
    }
    pw_sha256_init(&sha);
    download_set(PW_UPDATE_DOWNLOAD_RUNNING, 0, NULL);
    for (;;) {
        int n;
        if (download_cancel) { download_set(PW_UPDATE_DOWNLOAD_FAILED, done, "Cancelled"); goto removed; }
        if ((n = http.read(request, buffer, PW_UPDATE_CHUNK)) < 0) {
            snprintf(detail, sizeof(detail), "Download stopped at %llu MB (%#x)", done >> 20, (unsigned)n);
            note("update download: read %#x at %llu", (unsigned)n, done);
            download_set(PW_UPDATE_DOWNLOAD_FAILED, done, detail);
            goto removed;
        }
        if (n == 0) break;
        for (int at = 0; at < n;) {
            ssize_t wrote = write(fd, buffer + at, (size_t)(n - at));
            if (wrote <= 0) {
                snprintf(detail, sizeof(detail), "Could not write the package (errno %d%s)", errno,
                         errno == ENOSPC ? ": disk full" : "");
                note("update download: write errno=%d at %llu", errno, done);
                download_set(PW_UPDATE_DOWNLOAD_FAILED, done, detail);
                goto removed;
            }
            at += (int)wrote;
        }
        pw_sha256_update(&sha, buffer, (size_t)n);
        done += (unsigned)n;
        download_set(PW_UPDATE_DOWNLOAD_RUNNING, done, NULL);
        if (done - last_note >= (64ull << 20)) { note("update download: %llu MB", done >> 20); last_note = done; }
        if (done > asset.size) break;
    }
    pw_sha256_final(&sha, digest);
    pw_sha256_hex(digest, hex);
    if (fsync(fd) != 0 || close(fd) != 0) { fd = -1; download_set(PW_UPDATE_DOWNLOAD_FAILED, done, "Could not finish the file"); goto removed; }
    fd = -1;
    note("update download: %llu of %llu bytes, sha256 %s (want %s)", done, asset.size, hex, asset.sha256);
    if (done != asset.size || strcmp(hex, asset.sha256)) {
        download_set(PW_UPDATE_DOWNLOAD_FAILED, done, done != asset.size ? "Incomplete download" :
                                                                          "Checksum did not match GitHub's");
        goto removed;
    }
    if (rename(part, final) != 0) {
        snprintf(detail, sizeof(detail), "Could not rename into %s (errno %d)", PW_UPDATE_PKG_DIR, errno);
        download_set(PW_UPDATE_DOWNLOAD_FAILED, done, detail);
        goto removed;
    }
    snprintf(detail, sizeof(detail), "In %s: install it with GoldHEN", PW_UPDATE_PKG_DIR);
    download_set(PW_UPDATE_DOWNLOAD_DONE, done, detail);
    note("update download: verified, %s", final);
    goto done;
removed:
    if (fd >= 0) { close(fd); fd = -1; }
    unlink(part);
done:
    if (fd >= 0) close(fd);
    if (buffer != MAP_FAILED) munmap(buffer, PW_UPDATE_CHUNK);
    if (request >= 0) http.delete_request(request);
    if (connection >= 0) http.delete_connection(connection);
    if (template_id >= 0) http.delete_template(template_id);
    if (ctx >= 0) http.term(ctx);
    if (ssl_ctx >= 0) http.ssl_term(ssl_ctx);
    if (net_pool >= 0) sceNetPoolDestroy(net_pool);
    return NULL;
}

int pw_update_ps4_download_start(void)
{
    pthread_t thread;
    pthread_attr_t attr;
    int started;

    pthread_mutex_lock(&download_lock);
    started = download.state == PW_UPDATE_DOWNLOAD_READY || download.state == PW_UPDATE_DOWNLOAD_FAILED;
    if (started) {
        download.state = PW_UPDATE_DOWNLOAD_RUNNING;
        download.done = 0;
        download.detail[0] = 0;
    }
    pthread_mutex_unlock(&download_lock);
    if (!started) return -1;
    download_cancel = 0;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, download_run, NULL)) {
        download_set(PW_UPDATE_DOWNLOAD_FAILED, 0, "No thread for the download");
        started = 0;
    }
    pthread_attr_destroy(&attr);
    return started ? 0 : -1;
}

void pw_update_ps4_download_cancel(void)
{
    download_cancel = 1;
}

void pw_update_ps4_download_status(PwUpdateDownload *out)
{
    pthread_mutex_lock(&download_lock);
    *out = download;
    pthread_mutex_unlock(&download_lock);
}

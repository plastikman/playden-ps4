/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_UPDATE_PS4_H
#define PW_UPDATE_PS4_H
/*
 * The launcher's check for a newer stable release, on the console's own HTTPS
 * stack (libSceHttp over libSceSsl). It runs once, on a thread of its own, so
 * the launcher never waits for the network; pw_update_ps4_notice() answers
 * NULL until a newer release has been found, and then the line to show.
 *
 * Nothing is checked when the build does not know its own release
 * (PW_RELEASE_VERSION, from orbis-win's VERSION file), or when
 * <root>/no-update-check exists. Nothing is downloaded unless asked
 * (pw_update_ps4_download_start), and nothing is ever installed.
 */

/* Starts the check (once). root is the library root, e.g. /data/prospero-win;
 * log, if given, receives one line per step worth knowing about. */
void pw_update_ps4_start(const char *root, void (*log)(const char *line));

/* The notice to show, or NULL. Safe to call every frame. */
const char *pw_update_ps4_notice(void);

/* The newer release's package, downloaded on request into /data/pkg under
 * its release name, through <name>.part, and kept only when its size and
 * SHA-256 match what GitHub publishes for it. Installing stays GoldHEN's. */
typedef enum PwUpdateDownloadState {
    PW_UPDATE_DOWNLOAD_NONE,          /* no newer release with a package (yet) */
    PW_UPDATE_DOWNLOAD_READY,         /* one can be downloaded */
    PW_UPDATE_DOWNLOAD_RUNNING,
    PW_UPDATE_DOWNLOAD_DONE,          /* verified, in /data/pkg */
    PW_UPDATE_DOWNLOAD_FAILED,        /* detail says why; it can be started again */
} PwUpdateDownloadState;
typedef struct PwUpdateDownload {
    PwUpdateDownloadState state;
    unsigned long long done, total;   /* bytes */
    char tag[32];                     /* the release, e.g. "v0.0.3" */
    char detail[96];
} PwUpdateDownload;

/* Starts the download when one is READY (or FAILED, to try again); 0 when
 * it started. */
int pw_update_ps4_download_start(void);
/* Asks a running download to stop; its partial file is removed. */
void pw_update_ps4_download_cancel(void);
void pw_update_ps4_download_status(PwUpdateDownload *out);

#endif

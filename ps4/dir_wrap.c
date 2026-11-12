// Directory listing for the Playden UI PRX.
//
// On PS4 this title's opendir() fails with EPERM for every folder, even its own /app0 files
// (measured on 02.34: /data/PLDN10001, its games, /data/pkg, /app0/homebrew), while
// sceKernelOpen(O_DIRECTORY) + sceKernelGetdents lists all of them. The link step wraps
// opendir, fdopendir, readdir and closedir (-Wl,--wrap) so every caller in the module, the
// toolchain's std::filesystem included, lists folders through the kernel calls instead.
//
// sceKernelGetdents returns FreeBSD dirent records, which is the SDK's struct dirent layout
// (u32 fileno, u16 reclen, u8 type, u8 namlen, name), so readdir hands back records in place.

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int32_t sceKernelOpen(const char *, int32_t, uint16_t);
int32_t sceKernelClose(int32_t);
int32_t sceKernelGetdents(int32_t, char *, int);

enum {
    kDirectoryFlag = 0x00020000,    // FreeBSD O_DIRECTORY (O_RDONLY is 0)
    kBlock = 64 * 1024,             // /data's directory block; getdents wants a whole block
    kMagic = 0x504c4452,            // "PLDR"
};

struct PlaydenDir {
    uint32_t magic;
    int fd;
    int length;
    int offset;
    char block[kBlock];
};

static int kernel_errno(int32_t result)
{
    // SCE kernel errors are 0x80020000 + the FreeBSD errno.
    const uint32_t code = (uint32_t)result;
    return (code & 0xffff0000u) == 0x80020000u ? (int)(code & 0xffffu) : EIO;
}

static DIR *wrap_fd(int fd)
{
    struct PlaydenDir *directory = malloc(sizeof *directory);
    if (!directory) {
        errno = ENOMEM;
        return NULL;
    }
    directory->magic = kMagic;
    directory->fd = fd;
    directory->length = 0;
    directory->offset = 0;
    return (DIR *)directory;
}

DIR *__wrap_opendir(const char *path)
{
    if (!path || !path[0]) {
        errno = ENOENT;
        return NULL;
    }
    const int32_t fd = sceKernelOpen(path, kDirectoryFlag, 0);
    if (fd < 0) {
        errno = kernel_errno(fd);
        return NULL;
    }
    DIR *directory = wrap_fd(fd);
    if (!directory)
        sceKernelClose(fd);
    return directory;
}

DIR *__wrap_fdopendir(int fd)
{
    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }
    return wrap_fd(fd);
}

struct dirent *__wrap_readdir(DIR *handle)
{
    struct PlaydenDir *directory = (struct PlaydenDir *)handle;
    if (!directory || directory->magic != kMagic) {
        errno = EBADF;
        return NULL;
    }
    for (;;) {
        if (directory->offset >= directory->length) {
            const int32_t got = sceKernelGetdents(directory->fd, directory->block, kBlock);
            if (got <= 0) {
                if (got < 0)
                    errno = kernel_errno(got);
                directory->length = directory->offset = 0;
                return NULL;  // end of folder: errno untouched, as readdir does
            }
            directory->length = got;
            directory->offset = 0;
        }
        char *record = directory->block + directory->offset;
        const int left = directory->length - directory->offset;
        struct dirent *entry = (struct dirent *)record;
        if (left < 8 || entry->d_reclen < 8 || entry->d_reclen > left ||
            (int)entry->d_namlen + 8 >= entry->d_reclen) {
            directory->length = directory->offset = 0;
            errno = EIO;
            return NULL;
        }
        directory->offset += entry->d_reclen;
        if (entry->d_namlen == 0)
            continue;
        entry->d_name[entry->d_namlen] = '\0';  // within d_reclen (checked above)
        return entry;
    }
}

int __wrap_closedir(DIR *handle)
{
    struct PlaydenDir *directory = (struct PlaydenDir *)handle;
    if (!directory || directory->magic != kMagic) {
        errno = EBADF;
        return -1;
    }
    const int32_t result = sceKernelClose(directory->fd);
    directory->magic = 0;
    free(directory);
    if (result < 0) {
        errno = kernel_errno(result);
        return -1;
    }
    return 0;
}

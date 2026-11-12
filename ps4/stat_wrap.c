// stat/lstat/fstatat for the Playden UI PRX.
//
// The SDK's stat (orbis-compat src/orbis_stat.cpp) asks sceKernelStat, which on the PS4 fills the
// mode, size and times but leaves st_dev, st_ino, st_uid and st_gid zero for every path (02.38
// probe: lstat("/data/PLDN10001") dev=0 ino=0 uid=0, fstat of the opened folder dev=688127856
// ino=24676763 uid=1). Playden's ManagedDirectory compares lstat against fstat of what it opened
// and refused every folder with ESTALE, so game settings could not be stored.
//
// The link step wraps stat, lstat and fstatat (-Wl,--wrap=): when the kernel's answer carries no
// identity, the file or folder is opened and fstat's complete answer is used instead. lstat follows
// links like the SDK's (a PS4 title's filesystems have none). Orbis-Win's own build is unchanged.

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int __real_stat(const char *path, struct stat *out);
int __real_fstatat(int fd, const char *path, struct stat *out, int flag);

int __wrap_stat(const char *path, struct stat *out)
{
    const int rc = __real_stat(path, out);
    if (rc != 0 || out->st_ino != 0 || out->st_dev != 0)
        return rc;
    const int directory = S_ISDIR(out->st_mode);
    if (!directory && !S_ISREG(out->st_mode))
        return rc;
    const int saved = errno;
    const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | (directory ? O_DIRECTORY : 0));
    if (fd >= 0) {
        struct stat full;
        if (fstat(fd, &full) == 0 && (full.st_mode & S_IFMT) == (out->st_mode & S_IFMT))
            *out = full;
        close(fd);
    }
    errno = saved;
    return rc;
}

int __wrap_lstat(const char *path, struct stat *out)
{
    return __wrap_stat(path, out);
}

int __wrap_fstatat(int fd, const char *path, struct stat *out, int flag)
{
    if (path && out && (path[0] == '/' || fd == AT_FDCWD))
        return __wrap_stat(path, out);
    return __real_fstatat(fd, path, out, flag);
}

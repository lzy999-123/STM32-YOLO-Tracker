/* Apply AF41 to the IPv4 RTSP/RTP sockets owned by rkipc.
 * Linux 5.10 pidfd_getfd lets this small helper update existing sockets without
 * replacing the encoder, restarting Wi-Fi, or adding media buffers.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#if !defined(SYS_pidfd_open) || !defined(SYS_pidfd_getfd)
#if defined(__arm__) || defined(__aarch64__) || defined(__x86_64__) || defined(__i386__)
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif
#else
#error Build with Linux headers that define the pidfd syscall numbers
#endif
#endif

#define VIDEO_TOS 0x88 /* DSCP AF41; RTL8188EU maps this to WMM video priority 4. */
static volatile sig_atomic_t running = 1;

static void stop_service(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int number(const char *text)
{
    char *end;
    long value;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < 0 || value > INT_MAX)
        return -1;
    return (int)value;
}

static int is_encoder(int pid)
{
    char path[64], target[256];
    ssize_t size;
    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    size = readlink(path, target, sizeof(target) - 1);
    if (size < 0)
        return 0;
    target[size] = '\0';
    return strcmp(target, "/oem/usr/bin/rkipc") == 0;
}

static int mark_sockets(int pid, int tos)
{
    char directory[64];
    struct dirent *entry;
    DIR *fds;
    int pidfd, changed = 0;
    if (!is_encoder(pid))
        return 0;
    pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0)
        return errno == ESRCH ? 0 : -1;
    snprintf(directory, sizeof(directory), "/proc/%d/fd", pid);
    fds = opendir(directory);
    if (!fds) {
        close(pidfd);
        return 0;
    }
    while ((entry = readdir(fds))) {
        char path[96], target[80];
        struct sockaddr_in local;
        socklen_t size;
        int original = number(entry->d_name), fd, type, current;
        ssize_t length;
        if (original < 0)
            continue;
        snprintf(path, sizeof(path), "/proc/%d/fd/%d", pid, original);
        length = readlink(path, target, sizeof(target) - 1);
        if (length < 0)
            continue;
        target[length] = '\0';
        if (strncmp(target, "socket:[", 8))
            continue;
        fd = (int)syscall(SYS_pidfd_getfd, pidfd, original, 0);
        if (fd < 0) {
            if (errno == ENOSYS || errno == EPERM || errno == EACCES) {
                int saved = errno;
                closedir(fds);
                close(pidfd);
                errno = saved;
                return -1;
            }
            continue; /* Socket closed between enumeration and duplication. */
        }
        size = sizeof(local);
        if (getsockname(fd, (struct sockaddr *)&local, &size) || local.sin_family != AF_INET)
            goto next_socket;
        size = sizeof(type);
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size))
            goto next_socket;
        /* TCP is limited to RTSP port 554. rkipc's RTP/RTCP use dynamic UDP ports;
         * control/discovery sockets belong to other processes and are untouched. */
        if (!((type == SOCK_STREAM && ntohs(local.sin_port) == 554) ||
              (type == SOCK_DGRAM && local.sin_port != 0)))
            goto next_socket;
        size = sizeof(current);
        if (getsockopt(fd, IPPROTO_IP, IP_TOS, &current, &size))
            goto next_socket;
        if (current != tos && !setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)))
            ++changed;
next_socket:
        close(fd); /* Never retain the encoder's sockets or delay their teardown. */
    }
    closedir(fds);
    close(pidfd);
    if (changed)
        fprintf(stderr, "video-qos: rkipc %d, updated %d socket(s), tos=0x%02x\n", pid, changed, tos);
    return changed;
}

static int scan_encoders(int tos)
{
    DIR *processes = opendir("/proc");
    struct dirent *entry;
    int changed = 0;
    if (!processes)
        return -1;
    while ((entry = readdir(processes))) {
        int pid = number(entry->d_name), result;
        if (pid <= 0)
            continue;
        result = mark_sockets(pid, tos);
        if (result < 0) {
            int saved = errno;
            closedir(processes);
            errno = saved;
            return -1;
        }
        changed += result;
    }
    closedir(processes);
    return changed;
}

int main(int argc, char **argv)
{
    int once = 0, tos = VIDEO_TOS, pid = 0, argument;
    for (argument = 1; argument < argc; ++argument) {
        if (!strcmp(argv[argument], "--once"))
            once = 1;
        else if (!strcmp(argv[argument], "--restore")) {
            once = 1;
            tos = 0;
        } else if (!strcmp(argv[argument], "--pid") && argument + 1 < argc) {
            pid = number(argv[++argument]);
            if (pid <= 0)
                goto usage;
        } else
            goto usage;
    }
    if (pid && !once)
        goto usage;
    signal(SIGTERM, stop_service);
    signal(SIGINT, stop_service);
    do {
        int result = pid ? mark_sockets(pid, tos) : scan_encoders(tos);
        if (result < 0) {
            perror("video-qos: pidfd socket access failed");
            return 1;
        }
        if (!once && running)
            sleep(1);
    } while (!once && running);
    return 0;
usage:
    fprintf(stderr, "usage: %s [--once|--restore] [--pid PID]\n", argv[0]);
    return 2;
}

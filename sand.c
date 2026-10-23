/*
 * sand - run a command (default: a shell) in the current directory
 * with restrictions.
 *
 * The shell runs in its own user, mount, pid, net, ipc and uts namespaces.
 * It sees an empty root containing only:
 *   - the host's system directories (/usr /bin /sbin /lib /lib64 /etc), read-only
 *   - the current working directory, read-only, at its own path
 *   - any extra paths given with --ro or --rw, at their own paths,
 *     minus anything given with --hide
 *   - a fresh /tmp, a fresh /proc, and a minimal /dev with its own
 *     /dev/shm and /dev/pts
 * By default it has no network: the new net namespace holds nothing but
 * a down loopback. --net host keeps the host's network instead, and
 * --allow/--block rules then filter it by address and port: a seccomp
 * filter in the command traps the socket calls that carry an address and
 * init, acting as a broker, checks the address and performs the call on
 * the command's own socket. Copying the address out of the command's
 * memory first is what makes this race-free.
 *
 * Process layout:
 *   sand (host)  ->  init (pid 1 in the sandbox)  ->  command
 * init reaps orphans and forwards SIGTERM/SIGHUP to the command.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

/* The new root is staged on a tmpfs mounted over this directory.
 * The mount only exists inside the sandbox's mount namespace. */
#define STAGE "/tmp"

/* Room for a host path with STAGE prepended. */
#define STAGED_MAX (PATH_MAX + 32)

static const char *system_dirs[] = {
    "/usr",
    "/bin",
    "/sbin",
    "/lib",
    "/lib64",
    "/etc",
    NULL,
};

static const char *devices[] = {
    "null",
    "zero",
    "random",
    "urandom",
    "tty",
    NULL,
};

/* Host paths bound into the sandbox: the cwd plus every --ro and --rw,
 * in command line order, so a later flag wins over an earlier one.
 * Each is opened inside the new mount namespace before the staging tmpfs
 * hides anything, so paths under /tmp keep working. */
struct bind {
    char *path;      /* on the host */
    char *dst;       /* inside the sandbox, normally the same */
    int read_only;
    int hide;        /* cover dst with something empty instead */
    int fd;
};

/* Empty file bound over files to hide them. It lives on the /dev tmpfs:
 * the root tmpfs cannot be remounted read-only while it holds an
 * unlinked inode that is still in use. */
#define EMPTY_FILE STAGE "/dev/.empty"

#define MAX_BINDS 64

static struct bind binds[MAX_BINDS];
static int bind_count = 0;

/* The command to run, NULL-terminated argv; empty means the shell. */
static char **command = NULL;

/* --net host: stay in the host's network namespace. */
static int host_network = 0;

/* --allow / --block rules, in command line order; a later rule wins.
 * Destination rules apply to connect and to the address given to sendto,
 * sendmsg and sendmmsg. "src:" rules apply to bind. */
struct rule {
    int allow;
    int src;
    int family;             /* AF_INET, AF_INET6, or 0 for any */
    unsigned char addr[16];
    int prefix;
    int port_lo;
    int port_hi;
};

#define MAX_RULES 64

static struct rule rules[MAX_RULES];
static int rule_count = 0;

/* Pipe pair between init and the command for handing over the seccomp
 * listener; plain read/write, which the filter does not trap. */
static int notify_socket[2];

/* Extra environment entries from --env, as "NAME=VALUE" strings. */
#define MAX_ENV 64

static char *extra_env[MAX_ENV];
static int extra_env_count = 0;

/* The process that signals get forwarded to. */
static pid_t forward_to = 0;

static void die(const char *fmt, ...)
{
    va_list ap;
    int saved_errno = errno;

    fprintf(stderr, "sand: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (saved_errno != 0) fprintf(stderr, ": %s", strerror(saved_errno));
    fprintf(stderr, "\n");
    exit(1);
}

static void write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) die("open %s", path);
    if (write(fd, text, strlen(text)) < 0) die("write %s", path);
    close(fd);
}

/* mkdir -p for an absolute path. */
static void make_dirs(const char *path)
{
    char buf[STAGED_MAX];
    char *p;

    snprintf(buf, sizeof(buf), "%s", path);
    for (p = buf + 1; *p != '\0'; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) < 0 && errno != EEXIST) die("mkdir %s", buf);
        *p = '/';
    }
    if (mkdir(buf, 0755) < 0 && errno != EEXIST) die("mkdir %s", buf);
}

/* Arguments for mount_setattr(2); defined here so the build does not
 * depend on the C library shipping the wrapper. */
struct mount_attr_args {
    unsigned long long attr_set;
    unsigned long long attr_clr;
    unsigned long long propagation;
    unsigned long long userns_fd;
};

#define ATTR_RDONLY 0x01
#define ATTR_NOSUID 0x02
#define AT_RECURSIVE_FLAG 0x8000

/* Bind src onto dst recursively, then set flags on the whole subtree.
 * Recursive is required: inside a user namespace the kernel refuses a
 * plain bind of a directory that has locked submounts under it. */
static void bind_path(const char *src, const char *dst, int read_only)
{
    struct mount_attr_args attr;

    if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL) < 0) die("bind %s", src);

    memset(&attr, 0, sizeof(attr));
    attr.attr_set = ATTR_NOSUID;
    if (read_only) attr.attr_set |= ATTR_RDONLY;
    if (syscall(SYS_mount_setattr, AT_FDCWD, dst, AT_RECURSIVE_FLAG, &attr, sizeof(attr)) < 0) die("set flags on %s", dst);
}

/* Add one host system directory to the staged root.
 * On merged-usr systems /bin, /sbin and /lib are symlinks into /usr;
 * those are recreated as symlinks instead of being bound. */
static void add_system_dir(const char *dir)
{
    char dst[STAGED_MAX];
    char target[PATH_MAX];
    struct stat st;
    ssize_t n;

    if (lstat(dir, &st) < 0) return;
    snprintf(dst, sizeof(dst), STAGE "%s", dir);

    if (S_ISLNK(st.st_mode)) {
        n = readlink(dir, target, sizeof(target) - 1);
        if (n < 0) die("readlink %s", dir);
        target[n] = '\0';
        if (symlink(target, dst) < 0 && errno != EEXIST) die("symlink %s", dst);
        return;
    }

    make_dirs(dst);
    bind_path(dir, dst, 1);
}

/* Cover a path that is visible in the staged root with an empty
 * read-only directory or an empty file. A path that is not visible
 * needs nothing. */
static void hide_path(const char *path)
{
    char dst[STAGED_MAX];
    struct stat st;

    snprintf(dst, sizeof(dst), STAGE "%s", path);
    if (stat(dst, &st) < 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (mount("tmpfs", dst, "tmpfs", MS_RDONLY | MS_NOSUID | MS_NODEV, "mode=755") < 0) die("hide %s", path);
        return;
    }
    bind_path(EMPTY_FILE, dst, 1);
}

static void add_bind(const struct bind *b)
{
    char src[PATH_MAX];
    char dst[STAGED_MAX];
    char *slash;
    struct stat st;
    int fd;

    if (b->hide) {
        hide_path(b->dst);
        return;
    }

    snprintf(src, sizeof(src), "/proc/self/fd/%d", b->fd);
    snprintf(dst, sizeof(dst), STAGE "%s", b->dst);
    if (fstat(b->fd, &st) < 0) die("stat %s", b->path);

    if (S_ISDIR(st.st_mode)) {
        make_dirs(dst);
    } else {
        /* A file is bound onto an empty file of the same name.
         * Opened read-only so an existing file on a read-only bind
         * (for example under /etc) is accepted as the mount point. */
        slash = strrchr(dst, '/');
        *slash = '\0';
        make_dirs(dst);
        *slash = '/';
        fd = open(dst, O_RDONLY | O_CREAT, 0644);
        if (fd < 0) die("create %s", dst);
        close(fd);
    }
    bind_path(src, dst, b->read_only);
}

static void add_dev(void)
{
    const char **name;
    char src[PATH_MAX];
    char dst[STAGED_MAX];
    int fd;

    if (mount("tmpfs", STAGE "/dev", "tmpfs", MS_NOSUID | MS_NOEXEC, "mode=755") < 0) die("mount /dev");

    for (name = devices; *name != NULL; name++) {
        snprintf(src, sizeof(src), "/dev/%s", *name);
        snprintf(dst, sizeof(dst), STAGE "/dev/%s", *name);
        fd = open(dst, O_WRONLY | O_CREAT, 0666);
        if (fd < 0) die("create %s", dst);
        close(fd);
        if (mount(src, dst, NULL, MS_BIND, NULL) < 0) die("bind %s", src);
    }

    if (symlink("/proc/self/fd", STAGE "/dev/fd") < 0) die("symlink /dev/fd");
    if (symlink("/proc/self/fd/0", STAGE "/dev/stdin") < 0) die("symlink /dev/stdin");
    if (symlink("/proc/self/fd/1", STAGE "/dev/stdout") < 0) die("symlink /dev/stdout");
    if (symlink("/proc/self/fd/2", STAGE "/dev/stderr") < 0) die("symlink /dev/stderr");

    /* Shared memory for multiprocessing and friends. */
    make_dirs(STAGE "/dev/shm");
    if (mount("tmpfs", STAGE "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") < 0) die("mount /dev/shm");

    /* A private set of pseudo-terminals, so programs can open ptys. */
    make_dirs(STAGE "/dev/pts");
    if (mount("devpts", STAGE "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "newinstance,ptmxmode=0666,mode=0620") < 0) die("mount /dev/pts");
    if (symlink("pts/ptmx", STAGE "/dev/ptmx") < 0) die("symlink /dev/ptmx");
}

/* Runs as pid 1 inside the new namespaces, before the shell starts. */
static void build_root(const char *cwd)
{
    const char **dir;
    int i;
    int fd;

    /* Keep every mount change private to this namespace. */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) die("make / private");

    if (mount("tmpfs", STAGE, "tmpfs", MS_NOSUID | MS_NODEV, "mode=755") < 0) die("mount staging root");

    make_dirs(STAGE "/tmp");
    if (mount("tmpfs", STAGE "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") < 0) die("mount /tmp");

    make_dirs(STAGE "/proc");
    if (mount("proc", STAGE "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0) die("mount /proc");

    make_dirs(STAGE "/dev");
    add_dev();

    for (dir = system_dirs; *dir != NULL; dir++) {
        add_system_dir(*dir);
    }

    /* User paths go in after the system dirs so that a --rw under /usr
     * or /etc lands on top of the read-only bind instead of under it. */
    fd = open(EMPTY_FILE, O_WRONLY | O_CREAT, 0444);
    if (fd < 0) die("create %s", EMPTY_FILE);
    close(fd);
    for (i = 0; i < bind_count; i++) {
        add_bind(&binds[i]);
    }
    /* The hidden files keep the inode; the name can go. */
    if (unlink(EMPTY_FILE) < 0) die("unlink %s", EMPTY_FILE);

    /* Swap the staged tree in as root and drop the old one. */
    make_dirs(STAGE "/oldroot");
    if (syscall(SYS_pivot_root, STAGE, STAGE "/oldroot") < 0) die("pivot_root");
    if (chdir("/") < 0) die("chdir /");
    if (umount2("/oldroot", MNT_DETACH) < 0) die("unmount old root");
    if (rmdir("/oldroot") < 0) die("rmdir /oldroot");

    /* Nothing may be created at the top level either. */
    if (mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV, NULL) < 0) die("remount / read-only");

    if (chdir(cwd) < 0) die("chdir %s", cwd);
}

static void forward_signal(int sig)
{
    if (forward_to > 0) kill(forward_to, sig);
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = forward_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    /* The terminal delivers these to the shell directly. */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
}

static int exit_status(int status)
{
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

#if defined(__x86_64__)
#define SAND_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define SAND_ARCH AUDIT_ARCH_AARCH64
#else
#error "unsupported architecture"
#endif

/* Installed in the command right before exec. The listed calls are
 * handed to init; everything else runs normally. io_uring is refused
 * because it could issue the same calls without passing through here.
 * Returns the listener fd that init will take over. */
static int install_filter(void)
{
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SAND_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_connect, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_bind, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sendmsg, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sendmmsg, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_io_uring_setup, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        /* sendto only when an address is given: args[4] (64-bit) != 0.
         * send() is sendto with a NULL address and stays fast. */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sendto, 0, 5),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[4])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 2),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[4]) + 4),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog;
    int listener;

    prog.len = sizeof(filter) / sizeof(filter[0]);
    prog.filter = filter;
    listener = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
    if (listener < 0 && errno == EBUSY) {
        errno = 0;
        die("--allow/--block cannot be used inside a sandbox that already has them (the kernel allows one listener); the outer rules still apply");
    }
    if (listener < 0) die("seccomp");
    return listener;
}

/* ---- the broker, running in init ---- */

#define MAX_SEND (1024 * 1024)
#define MAX_IOV 64
#define MAX_MSGS 64

/* One trapped call being handled. */
struct request {
    int listener;
    struct seccomp_notif notif;
    pid_t pid;
    int sock;
};

static int read_mem(pid_t pid, unsigned long addr, void *buf, size_t len)
{
    struct iovec local;
    struct iovec remote;

    if (len == 0) return 0;
    local.iov_base = buf;
    local.iov_len = len;
    remote.iov_base = (void *)addr;
    remote.iov_len = len;
    if (process_vm_readv(pid, &local, 1, &remote, 1, 0) != (ssize_t)len) return -1;
    return 0;
}

static int write_mem(pid_t pid, unsigned long addr, const void *buf, size_t len)
{
    struct iovec local;
    struct iovec remote;

    local.iov_base = (void *)buf;
    local.iov_len = len;
    remote.iov_base = (void *)addr;
    remote.iov_len = len;
    if (process_vm_writev(pid, &local, 1, &remote, 1, 0) != (ssize_t)len) return -1;
    return 0;
}

/* Take a copy of the socket the trapped call was made on. The request
 * names a thread, and pidfd_open only accepts a thread group leader, so
 * the leader is looked up in /proc. Threads share their fd table; the
 * inode check catches the rare clone() that does not. */
static int take_socket(pid_t tid, unsigned long fd)
{
    char path[64];
    char line[128];
    FILE *f;
    pid_t tgid = -1;
    int pidfd;
    int sock;
    struct stat link;
    struct stat own;

    snprintf(path, sizeof(path), "/proc/%d/status", tid);
    f = fopen(path, "r");
    if (f == NULL) return -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "Tgid: %d", &tgid) == 1) break;
    }
    fclose(f);
    if (tgid < 0) return -1;

    pidfd = syscall(SYS_pidfd_open, tgid, 0);
    if (pidfd < 0) return -1;
    sock = syscall(SYS_pidfd_getfd, pidfd, fd, 0);
    close(pidfd);
    if (sock < 0) return -1;

    snprintf(path, sizeof(path), "/proc/%d/fd/%lu", tid, fd);
    if (stat(path, &link) < 0 || fstat(sock, &own) < 0 || link.st_ino != own.st_ino || link.st_dev != own.st_dev) {
        close(sock);
        return -1;
    }
    return sock;
}

/* The command may have died and its pid been reused since the request
 * arrived; the kernel confirms it is still the same pending call. */
static int request_valid(struct request *req)
{
    return ioctl(req->listener, SECCOMP_IOCTL_NOTIF_ID_VALID, &req->notif.id) == 0;
}

static int prefix_matches(const unsigned char *a, const unsigned char *b, int bits)
{
    int full = bits / 8;
    int rest = bits % 8;
    unsigned char mask;

    if (memcmp(a, b, full) != 0) return 0;
    if (rest == 0) return 1;
    mask = 0xff << (8 - rest);
    return (a[full] & mask) == (b[full] & mask);
}

static int rule_matches(const struct rule *r, int family, const unsigned char *addr, int port)
{
    if (port < r->port_lo || port > r->port_hi) return 0;
    if (r->family == 0) return 1;
    if (r->family != family) return 0;
    return prefix_matches(addr, r->addr, r->prefix);
}

/* Decide for one socket address. The default is allow, or deny as soon
 * as any --allow exists for that direction. Non-IP addresses are left to
 * the kernel, which will reject them on an IP socket. */
static int address_allowed(const struct sockaddr_storage *ss, socklen_t len, int src)
{
    static const unsigned char mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    const struct sockaddr_in *in4;
    const struct sockaddr_in6 *in6;
    const unsigned char *addr;
    char text[INET6_ADDRSTRLEN];
    int family;
    int port;
    int allowed = 1;
    int i;

    if (ss->ss_family == AF_INET && len >= sizeof(*in4)) {
        in4 = (const struct sockaddr_in *)ss;
        family = AF_INET;
        addr = (const unsigned char *)&in4->sin_addr;
        port = ntohs(in4->sin_port);
    } else if (ss->ss_family == AF_INET6 && len >= sizeof(*in6)) {
        in6 = (const struct sockaddr_in6 *)ss;
        family = AF_INET6;
        addr = in6->sin6_addr.s6_addr;
        port = ntohs(in6->sin6_port);
        /* IPv4 traffic on an IPv6 socket uses ::ffff:a.b.c.d; judge it as IPv4. */
        if (memcmp(addr, mapped, 12) == 0) {
            family = AF_INET;
            addr += 12;
        }
    } else {
        return 1;
    }

    for (i = 0; i < rule_count; i++) {
        if (rules[i].src == src && rules[i].allow) allowed = 0;
    }
    for (i = 0; i < rule_count; i++) {
        if (rules[i].src != src) continue;
        if (!rule_matches(&rules[i], family, addr, port)) continue;
        allowed = rules[i].allow;
    }
    if (!allowed) {
        inet_ntop(family, addr, text, sizeof(text));
        fprintf(stderr, "sand: blocked %s %s port %d\n", src ? "bind to" : "connection to", text, port);
    }
    return allowed;
}

static void do_connect_or_bind(struct request *req, struct seccomp_notif_resp *resp)
{
    struct sockaddr_storage ss;
    socklen_t len = req->notif.data.args[2];
    int is_bind = req->notif.data.nr == __NR_bind;
    int r;

    memset(&ss, 0, sizeof(ss));
    if (len > sizeof(ss)) len = sizeof(ss);
    if (read_mem(req->pid, req->notif.data.args[1], &ss, len) < 0) {
        resp->error = -EFAULT;
        return;
    }
    if (!request_valid(req)) {
        resp->error = -ESRCH;
        return;
    }
    if (!address_allowed(&ss, len, is_bind)) {
        resp->error = -ENETUNREACH;
        if (is_bind) resp->error = -EACCES;
        return;
    }
    if (is_bind) {
        r = bind(req->sock, (struct sockaddr *)&ss, len);
    } else {
        r = connect(req->sock, (struct sockaddr *)&ss, len);
    }
    if (r < 0) resp->error = -errno;
}

/* Send data read from the command's memory, to the given address if any.
 * Returns bytes sent or -errno. */
static long send_pieces(struct request *req, const struct iovec *remote_iov, int count, unsigned long name_addr, socklen_t name_len, int flags)
{
    struct sockaddr_storage ss;
    struct iovec local[MAX_IOV];
    struct msghdr mh;
    unsigned char *buf;
    size_t total = 0;
    size_t off = 0;
    long r;
    int i;

    for (i = 0; i < count; i++) {
        total += remote_iov[i].iov_len;
        if (total > MAX_SEND) return -EMSGSIZE;
    }
    buf = malloc(total + 1);
    if (buf == NULL) return -ENOMEM;
    for (i = 0; i < count; i++) {
        if (read_mem(req->pid, (unsigned long)remote_iov[i].iov_base, buf + off, remote_iov[i].iov_len) < 0) {
            free(buf);
            return -EFAULT;
        }
        local[i].iov_base = buf + off;
        local[i].iov_len = remote_iov[i].iov_len;
        off += remote_iov[i].iov_len;
    }

    memset(&ss, 0, sizeof(ss));
    memset(&mh, 0, sizeof(mh));
    if (name_addr != 0 && name_len > 0) {
        if (name_len > sizeof(ss)) name_len = sizeof(ss);
        if (read_mem(req->pid, name_addr, &ss, name_len) < 0) {
            free(buf);
            return -EFAULT;
        }
        mh.msg_name = &ss;
        mh.msg_namelen = name_len;
    }
    if (!request_valid(req)) {
        free(buf);
        return -ESRCH;
    }
    if (mh.msg_name != NULL && !address_allowed(&ss, name_len, 0)) {
        free(buf);
        return -ENETUNREACH;
    }
    mh.msg_iov = local;
    mh.msg_iovlen = count;
    r = sendmsg(req->sock, &mh, flags | MSG_NOSIGNAL);
    if (r < 0) r = -errno;
    free(buf);
    return r;
}

static void do_sendto(struct request *req, struct seccomp_notif_resp *resp)
{
    struct iovec piece;
    long r;

    piece.iov_base = (void *)(unsigned long)req->notif.data.args[1];
    piece.iov_len = req->notif.data.args[2];
    r = send_pieces(req, &piece, 1, req->notif.data.args[4], req->notif.data.args[5], req->notif.data.args[3]);
    if (r < 0) {
        resp->error = r;
    } else {
        resp->val = r;
    }
}

/* A msghdr already copied from the command; its iov array still has to be. */
static long send_remote_msghdr(struct request *req, const struct msghdr *mh, int flags)
{
    struct iovec remote_iov[MAX_IOV];

    if (mh->msg_control != NULL && mh->msg_controllen > 0) return -EPERM;
    if (mh->msg_iovlen > MAX_IOV) return -EMSGSIZE;
    if (read_mem(req->pid, (unsigned long)mh->msg_iov, remote_iov, mh->msg_iovlen * sizeof(struct iovec)) < 0) return -EFAULT;
    return send_pieces(req, remote_iov, mh->msg_iovlen, (unsigned long)mh->msg_name, mh->msg_namelen, flags);
}

static void do_sendmsg(struct request *req, struct seccomp_notif_resp *resp)
{
    struct msghdr mh;
    long r;

    if (read_mem(req->pid, req->notif.data.args[1], &mh, sizeof(mh)) < 0) {
        resp->error = -EFAULT;
        return;
    }
    r = send_remote_msghdr(req, &mh, req->notif.data.args[2]);
    if (r < 0) {
        resp->error = r;
    } else {
        resp->val = r;
    }
}

static void do_sendmmsg(struct request *req, struct seccomp_notif_resp *resp)
{
    struct mmsghdr msgs[MAX_MSGS];
    unsigned long base = req->notif.data.args[1];
    unsigned int count = req->notif.data.args[2];
    unsigned int sent = 0;
    unsigned int i;
    long r = 0;

    if (count > MAX_MSGS) count = MAX_MSGS;
    if (read_mem(req->pid, base, msgs, count * sizeof(msgs[0])) < 0) {
        resp->error = -EFAULT;
        return;
    }
    for (i = 0; i < count; i++) {
        r = send_remote_msghdr(req, &msgs[i].msg_hdr, req->notif.data.args[3]);
        if (r < 0) break;
        msgs[i].msg_len = r;
        write_mem(req->pid, base + i * sizeof(msgs[0]) + offsetof(struct mmsghdr, msg_len), &msgs[i].msg_len, sizeof(msgs[i].msg_len));
        sent++;
    }
    if (sent == 0 && r < 0) {
        resp->error = r;
    } else {
        resp->val = sent;
    }
}

static void handle_request(int listener)
{
    struct request req;
    struct seccomp_notif_resp resp;
    socklen_t len = sizeof(int);
    int domain;

    memset(&req, 0, sizeof(req));
    req.listener = listener;
    if (ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req.notif) < 0) {
        if (errno == ENOENT || errno == EINTR) return;
        die("seccomp receive");
    }
    memset(&resp, 0, sizeof(resp));
    resp.id = req.notif.id;
    req.pid = req.notif.pid;
    req.sock = take_socket(req.pid, req.notif.data.args[0]);

    if (req.sock < 0) {
        resp.error = -EBADF;
    } else if (getsockopt(req.sock, SOL_SOCKET, SO_DOMAIN, &domain, &len) < 0) {
        resp.error = -ENOTSOCK;
    } else if (domain != AF_INET && domain != AF_INET6) {
        /* Not an IP socket: whatever the address says, it cannot reach
         * an IP peer, so let the call run as is. */
        resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
    } else if (req.notif.data.nr == __NR_connect || req.notif.data.nr == __NR_bind) {
        do_connect_or_bind(&req, &resp);
    } else if (req.notif.data.nr == __NR_sendto) {
        do_sendto(&req, &resp);
    } else if (req.notif.data.nr == __NR_sendmsg) {
        do_sendmsg(&req, &resp);
    } else if (req.notif.data.nr == __NR_sendmmsg) {
        do_sendmmsg(&req, &resp);
    } else {
        resp.error = -ENOSYS;
    }

    if (req.sock >= 0) close(req.sock);
    if (ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp) < 0 && errno != ENOENT) die("seccomp respond");
}

/* Serve requests until the command exits; returns its wait status. */
static int supervise(int listener, pid_t child)
{
    struct pollfd fds[2];
    int child_fd;
    int status;
    pid_t pid;

    child_fd = syscall(SYS_pidfd_open, child, 0);
    if (child_fd < 0) die("pidfd_open");
    fds[0].fd = listener;
    fds[0].events = POLLIN;
    fds[1].fd = child_fd;
    fds[1].events = POLLIN;

    for (;;) {
        fds[0].revents = 0;
        fds[1].revents = 0;
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            die("poll");
        }
        if (fds[0].revents & POLLIN) handle_request(listener);
        for (;;) {
            pid = waitpid(-1, &status, WNOHANG);
            if (pid <= 0) break;
            if (pid == child) return status;
        }
    }
}

/* Environment variables the command inherits from the host by default.
 * Everything else is dropped unless named with --env: the host environment
 * may hold secrets, and prompt hooks pointing at files that do not exist
 * inside break the shell. */
static const char *kept_env[] = {
    "PATH",
    "TERM",
    "HOME",
    "USER",
    "LANG",
    "SHELL",
    NULL,
};

static void run_command(void)
{
    const char *shell = getenv("SHELL");
    const char **name;
    const char *value;
    char *env[16 + MAX_ENV];
    int n = 0;
    int i;
    int listener;
    char ack;

    for (name = kept_env; *name != NULL; name++) {
        value = getenv(*name);
        if (value == NULL) continue;
        if (asprintf(&env[n], "%s=%s", *name, value) < 0) die("asprintf");
        n++;
    }
    for (i = 0; i < extra_env_count; i++) {
        env[n] = extra_env[i];
        n++;
    }
    env[n] = "SAND=1";
    n++;
    env[n] = NULL;

    if (rule_count > 0) {
        /* Tell init the listener's fd number, wait until it has taken a
         * copy, then drop ours so the command never holds it. */
        listener = install_filter();
        if (write(notify_socket[1], &listener, sizeof(listener)) != sizeof(listener)) die("write to init");
        if (read(notify_socket[1], &ack, 1) != 1) die("read from init");
        close(listener);
        close(notify_socket[1]);
    }

    if (command != NULL) {
        execvpe(command[0], command, env);
        /* Same codes a shell uses, so callers can tell these apart. */
        fprintf(stderr, "sand: %s: %s\n", command[0], strerror(errno));
        if (errno == ENOENT) exit(127);
        exit(126);
    }
    if (shell != NULL) execle(shell, shell, (char *)NULL, env);
    execle("/bin/sh", "/bin/sh", (char *)NULL, env);
    die("exec shell");
}

/* pid 1 inside the sandbox: start the command, reap, exit when it exits. */
/* The new network namespace starts with loopback down; bring it up so
 * programs can talk to themselves on 127.0.0.1. Nothing else exists
 * in that namespace, so this grants no access to the host. */
static void loopback_up(void)
{
    struct ifreq ifr;
    int sock;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) die("socket");
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "lo");
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) die("get loopback flags");
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) die("bring loopback up");
    close(sock);
}

static int run_init(const char *cwd)
{
    pid_t child;
    pid_t pid;
    int status;
    int i;
    int number;
    int pidfd;
    int listener = -1;

    /* Opened here, inside the new mount namespace: the kernel refuses to
     * bind from an fd that belongs to another namespace. */
    for (i = 0; i < bind_count; i++) {
        if (binds[i].hide) continue;
        binds[i].fd = open(binds[i].path, O_PATH);
        if (binds[i].fd < 0) die("open %s", binds[i].path);
    }
    if (!host_network) loopback_up();
    build_root(cwd);
    for (i = 0; i < bind_count; i++) {
        if (binds[i].hide) continue;
        close(binds[i].fd);
    }
    if (sethostname("sand", 4) < 0) die("sethostname");

    if (rule_count > 0) {
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, notify_socket) < 0) die("socketpair");
    }

    child = fork();
    if (child < 0) die("fork");
    if (child == 0) run_command();

    forward_to = child;
    install_signal_handlers();

    if (rule_count > 0) {
        close(notify_socket[1]);
        if (read(notify_socket[0], &number, sizeof(number)) == sizeof(number)) {
            pidfd = syscall(SYS_pidfd_open, child, 0);
            if (pidfd < 0) die("pidfd_open");
            listener = syscall(SYS_pidfd_getfd, pidfd, number, 0);
            if (listener < 0) die("take seccomp listener");
            close(pidfd);
            if (write(notify_socket[0], "", 1) != 1) die("write to command");
        }
        close(notify_socket[0]);
    }
    if (listener >= 0) return exit_status(supervise(listener, child));

    for (;;) {
        pid = wait(&status);
        if (pid < 0) {
            if (errno == EINTR) continue;
            die("wait");
        }
        if (pid == child) break;
    }
    return exit_status(status);
}

static void enter_namespaces(void)
{
    uid_t uid = getuid();
    gid_t gid = getgid();
    char map[64];
    int flags = CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS;

    if (!host_network) flags |= CLONE_NEWNET;
    if (unshare(flags) < 0) die("unshare (are unprivileged user namespaces enabled?)");

    /* Map our own uid/gid to the same numbers inside. */
    write_file("/proc/self/setgroups", "deny");
    snprintf(map, sizeof(map), "%u %u 1", gid, gid);
    write_file("/proc/self/gid_map", map);
    snprintf(map, sizeof(map), "%u %u 1", uid, uid);
    write_file("/proc/self/uid_map", map);
}

/* A leading ~ stands for the home directory, for paths written in a
 * profile where no shell expands them. */
static char *expand_home(const char *arg)
{
    const char *home;
    char *path;

    if (arg[0] != '~' || (arg[1] != '\0' && arg[1] != '/')) {
        path = strdup(arg);
        if (path == NULL) die("strdup");
        return path;
    }
    home = getenv("HOME");
    if (home == NULL) {
        errno = 0;
        die("%s: HOME is not set", arg);
    }
    if (asprintf(&path, "%s%s", home, arg + 1) < 0) die("asprintf");
    return path;
}

static void add_path(const char *arg, int read_only, int hide)
{
    char *expanded;
    char *path;

    if (bind_count == MAX_BINDS) {
        errno = 0;
        die("too many paths (max %d)", MAX_BINDS);
    }
    expanded = expand_home(arg);
    path = realpath(expanded, NULL);
    free(expanded);
    /* A path that does not exist is already invisible. */
    if (path == NULL && hide && errno == ENOENT) return;
    if (path == NULL) die("%s", arg);
    if (strcmp(path, "/") == 0) {
        errno = 0;
        die("refusing to bind the root directory");
    }
    binds[bind_count].path = path;
    binds[bind_count].dst = path;
    binds[bind_count].read_only = read_only;
    binds[bind_count].hide = hide;
    bind_count++;
}

/* /etc/resolv.conf is often a symlink to a file outside the system dirs
 * (/run/systemd/resolve/stub-resolv.conf, /mnt/wsl/resolv.conf). Inside
 * the sandbox that link would dangle and name lookup would fail. Bind the
 * real file at the path the link points to, so the link resolves again. */
static void add_resolv_conf(void)
{
    char link[PATH_MAX];
    char *path;
    char *dst;
    ssize_t n;

    n = readlink("/etc/resolv.conf", link, sizeof(link) - 1);
    if (n < 0) return;
    link[n] = '\0';

    path = realpath("/etc/resolv.conf", NULL);
    if (path == NULL) return;

    if (link[0] == '/') {
        dst = strdup(link);
    } else {
        if (asprintf(&dst, "/etc/%s", link) < 0) die("asprintf");
    }
    if (dst == NULL) die("strdup");

    binds[bind_count].path = path;
    binds[bind_count].dst = dst;
    binds[bind_count].read_only = 1;
    bind_count++;
}

static int parse_number(const char *text, int lo, int hi, const char *what)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (*text == '\0' || *end != '\0' || value < lo || value > hi) {
        errno = 0;
        die("bad %s: %s", what, text);
    }
    return value;
}

static void parse_address(char *text, struct rule *r)
{
    char *slash;
    int max;

    if (strcmp(text, "all") == 0) return;
    slash = strchr(text, '/');
    if (slash != NULL) *slash = '\0';
    if (inet_pton(AF_INET, text, r->addr) == 1) {
        r->family = AF_INET;
        max = 32;
    } else if (inet_pton(AF_INET6, text, r->addr) == 1) {
        r->family = AF_INET6;
        max = 128;
    } else {
        errno = 0;
        die("bad address: %s", text);
    }
    r->prefix = max;
    if (slash != NULL) r->prefix = parse_number(slash + 1, 0, max, "prefix");
}

static void parse_ports(char *text, struct rule *r)
{
    char *dash = strchr(text, '-');

    if (dash == NULL) {
        r->port_lo = parse_number(text, 0, 65535, "port");
        r->port_hi = r->port_lo;
        return;
    }
    *dash = '\0';
    r->port_lo = parse_number(text, 0, 65535, "port");
    r->port_hi = parse_number(dash + 1, 0, 65535, "port");
    if (r->port_lo > r->port_hi) {
        errno = 0;
        die("bad port range: %s-%s", text, dash + 1);
    }
}

/* RULE = [src:][ADDR[/PREFIX]][:PORT[-PORT]] | all
 * An IPv6 address followed by a port needs brackets: [::1]:53.
 * A bare number or range is a port: 443, 8000-9000, src:5000-5100. */
static void add_rule(const char *arg, int allow)
{
    char text[256];
    char *p;
    char *addr = NULL;
    char *ports = NULL;
    char *end;
    struct rule *r;

    if (rule_count == MAX_RULES) {
        errno = 0;
        die("too many rules (max %d)", MAX_RULES);
    }
    r = &rules[rule_count];
    memset(r, 0, sizeof(*r));
    r->allow = allow;
    r->port_hi = 65535;

    snprintf(text, sizeof(text), "%s", arg);
    p = text;
    if (strncmp(p, "src:", 4) == 0) {
        r->src = 1;
        p += 4;
    }

    if (*p == '[') {
        addr = p + 1;
        end = strchr(addr, ']');
        if (end == NULL) {
            errno = 0;
            die("bad rule: %s", arg);
        }
        *end = '\0';
        ports = end + 1;
        if (*ports == ':') ports++;
    } else if (strspn(p, "0123456789-") == strlen(p)) {
        ports = p;
    } else if (strchr(p, ':') != NULL && strchr(p, ':') != strrchr(p, ':')) {
        addr = p;
    } else {
        addr = p;
        end = strchr(p, ':');
        if (end != NULL) {
            *end = '\0';
            ports = end + 1;
        }
    }

    if (addr != NULL && *addr != '\0') parse_address(addr, r);
    if (ports != NULL && *ports != '\0') parse_ports(ports, r);
    if ((addr == NULL || *addr == '\0') && (ports == NULL || *ports == '\0')) {
        errno = 0;
        die("bad rule: %s", arg);
    }
    rule_count++;
}

/* --env NAME passes the host's value through, --env NAME=VALUE sets one. */
static void add_env(const char *arg)
{
    const char *value;
    char *entry;

    if (extra_env_count == MAX_ENV) {
        errno = 0;
        die("too many --env entries (max %d)", MAX_ENV);
    }
    if (strchr(arg, '=') != NULL) {
        entry = strdup(arg);
        if (entry == NULL) die("strdup");
    } else {
        value = getenv(arg);
        if (value == NULL) return;
        if (asprintf(&entry, "%s=%s", arg, value) < 0) die("asprintf");
    }
    extra_env[extra_env_count] = entry;
    extra_env_count++;
}

/* --net none after --allow/--block rules drops them: a later flag wins,
 * and without the host's network there is nothing left to filter. */
static void set_net(const char *arg)
{
    if (strcmp(arg, "none") == 0) {
        host_network = 0;
        rule_count = 0;
        return;
    }
    if (strcmp(arg, "host") == 0) {
        host_network = 1;
        return;
    }
    errno = 0;
    die("--net: expected none or host, got %s", arg);
}

/* Profiles are files of flags under $XDG_CONFIG_HOME/sand, by default
 * ~/.config/sand. One flag per line, the same words as on the command
 * line with or without the dashes; `profile NAME` includes another
 * file. The profile named `default` is loaded first when it exists. */
#define MAX_PROFILE_DEPTH 8

static int profile_depth = 0;

static int apply_flag(const char *name, const char *value);

static char *profile_path(const char *name)
{
    const char *base;
    const char *home;
    char *path;

    if (name[0] == '\0' || strchr(name, '/') != NULL) {
        errno = 0;
        die("profile: expected a plain name, got %s", name);
    }
    base = getenv("XDG_CONFIG_HOME");
    if (base != NULL && base[0] != '\0') {
        if (asprintf(&path, "%s/sand/%s", base, name) < 0) die("asprintf");
        return path;
    }
    home = getenv("HOME");
    if (home == NULL) {
        errno = 0;
        die("profile %s: HOME is not set", name);
    }
    if (asprintf(&path, "%s/.config/sand/%s", home, name) < 0) die("asprintf");
    return path;
}

static void load_profile(const char *name, int required)
{
    char *path;
    FILE *file;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int lineno = 0;
    char *flag;
    char *value;

    if (profile_depth == MAX_PROFILE_DEPTH) {
        errno = 0;
        die("profile %s: nested too deep (a loop?)", name);
    }
    path = profile_path(name);
    file = fopen(path, "r");
    if (file == NULL) {
        if (!required && errno == ENOENT) {
            free(path);
            return;
        }
        die("profile %s", path);
    }
    profile_depth++;
    while ((len = getline(&line, &cap, file)) >= 0) {
        lineno++;
        while (len > 0 && strchr(" \t\r\n", line[len - 1]) != NULL) {
            len--;
            line[len] = '\0';
        }
        flag = line + strspn(line, " \t");
        if (*flag == '\0' || *flag == '#') continue;
        if (strncmp(flag, "--", 2) == 0) flag += 2;
        value = flag + strcspn(flag, " \t");
        if (*value != '\0') {
            *value = '\0';
            value++;
            value += strspn(value, " \t");
        }
        if (*value == '\0') {
            errno = 0;
            die("%s:%d: %s needs a value", path, lineno, flag);
        }
        if (!apply_flag(flag, value)) {
            errno = 0;
            die("%s:%d: unknown flag %s", path, lineno, flag);
        }
    }
    if (ferror(file)) die("profile %s", path);
    profile_depth--;
    free(line);
    free(path);
    fclose(file);
}

/* One flag, named without the dashes, from the command line or from a
 * profile. Returns 0 for a flag it does not know. */
static int apply_flag(const char *name, const char *value)
{
    if (strcmp(name, "ro") == 0) {
        add_path(value, 1, 0);
        return 1;
    }
    if (strcmp(name, "rw") == 0) {
        add_path(value, 0, 0);
        return 1;
    }
    if (strcmp(name, "hide") == 0) {
        add_path(value, 1, 1);
        return 1;
    }
    if (strcmp(name, "env") == 0) {
        add_env(value);
        return 1;
    }
    if (strcmp(name, "net") == 0) {
        set_net(value);
        return 1;
    }
    if (strcmp(name, "allow") == 0) {
        add_rule(value, 1);
        return 1;
    }
    if (strcmp(name, "block") == 0) {
        add_rule(value, 0);
        return 1;
    }
    if (strcmp(name, "profile") == 0) {
        load_profile(value, 1);
        return 1;
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: sand [-p NAME | --ro PATH | --rw PATH | --hide PATH | --env NAME[=VALUE] | --net MODE]... [COMMAND [ARG]...]\n");
    fprintf(stderr, "  -p, --profile NAME load the flags in ~/.config/sand/NAME\n");
    fprintf(stderr, "  --ro PATH          also make PATH visible, read-only\n");
    fprintf(stderr, "  --rw PATH          also make PATH visible, writable\n");
    fprintf(stderr, "  --hide PATH        make a visible PATH appear empty\n");
    fprintf(stderr, "  --env NAME         pass the host's NAME through to the command\n");
    fprintf(stderr, "  --env NAME=VALUE   set NAME for the command\n");
    fprintf(stderr, "  --net none|host    no network (default), or the host's network\n");
    fprintf(stderr, "  --allow RULE       with --net host: allow matching traffic (default becomes deny)\n");
    fprintf(stderr, "  --block RULE       with --net host: block matching traffic\n");
    fprintf(stderr, "    RULE = [src:][ADDR[/PREFIX]][:PORT[-PORT]] | all\n");
    fprintf(stderr, "    e.g. 10.0.0.0/8:443  1.1.1.1  :53  8000-9000  [::1]:80  src:8080\n");
    fprintf(stderr, "    src: rules apply to bind (listening and explicit source ports)\n");
    fprintf(stderr, "  COMMAND [ARG]...   run COMMAND instead of $SHELL; -- before it if it starts with -\n");
    fprintf(stderr, "Flags may repeat; a later flag wins. Use --rw . for a writable cwd.\n");
    fprintf(stderr, "The profile ~/.config/sand/default, if present, is loaded first.\n");
    exit(2);
}

int main(int argc, char **argv)
{
    char cwd[PATH_MAX];
    pid_t init;
    pid_t pid;
    int status;
    int i;
    const char *name;

    if (getcwd(cwd, sizeof(cwd)) == NULL) die("getcwd");
    if (strcmp(cwd, "/") == 0) {
        errno = 0;
        die("refusing to sandbox the root directory");
    }
    add_path(cwd, 1, 0);
    add_resolv_conf();

    load_profile("default", 0);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            if (i + 1 == argc) usage();
            command = argv + i + 1;
            break;
        }
        if (argv[i][0] != '-') {
            command = argv + i;
            break;
        }
        if (strcmp(argv[i], "-p") == 0) {
            name = "profile";
        } else if (strncmp(argv[i], "--", 2) == 0) {
            name = argv[i] + 2;
        } else {
            usage();
        }
        if (i + 1 == argc) usage();
        if (!apply_flag(name, argv[i + 1])) usage();
        i++;
    }

    if (rule_count > 0 && !host_network) {
        errno = 0;
        die("--allow and --block need --net host");
    }

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) die("no_new_privs");
    enter_namespaces();

    /* unshare(CLONE_NEWPID) only applies to children: fork to get pid 1. */
    init = fork();
    if (init < 0) die("fork");
    if (init == 0) exit(run_init(cwd));

    forward_to = init;
    install_signal_handlers();

    for (;;) {
        pid = waitpid(init, &status, 0);
        if (pid < 0) {
            if (errno == EINTR) continue;
            die("waitpid");
        }
        break;
    }
    return exit_status(status);
}

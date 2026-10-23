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
 * loopback. --net host keeps the host's network instead, and --net NAME
 * joins a network that sand-net built: its network namespace sits in a
 * user namespace owned by the user, and the owner of a user namespace
 * has every capability in it, so both can be entered without privilege.
 *
 * Process layout:
 *   sand (host)  ->  init (pid 1 in the sandbox)  ->  command
 * init reaps orphans and forwards SIGTERM/SIGHUP to the command, and
 * SIGINT/SIGQUIT when there is no terminal to deliver them. Init dies
 * with sand (PR_SET_PDEATHSIG), which ends the whole pid namespace.
 *
 * Terminal: the foreground process group the shell gave sand has no
 * visible id inside the pid namespace, so job control in the command
 * would see 0. Init therefore forms its own group and takes the
 * terminal; the command inherits that group. Ctrl-C and Ctrl-Z then
 * reach only init and the command. Init itself cannot be stopped (the
 * kernel drops default-action signals aimed at a namespace's pid 1),
 * so when the command stops init reports it over a pipe and sand stops
 * itself: the host shell sees its job stop. On fg sand hands the
 * terminal back to the command's group and continues it, and on bg
 * continues it without the terminal. The same model as sudo.
 *
 * Started in the background (sand cmd &), init still forms its group
 * but leaves the terminal alone; a command that then reads from it
 * stops with SIGTTIN like any background job, and fg works as above.
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
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <sys/wait.h>

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

/* --net NAME: the directory holding the user and net namespace files of
 * a network built by sand-net, plus its resolv.conf. */
#define NET_STATE "/var/lib/sand/net"

static char *net_dir = NULL;

/* Whether the sandbox gets a fresh, empty network namespace. */
static int own_network(void)
{
    return !host_network && net_dir == NULL;
}

/* Extra environment entries from --env, as "NAME=VALUE" strings. */
#define MAX_ENV 64

static char *extra_env[MAX_ENV];
static int extra_env_count = 0;

/* The process that signals get forwarded to. */
static pid_t forward_to = 0;

/* sand's own pid, for init to check its parent is still that. */
static pid_t sand_pid = 0;

/* The controlling terminal, or -1, and the group that held it when
 * sand started, to be restored on exit. */
static int tty_fd = -1;
static pid_t tty_pgrp = 0;

/* Whether the terminal is on our side: held by sand's group at start,
 * or handed to the command's group after fg. Only then is it given
 * back on exit; after bg someone else may be using it. */
static int foreground = 0;

/* init tells sand that the command stopped: one byte, the signal. */
static int stop_pipe[2] = {-1, -1};

static void report(const char *fmt, va_list ap, int saved_errno)
{
    fprintf(stderr, "sand: ");
    vfprintf(stderr, fmt, ap);
    if (saved_errno != 0) fprintf(stderr, ": %s", strerror(saved_errno));
    fprintf(stderr, "\n");
}

/* Exit with a message followed by the errno text. */
static void die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    report(fmt, ap, errno);
    va_end(ap);
    exit(1);
}

/* Exit with a message alone, for errors that are not a failed call. */
static void fail(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    report(fmt, ap, 0);
    va_end(ap);
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

/* Syscall numbers that headers older than the kernels they arrived in
 * lack; the same on every architecture since Linux 5.1. */
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442
#endif

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

    /* Opened here, inside the new mount namespace: the kernel refuses to
     * bind from an fd that belongs to another namespace. */
    for (i = 0; i < bind_count; i++) {
        if (binds[i].hide) continue;
        binds[i].fd = open(binds[i].path, O_PATH);
        if (binds[i].fd < 0) die("open %s", binds[i].path);
    }

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
        if (!binds[i].hide) close(binds[i].fd);
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

    /* With a terminal these reach the command's group directly, and
     * forwarding would deliver them twice. Without one they only
     * arrive by kill, from a script or a supervisor: pass them on. */
    if (tty_fd >= 0) {
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        return;
    }
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
}

static void find_terminal(void)
{
    int fd;

    for (fd = 0; fd <= 2; fd++) {
        tty_pgrp = tcgetpgrp(fd);
        if (tty_pgrp < 0) continue;
        tty_fd = fd;
        foreground = tty_pgrp == getpgrp();
        return;
    }
}

/* Hands the terminal to a process group. A caller outside the
 * foreground group would be stopped by SIGTTOU, so that is ignored for
 * the call. The group id is read in the caller's pid namespace. */
static int give_terminal(pid_t pgrp)
{
    struct sigaction ignore;
    struct sigaction saved;
    int result;

    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGTTOU, &ignore, &saved);
    result = tcsetpgrp(tty_fd, pgrp);
    sigaction(SIGTTOU, &saved, NULL);
    return result;
}

/* In init: the command stopped, tell sand. */
static void report_stop(int status)
{
    char sig = WSTOPSIG(status);

    if (stop_pipe[1] < 0) return;
    if (write(stop_pipe[1], &sig, 1) < 0) die("write to sand");
}

/* In sand: the command stopped. Stop the same way so the host shell
 * reports its job stopped; this returns when the shell continues us.
 * With fg the shell has given us the terminal, pass it on to the
 * command's group and continue it; with bg continue it without the
 * terminal, so it stops again if it touches the terminal. The group
 * is read from the terminal here on the host, where it has a visible
 * id, when our side held it; in the background it is init's. */
static void relay_stop(int sig, pid_t init)
{
    pid_t group = 0;

    if (foreground) group = tcgetpgrp(tty_fd);
    if (group <= 0) group = init;
    kill(0, sig);
    foreground = tcgetpgrp(tty_fd) == getpgrp();
    if (foreground) give_terminal(group);
    killpg(group, SIGCONT);
}

/* In sand: wait for init, relaying stops of the command meanwhile when
 * there is a terminal (the stop pipe is -1 otherwise). */
static int wait_for_init(pid_t init)
{
    struct pollfd fds[2];
    int status;
    pid_t pid;
    char sig;

    fds[0].fd = stop_pipe[0];
    fds[0].events = POLLIN;
    fds[1].fd = syscall(SYS_pidfd_open, init, 0);
    fds[1].events = POLLIN;
    if (fds[1].fd < 0) die("pidfd_open");
    for (;;) {
        fds[0].revents = 0;
        fds[1].revents = 0;
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            die("poll");
        }
        if (fds[0].revents & POLLIN) {
            if (read(stop_pipe[0], &sig, 1) == 1) relay_stop(sig, init);
            else fds[0].fd = -1;
        }
        if (fds[1].revents & POLLIN) break;
    }
    for (;;) {
        pid = waitpid(init, &status, 0);
        if (pid < 0) {
            if (errno == EINTR) continue;
            die("waitpid");
        }
        return status;
    }
}

static int exit_status(int status)
{
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

/* Reap until the command exits; returns its wait status. */
static int supervise(pid_t child)
{
    int status;
    pid_t pid;

    for (;;) {
        pid = waitpid(-1, &status, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR) continue;
            die("wait");
        }
        if (pid != child) continue;
        if (WIFSTOPPED(status)) report_stop(status);
        else return status;
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

/* Add a NAME=VALUE entry to env, replacing an entry of the same name:
 * with duplicates a shell would take the last and libc the first. */
static void set_env(char **env, int *count, char *entry)
{
    size_t name_len = strcspn(entry, "=");
    int i;

    for (i = 0; i < *count; i++) {
        if (strncmp(env[i], entry, name_len) != 0) continue;
        if (env[i][name_len] != '=') continue;
        env[i] = entry;
        return;
    }
    env[*count] = entry;
    *count = *count + 1;
}

static void run_command(void)
{
    const char *shell = getenv("SHELL");
    const char **name;
    const char *value;
    char *env[16 + MAX_ENV];
    char *entry;
    int n = 0;
    int i;

    for (name = kept_env; *name != NULL; name++) {
        value = getenv(*name);
        if (value == NULL) continue;
        if (asprintf(&entry, "%s=%s", *name, value) < 0) die("asprintf");
        set_env(env, &n, entry);
    }
    for (i = 0; i < extra_env_count; i++) {
        set_env(env, &n, extra_env[i]);
    }
    set_env(env, &n, "SAND=1");
    env[n] = NULL;

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

    /* If sand goes away, even by SIGKILL, so does the sandbox: killing
     * pid 1 of the namespace kills everything in it. The check covers
     * sand dying before the prctl took effect. */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0) die("pdeathsig");
    if (getppid() != sand_pid) raise(SIGKILL);

    /* Own process group, holding the terminal if sand did: see the
     * terminal note at the top. Done before the command exists so it
     * inherits the group. */
    if (tty_fd >= 0) {
        if (setpgid(0, 0) < 0) die("setpgid");
        if (foreground && give_terminal(getpid()) < 0) die("tcsetpgrp");
        close(stop_pipe[0]);
    }

    if (own_network()) loopback_up();
    build_root(cwd);
    if (sethostname("sand", 4) < 0) die("sethostname");

    child = fork();
    if (child < 0) die("fork");
    if (child == 0) run_command();

    forward_to = child;
    install_signal_handlers();
    return exit_status(supervise(child));
}

/* Open one namespace file of the network, checking it still is one:
 * after a reboot the mounts are gone and plain empty files remain. */
#define NSFS_MAGIC 0x6e736673

static int open_namespace(const char *kind)
{
    char path[PATH_MAX];
    struct statfs st;
    int fd;

    snprintf(path, sizeof(path), "%s/%s", net_dir, kind);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) die("open %s", path);
    if (fstatfs(fd, &st) < 0) die("statfs %s", path);
    if (st.f_type != NSFS_MAGIC) fail("%s: not a namespace; the network is down, run: sudo sand-net up", path);
    return fd;
}

/* Enter the network's user namespace, which we own, and then its
 * network namespace, which that ownership lets us enter. */
static void join_network(void)
{
    int user_fd = open_namespace("user");
    int net_fd = open_namespace("net");

    if (setns(user_fd, CLONE_NEWUSER) < 0) die("join the user namespace of %s", net_dir);
    if (setns(net_fd, CLONE_NEWNET) < 0) die("join the network namespace of %s", net_dir);
    close(user_fd);
    close(net_fd);
}

static void enter_namespaces(void)
{
    uid_t uid = getuid();
    gid_t gid = getgid();
    char map[64];
    int flags = CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS;

    if (net_dir != NULL) join_network();
    if (own_network()) flags |= CLONE_NEWNET;
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
    if (home == NULL) fail("%s: HOME is not set", arg);
    if (asprintf(&path, "%s%s", home, arg + 1) < 0) die("asprintf");
    return path;
}

static void add_path(const char *arg, int read_only, int hide)
{
    char *expanded;
    char *path;

    if (bind_count == MAX_BINDS) fail("too many paths (max %d)", MAX_BINDS);
    expanded = expand_home(arg);
    path = realpath(expanded, NULL);
    free(expanded);
    /* A path that does not exist is already invisible. */
    if (path == NULL && hide && errno == ENOENT) return;
    if (path == NULL) die("%s", arg);
    if (strcmp(path, "/") == 0) fail("refusing to bind the root directory");
    binds[bind_count].path = path;
    binds[bind_count].dst = path;
    binds[bind_count].read_only = read_only;
    binds[bind_count].hide = hide;
    bind_count++;
}

/* Make file serve as /etc/resolv.conf inside. That is often a symlink
 * to a file outside the system dirs (/run/systemd/resolve/stub-resolv.conf,
 * /mnt/wsl/resolv.conf), which would dangle inside; so the file is bound
 * at the path the link points to, and over /etc/resolv.conf itself when
 * there is no link. The host's own file is the default, a network's
 * replaces it. */
static void add_resolv_conf(const char *file)
{
    char link[PATH_MAX];
    char *path;
    char *dst;
    ssize_t n;

    path = realpath(file, NULL);
    if (path == NULL) return;

    n = readlink("/etc/resolv.conf", link, sizeof(link) - 1);
    if (n < 0) {
        /* The host's file, not a link: it is already in place. */
        if (strcmp(path, "/etc/resolv.conf") == 0) return;
        dst = strdup("/etc/resolv.conf");
    } else {
        link[n] = '\0';
        if (link[0] == '/') dst = strdup(link);
        else if (asprintf(&dst, "/etc/%s", link) < 0) die("asprintf");
    }
    if (dst == NULL) die("strdup");
    if (bind_count == MAX_BINDS) fail("too many paths (max %d)", MAX_BINDS);

    binds[bind_count].path = path;
    binds[bind_count].dst = dst;
    binds[bind_count].read_only = 1;
    bind_count++;
}

/* --env NAME passes the host's value through, --env NAME=VALUE sets one. */
static void add_env(const char *arg)
{
    const char *value;
    char *entry;

    if (extra_env_count == MAX_ENV) fail("too many --env entries (max %d)", MAX_ENV);
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

/* --net none: an empty network. host: the host's. Anything else names
 * a network built by sand-net, under /var/lib/sand/net, or with a slash
 * a directory holding such a network's files. */
static void set_net(const char *arg)
{
    host_network = 0;
    net_dir = NULL;
    if (strcmp(arg, "none") == 0) return;
    if (strcmp(arg, "host") == 0) {
        host_network = 1;
        return;
    }
    if (strchr(arg, '/') != NULL) {
        net_dir = expand_home(arg);
        return;
    }
    if (asprintf(&net_dir, NET_STATE "/%s", arg) < 0) die("asprintf");
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

    if (name[0] == '\0' || strchr(name, '/') != NULL) fail("profile: expected a plain name, got %s", name);
    base = getenv("XDG_CONFIG_HOME");
    if (base != NULL && base[0] != '\0') {
        if (asprintf(&path, "%s/sand/%s", base, name) < 0) die("asprintf");
        return path;
    }
    home = getenv("HOME");
    if (home == NULL) fail("profile %s: HOME is not set", name);
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

    if (profile_depth == MAX_PROFILE_DEPTH) fail("profile %s: nested too deep (a loop?)", name);
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
        if (*value == '\0') fail("%s:%d: %s needs a value", path, lineno, flag);
        if (!apply_flag(flag, value)) fail("%s:%d: unknown flag %s", path, lineno, flag);
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
    if (strcmp(name, "profile") == 0) {
        load_profile(value, 1);
        return 1;
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: sand [-p NAME | --ro PATH | --rw PATH | --hide PATH | --env NAME[=VALUE] | --net none|host|NAME]... [COMMAND [ARG]...]\n");
    fprintf(stderr, "  -p, --profile NAME load the flags in ~/.config/sand/NAME\n");
    fprintf(stderr, "  --ro PATH          also make PATH visible, read-only\n");
    fprintf(stderr, "  --rw PATH          also make PATH visible, writable\n");
    fprintf(stderr, "  --hide PATH        make a visible PATH appear empty\n");
    fprintf(stderr, "  --env NAME         pass the host's NAME through to the command\n");
    fprintf(stderr, "  --env NAME=VALUE   set NAME for the command\n");
    fprintf(stderr, "  --net none|host|NAME  no network (default), the host's, or one built by sand-net\n");
    fprintf(stderr, "  COMMAND [ARG]...   run COMMAND instead of $SHELL; -- before it if it starts with -\n");
    fprintf(stderr, "Flags may repeat; a later flag wins. Use --rw . for a writable cwd.\n");
    fprintf(stderr, "The profile ~/.config/sand/default, if present, is loaded first.\n");
    exit(2);
}

int main(int argc, char **argv)
{
    char cwd[PATH_MAX];
    pid_t init;
    int status;
    int i;
    const char *name;
    char *file;

    if (getcwd(cwd, sizeof(cwd)) == NULL) die("getcwd");
    if (strcmp(cwd, "/") == 0) fail("refusing to sandbox the root directory");
    add_path(cwd, 1, 0);
    add_resolv_conf("/etc/resolv.conf");

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

    if (net_dir != NULL) {
        if (asprintf(&file, "%s/resolv.conf", net_dir) < 0) die("asprintf");
        add_resolv_conf(file);
    }

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) die("no_new_privs");
    find_terminal();
    if (tty_fd >= 0) {
        if (pipe2(stop_pipe, O_CLOEXEC) < 0) die("pipe");
    }
    enter_namespaces();

    /* unshare(CLONE_NEWPID) only applies to children: fork to get pid 1. */
    sand_pid = getpid();
    init = fork();
    if (init < 0) die("fork");
    if (init == 0) exit(run_init(cwd));

    forward_to = init;
    install_signal_handlers();

    if (tty_fd >= 0) close(stop_pipe[1]);
    status = wait_for_init(init);
    if (foreground) give_terminal(tty_pgrp);
    return exit_status(status);
}

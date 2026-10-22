/*
 * sandbox - open a shell in the current directory with restrictions.
 *
 * The shell runs in its own user, mount, pid, net, ipc and uts namespaces.
 * It sees an empty root containing only:
 *   - the host's system directories (/usr /bin /sbin /lib /lib64 /etc), read-only
 *   - the current working directory, read-only, at its own path
 *   - any extra paths given with --ro or --rw, at their own paths
 *   - a fresh /tmp, a fresh /proc, and a minimal /dev
 * It has no network: the new net namespace holds nothing but a down loopback.
 *
 * Process layout:
 *   sandbox (host)  ->  init (pid 1 in the sandbox)  ->  shell
 * init reaps orphans and forwards SIGTERM/SIGHUP to the shell.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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
    char *path;
    int read_only;
    int fd;
};

#define MAX_BINDS 64

static struct bind binds[MAX_BINDS];
static int bind_count = 0;

/* The process that signals get forwarded to. */
static pid_t forward_to = 0;

static void die(const char *fmt, ...)
{
    va_list ap;
    int saved_errno = errno;

    fprintf(stderr, "sandbox: ");
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

static void add_bind(const struct bind *b)
{
    char src[PATH_MAX];
    char dst[STAGED_MAX];
    char *slash;
    struct stat st;
    int fd;

    snprintf(src, sizeof(src), "/proc/self/fd/%d", b->fd);
    snprintf(dst, sizeof(dst), STAGE "%s", b->path);
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
}

/* Runs as pid 1 inside the new namespaces, before the shell starts. */
static void build_root(const char *cwd)
{
    const char **dir;
    int i;

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
    for (i = 0; i < bind_count; i++) {
        add_bind(&binds[i]);
    }

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

/* Environment variables the shell is allowed to inherit from the host.
 * Everything else is dropped: the host environment may hold secrets, and
 * prompt hooks pointing at files that do not exist inside break the shell. */
static const char *kept_env[] = {
    "PATH",
    "TERM",
    "HOME",
    "USER",
    "LANG",
    "SHELL",
    NULL,
};

static void run_shell(void)
{
    const char *shell = getenv("SHELL");
    const char **name;
    const char *value;
    char *env[16];
    int n = 0;

    for (name = kept_env; *name != NULL; name++) {
        value = getenv(*name);
        if (value == NULL) continue;
        if (asprintf(&env[n], "%s=%s", *name, value) < 0) die("asprintf");
        n++;
    }
    env[n] = "SANDBOX=1";
    n++;
    env[n] = NULL;

    if (shell != NULL) execle(shell, shell, (char *)NULL, env);
    execle("/bin/sh", "/bin/sh", (char *)NULL, env);
    die("exec shell");
}

/* pid 1 inside the sandbox: start the shell, reap, exit when it exits. */
static int run_init(const char *cwd)
{
    pid_t shell;
    pid_t pid;
    int status;
    int i;

    /* Opened here, inside the new mount namespace: the kernel refuses to
     * bind from an fd that belongs to another namespace. */
    for (i = 0; i < bind_count; i++) {
        binds[i].fd = open(binds[i].path, O_PATH);
        if (binds[i].fd < 0) die("open %s", binds[i].path);
    }
    build_root(cwd);
    for (i = 0; i < bind_count; i++) {
        close(binds[i].fd);
    }
    if (sethostname("sandbox", 7) < 0) die("sethostname");

    shell = fork();
    if (shell < 0) die("fork");
    if (shell == 0) run_shell();

    forward_to = shell;
    install_signal_handlers();

    for (;;) {
        pid = wait(&status);
        if (pid < 0) {
            if (errno == EINTR) continue;
            die("wait");
        }
        if (pid == shell) break;
    }
    return exit_status(status);
}

static void enter_namespaces(void)
{
    uid_t uid = getuid();
    gid_t gid = getgid();
    char map[64];
    int flags = CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWIPC | CLONE_NEWUTS;

    if (unshare(flags) < 0) die("unshare (are unprivileged user namespaces enabled?)");

    /* Map our own uid/gid to the same numbers inside. */
    write_file("/proc/self/setgroups", "deny");
    snprintf(map, sizeof(map), "%u %u 1", gid, gid);
    write_file("/proc/self/gid_map", map);
    snprintf(map, sizeof(map), "%u %u 1", uid, uid);
    write_file("/proc/self/uid_map", map);
}

static void add_path(const char *arg, int read_only)
{
    char *path;

    if (bind_count == MAX_BINDS) {
        errno = 0;
        die("too many paths (max %d)", MAX_BINDS);
    }
    path = realpath(arg, NULL);
    if (path == NULL) die("%s", arg);
    if (strcmp(path, "/") == 0) {
        errno = 0;
        die("refusing to bind the root directory");
    }
    binds[bind_count].path = path;
    binds[bind_count].read_only = read_only;
    bind_count++;
}

static void usage(void)
{
    fprintf(stderr, "usage: sandbox [--ro PATH | --rw PATH]...\n");
    fprintf(stderr, "  --ro PATH   also make PATH visible, read-only\n");
    fprintf(stderr, "  --rw PATH   also make PATH visible, writable\n");
    fprintf(stderr, "Flags may repeat; a later flag wins. Use --rw . for a writable cwd.\n");
    exit(2);
}

int main(int argc, char **argv)
{
    char cwd[PATH_MAX];
    pid_t init;
    pid_t pid;
    int status;
    int i;

    if (getcwd(cwd, sizeof(cwd)) == NULL) die("getcwd");
    if (strcmp(cwd, "/") == 0) {
        errno = 0;
        die("refusing to sandbox the root directory");
    }
    add_path(cwd, 1);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ro") == 0 && i + 1 < argc) {
            i++;
            add_path(argv[i], 1);
            continue;
        }
        if (strcmp(argv[i], "--rw") == 0 && i + 1 < argc) {
            i++;
            add_path(argv[i], 0);
            continue;
        }
        usage();
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

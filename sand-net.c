/*
 * sand-net - named networks for sand: a network namespace with a way
 * out through the host, built once with root, joined by sand without.
 *
 *   sudo sand-net create NAME [--dns ADDR]... [--owner USER]
 *                         [--allow RULE]... [--block RULE]...
 *   sudo sand-net up [NAME]        rebuild whatever is missing, e.g. after a reboot
 *   sudo sand-net delete NAME
 *        sand-net list
 *        sand-net nft              print the firewall table that up would load
 *
 * State lives in /var/lib/sand/net/NAME:
 *   config       owner, subnet number, dns servers, rules
 *   user, net    the namespaces, bind-mounted so they outlive their maker
 *   resolv.conf  what sand binds over /etc/resolv.conf inside
 *
 * The network namespace sits inside a user namespace owned by the user.
 * The owner of a user namespace has every capability in it, which is
 * what setns needs, so sand enters both without privileges; the command
 * then runs one namespace deeper, with no capability over the network.
 * Root is for the host side only: a veth pair with one end inside
 * (eth0, 10.44.N.2/30) and one on the host (sand-NAME, 10.44.N.1/30),
 * forwarding, and one nft table `sand` with a masquerade and a chain
 * of rules per network, rebuilt from every config on each up. The
 * rules sit on the host end of the veth, out of the sandbox's reach:
 * nothing inside can loosen them. With Docker on the host, an
 * accept for the veth in its DOCKER-USER chain, since Docker drops
 * forwarded traffic by default.
 *
 * Runs ip(8) and nft(8), and iptables(8) when it is there.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/wait.h>

#define STATE "/var/lib/sand/net"
#define NSFS_MAGIC 0x6e736673

/* The host interface is sand-NAME and must fit IFNAMSIZ. */
#define MAX_NAME 10
#define MAX_DNS 8
#define MAX_NETS 254
#define MAX_RULES 64

/* Where the configs live; the tests point SAND_NET_STATE elsewhere. */
static const char *state = STATE;

/* RULE = [ADDR[/PREFIX]][:PORT[-PORT]] | all; kept as written. */
struct rule {
    int allow;
    char text[64];
    int family;             /* AF_INET, AF_INET6, or 0 for any address */
    unsigned char addr[16];
    int prefix;
    int port_lo;            /* 0-65535 when ports are given, else -1 */
    int port_hi;
};

struct net {
    char name[MAX_NAME + 1];
    uid_t owner;
    int index;              /* N in 10.44.N.0/30 */
    char dns[MAX_DNS][64];
    int dns_count;
    struct rule rules[MAX_RULES];
    int rule_count;
};

static void report(const char *fmt, va_list ap, int saved_errno)
{
    fprintf(stderr, "sand-net: ");
    vfprintf(stderr, fmt, ap);
    if (saved_errno != 0) fprintf(stderr, ": %s", strerror(saved_errno));
    fprintf(stderr, "\n");
}

static void die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    report(fmt, ap, errno);
    va_end(ap);
    exit(1);
}

static void fail(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    report(fmt, ap, 0);
    va_end(ap);
    exit(1);
}

/* For the kernel's own files under /proc. */
static void write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) die("open %s", path);
    if (write(fd, text, strlen(text)) < 0) die("write %s", path);
    close(fd);
}

static void state_path(char *buf, size_t size, const char *name, const char *file)
{
    snprintf(buf, size, "%s/%s/%s", state, name, file);
}

static void host_interface(char *buf, size_t size, const struct net *n)
{
    snprintf(buf, size, "sand-%.*s", MAX_NAME, n->name);
}

static void check_name(const char *name)
{
    if (name[0] == '\0' || strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(name)) fail("bad name: %s (letters, digits, _ and -)", name);
    if (strlen(name) > MAX_NAME) fail("%s: a name has at most %d characters (it becomes interface sand-NAME)", name, MAX_NAME);
}

/* ---- rules ---- */

static int parse_number(const char *text, int lo, int hi, const char *what)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (*text == '\0' || *end != '\0' || value < lo || value > hi) fail("bad %s: %s", what, text);
    return value;
}

static void parse_address(char *text, struct rule *r)
{
    char *slash;
    int max;

    slash = strchr(text, '/');
    if (slash != NULL) *slash = '\0';
    if (inet_pton(AF_INET, text, r->addr) == 1) {
        r->family = AF_INET;
        max = 32;
    } else if (inet_pton(AF_INET6, text, r->addr) == 1) {
        r->family = AF_INET6;
        max = 128;
    } else {
        fail("bad address: %s", text);
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
    if (r->port_lo > r->port_hi) fail("bad port range: %s-%s", text, dash + 1);
}

/* An IPv6 address followed by a port needs brackets: [2001:db8::1]:53.
 * A bare number or range is a port: 443, 8000-9000. */
static void add_rule(struct net *n, const char *arg, int allow)
{
    char text[sizeof(n->rules[0].text)];
    char *p;
    char *addr = NULL;
    char *ports = NULL;
    char *end;
    struct rule *r;

    if (n->rule_count == MAX_RULES) fail("%s: too many rules (max %d)", n->name, MAX_RULES);
    if (strlen(arg) >= sizeof(text)) fail("bad rule: %s", arg);
    r = &n->rules[n->rule_count];
    memset(r, 0, sizeof(*r));
    r->allow = allow;
    r->port_lo = -1;
    r->port_hi = -1;
    snprintf(r->text, sizeof(r->text), "%s", arg);
    if (strcmp(arg, "all") == 0) {
        n->rule_count++;
        return;
    }

    snprintf(text, sizeof(text), "%s", arg);
    p = text;
    if (*p == '[') {
        addr = p + 1;
        end = strchr(addr, ']');
        if (end == NULL) fail("bad rule: %s", arg);
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
    if (r->family == 0 && r->port_lo < 0) fail("bad rule: %s", arg);
    n->rule_count++;
}

/* ---- running programs ---- */

#define MAX_ARGS 32

/* Run argv to completion, inside the network namespace netfd when it is
 * not -1, feeding input to its stdin when given, with its output dropped
 * when quiet. Returns the exit status, 127 when it could not start. */
static int run_status(int netfd, char *const argv[], const char *input, int quiet)
{
    int in[2] = {-1, -1};
    pid_t pid;
    int status;
    int null;

    if (input != NULL && pipe(in) < 0) die("pipe");
    pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        if (netfd >= 0 && setns(netfd, CLONE_NEWNET) < 0) {
            perror("sand-net: setns");
            _exit(127);
        }
        if (input != NULL) {
            dup2(in[0], 0);
            close(in[0]);
            close(in[1]);
        }
        if (quiet) {
            null = open("/dev/null", O_WRONLY);
            dup2(null, 1);
            dup2(null, 2);
        }
        execvp(argv[0], argv);
        if (!quiet) perror(argv[0]);
        _exit(127);
    }
    if (input != NULL) {
        close(in[0]);
        if (write(in[1], input, strlen(input)) < 0) die("write to %s", argv[0]);
        close(in[1]);
    }
    if (waitpid(pid, &status, 0) < 0) die("wait for %s", argv[0]);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 127;
}

static int collect(char **argv, const char *first, va_list ap)
{
    int n = 0;

    argv[n] = (char *)first;
    n++;
    for (;;) {
        argv[n] = va_arg(ap, char *);
        if (argv[n] == NULL) break;
        n++;
        if (n == MAX_ARGS - 1) fail("too many arguments");
    }
    return n;
}

/* Run a program, exiting when it fails. */
static void run(int netfd, const char *first, ...)
{
    char *argv[MAX_ARGS];
    va_list ap;
    int status;

    va_start(ap, first);
    collect(argv, first, ap);
    va_end(ap);
    status = run_status(netfd, argv, NULL, 0);
    if (status == 127) fail("%s: not found; sand-net needs ip, nft and, with Docker, iptables", argv[0]);
    if (status != 0) fail("%s %s failed (exit %d)", argv[0], argv[1], status);
}

/* Run a program quietly for its exit status alone. */
static int probe(const char *first, ...)
{
    char *argv[MAX_ARGS];
    va_list ap;

    va_start(ap, first);
    collect(argv, first, ap);
    va_end(ap);
    return run_status(-1, argv, NULL, 1);
}

/* ---- the configs ---- */

static void read_config(struct net *n)
{
    char path[PATH_MAX];
    FILE *f;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    char *value;

    state_path(path, sizeof(path), n->name, "config");
    f = fopen(path, "r");
    if (f == NULL) die("open %s", path);
    n->owner = 0;
    n->index = 0;
    n->dns_count = 0;
    n->rule_count = 0;
    while ((len = getline(&line, &cap, f)) >= 0) {
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        value = strchr(line, ' ');
        if (value == NULL) continue;
        *value = '\0';
        value++;
        if (strcmp(line, "owner") == 0) n->owner = atoi(value);
        if (strcmp(line, "index") == 0) n->index = atoi(value);
        if (strcmp(line, "dns") == 0 && n->dns_count < MAX_DNS) {
            snprintf(n->dns[n->dns_count], sizeof(n->dns[0]), "%s", value);
            n->dns_count++;
        }
        if (strcmp(line, "allow") == 0) add_rule(n, value, 1);
        if (strcmp(line, "block") == 0) add_rule(n, value, 0);
    }
    free(line);
    fclose(f);
    if (n->index < 1 || n->index > MAX_NETS) fail("%s: bad subnet index %d", path, n->index);
}

static void make_dir(const char *path)
{
    if (mkdir(path, 0755) < 0 && errno != EEXIST) die("mkdir %s", path);
}

static void write_config(const struct net *n)
{
    char path[PATH_MAX];
    FILE *f;
    int i;

    state_path(path, sizeof(path), n->name, "config");
    f = fopen(path, "w");
    if (f == NULL) die("open %s", path);
    fprintf(f, "owner %u\n", n->owner);
    fprintf(f, "index %d\n", n->index);
    for (i = 0; i < n->dns_count; i++) {
        fprintf(f, "dns %s\n", n->dns[i]);
    }
    for (i = 0; i < n->rule_count; i++) {
        fprintf(f, "%s %s\n", n->rules[i].allow ? "allow" : "block", n->rules[i].text);
    }
    if (fclose(f) != 0) die("write %s", path);

    state_path(path, sizeof(path), n->name, "resolv.conf");
    f = fopen(path, "w");
    if (f == NULL) die("open %s", path);
    for (i = 0; i < n->dns_count; i++) {
        fprintf(f, "nameserver %s\n", n->dns[i]);
    }
    if (fclose(f) != 0) die("write %s", path);
}

/* Every network with a config, by name. */
static int load_all(struct net *nets)
{
    struct dirent **entries;
    char path[PATH_MAX];
    int count;
    int found = 0;
    int i;

    count = scandir(state, &entries, NULL, alphasort);
    if (count < 0 && errno == ENOENT) return 0;
    if (count < 0) die("read %s", state);
    for (i = 0; i < count; i++) {
        if (entries[i]->d_name[0] == '.') continue;
        if (strlen(entries[i]->d_name) > MAX_NAME) continue;
        snprintf(path, sizeof(path), "%s/%s/config", state, entries[i]->d_name);
        if (access(path, F_OK) != 0) continue;
        if (found == MAX_NETS) fail("too many networks");
        snprintf(nets[found].name, sizeof(nets[found].name), "%.*s", MAX_NAME, entries[i]->d_name);
        read_config(&nets[found]);
        found++;
    }
    free(entries);
    return found;
}

static struct net *find(struct net *nets, int count, const char *name)
{
    int i;

    for (i = 0; i < count; i++) {
        if (strcmp(nets[i].name, name) == 0) return &nets[i];
    }
    return NULL;
}

/* Nameservers from a resolv.conf that are not loopback, which only
 * exists on the host; returns how many were added. */
static int nameservers_from(const char *file, struct net *n)
{
    FILE *f;
    char line[256];
    char addr[64];
    int added = 0;

    f = fopen(file, "r");
    if (f == NULL) return 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "nameserver %63s", addr) != 1) continue;
        if (strncmp(addr, "127.", 4) == 0 || strcmp(addr, "::1") == 0) continue;
        if (n->dns_count == MAX_DNS) break;
        snprintf(n->dns[n->dns_count], sizeof(n->dns[0]), "%s", addr);
        n->dns_count++;
        added++;
    }
    fclose(f);
    return added;
}

/* The host's nameservers, or the ones behind its local stub resolver. */
static void default_dns(struct net *n)
{
    if (nameservers_from("/etc/resolv.conf", n) > 0) return;
    if (nameservers_from("/run/systemd/resolve/resolv.conf", n) > 0) return;
    fail("no nameserver on the host that a network can reach; give --dns ADDR");
}

/* ---- the namespaces ---- */

static int pinned(const struct net *n)
{
    char path[PATH_MAX];
    struct statfs st;

    state_path(path, sizeof(path), n->name, "net");
    if (statfs(path, &st) < 0) return 0;
    return st.f_type == NSFS_MAGIC;
}

static void bind_namespace(pid_t pid, const char *kind, const struct net *n)
{
    char src[64];
    char dst[PATH_MAX];
    int fd;

    snprintf(src, sizeof(src), "/proc/%d/ns/%s", pid, kind);
    state_path(dst, sizeof(dst), n->name, kind);
    fd = open(dst, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) die("create %s", dst);
    close(fd);
    if (mount(src, dst, NULL, MS_BIND, NULL) < 0) die("bind %s", dst);
}

/* A child becomes the owner, makes the user namespace with the owner's
 * uid mapped to itself and the network namespace inside it, and waits
 * while both are bind-mounted into place; the mounts keep them alive. */
static void make_namespaces(const struct net *n)
{
    struct passwd *pw;
    char map[64];
    int ready[2];
    int done[2];
    pid_t pid;
    char byte;

    pw = getpwuid(n->owner);
    if (pw == NULL) fail("%s: owner uid %u does not exist", n->name, n->owner);
    if (pipe(ready) < 0 || pipe(done) < 0) die("pipe");
    pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        if (setgroups(0, NULL) < 0) die("setgroups");
        if (setgid(pw->pw_gid) < 0) die("setgid");
        if (setuid(n->owner) < 0) die("setuid");
        if (prctl(PR_SET_DUMPABLE, 1) < 0) die("prctl");
        if (unshare(CLONE_NEWUSER) < 0) die("unshare user namespace");
        write_file("/proc/self/setgroups", "deny");
        snprintf(map, sizeof(map), "%u %u 1", pw->pw_gid, pw->pw_gid);
        write_file("/proc/self/gid_map", map);
        snprintf(map, sizeof(map), "%u %u 1", n->owner, n->owner);
        write_file("/proc/self/uid_map", map);
        if (unshare(CLONE_NEWNET) < 0) die("unshare network namespace");
        if (write(ready[1], "", 1) != 1) die("write to parent");
        if (read(done[0], &byte, 1) < 0) die("read from parent");
        _exit(0);
    }
    if (read(ready[0], &byte, 1) != 1) fail("%s: making the namespaces failed", n->name);
    bind_namespace(pid, "user", n);
    bind_namespace(pid, "net", n);
    if (write(done[1], "", 1) != 1) die("write to child");
    waitpid(pid, NULL, 0);
    close(ready[0]);
    close(ready[1]);
    close(done[0]);
    close(done[1]);
}

static int open_net(const struct net *n)
{
    char path[PATH_MAX];
    int fd;

    state_path(path, sizeof(path), n->name, "net");
    fd = open(path, O_RDONLY);
    if (fd < 0) die("open %s", path);
    return fd;
}

/* ---- the host side ---- */

/* Docker sets the forward policy to DROP and keeps DOCKER-USER for
 * rules of others; without an accept there our traffic goes nowhere. */
static void docker_accept(const char *iface)
{
    if (probe("iptables", "-n", "-L", "DOCKER-USER", NULL) != 0) return;
    if (probe("iptables", "-C", "DOCKER-USER", "-i", iface, "-j", "ACCEPT", NULL) != 0) {
        run(-1, "iptables", "-I", "DOCKER-USER", "-i", iface, "-j", "ACCEPT", NULL);
    }
    if (probe("iptables", "-C", "DOCKER-USER", "-o", iface, "-j", "ACCEPT", NULL) != 0) {
        run(-1, "iptables", "-I", "DOCKER-USER", "-o", iface, "-j", "ACCEPT", NULL);
    }
}

static void docker_remove(const char *iface)
{
    if (probe("iptables", "-n", "-L", "DOCKER-USER", NULL) != 0) return;
    while (probe("iptables", "-D", "DOCKER-USER", "-i", iface, "-j", "ACCEPT", NULL) == 0) continue;
    while (probe("iptables", "-D", "DOCKER-USER", "-o", iface, "-j", "ACCEPT", NULL) == 0) continue;
}

static int interface_exists(const char *iface)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "/sys/class/net/%s", iface);
    return access(path, F_OK) == 0;
}

/* The veth pair, addresses, routes and forwarding for one network;
 * every step is a no-op when already done. */
static void host_side(const struct net *n)
{
    char iface[32];
    char host_addr[32];
    char inner_addr[32];
    char gateway[32];
    int netfd;

    host_interface(iface, sizeof(iface), n);
    snprintf(host_addr, sizeof(host_addr), "10.44.%d.1/30", n->index);
    snprintf(inner_addr, sizeof(inner_addr), "10.44.%d.2/30", n->index);
    snprintf(gateway, sizeof(gateway), "10.44.%d.1", n->index);
    netfd = open_net(n);

    /* Made from inside, where "netns 1" is the host's pid 1 and so the
     * host's network: the inner end is eth0 whatever the name outside. */
    if (!interface_exists(iface)) {
        run(netfd, "ip", "link", "add", "eth0", "type", "veth", "peer", "name", iface, "netns", "1", NULL);
    }
    run(-1, "ip", "addr", "replace", host_addr, "dev", iface, NULL);
    run(-1, "ip", "link", "set", iface, "up", NULL);
    run(netfd, "ip", "addr", "replace", inner_addr, "dev", "eth0", NULL);
    run(netfd, "ip", "link", "set", "lo", "up", NULL);
    run(netfd, "ip", "link", "set", "eth0", "up", NULL);
    run(netfd, "ip", "route", "replace", "default", "via", gateway, "dev", "eth0", NULL);
    close(netfd);

    write_file("/proc/sys/net/ipv4/ip_forward", "1");
    docker_accept(iface);
}

/* One nft rule: the match for a rule's address and ports, then the
 * verdict. nft takes the first match, so the caller emits rules in
 * reverse, which keeps "a later rule wins". */
static void nft_rule(FILE *f, const struct rule *r)
{
    char addr[INET6_ADDRSTRLEN];

    fprintf(f, "        ");
    if (r->family != 0) {
        inet_ntop(r->family, r->addr, addr, sizeof(addr));
        fprintf(f, "%s daddr %s/%d ", r->family == AF_INET ? "ip" : "ip6", addr, r->prefix);
    }
    if (r->port_lo >= 0) {
        fprintf(f, "meta l4proto { tcp, udp } th dport %d", r->port_lo);
        if (r->port_hi != r->port_lo) fprintf(f, "-%d", r->port_hi);
        fprintf(f, " ");
    }
    fprintf(f, "%s\n", r->allow ? "accept" : "reject");
}

/* The chain for one network: its resolvers on port 53, the rules
 * last to first, and the default, closed as soon as anything is allowed. */
static void nft_chain(FILE *f, const struct net *n)
{
    unsigned char addr[16];
    int allowed = 0;
    int i;

    fprintf(f, "    chain net_%s {\n", n->name);
    for (i = 0; i < n->dns_count; i++) {
        if (inet_pton(AF_INET, n->dns[i], addr) == 1) {
            fprintf(f, "        ip daddr %s meta l4proto { tcp, udp } th dport 53 accept\n", n->dns[i]);
        } else {
            fprintf(f, "        ip6 daddr %s meta l4proto { tcp, udp } th dport 53 accept\n", n->dns[i]);
        }
    }
    for (i = n->rule_count - 1; i >= 0; i--) {
        nft_rule(f, &n->rules[i]);
        if (n->rules[i].allow) allowed = 1;
    }
    fprintf(f, "        %s\n", allowed ? "reject" : "accept");
    fprintf(f, "    }\n");
}

/* The nft table is ours alone and is rebuilt whole from the configs:
 * the masquerade that gives each network its way out and a chain of
 * rules per network, judged on the host, where forwarded traffic and
 * traffic to the host itself both arrive from the veth. Without the
 * table nothing gets out, so anything that flushes it fails closed. */
static char *nft_script(const struct net *nets, int count)
{
    char *script;
    size_t size;
    FILE *f;
    char iface[32];
    int i;

    f = open_memstream(&script, &size);
    if (f == NULL) die("open_memstream");
    fprintf(f, "add table inet sand\n");
    fprintf(f, "flush table inet sand\n");
    fprintf(f, "table inet sand {\n");
    fprintf(f, "    chain forward {\n");
    fprintf(f, "        type filter hook forward priority 0; policy accept;\n");
    fprintf(f, "        ct state established,related accept\n");
    fprintf(f, "        iifname \"sand-*\" oifname \"sand-*\" reject\n");
    for (i = 0; i < count; i++) {
        host_interface(iface, sizeof(iface), &nets[i]);
        fprintf(f, "        iifname \"%s\" jump net_%s\n", iface, nets[i].name);
    }
    fprintf(f, "    }\n");
    fprintf(f, "    chain input {\n");
    fprintf(f, "        type filter hook input priority 0; policy accept;\n");
    fprintf(f, "        ct state established,related accept\n");
    for (i = 0; i < count; i++) {
        host_interface(iface, sizeof(iface), &nets[i]);
        fprintf(f, "        iifname \"%s\" jump net_%s\n", iface, nets[i].name);
    }
    fprintf(f, "    }\n");
    for (i = 0; i < count; i++) {
        nft_chain(f, &nets[i]);
    }
    fprintf(f, "    chain postrouting {\n");
    fprintf(f, "        type nat hook postrouting priority 100; policy accept;\n");
    for (i = 0; i < count; i++) {
        host_interface(iface, sizeof(iface), &nets[i]);
        fprintf(f, "        ip saddr 10.44.%d.0/30 oifname != \"%s\" masquerade\n", nets[i].index, iface);
    }
    fprintf(f, "    }\n");
    fprintf(f, "}\n");
    if (fclose(f) != 0) die("build nft script");
    return script;
}

static void apply_nft(const struct net *nets, int count)
{
    char *argv[] = { "nft", "-f", "-", NULL };
    char *script;

    if (count == 0) {
        probe("nft", "delete", "table", "inet", "sand", NULL);
        return;
    }
    script = nft_script(nets, count);
    if (run_status(-1, argv, script, 0) != 0) fail("nft failed");
    free(script);
}

/* ---- commands ---- */

static void need_root(void)
{
    if (geteuid() != 0) fail("needs root: sudo sand-net ...");
}

/* Everything of one network, in order; idempotent. */
static void bring_up(const struct net *n)
{
    if (!pinned(n)) make_namespaces(n);
    host_side(n);
}

static uid_t owner_uid(const char *user)
{
    struct passwd *pw;
    const char *env;

    if (user != NULL) {
        pw = getpwnam(user);
        if (pw == NULL) fail("no user %s", user);
        return pw->pw_uid;
    }
    env = getenv("SUDO_UID");
    if (env == NULL) fail("whose network? run through sudo, or give --owner USER");
    return atoi(env);
}

static int free_index(const struct net *nets, int count)
{
    int index;
    int i;
    int used;

    for (index = 1; index <= MAX_NETS; index++) {
        used = 0;
        for (i = 0; i < count; i++) {
            if (nets[i].index == index) used = 1;
        }
        if (!used) return index;
    }
    fail("no subnet left");
    return 0;
}

static void create(int argc, char **argv)
{
    static struct net nets[MAX_NETS];
    struct net *n;
    char path[PATH_MAX];
    const char *owner = NULL;
    int count;
    int i;

    need_root();
    if (argc < 1 || argv[0][0] == '-') fail("create: which name?");
    check_name(argv[0]);
    count = load_all(nets);
    if (find(nets, count, argv[0]) != NULL) fail("%s exists", argv[0]);
    if (count == MAX_NETS) fail("no network left");
    n = &nets[count];
    memset(n, 0, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", argv[0]);
    for (i = 1; i < argc; i++) {
        if (i + 1 == argc) fail("%s needs a value", argv[i]);
        if (strcmp(argv[i], "--dns") == 0) {
            if (n->dns_count == MAX_DNS) fail("too many --dns");
            snprintf(n->dns[n->dns_count], sizeof(n->dns[0]), "%s", argv[i + 1]);
            n->dns_count++;
        } else if (strcmp(argv[i], "--owner") == 0) {
            owner = argv[i + 1];
        } else if (strcmp(argv[i], "--allow") == 0) {
            add_rule(n, argv[i + 1], 1);
        } else if (strcmp(argv[i], "--block") == 0) {
            add_rule(n, argv[i + 1], 0);
        } else {
            fail("create: unknown flag %s", argv[i]);
        }
        i++;
    }
    n->owner = owner_uid(owner);
    n->index = free_index(nets, count);
    if (n->dns_count == 0) default_dns(n);

    if (strcmp(state, STATE) == 0) make_dir("/var/lib/sand");
    make_dir(state);
    snprintf(path, sizeof(path), "%s/%s", state, n->name);
    if (mkdir(path, 0755) < 0) die("mkdir %s", path);
    write_config(n);
    bring_up(n);
    apply_nft(nets, count + 1);
    printf("%s: 10.44.%d.0/30, eth0 inside is 10.44.%d.2\n", n->name, n->index, n->index);
}

static void up(int argc, char **argv)
{
    static struct net nets[MAX_NETS];
    struct net *n;
    int count;
    int i;

    need_root();
    count = load_all(nets);
    if (argc > 0) {
        n = find(nets, count, argv[0]);
        if (n == NULL) fail("no network %s", argv[0]);
        bring_up(n);
    } else {
        for (i = 0; i < count; i++) {
            bring_up(&nets[i]);
        }
    }
    apply_nft(nets, count);
}

static void remove_file(const char *name, const char *file)
{
    char path[PATH_MAX];

    state_path(path, sizeof(path), name, file);
    umount2(path, MNT_DETACH);
    if (unlink(path) < 0 && errno != ENOENT) die("remove %s", path);
}

static void delete(int argc, char **argv)
{
    static struct net nets[MAX_NETS];
    struct net *n;
    char iface[32];
    char path[PATH_MAX];
    int count;

    need_root();
    if (argc < 1) fail("delete: which name?");
    count = load_all(nets);
    n = find(nets, count, argv[0]);
    if (n == NULL) fail("no network %s", argv[0]);
    host_interface(iface, sizeof(iface), n);
    docker_remove(iface);
    if (interface_exists(iface)) run(-1, "ip", "link", "del", iface, NULL);
    remove_file(n->name, "net");
    remove_file(n->name, "user");
    remove_file(n->name, "resolv.conf");
    remove_file(n->name, "config");
    snprintf(path, sizeof(path), "%s/%s", state, n->name);
    if (rmdir(path) < 0) die("remove %s", path);
    count = load_all(nets);
    apply_nft(nets, count);
}

static void list(void)
{
    static struct net nets[MAX_NETS];
    struct passwd *pw;
    char iface[32];
    const char *owner;
    const char *status;
    int count;
    int i;
    int j;

    count = load_all(nets);
    for (i = 0; i < count; i++) {
        host_interface(iface, sizeof(iface), &nets[i]);
        status = "down";
        if (pinned(&nets[i]) && interface_exists(iface)) status = "up";
        pw = getpwuid(nets[i].owner);
        owner = "?";
        if (pw != NULL) owner = pw->pw_name;
        printf("%-10s %-5s 10.44.%d.0/30  %s\n", nets[i].name, status, nets[i].index, owner);
        for (j = 0; j < nets[i].rule_count; j++) {
            printf("           %s %s\n", nets[i].rules[j].allow ? "allow" : "block", nets[i].rules[j].text);
        }
    }
}

static void print_nft(void)
{
    static struct net nets[MAX_NETS];
    char *script;
    int count;

    count = load_all(nets);
    if (count == 0) return;
    script = nft_script(nets, count);
    fputs(script, stdout);
    free(script);
}

static void usage(void)
{
    fprintf(stderr, "usage: sudo sand-net create NAME [--dns ADDR]... [--owner USER]\n");
    fprintf(stderr, "                             [--allow RULE]... [--block RULE]...\n");
    fprintf(stderr, "       sudo sand-net up [NAME]\n");
    fprintf(stderr, "       sudo sand-net delete NAME\n");
    fprintf(stderr, "            sand-net list\n");
    fprintf(stderr, "            sand-net nft\n");
    fprintf(stderr, "A network is a namespace with a way out through the host; sand --net NAME joins it.\n");
    fprintf(stderr, "RULE = [ADDR[/PREFIX]][:PORT[-PORT]] | all, e.g. :443, 10.0.0.0/8, [2001:db8::1]:53.\n");
    fprintf(stderr, "up rebuilds what is missing, which after a reboot is everything; nft prints its firewall table.\n");
    exit(2);
}

int main(int argc, char **argv)
{
    if (getenv("SAND_NET_STATE") != NULL) state = getenv("SAND_NET_STATE");
    if (argc < 2) usage();
    if (strcmp(argv[1], "create") == 0) {
        create(argc - 2, argv + 2);
    } else if (strcmp(argv[1], "up") == 0) {
        up(argc - 2, argv + 2);
    } else if (strcmp(argv[1], "delete") == 0) {
        delete(argc - 2, argv + 2);
    } else if (strcmp(argv[1], "list") == 0) {
        list();
    } else if (strcmp(argv[1], "nft") == 0) {
        print_nft();
    } else {
        usage();
    }
    return 0;
}

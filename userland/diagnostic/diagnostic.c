/* Small freestanding Darwin diagnostic userland, using real BSD syscalls.
 * This is a boot/ABI test environment, not a complete Darwin distribution.
 */
#include <stddef.h>
#include <stdint.h>

struct result { long value, second; };
static struct result syscall6(long number, long a, long b, long c, long d, long e, long f) {
    unsigned failed;
#if defined(__aarch64__)
    register long r0 __asm__("x0") = a, r1 __asm__("x1") = b;
    register long r2 __asm__("x2") = c, r3 __asm__("x3") = d;
    register long r4 __asm__("x4") = e, r5 __asm__("x5") = f;
    register long call __asm__("x16") = number;
    __asm__ volatile("svc #0x80\n cset %w2, cs"
                     : "+r"(r0), "+r"(r1), "=r"(failed)
                     : "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(call) : "memory", "cc");
#else
    register long r0 __asm__("rax") = number | 0x2000000;
    register long r1 __asm__("rdx") = c;
    register long r3 __asm__("r10") = d, r4 __asm__("r8") = e, r5 __asm__("r9") = f;
    unsigned char carry;
    __asm__ volatile("syscall\n setc %2"
                     : "+a"(r0), "+d"(r1), "=qm"(carry)
                     : "D"(a), "S"(b), "r"(r3), "r"(r4), "r"(r5) : "rcx", "r11", "memory", "cc");
    failed = carry;
#endif
    return (struct result){ failed ? -r0 : r0, r1 };
}
static long call(long n, long a, long b, long c, long d, long e, long f) {
    return syscall6(n, a, b, c, d, e, f).value;
}
static size_t length(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; } return *a == *b;
}
static void bytes(int fd, const void *data, size_t n) {
    const char *p = data;
    while (n) { long done = call(4, fd, (long)p, n, 0, 0, 0); if (done <= 0) return; p += done; n -= done; }
}
static void say(const char *s) { bytes(1, s, length(s)); }
static void close_fd(int fd) { call(6, fd, 0, 0, 0, 0, 0); }
static void exit_process(int status) {
    call(1, status, 0, 0, 0, 0, 0);
    for (;;) {}
}
static long fork_process(void) {
    struct result r = syscall6(2, 0, 0, 0, 0, 0, 0);
    return r.value < 0 ? r.value : (r.second ? 0 : r.value);
}
static int pipe_fds(int fds[2]) {
    struct result r = syscall6(42, 0, 0, 0, 0, 0, 0);
    if (r.value < 0) return -1;
    fds[0] = r.value; fds[1] = r.second; return 0;
}
static int kernel_string(int kind, int selector, char *out, size_t capacity) {
    int mib[2] = {kind, selector}; size_t n = capacity;
    if (call(202, (long)mib, 2, (long)out, (long)&n, 0, 0) < 0 || !n || n > capacity) return -1;
    out[capacity - 1] = 0; return 0;
}
static int uname_main(int argc, char **argv) {
    char value[128];
    int kind = 1, selector = 1; /* CTL_KERN / KERN_OSTYPE */
    if (argc == 2 && equal(argv[1], "-r")) selector = 2; /* KERN_OSRELEASE */
    else if (argc == 2 && equal(argv[1], "-m")) { kind = 6; selector = 1; } /* CTL_HW / HW_MACHINE */
    else if (argc != 1) { say("usage: uname [-r|-m]\n"); return 2; }
    if (kernel_string(kind, selector, value, sizeof(value))) { say("uname: sysctl failed\n"); return 1; }
    say(value); say("\n"); return 0;
}

#define MAX_WORDS 12
#define WORD_SIZE 256
static int parse(const char *, char [MAX_WORDS][WORD_SIZE], char **, unsigned);
static int launch(char **argv, char *capture, size_t capacity) {
    char path[WORD_SIZE]; size_t n = 0;
    if (argv[0][0] != '/') { const char *prefix = "/bin/"; while (*prefix) path[n++] = *prefix++; }
    const char *command = argv[0];
    while (*command && n + 1 < sizeof(path)) path[n++] = *command++;
    if (*command) return -1;
    path[n] = 0;
    int fds[2] = {-1,-1};
    if (capture && pipe_fds(fds)) return -1;
    long pid = fork_process();
    if (pid < 0) { if (capture) { close_fd(fds[0]); close_fd(fds[1]); } return -1; }
    if (!pid) {
        if (capture) { close_fd(fds[0]); call(90, fds[1], 1, 0, 0, 0, 0); close_fd(fds[1]); }
        char *environment[] = {0};
        call(59, (long)path, (long)argv, (long)environment, 0, 0, 0);
        say("exec failed\n"); exit_process(127);
    }
    if (capture) {
        close_fd(fds[1]); n = 0;
        char buffer[128];
        for (;;) {
            long got = call(3, fds[0], (long)buffer, sizeof(buffer), 0, 0, 0);
            if (got <= 0) break;
            for (long i = 0; i < got; ++i) if (n + 1 < capacity) capture[n++] = buffer[i];
        }
        close_fd(fds[0]);
        while (n && (capture[n-1] == '\n' || capture[n-1] == '\r')) --n;
        capture[n] = 0;
    }
    int status = 0;
    if (call(7, pid, (long)&status, 0, 0, 0, 0) < 0) return -1;
    return status ? -1 : 0;
}
static int parse(const char *line, char words[MAX_WORDS][WORD_SIZE], char **argv, unsigned depth) {
    int count = 0;
    if (depth > 4) return -1;
    while (*line) {
        while (*line == ' ' || *line == '\t') ++line;
        if (!*line) break;
        if (count == MAX_WORDS - 1) return -1;
        size_t n = 0; char quote = 0;
        while (*line && (quote || (*line != ' ' && *line != '\t'))) {
            if ((*line == '"' || *line == '\'') && (!quote || quote == *line)) {
                quote = quote ? 0 : *line; ++line; continue;
            }
            if (quote != '\'' && line[0] == '$' && line[1] == '(') {
                char command[WORD_SIZE], output[WORD_SIZE]; size_t size = 0;
                line += 2; unsigned nesting = 1;
                while (*line && nesting) {
                    char ch = *line++;
                    if (ch == '(') ++nesting;
                    if (ch == ')' && !--nesting) break;
                    if (size + 1 >= sizeof(command)) return -1;
                    command[size++] = ch;
                }
                if (nesting) return -1;
                command[size] = 0;
                char inner[MAX_WORDS][WORD_SIZE], *args[MAX_WORDS];
                if (parse(command, inner, args, depth + 1) <= 0 || launch(args, output, sizeof(output))) return -1;
                for (size_t i = 0; output[i]; ++i) { if (n + 1 >= WORD_SIZE) return -1; words[count][n++] = output[i]; }
                continue;
            }
            if (quote != '\'' && line[0] == '\\' && line[1]) {
                if (!quote || line[1] == '\\' || line[1] == '"' || line[1] == '$') ++line;
            }
            if (n + 1 >= WORD_SIZE) return -1;
            words[count][n++] = *line++;
        }
        if (quote) return -1;
        words[count][n] = 0; argv[count] = words[count]; ++count;
    }
    argv[count] = 0; return count;
}
static void printf_command(int argc, char **argv) {
    if (argc < 2) return;
    int argument = 2;
    for (const char *p = argv[1]; *p; ++p) {
        if (*p == '%' && p[1] == 's') { ++p; if (argument < argc) say(argv[argument++]); }
        else if (*p == '%' && p[1] == '%') { ++p; say("%"); }
        else if (*p == '\\' && p[1] == 'n') { ++p; say("\n"); }
        else if (*p == '\\' && p[1] == 'r') { ++p; say("\r"); }
        else bytes(1, p, 1);
    }
}
static void cat_command(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        long fd = call(5, (long)argv[i], 0, 0, 0, 0, 0);
        if (fd < 0) { say("cat: open failed\n"); continue; }
        char data[512]; long got;
        while ((got = call(3, fd, (long)data, sizeof(data), 0, 0, 0)) > 0) bytes(1, data, got);
        close_fd(fd);
    }
}
static void shell(void) {
    say("Darwin diagnostic shell: uname, cat, echo, printf, help\n");
    for (;;) {
        say("\nroot@darwin-embedded# ");
        char line[1024]; size_t n = 0;
        for (;;) {
            char ch; long got = call(3, 0, (long)&ch, 1, 0, 0, 0);
            if (got <= 0) { say("console read failed\n"); for (;;) {} }
            if (ch == '\n' || ch == '\r') break;
            if (n + 1 < sizeof(line)) line[n++] = ch;
        }
        line[n] = 0;
        char words[MAX_WORDS][WORD_SIZE], *argv[MAX_WORDS];
        int argc = parse(line, words, argv, 0);
        if (argc < 0) { say("invalid command or substitution failed\n"); continue; }
        if (!argc) continue;
        if (equal(argv[0], "printf")) printf_command(argc, argv);
        else if (equal(argv[0], "echo")) { for (int i = 1; i < argc; ++i) { if (i != 1) say(" "); say(argv[i]); } say("\n"); }
        else if (equal(argv[0], "cat")) cat_command(argc, argv);
        else if (equal(argv[0], "help")) say("Commands use real BSD system calls. uname runs through fork and exec.\n");
        else if (launch(argv, 0, 0)) say("command failed\n");
    }
}
void user_main(int argc, char **argv) {
    const char *program = argc > 0 ? argv[0] : "";
    const char *name = program;
    for (const char *p = program; *p; ++p) if (*p == '/') name = p + 1;
    if (equal(name, "uname")) exit_process(uname_main(argc, argv));
    long fd = call(5, (long)"/dev/console", 2, 0, 0, 0, 0);
    if (fd < 0) exit_process(126);
    for (int i = 0; i < 3; ++i) call(90, fd, i, 0, 0, 0, 0);
    if (fd > 2) close_fd(fd);
    shell();
}

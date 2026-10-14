/* Internal state for native, transactional Mach-O module loading. */
#include <setjmp.h>
extern int _setjmp(jmp_buf) __attribute__((returns_twice));
extern void _longjmp(jmp_buf, int) __attribute__((noreturn));

#define MAX_HANDLES 256
#define MAX_ERRORS 1024
struct image;
struct load_transaction {
    jmp_buf recovery;
    struct image *created[MAX_IMAGES];
    unsigned count;
    unsigned no_load;
    unsigned local;
    struct image *root;
    char error[MAX_ERRORS];
};
static struct load_transaction *active_transaction;
static uint64_t loader_lock_owner;
static unsigned loader_lock_depth;
static uint64_t mutation_lock_owner;
static unsigned mutation_lock_depth;
struct loader_transition { struct loader_transition *next; uint64_t owner; };
static struct loader_transition *loader_transitions;
static void begin_transition(struct loader_transition *);
static void end_transition(struct loader_transition *);
int NXArgc;
char **NXArgv, **environ;
static char **process_apple;
const char *__progname;
/* Exact non-Exclave dyld1378 ProgramVars layout. */
struct native_program_vars {
    const void *mh;
    int *argc;
    char ***argv;
    char ***environment;
    const char **name;
};
static struct native_program_vars program_vars;
static void lock_loader(void);
static void unlock_loader(void);
static void wait_loader(void);
static unsigned release_loader(void);
static void restore_loader(unsigned);
static void lock_mutation(void);
static void unlock_mutation(void);
static uint64_t native_thread_identity(void);
static void unlock_scope(unsigned *held) { if (*held) unlock_loader(); }
#define LOADER_LOCKED() lock_loader(); unsigned lock_held __attribute__((cleanup(unlock_scope))) = 1
static void unlock_mutation_scope(unsigned *held) { if (*held) unlock_mutation(); }
#define MUTATION_LOCKED() lock_mutation(); unsigned mutation_held __attribute__((cleanup(unlock_mutation_scope))) = 1
static void error_text(char *, const char *, const char *);
static void notify_images(struct image *[], unsigned);
static void collect_images(void);
static int in_transaction(const struct image *);
static void remember_binding(struct image *, uintptr_t);
static uintptr_t runtime_symbol(const char *);
static void validate_module(struct image *);
static const struct image *image_for_address(const void *);
static void retain_permanently(struct image *);
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int dlclose(void *);
char *dlerror(void);
void _dyld_dlopen_atfork_prepare(void);
void _dyld_dlopen_atfork_parent(void);
void _dyld_dlopen_atfork_child(void);

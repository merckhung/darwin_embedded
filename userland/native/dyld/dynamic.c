/* Included by loader.c: these routines share its validated image registry. */
struct native_handle { struct image *image; unsigned references, first, all; };
static struct native_handle handles[MAX_HANDLES];
struct error_state { unsigned available; char text[MAX_ERRORS]; };
/* pthread_key_t is unsigned long in the matching Apple pthread headers. */
static unsigned long error_key;
static int error_key_ready;
static int (*thread_key_create)(unsigned long *, void (*)(void *));
static void *(*thread_getspecific)(unsigned long);
static int (*thread_setspecific)(unsigned long, const void *);
static unsigned collecting;

static uint64_t native_thread_identity(void) {
#ifdef NATIVE_DYLD_HOST_TEST
    /* Host-only parser tests never execute Darwin TSD instructions. */
    return 1;
#else
    return (uintptr_t)_os_tsd_get_direct(__TSD_MACH_THREAD_SELF);
#endif
}

static void lock_loader(void) {
    uint64_t self = native_thread_identity();
    require(self != 0, "native loader requires initialized thread identity");
    if (__atomic_load_n(&loader_lock_owner, __ATOMIC_ACQUIRE) == self) {
        ++loader_lock_depth;
        return;
    }
    uint64_t empty = 0;
    while (!__atomic_compare_exchange_n(&loader_lock_owner, &empty, self, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        empty = 0;
        swtch();
    }
    loader_lock_depth = 1;
}
static void unlock_loader(void) {
    require(loader_lock_depth && loader_lock_owner == native_thread_identity(), "native loader lock ownership");
    if (!--loader_lock_depth) __atomic_store_n(&loader_lock_owner, 0, __ATOMIC_RELEASE);
}
static unsigned release_loader(void) {
    /* A reentrant dlopen from a finalizer may hold several lock levels.
     * Waiting with any level retained would prevent the initializing thread
     * from publishing completion. The caller's pins/transitions remain live. */
    unsigned depth = loader_lock_depth;
    require(depth != 0, "native loader wait requires lock ownership");
    for (unsigned n = 0; n < depth; ++n) unlock_loader();
    return depth;
}
static void restore_loader(unsigned depth) {
    for (unsigned n = 0; n < depth; ++n) lock_loader();
}
static void wait_loader(void) {
    unsigned depth = release_loader();
    swtch();
    restore_loader(depth);
}
static void lock_mutation(void) {
    uint64_t self = native_thread_identity();
    require(self != 0, "native loader mutation requires initialized thread identity");
    if (__atomic_load_n(&mutation_lock_owner, __ATOMIC_ACQUIRE) == self) {
        ++mutation_lock_depth;
        return;
    }
    uint64_t empty = 0;
    while (!__atomic_compare_exchange_n(&mutation_lock_owner, &empty, self, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        empty = 0;
        swtch();
    }
    mutation_lock_depth = 1;
}
static void unlock_mutation(void) {
    require(mutation_lock_depth && mutation_lock_owner == native_thread_identity(),
            "native loader mutation lock ownership");
    if (!--mutation_lock_depth) __atomic_store_n(&mutation_lock_owner, 0, __ATOMIC_RELEASE);
}
static void begin_transition(struct loader_transition *transition) {
    transition->owner = native_thread_identity();
    transition->next = loader_transitions;
    loader_transitions = transition;
}
static void end_transition(struct loader_transition *transition) {
    struct loader_transition **cursor = &loader_transitions;
    while (*cursor && *cursor != transition) cursor = &(*cursor)->next;
    require(*cursor != 0, "native loader transition ownership");
    *cursor = transition->next;
}
void _dyld_dlopen_atfork_prepare(void) {
    /* dyld1378 keeps a separate recursive mutation/API lock across notifiers
     * and initializers, while its registry lock is released for user code.
     * Taking that lock drains other threads without treating blocked openers
     * as unfinished callbacks when an initializer itself calls fork. */
    lock_mutation();
    lock_loader();
    for (struct loader_transition *cursor = loader_transitions; cursor; cursor = cursor->next)
        require(cursor->owner == native_thread_identity(), "native loader mutation did not drain transitions");
}
void _dyld_dlopen_atfork_parent(void) { unlock_loader(); unlock_mutation(); }
void _dyld_dlopen_atfork_child(void) {
    /* Only the calling thread survives fork; inherited ownership is obsolete. */
    uint64_t previous = loader_lock_owner, current = native_thread_identity();
    for (struct loader_transition *cursor = loader_transitions; cursor; cursor = cursor->next) {
        require(cursor->owner == previous, "fork inherited another native loader transition");
        cursor->owner = current;
    }
    for (unsigned n = 0; n < image_count; ++n) {
        if (images[n].loading_thread == previous) images[n].loading_thread = current;
        if (images[n].initializing_thread == previous) images[n].initializing_thread = current;
    }
    require(loader_lock_depth != 0, "native loader fork preparation missing");
    --loader_lock_depth;
    active_transaction = 0;
    __atomic_store_n(&loader_lock_owner, loader_lock_depth ? current : 0, __ATOMIC_RELEASE);
    require(mutation_lock_depth && mutation_lock_owner == previous, "native loader fork mutation ownership");
    --mutation_lock_depth;
    __atomic_store_n(&mutation_lock_owner, mutation_lock_depth ? current : 0, __ATOMIC_RELEASE);
}
static void error_text(char *out, const char *reason, const char *detail) {
    size_t n = 0;
    while (*reason && n + 1 < MAX_ERRORS) out[n++] = *reason++;
    if (detail) {
        if (n + 2 < MAX_ERRORS) { out[n++] = ':'; out[n++] = ' '; }
        while (*detail && n + 1 < MAX_ERRORS) out[n++] = *detail++;
    }
    out[n] = 0;
}
static uintptr_t runtime_symbol(const char *name) {
    for (unsigned n = 0; n < image_count; ++n) {
        if (!images[n].header || images[n].unloading) continue;
        uintptr_t result = exported(images + n, name, 0);
        if (result) return result;
    }
    return 0;
}
static void validate_module(struct image *image) {
    uint64_t template_begin = 0, template_end = 0;
    unsigned template_segment = 0, found_template = 0, initialized_content = 0;
    uint64_t descriptors = 0;
    struct native_thunk { uintptr_t function, key, offset; };
    for (unsigned n = 0; n < image->segment_count; ++n) {
        const struct segment_command_64 *segment = image->segments[n];
        const struct section_64 *sections = (const void *)(segment + 1);
        for (unsigned j = 0; j < segment->nsects; ++j) {
            const struct section_64 *section = sections + j;
            unsigned type = section->flags & SECTION_TYPE;
            if (type == S_MOD_INIT_FUNC_POINTERS || type == S_MOD_TERM_FUNC_POINTERS) {
                require(!(section->size & 7) && !(section->addr & 7), "invalid native initializer array");
                const uintptr_t *functions = (const void *)(section->addr + image->slide);
                for (uint64_t k = 0; k < section->size / sizeof(*functions); ++k) {
                    const struct image *owner = image_for_address((void *)functions[k]);
                    const struct segment_command_64 *code = owner ?
                        segment_for_range(owner, functions[k], 1) : 0;
                    require(code && (code->initprot & 4), "native initializer is not executable");
                }
            }
            if ((type == S_THREAD_LOCAL_REGULAR || type == S_THREAD_LOCAL_ZEROFILL) && section->size) {
                require((segment->initprot & 1) && (!found_template ||
                        (template_segment == n && section->addr >= template_end)), "invalid native TLS template range");
                if (!found_template) { template_begin = section->addr; template_segment = n; found_template = 1; }
                template_end = section->addr + section->size;
                initialized_content |= type == S_THREAD_LOCAL_REGULAR;
            }
            if (type == S_THREAD_LOCAL_VARIABLES) {
                require((segment->initprot & 2) && !(segment->flags & SG_READ_ONLY) &&
                        !(section->addr & 7) && section->size % sizeof(struct native_thunk) == 0,
                        "invalid native TLS descriptor section");
                descriptors += section->size / sizeof(struct native_thunk);
            }
            require(type != S_THREAD_LOCAL_INIT_FUNCTION_POINTERS || !section->size,
                    "native TLS initializer pointers are unsupported");
        }
    }
    if (!found_template && !descriptors && !(image->header->flags & MH_HAS_TLV_DESCRIPTORS)) return;
    require((image->header->flags & MH_HAS_TLV_DESCRIPTORS) && !(image->header->flags & MH_DYLIB_IN_CACHE) &&
            found_template && descriptors && template_end - template_begin <= UINT32_MAX,
            "invalid native TLS image metadata");
    uintptr_t bootstrap = runtime_symbol("__tlv_bootstrap");
    require(bootstrap != 0, "native compiler TLS provider is unavailable");
    for (unsigned n = 0; n < image->segment_count; ++n) {
        const struct segment_command_64 *segment = image->segments[n];
        const struct section_64 *sections = (const void *)(segment + 1);
        for (unsigned j = 0; j < segment->nsects; ++j) {
            const struct section_64 *section = sections + j;
            if ((section->flags & SECTION_TYPE) != S_THREAD_LOCAL_VARIABLES) continue;
            const struct native_thunk *thunks = (const void *)(section->addr + image->slide);
            for (uint64_t k = 0; k < section->size / sizeof(*thunks); ++k) {
                require(!thunks[k].key && thunks[k].offset < template_end - template_begin &&
                        thunks[k].function == bootstrap, "invalid native TLS descriptor");
                uintptr_t from = (uintptr_t)&thunks[k].offset, to = template_begin + image->slide;
                require(!initialized_content || (to >= from ? to - from <= INT32_MAX :
                        from - to <= (uint64_t)INT32_MAX + 1), "native TLS template delta is too large");
            }
        }
    }
}
static void destroy_error(void *state) { __munmap(state, PAGE_SIZE); }
static struct error_state *thread_error(void) {
    if (!error_key_ready) {
        thread_key_create = (void *)runtime_symbol("_pthread_key_create");
        thread_getspecific = (void *)runtime_symbol("_pthread_getspecific");
        thread_setspecific = (void *)runtime_symbol("_pthread_setspecific");
        if (!thread_key_create || !thread_getspecific || !thread_setspecific ||
            thread_key_create(&error_key, destroy_error)) return 0;
        error_key_ready = 1;
    }
    struct error_state *state = thread_getspecific(error_key);
    if (!state) {
        state = __mmap(0, PAGE_SIZE, 3, 0x1002, -1, 0);
        if (state == (void *)-1) return 0;
        if (thread_setspecific(error_key, state)) {
            __munmap(state, PAGE_SIZE);
            return 0;
        }
    }
    return state;
}
static void set_error(struct error_state *state, const char *reason, const char *detail) {
    /* Like dyld1378, no error string is available before pthread setup or
     * when allocating the calling thread's error buffer fails. */
    if (!state) return;
    error_text(state->text, reason, detail);
    state->available = 1;
}
char *dlerror(void) {
    LOADER_LOCKED();
    struct error_state *state = thread_error();
    if (!state || !state->available) return 0;
    state->available = 0;
    return state->text;
}
static int in_transaction(const struct image *image) {
    if (!active_transaction) return 0;
    for (unsigned n = 0; n < active_transaction->count; ++n)
        if (active_transaction->created[n] == image) return 1;
    return 0;
}
static void remember_binding(struct image *image, uintptr_t address) {
    struct image *target = (void *)image_for_address((void *)address);
    if (!target || target == image || target == &linker_image) return;
    for (unsigned n = 0; n < image->binding_dependency_count; ++n)
        if (image->binding_dependencies[n] == target) return;
    require(image->binding_dependency_count < MAX_IMAGES, "too many native binding dependencies");
    image->binding_dependencies[image->binding_dependency_count++] = target;
}
static void retain_permanently(struct image *image) {
    if (!image || image->permanent) return;
    image->permanent = 1;
    for (unsigned n = 0; n < image->dependency_count; ++n) retain_permanently(image->dependencies[n]);
    for (unsigned n = 0; n < image->binding_dependency_count; ++n) retain_permanently(image->binding_dependencies[n]);
}
static void make_global(struct image *image) {
    if (!image || image->global) return;
    image->global = 1;
    for (unsigned n = 0; n < image->dependency_count; ++n) make_global(image->dependencies[n]);
}
static void mark_image(struct image *image) {
    if (!image || image->marked) return;
    image->marked = 1;
    for (unsigned n = 0; n < image->dependency_count; ++n) mark_image(image->dependencies[n]);
    for (unsigned n = 0; n < image->binding_dependency_count; ++n) mark_image(image->binding_dependencies[n]);
}
static void discard_image(struct image *image) {
    for (unsigned n = 0; n < image_count; ++n)
        if (images[n].load_parent == image) images[n].load_parent = image->load_parent;
    if (image->descriptor >= 0) __close_nocancel(image->descriptor);
    if (image->reservation) __munmap(image->reservation, image->reservation_size);
    memset(image, 0, sizeof(*image));
    image->descriptor = -1;
}
static void notify_images(struct image *snapshot[], unsigned count) {
    const struct mach_header *headers[MAX_IMAGES];
    const char *paths[MAX_IMAGES];
    bulk_image_callback callbacks[64];
    unsigned callback_count = image_callback_count;
    for (unsigned n = 0; n < callback_count; ++n) callbacks[n] = image_callbacks[n];
    for (unsigned n = 0; n < count; ++n) {
        ++snapshot[n]->pins;
        headers[n] = (const void *)snapshot[n]->header;
        paths[n] = snapshot[n]->path;
    }
    unsigned depth = release_loader();
    for (unsigned n = 0; n < callback_count; ++n) callbacks[n](count, headers, paths);
    restore_loader(depth);
    for (unsigned n = 0; n < count; ++n) --snapshot[n]->pins;
}

/* Actual Libc/stdlib/FreeBSD/atexit.h ABI. */
struct native_cxa_range { const void *address; size_t size; };
static void collect_images(void) {
    if (collecting++) return;
    do {
        for (unsigned n = 0; n < image_count; ++n) images[n].marked = 0;
        for (unsigned n = 0; n < image_count; ++n)
            if (images[n].header && (images[n].permanent || images[n].open_count || images[n].pins ||
                                   images[n].loading_thread || images[n].initialized == 1)) mark_image(images + n);
        struct image *dead[MAX_IMAGES];
        unsigned count = 0;
        for (unsigned n = 0; n < image_count; ++n) {
            if (!images[n].header || images[n].marked) continue;
            images[n].unloading = 1;
            dead[count++] = images + n;
        }
        /* Dependents finish initialization after their dependencies. */
        for (unsigned n = 1; n < count; ++n) {
            struct image *item = dead[n]; unsigned j = n;
            while (j && dead[j - 1]->initialization_order < item->initialization_order) {
                dead[j] = dead[j - 1]; --j;
            }
            dead[j] = item;
        }
        void (*finalize)(const struct native_cxa_range *, unsigned) =
            (void *)runtime_symbol("___cxa_finalize_ranges");
        remove_image_callback callbacks[64];
        unsigned callback_count = remove_image_callback_count;
        for (unsigned n = 0; n < callback_count; ++n) callbacks[n] = remove_image_callbacks[n];
        /* Like dyld1378's reaper, keep the recursive mutation/API lock during
         * teardown. Release the registry lock for finalizers; reentrant
         * dlclose schedules another collection pass. */
        unsigned depth = release_loader();
        for (unsigned n = 0; n < count; ++n) {
            struct image *image = dead[n];
            if (image->initialized != 2) continue;
            struct native_cxa_range ranges[MAX_SEGMENTS]; unsigned range_count = 0;
            for (unsigned j = 0; j < image->segment_count; ++j) {
                const struct segment_command_64 *segment = image->segments[j];
                if (segment->initprot & 4)
                    ranges[range_count++] = (struct native_cxa_range){
                        (void *)(segment->vmaddr + image->slide), segment->vmsize};
            }
            if (finalize && range_count) finalize(ranges, range_count);
            for (unsigned j = image->segment_count; j; --j) {
                const struct segment_command_64 *segment = image->segments[j - 1];
                const struct section_64 *sections = (const void *)(segment + 1);
                for (unsigned k = segment->nsects; k; --k) {
                    const struct section_64 *section = sections + k - 1;
                    if ((section->flags & SECTION_TYPE) != S_MOD_TERM_FUNC_POINTERS) continue;
                    void (**functions)(void) = (void *)(section->addr + image->slide);
                    for (uint64_t i = section->size / sizeof(*functions); i; --i) functions[i - 1]();
                }
            }
        }
        for (unsigned n = 0; n < count; ++n) {
            if (dead[n]->initialized != 2) continue;
            for (unsigned j = 0; j < callback_count; ++j)
                callbacks[j]((const void *)dead[n]->header, (intptr_t)dead[n]->slide);
        }
        restore_loader(depth);
        for (unsigned n = 0; n < count; ++n) discard_image(dead[n]);
    } while (--collecting);
}
static struct native_handle *checked_handle(void *value) {
    uintptr_t pointer = (uintptr_t)value, start = (uintptr_t)handles;
    if (pointer < start || pointer - start >= sizeof(handles) ||
        (pointer - start) % sizeof(*handles)) return 0;
    struct native_handle *handle = value;
    return handle->references ? handle : 0;
}
static struct native_handle *new_handle(struct image *image, unsigned first, unsigned all) {
    struct native_handle *empty = 0;
    for (unsigned n = 0; n < MAX_HANDLES; ++n) {
        struct native_handle *handle = handles + n;
        if (!handle->references) { if (!empty) empty = handle; continue; }
        if (handle->image == image && handle->first == first && handle->all == all) {
            require(handle->references != UINT32_MAX, "native module handle count overflow");
            ++handle->references;
            return handle;
        }
    }
    if (!empty) return 0;
    *empty = (struct native_handle){image, 1, first, all};
    return empty;
}
static int search_closure(struct image *root, const char *name, int first, uintptr_t *result) {
    struct image *queue[MAX_IMAGES]; unsigned count = 0;
    queue[count++] = root;
    for (unsigned n = 0; n < count; ++n) {
        struct image *image = queue[n];
        if (!image || image->unloading) continue;
        if (find_export(image, name, 0, result)) return 1;
        if (first) break;
        for (unsigned j = 0; j < image->dependency_count; ++j) {
            struct image *dependency = image->dependencies[j];
            unsigned k = 0;
            while (k < count && queue[k] != dependency) ++k;
            if (dependency && k == count && count < MAX_IMAGES) queue[count++] = dependency;
        }
    }
    return 0;
}
void *dlsym(void *value, const char *symbol) {
    void *caller_address = __builtin_return_address(0);
    LOADER_LOCKED();
    struct error_state *error = thread_error(); if (error) error->available = 0;
    if (!symbol || length(symbol) + 2 > MAX_PATH) {
        set_error(error, "invalid native symbol name", 0); return 0;
    }
    char name[MAX_PATH]; name[0] = '_'; copy_path(name + 1, symbol);
    uintptr_t address = 0;
    int found = 0;
    struct native_handle *handle = checked_handle(value);
    intptr_t special = (intptr_t)value;
    if (handle && !handle->all) found = search_closure(handle->image, name, handle->first, &address);
    else if ((handle && handle->all) || special == -2 || special == -1 || special == -3) {
        const struct image *caller = image_for_address(caller_address);
        int ready = special != -1 && special != -3;
        for (unsigned n = 0; n < image_count && !found; ++n) {
            struct image *image = images + n;
            if (!image->header || image->unloading) continue;
            if (image == caller) { ready = 1; if (special == -1) continue; }
            if (ready && (image->global || image == caller)) found = find_export(image, name, 0, &address);
        }
        if (!found && special != -1 && special != -3) { address = loader_api(name); found = address != 0; }
    } else if (special == -5) found = find_export(images, name, 0, &address);
    else { set_error(error, "invalid native module handle", 0); return 0; }
    if (!found) set_error(error, "native symbol not found", symbol);
    return (void *)address;
}
void *dlopen(const char *path, int mode) {
    void *caller_address = __builtin_return_address(0);
    MUTATION_LOCKED();
    LOADER_LOCKED();
    struct error_state *error = thread_error(); if (error) error->available = 0;
    unsigned known = NATIVE_RTLD_LAZY | NATIVE_RTLD_NOW | NATIVE_RTLD_LOCAL | NATIVE_RTLD_GLOBAL |
                     NATIVE_RTLD_NOLOAD | NATIVE_RTLD_NODELETE | NATIVE_RTLD_FIRST;
    if (((unsigned)mode & ~known) || ((mode & NATIVE_RTLD_LOCAL) && (mode & NATIVE_RTLD_GLOBAL))) {
        set_error(error, "unsupported native dlopen mode", 0); return 0;
    }
    if (!path) {
        struct native_handle *handle = new_handle(images, (mode & NATIVE_RTLD_FIRST) != 0,
                                                   (mode & NATIVE_RTLD_FIRST) == 0);
        if (!handle) set_error(error, "too many native module handles", 0);
        return handle;
    }
    if (!*path) { set_error(error, "empty native module path", 0); return 0; }
    struct load_transaction *transaction = __mmap(0, rounded(sizeof(*transaction)), 3, 0x1002, -1, 0);
    if (transaction == (void *)-1) { set_error(error, "cannot allocate native load transaction", 0); return 0; }
    transaction->no_load = (mode & NATIVE_RTLD_NOLOAD) != 0;
    active_transaction = transaction;
    if (_setjmp(transaction->recovery)) {
        active_transaction = 0;
        for (unsigned n = transaction->count; n; --n) discard_image(transaction->created[n - 1]);
        set_error(error, transaction->error, 0);
        __munmap(transaction, rounded(sizeof(*transaction)));
        return 0;
    }
    struct image *owner = (void *)image_for_address(caller_address);
    if (!owner) owner = images;
    struct image *root = load_library(path, 0, owner);
    transaction->root = root;
    transaction->local = (mode & NATIVE_RTLD_LOCAL) != 0;
    for (unsigned n = 0; n < transaction->count; ++n) {
        struct image *image = transaction->created[n];
        writable_fixups(image, 0); rebase(image);
        if (image->info) {
            bind(image, image->info->bind_off, image->info->bind_size, 0, 0);
            bind(image, image->info->lazy_bind_off, image->info->lazy_bind_size, 1, 0);
            bind(image, image->info->weak_bind_off, image->info->weak_bind_size, 0, 1);
        }
        writable_fixups(image, 1);
    }
    for (unsigned n = 0; n < transaction->count; ++n) validate_module(transaction->created[n]);
    struct native_handle *handle = new_handle(root, (mode & NATIVE_RTLD_FIRST) != 0, 0);
    require(handle != 0, "too many native module handles");
    ++root->open_count;
    for (unsigned n = 0; n < transaction->count; ++n) {
        struct image *image = transaction->created[n];
        if (image->header->flags & MH_HAS_TLV_DESCRIPTORS) retain_permanently(image);
        if (image != root) make_global(image); /* Apple's RTLD_LOCAL applies to the root. */
    }
    if (!(mode & NATIVE_RTLD_LOCAL)) make_global(root);
    if (mode & NATIVE_RTLD_NODELETE) retain_permanently(root);
    active_transaction = 0;
    coalesce_loaded_images();
    struct loader_transition transition;
    begin_transition(&transition);
    if (transaction->count) notify_images(transaction->created, transaction->count);
    for (unsigned n = 0; n < transaction->count; ++n) transaction->created[n]->loading_thread = 0;
    __munmap(transaction, rounded(sizeof(*transaction)));
    unlock_loader(); lock_held = 0;
    initialize(root, NXArgc, NXArgv, environ, process_apple);
    lock_loader();
    end_transition(&transition);
    unlock_loader();
    return handle;
}
int dlclose(void *value) {
    MUTATION_LOCKED();
    LOADER_LOCKED();
    struct error_state *error = thread_error(); if (error) error->available = 0;
    struct native_handle *handle = checked_handle(value);
    if (!handle) { set_error(error, "invalid native module handle", 0); return -1; }
    --handle->references;
    if (handle->image && handle->image != images) --handle->image->open_count;
    collect_images();
    return 0;
}

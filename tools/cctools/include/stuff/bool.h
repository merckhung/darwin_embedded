/* bool.h */
#ifndef ENUM_DYLD_BOOL
#define ENUM_DYLD_BOOL

#ifdef bool
#undef bool
#endif

#undef FALSE
#undef TRUE

#define bool _darwin_bool
typedef enum _darwin_bool {
    FALSE = 0,
    TRUE = 1
} _darwin_bool;

#endif /* ENUM_DYLD_BOOL */

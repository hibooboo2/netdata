#include "common.h"

#ifdef __APPLE__
#define INHERIT_NONE 0
#endif /* __APPLE__ */
#if defined(__FreeBSD__) || defined(__APPLE__)
#    define O_NOATIME     0
#    define MADV_DONTFORK INHERIT_NONE
#endif /* __FreeBSD__ || __APPLE__*/

char *netdata_configured_hostname    = NULL;
char *netdata_configured_config_dir  = NULL;
char *netdata_configured_log_dir     = NULL;
char *netdata_configured_plugins_dir = NULL;
char *netdata_configured_web_dir     = NULL;
char *netdata_configured_cache_dir   = NULL;
char *netdata_configured_varlib_dir  = NULL;
char *netdata_configured_home_dir    = NULL;
char *netdata_configured_host_prefix = NULL;
char *netdata_configured_timezone    = NULL;

int enable_ksm = 1;

volatile sig_atomic_t netdata_exit = 0;
const char *os_type = NETDATA_OS_TYPE;
const char *program_version = VERSION;

// ----------------------------------------------------------------------------
// memory allocation functions that handle failures

// although netdata does not use memory allocations too often (netdata tries to
// maintain its memory footprint stable during runtime, i.e. all buffers are
// allocated during initialization and are adapted to current use throughout
// its lifetime), these can be used to override the default system allocation
// routines.

#ifdef NETDATA_LOG_ALLOCATIONS
static struct memory_statistics {
    volatile size_t malloc_calls_made;
    volatile size_t calloc_calls_made;
    volatile size_t realloc_calls_made;
    volatile size_t strdup_calls_made;
    volatile size_t free_calls_made;
    volatile size_t memory_calls_made;
    volatile size_t allocated_memory;
    volatile size_t mmapped_memory;
} memory_statistics;

static inline void print_allocations(const char *file, const char *function, const unsigned long line) {
    static struct memory_statistics old = { 0, 0, 0, 0, 0, 0, 0, 0 };

    //if(unlikely(!(memory_statistics.memory_calls_made % 5))) {
        fprintf(stderr, "(%04lu@%-10.10s:%-15.15s): Allocated %zu KB (+%zu B), mmapped %zu KB (+%zu B): malloc %zu (+%zu), calloc %zu (+%zu), realloc %zu (+%zu), strdup %zu (+%zu), free %zu (+%zu)\n",
                line, file, function,
                (memory_statistics.allocated_memory + 512) / 1024, memory_statistics.allocated_memory - old.allocated_memory,
                (memory_statistics.mmapped_memory + 512) / 1024, memory_statistics.mmapped_memory - old.mmapped_memory,
                memory_statistics.malloc_calls_made, memory_statistics.malloc_calls_made - old.malloc_calls_made,
                memory_statistics.calloc_calls_made, memory_statistics.calloc_calls_made - old.calloc_calls_made,
                memory_statistics.realloc_calls_made, memory_statistics.realloc_calls_made - old.realloc_calls_made,
                memory_statistics.strdup_calls_made, memory_statistics.strdup_calls_made - old.strdup_calls_made,
                memory_statistics.free_calls_made, memory_statistics.free_calls_made - old.free_calls_made
        );

        memcpy(&old, &memory_statistics, sizeof(struct memory_statistics));
    //}
}

static inline void malloc_accounting(const char *file, const char *function, const unsigned long line, size_t size) {
#if defined(HAVE_C___ATOMIC) && !defined(NETDATA_NO_ATOMIC_INSTRUCTIONS)
    __atomic_fetch_add(&memory_statistics.memory_calls_made, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&memory_statistics.malloc_calls_made, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&memory_statistics.allocated_memory, size, __ATOMIC_SEQ_CST);
#else
    // this is for debugging - we don't care locking it
    memory_statistics.memory_calls_made++;
    memory_statistics.malloc_calls_made++;
    memory_statistics.allocated_memory += size;
#endif
    print_allocations(file, function, line);
}

static inline void mmap_accounting(size_t size) {
#if defined(HAVE_C___ATOMIC) && !defined(NETDATA_NO_ATOMIC_INSTRUCTIONS)
    __atomic_fetch_add(&memory_statistics.malloc_calls_made, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&memory_statistics.mmapped_memory, size, __ATOMIC_SEQ_CST);
#else
    // this is for debugging - we don't care locking it
    memory_statistics.memory_calls_made++;
    memory_statistics.mmapped_memory += size;
#endif
}

static inline void calloc_accounting(const char *file, const char *function, const unsigned long line, size_t size) {
#if defined(HAVE_C___ATOMIC) && !defined(NETDATA_NO_ATOMIC_INSTRUCTIONS)
    __atomic_fetch_add(&memory_statistics.memory_calls_made, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&memory_statistics.calloc_calls_made, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&memory_statistics.allocated_memory, size, __ATOMIC_SEQ_CST);
#else
    // this is for debugging - we don't care locking it
    memory_statistics.memory_calls_made++;
    memory_statistics.calloc_calls_made++;
    memory_statistics.allocated_memory += size;
#endif
    print_allocations(file, function, line);
}

static inline void realloc_accounting(const char *file, const char *function, const unsigned long line, void *ptr, size_t size) {
    (void)ptr;

#if defined(HAVE_C___ATOMIC) && !defined(NETDATA_NO_ATOMIC_INSTRUCTIONS)
    __atomic_fetch_add(&memory_statistics.memory_calls_made, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&memory_statistics.realloc_calls_made, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&memory_statistics.allocated_memory, size, __ATOMIC_SEQ_CST);
#else
    // this is for debugging - we don't care locking it
    memory_statistics.memory_calls_made++;
    memory_statistics.realloc_calls_made++;
    memory_statistics.allocated_memory += size;
#endif
    print_allocations(file, function, line);
}

static inline void strdup_accounting(const char *file, const char *function, const unsigned long line, const char *s) {
    size_t size = strlen(s) + 1;

#if defined(HAVE_C___ATOMIC) && !defined(NETDATA_NO_ATOMIC_INSTRUCTIONS)
    __atomic_fetch_add(&memory_statistics.memory_calls_made, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&memory_statistics.strdup_calls_made, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&memory_statistics.allocated_memory, size, __ATOMIC_SEQ_CST);
#else
    // this is for debugging - we don't care locking it
    memory_statistics.memory_calls_made++;
    memory_statistics.strdup_calls_made++;
    memory_statistics.allocated_memory += size;
#endif
    print_allocations(file, function, line);
}

static inline void free_accounting(const char *file, const char *function, const unsigned long line, void *ptr) {
    (void)file;
    (void)function;
    (void)line;

    if(likely(ptr)) {
#if defined(HAVE_C___ATOMIC) && !defined(NETDATA_NO_ATOMIC_INSTRUCTIONS)
        __atomic_fetch_add(&memory_statistics.memory_calls_made, 1, __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&memory_statistics.free_calls_made, 1, __ATOMIC_SEQ_CST);
#else
        // this is for debugging - we don't care locking it
        memory_statistics.memory_calls_made++;
        memory_statistics.free_calls_made++;
#endif
    }
}
#endif

#ifdef NETDATA_LOG_ALLOCATIONS
char *strdupz_int(const char *file, const char *function, const unsigned long line, const char *s) {
    strdup_accounting(file, function, line, s);
#else
char *strdupz(const char *s) {
#endif

    char *t = strdup(s);
    if (unlikely(!t)) fatal("Cannot strdup() string '%s'", s);
    return t;
}

#ifdef NETDATA_LOG_ALLOCATIONS
void *mallocz_int(const char *file, const char *function, const unsigned long line, size_t size) {
    malloc_accounting(file, function, line, size);
#else
void *mallocz(size_t size) {
#endif

    void *p = malloc(size);
    if (unlikely(!p)) fatal("Cannot allocate %zu bytes of memory.", size);
    return p;
}

#ifdef NETDATA_LOG_ALLOCATIONS
void *callocz_int(const char *file, const char *function, const unsigned long line, size_t nmemb, size_t size) {
    calloc_accounting(file, function, line, nmemb * size);
#else
void *callocz(size_t nmemb, size_t size) {
#endif

    void *p = calloc(nmemb, size);
    if (unlikely(!p)) fatal("Cannot allocate %zu bytes of memory.", nmemb * size);
    return p;
}

#ifdef NETDATA_LOG_ALLOCATIONS
void *reallocz_int(const char *file, const char *function, const unsigned long line, void *ptr, size_t size) {
    realloc_accounting(file, function, line, ptr, size);
#else
void *reallocz(void *ptr, size_t size) {
#endif

    void *p = realloc(ptr, size);
    if (unlikely(!p)) fatal("Cannot re-allocate memory to %zu bytes.", size);
    return p;
}

#ifdef NETDATA_LOG_ALLOCATIONS
void freez_int(const char *file, const char *function, const unsigned long line, void *ptr) {
    free_accounting(file, function, line, ptr);
#else
void freez(void *ptr) {
#endif

    free(ptr);
}

void json_escape_string(char *dst, const char *src, size_t size) {
    const char *t;
    char *d = dst, *e = &dst[size - 1];

    for(t = src; *t && d < e ;t++) {
        if(unlikely(*t == '\\' || *t == '"')) {
            if(unlikely(d + 1 >= e)) break;
            *d++ = '\\';
        }
        *d++ = *t;
    }

    *d = '\0';
}

void json_fix_string(char *s) {
    unsigned char c;
    while((c = (unsigned char)*s)) {
        if(unlikely(c == '\\'))
            *s++ = '/';
        else if(unlikely(c == '"'))
            *s++ = '\'';
        else if(unlikely(isspace(c) || iscntrl(c)))
            *s++ = ' ';
        else if(unlikely(!isprint(c) || c > 127))
            *s++ = '_';
        else
            s++;
    }
}

int sleep_usec(usec_t usec) {

#ifndef NETDATA_WITH_USLEEP
    // we expect microseconds (1.000.000 per second)
    // but timespec is nanoseconds (1.000.000.000 per second)
    struct timespec rem, req = {
            .tv_sec = (time_t) (usec / 1000000),
            .tv_nsec = (suseconds_t) ((usec % 1000000) * 1000)
    };

    while (nanosleep(&req, &rem) == -1) {
        if (likely(errno == EINTR)) {
            debug(D_SYSTEM, "nanosleep() interrupted (while sleeping for %llu microseconds).", usec);
            req.tv_sec = rem.tv_sec;
            req.tv_nsec = rem.tv_nsec;
        } else {
            error("Cannot nanosleep() for %llu microseconds.", usec);
            break;
        }
    }

    return 0;
#else
    int ret = usleep(usec);
    if(unlikely(ret == -1 && errno == EINVAL)) {
        // on certain systems, usec has to be up to 999999
        if(usec > 999999) {
            int counter = usec / 999999;
            while(counter--)
                usleep(999999);

            usleep(usec % 999999);
        }
        else {
            error("Cannot usleep() for %llu microseconds.", usec);
            return ret;
        }
    }

    if(ret != 0)
        error("usleep() failed for %llu microseconds.", usec);

    return ret;
#endif
}

unsigned char netdata_map_chart_names[256] = {
        [0] = '\0', //
        [1] = '_', //
        [2] = '_', //
        [3] = '_', //
        [4] = '_', //
        [5] = '_', //
        [6] = '_', //
        [7] = '_', //
        [8] = '_', //
        [9] = '_', //
        [10] = '_', //
        [11] = '_', //
        [12] = '_', //
        [13] = '_', //
        [14] = '_', //
        [15] = '_', //
        [16] = '_', //
        [17] = '_', //
        [18] = '_', //
        [19] = '_', //
        [20] = '_', //
        [21] = '_', //
        [22] = '_', //
        [23] = '_', //
        [24] = '_', //
        [25] = '_', //
        [26] = '_', //
        [27] = '_', //
        [28] = '_', //
        [29] = '_', //
        [30] = '_', //
        [31] = '_', //
        [32] = '_', //
        [33] = '_', // !
        [34] = '_', // "
        [35] = '_', // #
        [36] = '_', // $
        [37] = '_', // %
        [38] = '_', // &
        [39] = '_', // '
        [40] = '_', // (
        [41] = '_', // )
        [42] = '_', // *
        [43] = '_', // +
        [44] = '.', // ,
        [45] = '-', // -
        [46] = '.', // .
        [47] = '/', // /
        [48] = '0', // 0
        [49] = '1', // 1
        [50] = '2', // 2
        [51] = '3', // 3
        [52] = '4', // 4
        [53] = '5', // 5
        [54] = '6', // 6
        [55] = '7', // 7
        [56] = '8', // 8
        [57] = '9', // 9
        [58] = '_', // :
        [59] = '_', // ;
        [60] = '_', // <
        [61] = '_', // =
        [62] = '_', // >
        [63] = '_', // ?
        [64] = '_', // @
        [65] = 'a', // A
        [66] = 'b', // B
        [67] = 'c', // C
        [68] = 'd', // D
        [69] = 'e', // E
        [70] = 'f', // F
        [71] = 'g', // G
        [72] = 'h', // H
        [73] = 'i', // I
        [74] = 'j', // J
        [75] = 'k', // K
        [76] = 'l', // L
        [77] = 'm', // M
        [78] = 'n', // N
        [79] = 'o', // O
        [80] = 'p', // P
        [81] = 'q', // Q
        [82] = 'r', // R
        [83] = 's', // S
        [84] = 't', // T
        [85] = 'u', // U
        [86] = 'v', // V
        [87] = 'w', // W
        [88] = 'x', // X
        [89] = 'y', // Y
        [90] = 'z', // Z
        [91] = '_', // [
        [92] = '/', // backslash
        [93] = '_', // ]
        [94] = '_', // ^
        [95] = '_', // _
        [96] = '_', // `
        [97] = 'a', // a
        [98] = 'b', // b
        [99] = 'c', // c
        [100] = 'd', // d
        [101] = 'e', // e
        [102] = 'f', // f
        [103] = 'g', // g
        [104] = 'h', // h
        [105] = 'i', // i
        [106] = 'j', // j
        [107] = 'k', // k
        [108] = 'l', // l
        [109] = 'm', // m
        [110] = 'n', // n
        [111] = 'o', // o
        [112] = 'p', // p
        [113] = 'q', // q
        [114] = 'r', // r
        [115] = 's', // s
        [116] = 't', // t
        [117] = 'u', // u
        [118] = 'v', // v
        [119] = 'w', // w
        [120] = 'x', // x
        [121] = 'y', // y
        [122] = 'z', // z
        [123] = '_', // {
        [124] = '_', // |
        [125] = '_', // }
        [126] = '_', // ~
        [127] = '_', //
        [128] = '_', //
        [129] = '_', //
        [130] = '_', //
        [131] = '_', //
        [132] = '_', //
        [133] = '_', //
        [134] = '_', //
        [135] = '_', //
        [136] = '_', //
        [137] = '_', //
        [138] = '_', //
        [139] = '_', //
        [140] = '_', //
        [141] = '_', //
        [142] = '_', //
        [143] = '_', //
        [144] = '_', //
        [145] = '_', //
        [146] = '_', //
        [147] = '_', //
        [148] = '_', //
        [149] = '_', //
        [150] = '_', //
        [151] = '_', //
        [152] = '_', //
        [153] = '_', //
        [154] = '_', //
        [155] = '_', //
        [156] = '_', //
        [157] = '_', //
        [158] = '_', //
        [159] = '_', //
        [160] = '_', //
        [161] = '_', //
        [162] = '_', //
        [163] = '_', //
        [164] = '_', //
        [165] = '_', //
        [166] = '_', //
        [167] = '_', //
        [168] = '_', //
        [169] = '_', //
        [170] = '_', //
        [171] = '_', //
        [172] = '_', //
        [173] = '_', //
        [174] = '_', //
        [175] = '_', //
        [176] = '_', //
        [177] = '_', //
        [178] = '_', //
        [179] = '_', //
        [180] = '_', //
        [181] = '_', //
        [182] = '_', //
        [183] = '_', //
        [184] = '_', //
        [185] = '_', //
        [186] = '_', //
        [187] = '_', //
        [188] = '_', //
        [189] = '_', //
        [190] = '_', //
        [191] = '_', //
        [192] = '_', //
        [193] = '_', //
        [194] = '_', //
        [195] = '_', //
        [196] = '_', //
        [197] = '_', //
        [198] = '_', //
        [199] = '_', //
        [200] = '_', //
        [201] = '_', //
        [202] = '_', //
        [203] = '_', //
        [204] = '_', //
        [205] = '_', //
        [206] = '_', //
        [207] = '_', //
        [208] = '_', //
        [209] = '_', //
        [210] = '_', //
        [211] = '_', //
        [212] = '_', //
        [213] = '_', //
        [214] = '_', //
        [215] = '_', //
        [216] = '_', //
        [217] = '_', //
        [218] = '_', //
        [219] = '_', //
        [220] = '_', //
        [221] = '_', //
        [222] = '_', //
        [223] = '_', //
        [224] = '_', //
        [225] = '_', //
        [226] = '_', //
        [227] = '_', //
        [228] = '_', //
        [229] = '_', //
        [230] = '_', //
        [231] = '_', //
        [232] = '_', //
        [233] = '_', //
        [234] = '_', //
        [235] = '_', //
        [236] = '_', //
        [237] = '_', //
        [238] = '_', //
        [239] = '_', //
        [240] = '_', //
        [241] = '_', //
        [242] = '_', //
        [243] = '_', //
        [244] = '_', //
        [245] = '_', //
        [246] = '_', //
        [247] = '_', //
        [248] = '_', //
        [249] = '_', //
        [250] = '_', //
        [251] = '_', //
        [252] = '_', //
        [253] = '_', //
        [254] = '_', //
        [255] = '_'  //
};

// make sure the supplied string
// is good for a netdata chart/dimension ID/NAME
void netdata_fix_chart_name(char *s) {
    while ((*s = netdata_map_chart_names[(unsigned char) *s])) s++;
}

unsigned char netdata_map_chart_ids[256] = {
        [0] = '\0', //
        [1] = '_', //
        [2] = '_', //
        [3] = '_', //
        [4] = '_', //
        [5] = '_', //
        [6] = '_', //
        [7] = '_', //
        [8] = '_', //
        [9] = '_', //
        [10] = '_', //
        [11] = '_', //
        [12] = '_', //
        [13] = '_', //
        [14] = '_', //
        [15] = '_', //
        [16] = '_', //
        [17] = '_', //
        [18] = '_', //
        [19] = '_', //
        [20] = '_', //
        [21] = '_', //
        [22] = '_', //
        [23] = '_', //
        [24] = '_', //
        [25] = '_', //
        [26] = '_', //
        [27] = '_', //
        [28] = '_', //
        [29] = '_', //
        [30] = '_', //
        [31] = '_', //
        [32] = '_', //
        [33] = '_', // !
        [34] = '_', // "
        [35] = '_', // #
        [36] = '_', // $
        [37] = '_', // %
        [38] = '_', // &
        [39] = '_', // '
        [40] = '_', // (
        [41] = '_', // )
        [42] = '_', // *
        [43] = '_', // +
        [44] = '.', // ,
        [45] = '-', // -
        [46] = '.', // .
        [47] = '_', // /
        [48] = '0', // 0
        [49] = '1', // 1
        [50] = '2', // 2
        [51] = '3', // 3
        [52] = '4', // 4
        [53] = '5', // 5
        [54] = '6', // 6
        [55] = '7', // 7
        [56] = '8', // 8
        [57] = '9', // 9
        [58] = '_', // :
        [59] = '_', // ;
        [60] = '_', // <
        [61] = '_', // =
        [62] = '_', // >
        [63] = '_', // ?
        [64] = '_', // @
        [65] = 'a', // A
        [66] = 'b', // B
        [67] = 'c', // C
        [68] = 'd', // D
        [69] = 'e', // E
        [70] = 'f', // F
        [71] = 'g', // G
        [72] = 'h', // H
        [73] = 'i', // I
        [74] = 'j', // J
        [75] = 'k', // K
        [76] = 'l', // L
        [77] = 'm', // M
        [78] = 'n', // N
        [79] = 'o', // O
        [80] = 'p', // P
        [81] = 'q', // Q
        [82] = 'r', // R
        [83] = 's', // S
        [84] = 't', // T
        [85] = 'u', // U
        [86] = 'v', // V
        [87] = 'w', // W
        [88] = 'x', // X
        [89] = 'y', // Y
        [90] = 'z', // Z
        [91] = '_', // [
        [92] = '/', // backslash
        [93] = '_', // ]
        [94] = '_', // ^
        [95] = '_', // _
        [96] = '_', // `
        [97] = 'a', // a
        [98] = 'b', // b
        [99] = 'c', // c
        [100] = 'd', // d
        [101] = 'e', // e
        [102] = 'f', // f
        [103] = 'g', // g
        [104] = 'h', // h
        [105] = 'i', // i
        [106] = 'j', // j
        [107] = 'k', // k
        [108] = 'l', // l
        [109] = 'm', // m
        [110] = 'n', // n
        [111] = 'o', // o
        [112] = 'p', // p
        [113] = 'q', // q
        [114] = 'r', // r
        [115] = 's', // s
        [116] = 't', // t
        [117] = 'u', // u
        [118] = 'v', // v
        [119] = 'w', // w
        [120] = 'x', // x
        [121] = 'y', // y
        [122] = 'z', // z
        [123] = '_', // {
        [124] = '_', // |
        [125] = '_', // }
        [126] = '_', // ~
        [127] = '_', //
        [128] = '_', //
        [129] = '_', //
        [130] = '_', //
        [131] = '_', //
        [132] = '_', //
        [133] = '_', //
        [134] = '_', //
        [135] = '_', //
        [136] = '_', //
        [137] = '_', //
        [138] = '_', //
        [139] = '_', //
        [140] = '_', //
        [141] = '_', //
        [142] = '_', //
        [143] = '_', //
        [144] = '_', //
        [145] = '_', //
        [146] = '_', //
        [147] = '_', //
        [148] = '_', //
        [149] = '_', //
        [150] = '_', //
        [151] = '_', //
        [152] = '_', //
        [153] = '_', //
        [154] = '_', //
        [155] = '_', //
        [156] = '_', //
        [157] = '_', //
        [158] = '_', //
        [159] = '_', //
        [160] = '_', //
        [161] = '_', //
        [162] = '_', //
        [163] = '_', //
        [164] = '_', //
        [165] = '_', //
        [166] = '_', //
        [167] = '_', //
        [168] = '_', //
        [169] = '_', //
        [170] = '_', //
        [171] = '_', //
        [172] = '_', //
        [173] = '_', //
        [174] = '_', //
        [175] = '_', //
        [176] = '_', //
        [177] = '_', //
        [178] = '_', //
        [179] = '_', //
        [180] = '_', //
        [181] = '_', //
        [182] = '_', //
        [183] = '_', //
        [184] = '_', //
        [185] = '_', //
        [186] = '_', //
        [187] = '_', //
        [188] = '_', //
        [189] = '_', //
        [190] = '_', //
        [191] = '_', //
        [192] = '_', //
        [193] = '_', //
        [194] = '_', //
        [195] = '_', //
        [196] = '_', //
        [197] = '_', //
        [198] = '_', //
        [199] = '_', //
        [200] = '_', //
        [201] = '_', //
        [202] = '_', //
        [203] = '_', //
        [204] = '_', //
        [205] = '_', //
        [206] = '_', //
        [207] = '_', //
        [208] = '_', //
        [209] = '_', //
        [210] = '_', //
        [211] = '_', //
        [212] = '_', //
        [213] = '_', //
        [214] = '_', //
        [215] = '_', //
        [216] = '_', //
        [217] = '_', //
        [218] = '_', //
        [219] = '_', //
        [220] = '_', //
        [221] = '_', //
        [222] = '_', //
        [223] = '_', //
        [224] = '_', //
        [225] = '_', //
        [226] = '_', //
        [227] = '_', //
        [228] = '_', //
        [229] = '_', //
        [230] = '_', //
        [231] = '_', //
        [232] = '_', //
        [233] = '_', //
        [234] = '_', //
        [235] = '_', //
        [236] = '_', //
        [237] = '_', //
        [238] = '_', //
        [239] = '_', //
        [240] = '_', //
        [241] = '_', //
        [242] = '_', //
        [243] = '_', //
        [244] = '_', //
        [245] = '_', //
        [246] = '_', //
        [247] = '_', //
        [248] = '_', //
        [249] = '_', //
        [250] = '_', //
        [251] = '_', //
        [252] = '_', //
        [253] = '_', //
        [254] = '_', //
        [255] = '_'  //
};

// make sure the supplied string
// is good for a netdata chart/dimension ID/NAME
void netdata_fix_chart_id(char *s) {
    while ((*s = netdata_map_chart_ids[(unsigned char) *s])) s++;
}

/*
// http://stackoverflow.com/questions/7666509/hash-function-for-string
uint32_t simple_hash(const char *name)
{
    const char *s = name;
    uint32_t hash = 5381;
    int i;

    while((i = *s++)) hash = ((hash << 5) + hash) + i;

    // fprintf(stderr, "HASH: %lu %s\n", hash, name);

    return hash;
}
*/

/*
// http://isthe.com/chongo/tech/comp/fnv/#FNV-1a
uint32_t simple_hash(const char *name) {
    unsigned char *s = (unsigned char *) name;
    uint32_t hval = 0x811c9dc5;

    // FNV-1a algorithm
    while (*s) {
        // multiply by the 32 bit FNV magic prime mod 2^32
        // NOTE: No need to optimize with left shifts.
        //       GCC will use imul instruction anyway.
        //       Tested with 'gcc -O3 -S'
        //hval += (hval<<1) + (hval<<4) + (hval<<7) + (hval<<8) + (hval<<24);
        hval *= 16777619;

        // xor the bottom with the current octet
        hval ^= (uint32_t) *s++;
    }

    // fprintf(stderr, "HASH: %u = %s\n", hval, name);
    return hval;
}

uint32_t simple_uhash(const char *name) {
    unsigned char *s = (unsigned char *) name;
    uint32_t hval = 0x811c9dc5, c;

    // FNV-1a algorithm
    while ((c = *s++)) {
        if (unlikely(c >= 'A' && c <= 'Z')) c += 'a' - 'A';
        hval *= 16777619;
        hval ^= c;
    }
    return hval;
}
*/

/*
// http://eternallyconfuzzled.com/tuts/algorithms/jsw_tut_hashing.aspx
// one at a time hash
uint32_t simple_hash(const char *name) {
    unsigned char *s = (unsigned char *)name;
    uint32_t h = 0;

    while(*s) {
        h += *s++;
        h += (h << 10);
        h ^= (h >> 6);
    }

    h += (h << 3);
    h ^= (h >> 11);
    h += (h << 15);

    // fprintf(stderr, "HASH: %u = %s\n", h, name);

    return h;
}
*/

void strreverse(char *begin, char *end) {
    while (end > begin) {
        // clearer code.
        char aux = *end;
        *end-- = *begin;
        *begin++ = aux;
    }
}

char *mystrsep(char **ptr, char *s) {
    char *p = "";
    while (p && !p[0] && *ptr) p = strsep(ptr, s);
    return (p);
}

char *trim(char *s) {
    // skip leading spaces
    while (*s && isspace(*s)) s++;
    if (!*s) return NULL;

    // skip tailing spaces
    // this way is way faster. Writes only one NUL char.
    ssize_t l = strlen(s);
    if (--l >= 0) {
        char *p = s + l;
        while (p > s && isspace(*p)) p--;
        *++p = '\0';
    }

    if (!*s) return NULL;

    return s;
}

inline char *trim_all(char *buffer) {
    char *d = buffer, *s = buffer;

    // skip spaces
    while(isspace(*s)) s++;

    while(*s) {
        // copy the non-space part
        while(*s && !isspace(*s)) *d++ = *s++;

        // add a space if we have to
        if(*s && isspace(*s)) {
            *d++ = ' ';
            s++;
        }

        // skip spaces
        while(isspace(*s)) s++;
    }

    *d = '\0';

    if(d > buffer) {
        d--;
        if(isspace(*d)) *d = '\0';
    }

    if(!buffer[0]) return NULL;
    return buffer;
}

static int memory_file_open(const char *filename, size_t size) {
    // info("memory_file_open('%s', %zu", filename, size);

    int fd = open(filename, O_RDWR | O_CREAT | O_NOATIME, 0664);
    if (fd != -1) {
        if (lseek(fd, size, SEEK_SET) == (off_t) size) {
            if (write(fd, "", 1) == 1) {
                if (ftruncate(fd, size))
                    error("Cannot truncate file '%s' to size %zu. Will use the larger file.", filename, size);
            }
            else error("Cannot write to file '%s' at position %zu.", filename, size);
        }
        else error("Cannot seek file '%s' to size %zu.", filename, size);
    }
    else error("Cannot create/open file '%s'.", filename);

    return fd;
}

// mmap_shared is used for memory mode = map
static void *memory_file_mmap(const char *filename, size_t size, int flags) {
    // info("memory_file_mmap('%s', %zu", filename, size);
    static int log_madvise = 1;

    int fd = -1;
    if(filename) {
        fd = memory_file_open(filename, size);
        if(fd == -1) return MAP_FAILED;
    }

    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, fd, 0);
    if (mem != MAP_FAILED) {
#ifdef NETDATA_LOG_ALLOCATIONS
        mmap_accounting(size);
#endif
        int advise = MADV_SEQUENTIAL | MADV_DONTFORK;
        if (flags & MAP_SHARED) advise |= MADV_WILLNEED;

        if (madvise(mem, size, advise) != 0 && log_madvise) {
            error("Cannot advise the kernel about shared memory usage.");
            log_madvise--;
        }
    }

    if(fd != -1)
        close(fd);

    return mem;
}

#ifdef MADV_MERGEABLE
static void *memory_file_mmap_ksm(const char *filename, size_t size, int flags) {
    // info("memory_file_mmap_ksm('%s', %zu", filename, size);
    static int log_madvise_2 = 1, log_madvise_3 = 1;

    int fd = -1;
    if(filename) {
        fd = memory_file_open(filename, size);
        if(fd == -1) return MAP_FAILED;
    }

    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, flags | MAP_ANONYMOUS, -1, 0);
    if (mem != MAP_FAILED) {
#ifdef NETDATA_LOG_ALLOCATIONS
        mmap_accounting(size);
#endif
        if(fd != -1) {
            if (lseek(fd, 0, SEEK_SET) == 0) {
                if (read(fd, mem, size) != (ssize_t) size)
                    error("Cannot read from file '%s'", filename);
            }
            else error("Cannot seek to beginning of file '%s'.", filename);
        }

        // don't use MADV_SEQUENTIAL|MADV_DONTFORK, they disable MADV_MERGEABLE
        if (madvise(mem, size, MADV_SEQUENTIAL | MADV_DONTFORK) != 0 && log_madvise_2) {
            error("Cannot advise the kernel about the memory usage (MADV_SEQUENTIAL|MADV_DONTFORK) of file '%s'.", filename);
            log_madvise_2--;
        }

        if (madvise(mem, size, MADV_MERGEABLE) != 0 && log_madvise_3) {
            error("Cannot advise the kernel about the memory usage (MADV_MERGEABLE) of file '%s'.", filename);
            log_madvise_3--;
        }
    }

    if(fd != -1)
        close(fd);

    return mem;
}
#else
static void *memory_file_mmap_ksm(const char *filename, size_t size, int flags) {
    // info("memory_file_mmap_ksm FALLBACK ('%s', %zu", filename, size);

    if(filename)
        return memory_file_mmap(filename, size, flags);

    // when KSM is not available and no filename is given (memory mode = ram),
    // we just report failure
    return MAP_FAILED;
}
#endif

void *mymmap(const char *filename, size_t size, int flags, int ksm) {
    void *mem = NULL;

    if (filename && (flags & MAP_SHARED || !enable_ksm || !ksm))
        // memory mode = map | save
        // when KSM is not enabled
        // MAP_SHARED is used for memory mode = map (no KSM possible)
        mem = memory_file_mmap(filename, size, flags);

    else
        // memory mode = save | ram
        // when KSM is enabled
        // for memory mode = ram, the filename is NULL
        mem = memory_file_mmap_ksm(filename, size, flags);

    if(mem == MAP_FAILED) return NULL;

    errno = 0;
    return mem;
}

int memory_file_save(const char *filename, void *mem, size_t size) {
    char tmpfilename[FILENAME_MAX + 1];

    snprintfz(tmpfilename, FILENAME_MAX, "%s.%ld.tmp", filename, (long) getpid());

    int fd = open(tmpfilename, O_RDWR | O_CREAT | O_NOATIME, 0664);
    if (fd < 0) {
        error("Cannot create/open file '%s'.", filename);
        return -1;
    }

    if (write(fd, mem, size) != (ssize_t) size) {
        error("Cannot write to file '%s' %ld bytes.", filename, (long) size);
        close(fd);
        return -1;
    }

    close(fd);

    if (rename(tmpfilename, filename)) {
        error("Cannot rename '%s' to '%s'", tmpfilename, filename);
        return -1;
    }

    return 0;
}

int fd_is_valid(int fd) {
    return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

pid_t gettid(void) {
#ifdef __FreeBSD__
    return (pid_t)pthread_getthreadid_np();
#elif defined(__APPLE__)
    uint64_t curthreadid;
    pthread_threadid_np(NULL, &curthreadid);
    return (pid_t)curthreadid;
#else
    return (pid_t)syscall(SYS_gettid);
#endif /* __FreeBSD__, __APPLE__*/
}

char *fgets_trim_len(char *buf, size_t buf_size, FILE *fp, size_t *len) {
    char *s = fgets(buf, (int)buf_size, fp);
    if (!s) return NULL;

    char *t = s;
    if (*t != '\0') {
        // find the string end
        while (*++t != '\0');

        // trim trailing spaces/newlines/tabs
        while (--t > s && *t == '\n')
            *t = '\0';
    }

    if (len)
        *len = t - s + 1;

    return s;
}

int vsnprintfz(char *dst, size_t n, const char *fmt, va_list args) {
    int size = vsnprintf(dst, n, fmt, args);

    if (unlikely((size_t) size > n)) {
        // truncated
        size = (int)n;
    }

    dst[size] = '\0';
    return size;
}

int snprintfz(char *dst, size_t n, const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    int ret = vsnprintfz(dst, n, fmt, args);
    va_end(args);

    return ret;
}

// ----------------------------------------------------------------------------
// system functions
// to retrieve settings of the system

int processors = 1;
long get_system_cpus(void) {
    processors = 1;

    #ifdef __APPLE__
        int32_t tmp_processors;

        if (unlikely(GETSYSCTL_BY_NAME("hw.logicalcpu", tmp_processors))) {
            error("Assuming system has %d processors.", processors);
        } else {
            processors = tmp_processors;
        }

        return processors;
    #elif __FreeBSD__
        int32_t tmp_processors;

        if (unlikely(GETSYSCTL_BY_NAME("hw.ncpu", tmp_processors))) {
            error("Assuming system has %d processors.", processors);
        } else {
            processors = tmp_processors;
        }

        return processors;
    #else

    char filename[FILENAME_MAX + 1];
    snprintfz(filename, FILENAME_MAX, "%s/proc/stat", netdata_configured_host_prefix);

    procfile *ff = procfile_open(filename, NULL, PROCFILE_FLAG_DEFAULT);
    if(!ff) {
        error("Cannot open file '%s'. Assuming system has %d processors.", filename, processors);
        return processors;
    }

    ff = procfile_readall(ff);
    if(!ff) {
        error("Cannot open file '%s'. Assuming system has %d processors.", filename, processors);
        return processors;
    }

    processors = 0;
    unsigned int i;
    for(i = 0; i < procfile_lines(ff); i++) {
        if(!procfile_linewords(ff, i)) continue;

        if(strncmp(procfile_lineword(ff, i, 0), "cpu", 3) == 0) processors++;
    }
    processors--;
    if(processors < 1) processors = 1;

    procfile_close(ff);

    debug(D_SYSTEM, "System has %d processors.", processors);
    return processors;

    #endif /* __APPLE__, __FreeBSD__ */
}

pid_t pid_max = 32768;
pid_t get_system_pid_max(void) {
    #ifdef __APPLE__
        // As we currently do not know a solution to query pid_max from the os
        // we use the number defined in bsd/sys/proc_internal.h in XNU sources
        pid_max = 99999;
        return pid_max;
    #elif __FreeBSD__
        int32_t tmp_pid_max;

        if (unlikely(GETSYSCTL_BY_NAME("kern.pid_max", tmp_pid_max))) {
            pid_max = 99999;
            error("Assuming system's maximum pid is %d.", pid_max);
        } else {
            pid_max = tmp_pid_max;
        }

        return pid_max;
    #else

    static char read = 0;
    if(unlikely(read)) return pid_max;
    read = 1;

    char filename[FILENAME_MAX + 1];
    snprintfz(filename, FILENAME_MAX, "%s/proc/sys/kernel/pid_max", netdata_configured_host_prefix);

    unsigned long long max = 0;
    if(read_single_number_file(filename, &max) != 0) {
        error("Cannot open file '%s'. Assuming system supports %d pids.", filename, pid_max);
        return pid_max;
    }

    if(!max) {
        error("Cannot parse file '%s'. Assuming system supports %d pids.", filename, pid_max);
        return pid_max;
    }

    pid_max = (pid_t) max;
    return pid_max;

    #endif /* __APPLE__, __FreeBSD__ */
}

unsigned int hz;
void get_system_HZ(void) {
    long ticks;

    if ((ticks = sysconf(_SC_CLK_TCK)) == -1) {
        error("Cannot get system clock ticks");
    }

    hz = (unsigned int) ticks;
}

/*
// poor man cycle counting
static unsigned long tsc;
void begin_tsc(void) {
    unsigned long a, d;
    asm volatile ("cpuid\nrdtsc" : "=a" (a), "=d" (d) : "0" (0) : "ebx", "ecx");
    tsc = ((unsigned long)d << 32) | (unsigned long)a;
}
unsigned long end_tsc(void) {
    unsigned long a, d;
    asm volatile ("rdtscp" : "=a" (a), "=d" (d) : : "ecx");
    return (((unsigned long)d << 32) | (unsigned long)a) - tsc;
}
*/

int recursively_delete_dir(const char *path, const char *reason) {
    DIR *dir = opendir(path);
    if(!dir) {
        error("Cannot read %s directory to be deleted '%s'", reason?reason:"", path);
        return -1;
    }

    int ret = 0;
    struct dirent *de = NULL;
    while((de = readdir(dir))) {
        if(de->d_type == DT_DIR
           && (
                   (de->d_name[0] == '.' && de->d_name[1] == '\0')
                   || (de->d_name[0] == '.' && de->d_name[1] == '.' && de->d_name[2] == '\0')
           ))
            continue;

        char fullpath[FILENAME_MAX + 1];
        snprintfz(fullpath, FILENAME_MAX, "%s/%s", path, de->d_name);

        if(de->d_type == DT_DIR) {
            int r = recursively_delete_dir(fullpath, reason);
            if(r > 0) ret += r;
            continue;
        }

        info("Deleting %s file '%s'", reason?reason:"", fullpath);
        if(unlikely(unlink(fullpath) == -1))
            error("Cannot delete %s file '%s'", reason?reason:"", fullpath);
        else
            ret++;
    }

    info("Deleting empty directory '%s'", path);
    if(unlikely(rmdir(path) == -1))
        error("Cannot delete empty directory '%s'", path);
    else
        ret++;

    closedir(dir);

    return ret;
}

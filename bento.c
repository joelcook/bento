/* bento -- A very simple editor in less than 1-bento lines of code (as counted
 *         by "cloc"). Does not depend on libcurses, directly emits VT100
 *         escapes on the terminal.
 *
 */

#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif

#ifndef bento_VERSION
#define bento_VERSION "0.0.1"
#endif

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define getcwd _getcwd
#define access _access
#ifndef R_OK
#define R_OK 4
#endif
#else
#include <dirent.h>
#include <unistd.h>
#endif

#ifndef bento_RAYLIB
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#endif

#ifdef bento_RAYLIB
#include "raylib.h"
/* rlgl.h gives us the low-level batched-quads API (rlBegin / rlVertex2f
 * / rlSetTexture / RL_QUADS) used by the GUI text batcher. */
#include "rlgl.h"
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Syntax highlight types */
#define HL_NORMAL 0
#define HL_NONPRINT 1
#define HL_COMMENT 2   /* Single line comment. */
#define HL_MLCOMMENT 3 /* Multi-line comment. */
#define HL_KEYWORD1 4
#define HL_KEYWORD2 5
#define HL_STRING 6
#define HL_NUMBER 7
#define HL_MATCH 8 /* Search match. */
#define HL_FUNCTION 9

#define HL_HIGHLIGHT_STRINGS (1 << 0)
#define HL_HIGHLIGHT_NUMBERS (1 << 1)

/* Cheap per-row content flags. These are populated while rebuilding
 * row->render and let the GUI renderer choose fast paths without rescanning
 * raw row bytes every frame. */
#define ROW_META_TAB              (1u << 0)
#define ROW_META_NON_ASCII        (1u << 1)
#define ROW_META_NONPRINT         (1u << 2)
#define ROW_META_QUOTE            (1u << 3)
#define ROW_META_SLASH_OR_STAR    (1u << 4)
#define ROW_META_KEYWORD_CANDIDATE (1u << 5)

enum bentoLanguage {
  BENTO_LANG_NONE = 0,
  BENTO_LANG_C,
  BENTO_LANG_PYTHON,
  BENTO_LANG_JS,
  BENTO_LANG_GO,
  BENTO_LANG_RUST,
  BENTO_LANG_SHELL,
  BENTO_LANG_MARKDOWN
};

struct editorSyntax {
  char *name;
  int language;
  char **filematch;
  char **keywords;
  char singleline_comment_start[3];
  char multiline_comment_start[3];
  char multiline_comment_end[3];
  int flags;
  /* Lazily-populated keyword caches (populated by ensureKeywordCache).
   * keyword_lens[i] is strlen(keywords[i]) minus the trailing '|' if any.
   * keyword_kw2[i] is 1 if keywords[i] ended in '|' (HL_KEYWORD2). Avoids
   * calling strlen on every keyword for every identifier in the syntax
   * pass — a measurable cost when re-highlighting large files. */
  int *keyword_lens;
  unsigned char *keyword_kw2;
  int keyword_count;
};

/* This structure represents a single line of the file we are editing. */
typedef struct erow {
  int idx;           /* Row index in the file, zero-based. */
  int size;          /* Size of the row, excluding the null term. */
  int rsize;         /* Size of the rendered row. */
  char *chars;       /* Row content. */
  char *render;      /* Row content "rendered" for screen (for TABs). */
  unsigned char *hl; /* Syntax highlight type for each character in render.*/
  int hl_oc;         /* Row had open comment at end in last syntax highlight
                        check. */
  unsigned int meta; /* ROW_META_* flags for GUI render/syntax fast paths. */
} erow;

typedef struct hlcolor {
  int r, g, b;
} hlcolor;

struct editorConfig {
  int cx, cy;     /* Cursor x and y position in characters */
  int rowoff;     /* Offset of row displayed. */
  int coloff;     /* Offset of column displayed. */
  int screenrows; /* Number of rows that we can show */
  int screencols; /* Number of cols that we can show */
  int numrows;    /* Number of rows */
  int rawmode;    /* Is terminal raw mode enabled? */
  erow *row;      /* Rows */
  /* Parallel dense array of line sizes — SoA experiment. tbLineLength
   * reads from this instead of E.row[i].size, so scan loops hit a tight
   * int[] (4 bytes/row) rather than the 48-byte erow struct. Kept in
   * sync with row->size by the row mutators. NULL until first allocation. */
  int *line_sizes;
  int line_sizes_cap;
  int dirty;      /* File modified but not saved. */
  char *filename; /* Currently open filename */
  char statusmsg[80];
  time_t statusmsg_time;
  struct editorSyntax *syntax; /* Current syntax highlight, or NULL. */
  time_t file_mtime;           /* Last known on-disk modified time. */
};

static struct editorConfig E;
static int g_tab_stop = 8;

/* Forward declarations of the row helpers the TextBuffer API forwards to. */
void editorInsertRow(int at, char *s, size_t len);
void editorDelRow(int at);
void editorRowInsertChar(erow *row, int at, int c);
void editorRowAppendString(erow *row, char *s, size_t len);
void editorRowDelChar(erow *row, int at);
void editorRowReplace(erow *row, int at, int len, const char *replacement,
                      int replacement_len);

/* ============================ TextBuffer API ============================
 *
 * Thin abstraction over the editor's text storage. All code that needs to
 * read or mutate buffer contents should go through this API; the backing
 * store can then be swapped (e.g. to chunked text arenas + line-metadata
 * arrays) without touching call sites.
 *
 * Phase 1: implemented as a forwarding layer over the existing erow array.
 * Reads peek directly into E.row for now; writes call the existing
 * editorRow... / editorInsertRow / editorDelRow helpers so undo, dirty
 * tracking, render regeneration, and syntax highlighting keep working
 * unchanged.
 *
 * Conventions:
 *  - `row` is a zero-based line index into the buffer.
 *  - Slice/render pointers returned by tb* are read-only views; they may be
 *    invalidated by the next mutating tb* call, so don't hold them across
 *    inserts/deletes.
 *  - `len` outputs are optional (may be NULL).
 */

typedef struct TextBuffer {
  struct editorConfig *ed;  /* opaque backing pointer (currently &E) */
} TextBuffer;

static TextBuffer TB_storage = {0};
static TextBuffer *TB = &TB_storage;

static inline void tbInit(TextBuffer *tb, struct editorConfig *ed) {
  tb->ed = ed;
}

/* --- Read accessors --- */

static inline int tbLineCount(TextBuffer *tb) {
  return tb->ed->numrows;
}

static inline int tbLineLength(TextBuffer *tb, int row) {
  if (row < 0 || row >= tb->ed->numrows) return 0;
  /* Read from the dense parallel array if it's been built; fall back to
   * the row struct for any code path that mutates row->size without going
   * through tbStoreSize (defensive — we currently keep them in sync).
   *
   * Note: A/B benchmarking on a 36k-line file showed this SoA read path
   * delivers no measurable speedup over reading from the erow struct
   * (within ±2% run-to-run noise). The dominant cost on this workload is
   * editorUpdateSyntax itself, which touches every field of the row and
   * therefore isn't helped by metadata-only SoA. The infrastructure
   * stays in place because it's harmless and may help future scan-heavy
   * features (e.g. large-file outline view, fuzzy file-wide search). */
  if (tb->ed->line_sizes && row < tb->ed->line_sizes_cap)
    return tb->ed->line_sizes[row];
  return tb->ed->row[row].size;
}

/* Grow the parallel line_sizes array to at least `need` entries. Returns
 * 1 on success, 0 on allocation failure (caller falls back to the row
 * struct, which is what tbLineLength does anyway). */
static int tbGrowLineSizes(struct editorConfig *ed, int need) {
  if (need <= ed->line_sizes_cap) return 1;
  int new_cap = ed->line_sizes_cap ? ed->line_sizes_cap * 2 : 256;
  while (new_cap < need) new_cap *= 2;
  int *grown = realloc(ed->line_sizes, (size_t)new_cap * sizeof(int));
  if (!grown) return 0;
  ed->line_sizes = grown;
  ed->line_sizes_cap = new_cap;
  return 1;
}

/* Update the parallel size array for `row`. Always also update row->size
 * in the erow struct (which mutator code does already) so the two stay
 * in sync. Pure idempotent write — safe to call from anywhere. */
static inline void tbStoreSize(struct editorConfig *ed, int row, int size) {
  if (row < 0) return;
  if (row >= ed->line_sizes_cap && !tbGrowLineSizes(ed, row + 1))
    return;
  ed->line_sizes[row] = size;
}

/* Pointer to the raw (un-rendered) line bytes. Read-only; may be invalidated
 * by the next mutating tb* call. */
static inline const char *tbLineSlice(TextBuffer *tb, int row, int *len) {
  if (row < 0 || row >= tb->ed->numrows) {
    if (len) *len = 0;
    return NULL;
  }
  if (len) *len = tb->ed->row[row].size;
  return tb->ed->row[row].chars;
}

/* Pointer to the rendered (tab-expanded) line bytes. Read-only. */
static inline const char *tbRenderSlice(TextBuffer *tb, int row, int *len) {
  if (row < 0 || row >= tb->ed->numrows) {
    if (len) *len = 0;
    return NULL;
  }
  if (len) *len = tb->ed->row[row].rsize;
  return tb->ed->row[row].render;
}

/* Per-rendered-character syntax highlight array; same length as the rendered
 * line slice. Read-only; NULL if highlighting hasn't been computed yet. */
static inline const unsigned char *tbHighlight(TextBuffer *tb, int row) {
  if (row < 0 || row >= tb->ed->numrows) return NULL;
  return tb->ed->row[row].hl;
}

/* Did this row end inside an open multi-line comment after its last
 * highlight pass? Used by editorUpdateSyntax to propagate state. */
static inline int tbRowOpenComment(TextBuffer *tb, int row) {
  if (row < 0 || row >= tb->ed->numrows) return 0;
  return tb->ed->row[row].hl_oc;
}

/* --- Write API (forwards to existing helpers) --- */

static inline void tbInsertLine(TextBuffer *tb, int at, const char *s,
                                int len) {
  (void)tb;
  editorInsertRow(at, (char *)s, (size_t)len);
}

static inline void tbDeleteLine(TextBuffer *tb, int at) {
  (void)tb;
  editorDelRow(at);
}

static inline void tbInsertChar(TextBuffer *tb, int row, int col, int c) {
  if (row < 0 || row >= tb->ed->numrows) return;
  editorRowInsertChar(&tb->ed->row[row], col, c);
}

static inline void tbDeleteChar(TextBuffer *tb, int row, int col) {
  if (row < 0 || row >= tb->ed->numrows) return;
  editorRowDelChar(&tb->ed->row[row], col);
}

static inline void tbAppendToLine(TextBuffer *tb, int row, const char *s,
                                  int len) {
  if (row < 0 || row >= tb->ed->numrows) return;
  editorRowAppendString(&tb->ed->row[row], (char *)s, (size_t)len);
}

/* Escape hatch for code that still needs the raw erow. */
static inline erow *tbRowMut(TextBuffer *tb, int row) {
  if (row < 0 || row >= tb->ed->numrows) return NULL;
  return &tb->ed->row[row];
}

/* ============================== Text Arena ==============================
 *
 * A single big append-only byte arena that backs line `chars` for newly
 * inserted rows (i.e. the dominant file-load case). Reading a freshly-
 * loaded file therefore walks one contiguous arena rather than 30k
 * scattered malloc slabs.
 *
 * Lifecycle:
 *  - editorInsertRow tries arenaAlloc first; on miss, falls back to malloc.
 *  - editorFreeRow / editorFreeRows skip free() for arena-backed chars.
 *  - editorRowInsertChar / editorRowAppendString / editorRowReplace and the
 *    undo/delete paths must call charsMigrate(row) before realloc, so an
 *    arena-backed line gets promoted to its own heap allocation the moment
 *    it's about to grow or shrink.
 *  - arenaReset() drops everything (used on initEditor and tab swap-out).
 *
 * The arena memory is leaked for rows that have since migrated out — that
 * waste is bounded by the file size, and is cheap compared to the cache
 * win on the read path.
 */

typedef struct TextChunk {
  char *data;
  size_t used;
  size_t cap;
} TextChunk;

static TextChunk g_chars_arena = {0};

/* Default arena size; small files load entirely in here. Lines that don't
 * fit fall back to malloc. */
#define BENTO_ARENA_DEFAULT_CAP (1u << 20)  /* 1 MiB */

static void arenaInit(TextChunk *a) {
  if (a->data)
    return;
  a->cap = BENTO_ARENA_DEFAULT_CAP;
  a->used = 0;
  a->data = malloc(a->cap);
  if (!a->data)
    a->cap = 0;
}

static void arenaReset(TextChunk *a) {
  a->used = 0;
}

static int arenaContains(TextChunk *a, const char *p) {
  return a->data && p >= a->data && p < a->data + a->cap;
}

/* Try to allocate `size` bytes from the arena. Returns NULL if the arena is
 * uninitialised or doesn't have room — caller falls back to malloc. */
static char *arenaAlloc(TextChunk *a, size_t size) {
  if (!a->data)
    arenaInit(a);
  if (!a->data || a->used + size > a->cap)
    return NULL;
  char *p = a->data + a->used;
  a->used += size;
  return p;
}

/* Promote an arena-backed line to its own heap allocation. After this
 * returns, row->chars points to a malloc'd buffer of size (row->size + 1)
 * and is safe to realloc. No-op for already heap-backed rows. */
static void charsMigrate(erow *row) {
  if (!row || !row->chars)
    return;
  if (!arenaContains(&g_chars_arena, row->chars))
    return;
  char *fresh = malloc((size_t)row->size + 1);
  if (!fresh)
    return;  /* OOM — leave row arena-backed; subsequent realloc will fault loudly */
  memcpy(fresh, row->chars, (size_t)row->size + 1);
  row->chars = fresh;
}

/* Free a chars pointer that came from either arena or malloc. */
static void charsFree(char *p) {
  if (!p)
    return;
  if (arenaContains(&g_chars_arena, p))
    return;
  free(p);
}

/* Bumped whenever any row's render or hl bytes change. The GUI uses this
 * as a content version to decide whether its frame-local run list can be
 * reused from the previous frame. Lives outside the GUI config struct so
 * shared editor code (which compiles in both terminal and GUI builds) can
 * touch it without an ifdef. */
static unsigned long long g_render_gen = 0;

/* ============================ Dirty Syntax ==============================
 *
 * Edits used to call editorUpdateSyntax synchronously, and when a row's
 * open-comment state changed it would recurse all the way to the bottom
 * of the file. Typing an open-comment marker at the top of a 30k-line C
 * file therefore re-highlighted 30k rows on every keystroke.
 *
 * Now: editorUpdateRow rebuilds row->render but does NOT recompute hl.
 * It marks the row syntax-dirty instead. Render-time (and any other
 * code that needs fresh hl) calls syntaxRefreshUpTo(upto_row) which
 * walks the dirty range up to that row and recomputes hl in place.
 *
 * The dirty range is a coarse bounding interval `[start, end)`. Two
 * independent dirty rows widen the interval; we re-do some clean work
 * occasionally in exchange for very cheap bookkeeping. Row-insert /
 * row-delete uses dirtyMarkFromHere(at) to mark everything from `at`
 * onward, because index shifts invalidate any precise tracking.
 */

/* ============================== Profiling ===============================
 *
 * Opt-in instrumentation. Build with `-DBENTO_PROFILE` and call
 * profDump() (or run --bench, which calls it automatically) to see
 * per-region totals. Buckets are looked up by stringified region name;
 * zero overhead when the macro is undefined.
 */

#ifdef BENTO_PROFILE
typedef struct ProfBucket {
  const char *name;
  double total_ms;
  long count;
} ProfBucket;
#define BENTO_PROF_MAX 16
static ProfBucket g_prof[BENTO_PROF_MAX];

static int profSlot(const char *name) {
  for (int i = 0; i < BENTO_PROF_MAX; i++) {
    if (g_prof[i].name == name) return i;
    if (!g_prof[i].name) { g_prof[i].name = name; return i; }
  }
  return 0;
}

static double profDiffMs(struct timespec a, struct timespec b) {
  return (b.tv_sec - a.tv_sec) * 1000.0 +
         (b.tv_nsec - a.tv_nsec) / 1.0e6;
}

static void profDump(void) {
  fprintf(stderr, "--- profile ---\n");
  for (int i = 0; i < BENTO_PROF_MAX; i++) {
    if (!g_prof[i].name) continue;
    double per = g_prof[i].count > 0 ? g_prof[i].total_ms * 1000.0 / g_prof[i].count : 0.0;
    fprintf(stderr, "%-24s %10.3f ms total %10ld calls %10.3f us/call\n",
            g_prof[i].name, g_prof[i].total_ms, g_prof[i].count, per);
  }
}

#define PROF_BEGIN(name) struct timespec _prof_t0_##name; clock_gettime(CLOCK_MONOTONIC, &_prof_t0_##name)
#define PROF_END(name) do {                                                    \
    struct timespec _prof_t1;                                                  \
    clock_gettime(CLOCK_MONOTONIC, &_prof_t1);                                 \
    int _slot = profSlot(#name);                                               \
    g_prof[_slot].total_ms += profDiffMs(_prof_t0_##name, _prof_t1);           \
    g_prof[_slot].count++;                                                     \
  } while (0)
#else
#define PROF_BEGIN(name) ((void)0)
#define PROF_END(name) ((void)0)
static inline void profDump(void) {}
#endif

typedef struct DirtyRange {
  int start;  /* inclusive */
  int end;    /* exclusive — empty range if start >= end */
} DirtyRange;

static DirtyRange g_dirty_syntax = {0, 0};

static inline void dirtyClear(void) {
  g_dirty_syntax.start = 0;
  g_dirty_syntax.end = 0;
}

static inline int dirtyEmpty(void) {
  return g_dirty_syntax.start >= g_dirty_syntax.end;
}

/* Mark a single row dirty. */
static inline void dirtyMark(int row) {
  if (row < 0) return;
  if (dirtyEmpty()) {
    g_dirty_syntax.start = row;
    g_dirty_syntax.end = row + 1;
    return;
  }
  if (row < g_dirty_syntax.start) g_dirty_syntax.start = row;
  if (row >= g_dirty_syntax.end) g_dirty_syntax.end = row + 1;
}

/* Mark [from_row, end-of-file) dirty. Used after row insert/delete because
 * index shifts make precise tracking of pre-existing dirty rows fragile. */
static inline void dirtyMarkFromHere(int from_row, int line_count) {
  if (from_row < 0) from_row = 0;
  if (dirtyEmpty() || from_row < g_dirty_syntax.start)
    g_dirty_syntax.start = from_row;
  if (line_count > g_dirty_syntax.end)
    g_dirty_syntax.end = line_count;
}

enum KEY_ACTION {
  bento_KEY_NULL = 0, /* NULL */
  CTRL_C = 3,      /* Ctrl-c */
  CTRL_D = 4,      /* Ctrl-d */
  CTRL_F = 6,      /* Ctrl-f */
  CTRL_H = 8,      /* Ctrl-h */
  TAB = 9,         /* Tab */
  CTRL_L = 12,     /* Ctrl+l */
  ENTER = 13,      /* Enter */
  CTRL_Q = 17,     /* Ctrl-q */
  CTRL_S = 19,     /* Ctrl-s */
  CTRL_U = 21,     /* Ctrl-u */
  CTRL_Z = 26,     /* Ctrl-z (undo) */
  ESC = 27,        /* Escape */
  BACKSPACE = 127, /* Backspace */
  /* The following are just soft codes, not really reported by the
   * terminal directly. */
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

void editorSetStatusMessage(const char *fmt, ...);

/* =========================== Syntax highlights DB =========================
 *
 * In order to add a new syntax, define two arrays with a list of file name
 * matches and keywords. The file name matches are used in order to match
 * a given syntax with a given file name: if a match pattern starts with a
 * dot, it is matched as the last past of the filename, for example ".c".
 * Otherwise the pattern is just searched inside the filenme, like "Makefile").
 *
 * The list of keywords to highlight is just a list of words, however if they
 * a trailing '|' character is added at the end, they are highlighted in
 * a different color, so that you can have two different sets of keywords.
 *
 * Finally add a stanza in the HLDB global variable with two two arrays
 * of strings, and a set of flags in order to enable highlighting of
 * comments and numbers.
 *
 * The characters for single and multi line comments must be exactly two
 * and must be provided as well (see the C language example).
 *
 * There is no support to highlight patterns currently. */

/* C / C++ */
char *C_HL_extensions[] = {".c",   ".h",   ".cc", ".cpp", ".cxx",
                           ".hpp", ".hh",  ".hxx", ".m",   ".mm",
                           NULL};
char *C_HL_keywords[] = {
    /* C Keywords */
    "auto", "break", "case", "continue", "default", "do", "else", "enum",
    "extern", "for", "goto", "if", "register", "return", "sizeof", "static",
    "struct", "switch", "typedef", "union", "volatile", "while", "NULL",

    /* C++ Keywords */
    "alignas", "alignof", "and", "and_eq", "asm", "bitand", "bitor", "class",
    "compl", "constexpr", "const_cast", "deltype", "delete", "dynamic_cast",
    "explicit", "export", "false", "friend", "inline", "mutable", "namespace",
    "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq",
    "private", "protected", "public", "reinterpret_cast", "static_assert",
    "static_cast", "template", "this", "thread_local", "throw", "true", "try",
    "typeid", "typename", "virtual", "xor", "xor_eq",

    /* C types */
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", "short|", "auto|", "const|", "bool|", NULL};

char *PY_HL_extensions[] = {".py", ".pyw", NULL};
char *PY_HL_keywords[] = {
    "and", "as", "assert", "break", "class", "continue", "def", "del",
    "elif", "else", "except", "finally", "for", "from", "global", "if",
    "import", "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise",
    "return", "try", "while", "with", "yield", "False|", "None|", "True|",
    "async|", "await|", NULL};

char *JS_HL_extensions[] = {".js", ".jsx", ".ts", ".tsx", ".mjs", ".cjs",
                            NULL};
char *JS_HL_keywords[] = {
    "break", "case", "catch", "class", "const", "continue", "debugger",
    "default", "delete", "do", "else", "export", "extends", "finally", "for",
    "from", "function", "if", "import", "in", "instanceof", "let", "new",
    "return", "super", "switch", "this", "throw", "try", "typeof", "var",
    "void", "while", "with", "yield", "async|", "await|", "false|", "null|",
    "true|", "undefined|", "number|", "string|", "boolean|", "type|",
    "interface|", NULL};

char *GO_HL_extensions[] = {".go", NULL};
char *GO_HL_keywords[] = {
    "break", "case", "chan", "const", "continue", "default", "defer", "else",
    "fallthrough", "for", "func", "go", "goto", "if", "import", "interface",
    "map", "package", "range", "return", "select", "struct", "switch", "type",
    "var", "bool|", "byte|", "complex64|", "complex128|", "error|",
    "float32|", "float64|", "int|", "int8|", "int16|", "int32|", "int64|",
    "rune|", "string|", "uint|", "uint8|", "uint16|", "uint32|", "uint64|",
    "uintptr|", "nil|", "true|", "false|", NULL};

char *RUST_HL_extensions[] = {".rs", NULL};
char *RUST_HL_keywords[] = {
    "as", "break", "const", "continue", "crate", "else", "enum", "extern",
    "false", "fn", "for", "if", "impl", "in", "let", "loop", "match", "mod",
    "move", "mut", "pub", "ref", "return", "self", "Self", "static",
    "struct", "super", "trait", "true", "type", "unsafe", "use", "where",
    "while", "async|", "await|", "dyn|", "bool|", "char|", "f32|", "f64|",
    "i8|", "i16|", "i32|", "i64|", "i128|", "isize|", "str|", "u8|",
    "u16|", "u32|", "u64|", "u128|", "usize|", NULL};

char *SH_HL_extensions[] = {".sh", ".bash", ".zsh", NULL};
char *SH_HL_keywords[] = {
    "case", "do", "done", "elif", "else", "esac", "fi", "for", "function",
    "if", "in", "select", "then", "until", "while", "break|", "continue|",
    "return|", "export|", "local|", "readonly|", NULL};

char *MD_HL_extensions[] = {".md", ".markdown", NULL};
char *MD_HL_keywords[] = {NULL};

/* Here we define an array of syntax highlights by extensions, keywords,
 * comments delimiters and flags. */
struct editorSyntax HLDB[] = {
    {"C/C++", BENTO_LANG_C, C_HL_extensions, C_HL_keywords, "//", "/*", "*/",
     HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL, NULL, 0},
    {"Python", BENTO_LANG_PYTHON, PY_HL_extensions, PY_HL_keywords, "#", "", "",
     HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL, NULL, 0},
    {"JavaScript", BENTO_LANG_JS, JS_HL_extensions, JS_HL_keywords, "//", "/*",
     "*/", HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL, NULL, 0},
    {"Go", BENTO_LANG_GO, GO_HL_extensions, GO_HL_keywords, "//", "/*", "*/",
     HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL, NULL, 0},
    {"Rust", BENTO_LANG_RUST, RUST_HL_extensions, RUST_HL_keywords, "//", "/*",
     "*/", HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL, NULL, 0},
    {"Shell", BENTO_LANG_SHELL, SH_HL_extensions, SH_HL_keywords, "#", "", "",
     HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL, NULL, 0},
    {"Markdown", BENTO_LANG_MARKDOWN, MD_HL_extensions, MD_HL_keywords, "", "",
     "", 0, NULL, NULL, 0}};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/* ======================= Low level terminal handling ====================== */

#ifndef bento_RAYLIB

static struct termios orig_termios; /* In order to restore at exit.*/

void disableRawMode(int fd) {
  /* Don't even check the return value as it's too late. */
  if (E.rawmode) {
    tcsetattr(fd, TCSAFLUSH, &orig_termios);
    E.rawmode = 0;
  }
}

/* Called at exit to avoid remaining in raw mode. */
void editorAtExit(void) { disableRawMode(STDIN_FILENO); }

/* Raw mode: 1960 magic shit. */
int enableRawMode(int fd) {
  struct termios raw;

  if (E.rawmode)
    return 0; /* Already enabled. */
  if (!isatty(STDIN_FILENO))
    goto fatal;
  atexit(editorAtExit);
  if (tcgetattr(fd, &orig_termios) == -1)
    goto fatal;

  raw = orig_termios; /* modify the original mode */
  /* input modes: no break, no CR to NL, no parity check, no strip char,
   * no start/stop output control. */
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  /* output modes - disable post processing */
  raw.c_oflag &= ~(OPOST);
  /* control modes - set 8 bit chars */
  raw.c_cflag |= (CS8);
  /* local modes - choing off, canonical off, no extended functions,
   * no signal chars (^Z,^C) */
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  /* control chars - set return condition: min number of bytes and timer. */
  raw.c_cc[VMIN] = 0;  /* Return each byte, or zero for timeout. */
  raw.c_cc[VTIME] = 1; /* 100 ms timeout (unit is tens of second). */

  /* put terminal in raw mode after flushing */
  if (tcsetattr(fd, TCSAFLUSH, &raw) < 0)
    goto fatal;
  E.rawmode = 1;
  return 0;

fatal:
  errno = ENOTTY;
  return -1;
}

/* Read a key from the terminal put in raw mode, trying to handle
 * escape sequences. */
int editorReadKey(int fd) {
  int nread;
  char c, seq[3];
  while ((nread = read(fd, &c, 1)) == 0)
    ;
  if (nread == -1)
    exit(1);

  while (1) {
    switch (c) {
    case ESC: /* escape sequence */
      /* If this is just an ESC, we'll timeout here. */
      if (read(fd, seq, 1) == 0)
        return ESC;
      if (read(fd, seq + 1, 1) == 0)
        return ESC;

      /* ESC [ sequences. */
      if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
          /* Extended escape, read additional byte. */
          if (read(fd, seq + 2, 1) == 0)
            return ESC;
          if (seq[2] == '~') {
            switch (seq[1]) {
            case '3':
              return DEL_KEY;
            case '5':
              return PAGE_UP;
            case '6':
              return PAGE_DOWN;
            }
          }
        } else {
          switch (seq[1]) {
          case 'A':
            return ARROW_UP;
          case 'B':
            return ARROW_DOWN;
          case 'C':
            return ARROW_RIGHT;
          case 'D':
            return ARROW_LEFT;
          case 'H':
            return HOME_KEY;
          case 'F':
            return END_KEY;
          }
        }
      }

      /* ESC O sequences. */
      else if (seq[0] == 'O') {
        switch (seq[1]) {
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
      break;
    default:
      return c;
    }
  }
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor is stored at *rows and *cols and 0 is returned. */
int getCursorPosition(int ifd, int ofd, int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  /* Report cursor location */
  if (write(ofd, "\x1b[6n", 4) != 4)
    return -1;

  /* Read the response: ESC [ rows ; cols R */
  while (i < sizeof(buf) - 1) {
    if (read(ifd, buf + i, 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }
  buf[i] = '\0';

  /* Parse it. */
  if (buf[0] != ESC || buf[1] != '[')
    return -1;
  if (sscanf(buf + 2, "%d;%d", rows, cols) != 2)
    return -1;
  return 0;
}

/* Try to get the number of columns in the current terminal. If the ioctl()
 * call fails the function will try to query the terminal itself.
 * Returns 0 on success, -1 on error. */
int getWindowSize(int ifd, int ofd, int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    /* ioctl() failed. Try to query the terminal itself. */
    int orig_row, orig_col, retval;

    /* Get the initial position so we can restore it later. */
    retval = getCursorPosition(ifd, ofd, &orig_row, &orig_col);
    if (retval == -1)
      goto failed;

    /* Go to right/bottom margin and get position. */
    if (write(ofd, "\x1b[999C\x1b[999B", 12) != 12)
      goto failed;
    retval = getCursorPosition(ifd, ofd, rows, cols);
    if (retval == -1)
      goto failed;

    /* Restore position. */
    char seq[32];
    snprintf(seq, 32, "\x1b[%d;%dH", orig_row, orig_col);
    if (write(ofd, seq, strlen(seq)) == -1) {
      /* Can't recover... */
    }
    return 0;
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }

failed:
  return -1;
}

#endif /* bento_RAYLIB */

/* ====================== Syntax highlight color scheme  ==================== */

int is_separator(int c) {
  unsigned char uc = (unsigned char)c;
  return c == '\0' || isspace(uc) || strchr(",.()+-/*=~%[];:{}<>", c) != NULL;
}

static int editorIsIdentChar(int c) {
  return isalnum((unsigned char)c) || c == '_';
}

static int editorTokenEquals(const char *p, int len, const char *word) {
  return (int)strlen(word) == len && !memcmp(p, word, (size_t)len);
}

static int editorIsControlLikeToken(const char *p, int len) {
  return editorTokenEquals(p, len, "if") || editorTokenEquals(p, len, "for") ||
         editorTokenEquals(p, len, "while") ||
         editorTokenEquals(p, len, "switch") ||
         editorTokenEquals(p, len, "catch") ||
         editorTokenEquals(p, len, "match") ||
         editorTokenEquals(p, len, "select") ||
         editorTokenEquals(p, len, "return") ||
         editorTokenEquals(p, len, "sizeof") ||
         editorTokenEquals(p, len, "def") ||
         editorTokenEquals(p, len, "class") ||
         editorTokenEquals(p, len, "func") ||
         editorTokenEquals(p, len, "fn");
}

static int editorTokenStartsWith(const char *p, int remaining,
                                 const char *token, int len) {
  if (!token || len <= 0 || remaining < len)
    return 0;
  return !memcmp(p, token, (size_t)len);
}

static int editorTokenHasSlashOrStar(const char *token, int len) {
  for (int i = 0; i < len; i++) {
    if (token[i] == '/' || token[i] == '*')
      return 1;
  }
  return 0;
}

/* Return true if the specified row last char is part of a multi line comment
 * that starts at this row or at one before, and does not end at the end
 * of the row but spawns to the next row. */
int editorRowHasOpenComment(erow *row) {
  if (E.syntax && E.syntax->multiline_comment_end[0] && row->hl &&
      row->rsize && row->hl[row->rsize - 1] == HL_MLCOMMENT) {
    int len = strlen(E.syntax->multiline_comment_end);
    if (row->rsize < len ||
        memcmp(row->render + row->rsize - len,
               E.syntax->multiline_comment_end, (size_t)len) != 0)
      return 1;
  }
  return 0;
}

/* Set every byte of row->hl (that corresponds to every character in the line)
 * to the right syntax highlight type (HL_* defines). */
void editorUpdateSyntaxImpl(erow *row);
void editorUpdateSyntax(erow *row) {
  PROF_BEGIN(updateSyntax);
  editorUpdateSyntaxImpl(row);
  PROF_END(updateSyntax);
}
void editorUpdateSyntaxImpl(erow *row) {
  row->hl = realloc(row->hl, row->rsize);
  memset(row->hl, HL_NORMAL, row->rsize);

  if (E.syntax == NULL)
    return; /* No syntax, everything is HL_NORMAL. */

  int i, prev_sep, in_string, in_comment;
  char *p;
  char **keywords = E.syntax->keywords;
  char *scs = E.syntax->singleline_comment_start;
  char *mcs = E.syntax->multiline_comment_start;
  char *mce = E.syntax->multiline_comment_end;
  int scs_len = strlen(scs);
  int mcs_len = strlen(mcs);
  int mce_len = strlen(mce);
  int has_quotes = (row->meta & ROW_META_QUOTE) != 0;
  int has_slash_star = (row->meta & ROW_META_SLASH_OR_STAR) != 0;
  int has_words = (row->meta & ROW_META_KEYWORD_CANDIDATE) != 0;
  int has_nonprint = (row->meta & ROW_META_NONPRINT) != 0;
  int check_strings = (E.syntax->flags & HL_HIGHLIGHT_STRINGS) && has_quotes;
  int check_line_comment =
      scs_len > 0 &&
      (!editorTokenHasSlashOrStar(scs, scs_len) || has_slash_star);
  int check_ml_start =
      mcs_len > 0 &&
      (!editorTokenHasSlashOrStar(mcs, mcs_len) || has_slash_star);
  int check_ml_end =
      mce_len > 0 &&
      (!editorTokenHasSlashOrStar(mce, mce_len) || has_slash_star);
  int check_words = has_words && E.syntax->keyword_count > 0;
  int check_functions =
      has_words && E.syntax->language != BENTO_LANG_MARKDOWN;

  /* Point to the first non-space char. */
  p = row->render;
  i = 0; /* Current char offset */
  while (*p && isspace((unsigned char)*p)) {
    p++;
    i++;
  }
  prev_sep = 1;   /* Tell the parser if 'i' points to start of word. */
  in_string = 0;  /* Are we inside "" or '' ? */
  in_comment = 0; /* Are we inside multi-line comment? */

  /* If the previous line has an open comment, this line starts
   * with an open comment state. */
  if (row->idx > 0 && tbRowOpenComment(TB, row->idx - 1))
    in_comment = 1;

  while (*p) {
    /* Handle line comments. */
    if (check_line_comment &&
        editorTokenStartsWith(p, row->rsize - i, scs, scs_len)) {
      /* From here to end is a comment */
      memset(row->hl + i, HL_COMMENT, row->rsize - i);
      return;
    }

    /* Handle multi line comments. */
    if (in_comment) {
      row->hl[i] = HL_MLCOMMENT;
      if (check_ml_end &&
          editorTokenStartsWith(p, row->rsize - i, mce, mce_len)) {
        memset(row->hl + i, HL_MLCOMMENT, (size_t)mce_len);
        p += mce_len;
        i += mce_len;
        in_comment = 0;
        prev_sep = 1;
        continue;
      } else {
        prev_sep = 0;
        p++;
        i++;
        continue;
      }
    } else if (check_ml_start &&
               editorTokenStartsWith(p, row->rsize - i, mcs, mcs_len)) {
      memset(row->hl + i, HL_MLCOMMENT, (size_t)mcs_len);
      p += mcs_len;
      i += mcs_len;
      in_comment = 1;
      prev_sep = 0;
      continue;
    }

    /* Handle "" and '' */
    if (check_strings && in_string) {
      row->hl[i] = HL_STRING;
      if (*p == '\\') {
        if (i + 1 < row->rsize) {
          row->hl[i + 1] = HL_STRING;
          p += 2;
          i += 2;
        } else {
          p++;
          i++;
        }
        prev_sep = 0;
        continue;
      }
      if (*p == in_string)
        in_string = 0;
      p++;
      i++;
      continue;
    } else {
      if (check_strings && (*p == '"' || *p == '\'')) {
        in_string = *p;
        row->hl[i] = HL_STRING;
        p++;
        i++;
        prev_sep = 0;
        continue;
      }
    }

    /* Handle non printable chars. */
    if (has_nonprint && (unsigned char)*p < 128 &&
        !isprint((unsigned char)*p)) {
      row->hl[i] = HL_NONPRINT;
      p++;
      i++;
      prev_sep = 0;
      continue;
    }

    /* Handle numbers */
    if ((E.syntax->flags & HL_HIGHLIGHT_NUMBERS) &&
        ((isdigit((unsigned char)*p) &&
          (prev_sep || row->hl[i - 1] == HL_NUMBER)) ||
         (*p == '.' && i > 0 && row->hl[i - 1] == HL_NUMBER))) {
      row->hl[i] = HL_NUMBER;
      p++;
      i++;
      prev_sep = 0;
      continue;
    }

    /* Handle keywords and lib calls. Uses E.syntax->keyword_lens /
     * keyword_kw2 caches populated by ensureKeywordCache so we don't pay
     * strlen + trailing-pipe scan on every check. */
    if (check_words && prev_sep) {
      int j;
      int kwc = E.syntax->keyword_count;
      const int *klens = E.syntax->keyword_lens;
      const unsigned char *kw2s = E.syntax->keyword_kw2;
      char c0 = *p;
      for (j = 0; j < kwc; j++) {
        /* First-character prefilter — vast majority of keywords fail this
         * cheap check, avoiding the memcmp call. */
        if (keywords[j][0] != c0)
          continue;
        int klen = klens[j];
        if (!memcmp(p, keywords[j], (size_t)klen) &&
            is_separator(*(p + klen))) {
          /* Keyword */
          memset(row->hl + i, kw2s[j] ? HL_KEYWORD2 : HL_KEYWORD1,
                 (size_t)klen);
          p += klen;
          i += klen;
          break;
        }
      }
      if (j < kwc) {
        prev_sep = 0;
        continue; /* We had a keyword match */
      }
    }

    if (check_functions && prev_sep &&
        editorIsIdentChar((unsigned char)*p)) {
      int ident_len = 0;
      char *q;

      while (editorIsIdentChar((unsigned char)p[ident_len]))
        ident_len++;
      q = p + ident_len;
      while (*q && isspace((unsigned char)*q))
        q++;
      if (*q == '(' && !editorIsControlLikeToken(p, ident_len)) {
        memset(row->hl + i, HL_FUNCTION, (size_t)ident_len);
        p += ident_len;
        i += ident_len;
        prev_sep = 0;
        continue;
      }
    }

    /* Not special chars */
    prev_sep = is_separator((unsigned char)*p);
    p++;
    i++;
  }

  /* If the row's open-comment state changed, the next row's highlight is
   * now stale — mark it dirty so the next refresh picks it up (or the
   * lazy-refresh loop in syntaxRefreshUpTo continues into it). No
   * recursion: a cascade that used to re-highlight the entire file now
   * only re-highlights as far as the visible window. */
  int oc = editorRowHasOpenComment(row);
  if (row->hl_oc != oc && row->idx + 1 < tbLineCount(TB))
    dirtyMark(row->idx + 1);
  row->hl_oc = oc;
  /* hl bytes changed → invalidate the GUI run-list cache. */
  g_render_gen++;
}

/* Refresh syntax for any dirty rows whose index is < upto. Stops at upto
 * even if a cascade extends the dirty range past it — the unrefreshed
 * portion stays dirty for the next call. */
void syntaxRefreshUpToImpl(int upto);
void syntaxRefreshUpTo(int upto) {
  PROF_BEGIN(syntaxRefresh);
  syntaxRefreshUpToImpl(upto);
  PROF_END(syntaxRefresh);
}
void syntaxRefreshUpToImpl(int upto) {
  if (dirtyEmpty())
    return;
  int line_count = tbLineCount(TB);
  if (upto > line_count) upto = line_count;
  int i = g_dirty_syntax.start;
  if (i < 0) i = 0;
  while (i < g_dirty_syntax.end && i < upto) {
    erow *r = tbRowMut(TB, i);
    if (r) editorUpdateSyntax(r);
    i++;
  }
  if (i >= g_dirty_syntax.end)
    dirtyClear();
  else
    g_dirty_syntax.start = i;
}

/* Refresh all dirty rows regardless of viewport. Used by code paths that
 * read hl outside of rendering (search, etc). */
void syntaxRefreshAll(void) {
  syntaxRefreshUpTo(tbLineCount(TB));
}

/* Maps syntax highlight token types to terminal colors. */
int editorSyntaxToColor(int hl) {
  switch (hl) {
  case HL_COMMENT:
  case HL_MLCOMMENT:
    return 36; /* cyan */
  case HL_KEYWORD1:
    return 33; /* yellow */
  case HL_KEYWORD2:
    return 32; /* green */
  case HL_STRING:
    return 35; /* magenta */
  case HL_NUMBER:
    return 31; /* red */
  case HL_MATCH:
    return 34; /* blu */
  case HL_FUNCTION:
    return 32; /* green */
  default:
    return 37; /* white */
  }
}

/* Select the syntax highlight scheme depending on the filename,
 * setting it in the global state E.syntax. */
struct editorSyntax *editorSyntaxForFilename(const char *filename) {
  if (!filename)
    return NULL;
  for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
    struct editorSyntax *s = HLDB + j;
    unsigned int i = 0;
    while (s->filematch[i]) {
      char *p;
      int patlen = strlen(s->filematch[i]);
      if ((p = strstr(filename, s->filematch[i])) != NULL) {
        if (s->filematch[i][0] != '.' || p[patlen] == '\0') {
          return s;
        }
      }
      i++;
    }
  }
  return NULL;
}

/* Lazily build the per-keyword length / kw2 cache so the syntax inner loop
 * doesn't have to call strlen + scan for trailing '|' on every check. */
static void ensureKeywordCache(struct editorSyntax *syn) {
  if (!syn || syn->keyword_lens)
    return;
  int count = 0;
  while (syn->keywords[count])
    count++;
  syn->keyword_count = count;
  if (count == 0)
    return;
  syn->keyword_lens = malloc(sizeof(int) * (size_t)count);
  syn->keyword_kw2 = malloc(sizeof(unsigned char) * (size_t)count);
  if (!syn->keyword_lens || !syn->keyword_kw2) {
    free(syn->keyword_lens);
    free(syn->keyword_kw2);
    syn->keyword_lens = NULL;
    syn->keyword_kw2 = NULL;
    syn->keyword_count = 0;
    return;
  }
  for (int i = 0; i < count; i++) {
    int len = (int)strlen(syn->keywords[i]);
    int kw2 = (len > 0 && syn->keywords[i][len - 1] == '|');
    if (kw2)
      len--;
    syn->keyword_lens[i] = len;
    syn->keyword_kw2[i] = (unsigned char)kw2;
  }
}

void editorSelectSyntaxHighlight(char *filename) {
  E.syntax = editorSyntaxForFilename(filename);
  ensureKeywordCache(E.syntax);
}

/* ======================= Editor rows implementation ======================= */

/* Update the rendered version and the syntax highlight of a row. */
void editorUpdateRowImpl(erow *row);
void editorUpdateRow(erow *row) {
  PROF_BEGIN(updateRow);
  editorUpdateRowImpl(row);
  PROF_END(updateRow);
}
void editorUpdateRowImpl(erow *row) {
  unsigned int tabs = 0, nonprint = 0;
  unsigned int meta = 0;
  int j, idx;

  /* Create a version of the row we can directly print on the screen,
   * respecting tabs, substituting non printable characters with '?'. */
  free(row->render);
  for (j = 0; j < row->size; j++) {
    unsigned char ch = (unsigned char)row->chars[j];
    if (ch == TAB) {
      tabs++;
      meta |= ROW_META_TAB;
    } else if (ch >= 128) {
      meta |= ROW_META_NON_ASCII;
    } else if (!isprint(ch)) {
      nonprint++;
      meta |= ROW_META_NONPRINT;
    }
    if (ch == '"' || ch == '\'')
      meta |= ROW_META_QUOTE;
    if (ch == '/' || ch == '*')
      meta |= ROW_META_SLASH_OR_STAR;
    if (isalpha(ch) || ch == '_')
      meta |= ROW_META_KEYWORD_CANDIDATE;
  }

  unsigned long long allocsize =
      (unsigned long long)row->size + tabs * g_tab_stop + nonprint * 9 + 1;
  if (allocsize > UINT32_MAX) {
    printf("Some line of the edited file is too long for bento\n");
    exit(1);
  }

  row->render = malloc(row->size + tabs * g_tab_stop + nonprint * 9 + 1);
  idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == TAB) {
      row->render[idx++] = ' ';
      while ((idx + 1) % g_tab_stop != 0)
        row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->rsize = idx;
  row->render[idx] = '\0';
  row->meta = meta;

  /* Defer syntax: just mark this row dirty. syntaxRefreshUpTo() runs
   * before the next render (or before any explicit hl read like the
   * search path). */
  dirtyMark(row->idx);
  /* Bumping g_render_gen invalidates the GUI run-list cache so the next
   * frame re-emits this row's runs. */
  g_render_gen++;
}

/* ================================= Undo =================================
 *
 * Operation-based undo stack. Every edit is recorded as either an INSERT
 * or a DELETE of a chunk of text at a (row, col) position. Each entry also
 * stores the cursor / viewport state so an undo can restore the cursor to
 * where the user was when they made the edit.
 *
 * Ops are grouped into "logical actions" the user thinks of as one step:
 *  - Same-kind ops at adjacent positions auto-merge (e.g. consecutive
 *    keystrokes, consecutive backspaces).
 *  - Explicit transactions (undoBeginTxn/undoEndTxn) force a group so
 *    compound actions (paste, type-over-selection, find/replace-all) form
 *    a single undo step.
 *
 * editorUndo() pops the top group and applies the inverse of each op. The
 * dirty flag is derived from a save-point index so undoing back to disk
 * state clears the modified marker. */

void editorInsertRow(int at, char *s, size_t len);
void editorDelRow(int at);

#define UNDO_INSERT 1
#define UNDO_DELETE 2

typedef enum {
  UNDO_KIND_NONE = 0,
  UNDO_KIND_INSERT_CHAR,
  UNDO_KIND_INSERT_NL,
  UNDO_KIND_DELETE_BACK,
  UNDO_KIND_DELETE_FWD,
  UNDO_KIND_DELETE_NL,
  UNDO_KIND_BLOCK
} undoKind;

typedef struct undoOp {
  int op;             /* UNDO_INSERT or UNDO_DELETE */
  int row, col;
  char *text;         /* malloc'd copy of the affected text */
  int len;
  int starts_group;   /* first op of an undo group */
  /* Cursor / view state to restore when this op is undone: */
  int cx, cy, coloff, rowoff;
} undoOp;

typedef struct undoStack {
  undoOp *ops;
  int size;
  int cap;
  int suppress;             /* >0 while applying undo (don't re-record) */
  int txn_depth;            /* nested undoBeginTxn / undoEndTxn */
  int txn_started;          /* first op of current txn already pushed */
  /* Auto-batching state for the last recorded op: */
  undoKind last_kind;
  int last_row, last_col, last_len;
  /* Save-point tracking for accurate dirty flag: */
  int save_point;           /* stack size at last save; -1 = unreachable */
  int delete_is_forward;    /* set by guiDeleteForwardChar */
} undoStack;

static undoStack U;

static void undoInit(void) {
  U.ops = NULL;
  U.size = 0;
  U.cap = 0;
  U.suppress = 0;
  U.txn_depth = 0;
  U.txn_started = 0;
  U.last_kind = UNDO_KIND_NONE;
  U.last_row = -1;
  U.last_col = -1;
  U.last_len = 0;
  U.save_point = 0;
  U.delete_is_forward = 0;
}

static void undoFreeOp(undoOp *op) {
  free(op->text);
  op->text = NULL;
}

static void undoClear(void) {
  for (int i = 0; i < U.size; i++)
    undoFreeOp(&U.ops[i]);
  U.size = 0;
  U.last_kind = UNDO_KIND_NONE;
  U.last_row = -1;
  U.last_col = -1;
  U.last_len = 0;
  U.txn_depth = 0;
  U.txn_started = 0;
  U.save_point = 0;
}

/* Drop the oldest undo group when the stack outgrows this many ops. */
#define UNDO_STACK_SOFT_LIMIT 8192

static void undoEvictOldestGroup(void) {
  if (U.size <= 1)
    return;
  /* Find the second group's start (the next starts_group at index >= 1). */
  int boundary = 1;
  while (boundary < U.size && !U.ops[boundary].starts_group)
    boundary++;
  if (boundary >= U.size)
    boundary = U.size; /* No second group — drop everything. */
  for (int i = 0; i < boundary; i++)
    undoFreeOp(&U.ops[i]);
  memmove(U.ops, U.ops + boundary, sizeof(undoOp) * (U.size - boundary));
  U.size -= boundary;
  if (U.save_point >= 0) {
    U.save_point -= boundary;
    if (U.save_point < 0)
      U.save_point = -1;
  }
}

static undoOp *undoPushSlot(void) {
  if (U.size >= UNDO_STACK_SOFT_LIMIT)
    undoEvictOldestGroup();
  if (U.size == U.cap) {
    int new_cap = U.cap ? U.cap * 2 : 64;
    undoOp *new_ops = realloc(U.ops, sizeof(undoOp) * new_cap);
    if (!new_ops)
      return NULL;
    U.ops = new_ops;
    U.cap = new_cap;
  }
  return &U.ops[U.size++];
}

static int undoOpsMerge(undoKind kind, int row, int col, int len) {
  if (U.last_kind != kind || U.last_row != row)
    return 0;
  switch (kind) {
  case UNDO_KIND_INSERT_CHAR:
    return col == U.last_col + U.last_len;
  case UNDO_KIND_DELETE_BACK:
    return len == 1 && col == U.last_col - 1;
  case UNDO_KIND_DELETE_FWD:
    return col == U.last_col;
  default:
    return 0;
  }
}

static void undoInvalidateSavePointIfNeeded(void) {
  if (U.save_point > U.size)
    U.save_point = -1;
}

static void undoUpdateDirty(void) {
  E.dirty = (U.save_point >= 0 && U.save_point == U.size) ? 0 : 1;
}

static void undoRecord(int op_type, int row, int col, const char *text, int len,
                       undoKind kind) {
  if (U.suppress)
    return;
  if (len <= 0 || !text)
    return;
  undoInvalidateSavePointIfNeeded();

  int new_group;
  if (U.txn_depth > 0) {
    new_group = !U.txn_started;
    U.txn_started = 1;
  } else {
    new_group = !undoOpsMerge(kind, row, col, len);
  }

  undoOp *slot = undoPushSlot();
  if (!slot)
    return;
  slot->op = op_type;
  slot->row = row;
  slot->col = col;
  slot->len = len;
  slot->text = malloc((size_t)len + 1);
  if (!slot->text) {
    U.size--;
    return;
  }
  memcpy(slot->text, text, len);
  slot->text[len] = '\0';
  slot->starts_group = new_group;
  slot->cx = E.cx;
  slot->cy = E.cy;
  slot->coloff = E.coloff;
  slot->rowoff = E.rowoff;

  U.last_kind = kind;
  U.last_row = row;
  U.last_col = col;
  U.last_len = len;
}

static void undoRecordInsert(int row, int col, const char *text, int len,
                             undoKind kind) {
  undoRecord(UNDO_INSERT, row, col, text, len, kind);
}

static void undoRecordDelete(int row, int col, const char *text, int len,
                             undoKind kind) {
  undoRecord(UNDO_DELETE, row, col, text, len, kind);
}

#ifdef bento_RAYLIB
static void undoBeginTxn(void) {
  if (U.txn_depth++ == 0)
    U.txn_started = 0;
}

static void undoEndTxn(void) {
  if (U.txn_depth > 0 && --U.txn_depth == 0) {
    U.txn_started = 0;
    /* After a txn, break auto-batching with any prior op. */
    U.last_kind = UNDO_KIND_NONE;
  }
}
#endif

static void undoMarkSaved(void) {
  U.save_point = U.size;
  U.last_kind = UNDO_KIND_NONE;
}

/* Compute (row, col) reached by walking `len` chars (with embedded newlines)
 * forward from (start_row, start_col). */
static void undoEndPosition(int start_row, int start_col, const char *text,
                            int len, int *out_row, int *out_col) {
  int r = start_row, c = start_col;
  for (int i = 0; i < len; i++) {
    if (text[i] == '\n') {
      r++;
      c = 0;
    } else {
      c++;
    }
  }
  *out_row = r;
  *out_col = c;
}

/* Direct buffer mutation used while applying undo. Does NOT touch the cursor
 * or record new undo entries. */
static void undoInsertTextAt(int row, int col, const char *text, int len) {
  if (len <= 0)
    return;
  while (tbLineCount(TB) <= row)
    tbInsertLine(TB, tbLineCount(TB), "", 0);

  int i = 0;
  while (i < len) {
    int seg_end = i;
    while (seg_end < len && text[seg_end] != '\n')
      seg_end++;
    int seg_len = seg_end - i;
    if (seg_len > 0) {
      erow *r = tbRowMut(TB, row);
      if (col > r->size)
        col = r->size;
      charsMigrate(r);
      r->chars = realloc(r->chars, (size_t)r->size + seg_len + 1);
      memmove(r->chars + col + seg_len, r->chars + col,
              (size_t)r->size - col + 1);
      memcpy(r->chars + col, text + i, seg_len);
      r->size += seg_len;
      tbStoreSize(&E, r->idx, r->size);
      editorUpdateRow(r);
      col += seg_len;
    }
    if (seg_end < len && text[seg_end] == '\n') {
      erow *r = tbRowMut(TB, row);
      if (col > r->size)
        col = r->size;
      tbInsertLine(TB, row + 1, r->chars + col, r->size - col);
      r = tbRowMut(TB, row);
      charsMigrate(r);
      r->chars[col] = '\0';
      r->size = col;
      tbStoreSize(&E, r->idx, r->size);
      editorUpdateRow(r);
      row++;
      col = 0;
      seg_end++;
    }
    i = seg_end;
  }
  E.dirty++;
}

static void undoDeleteSpan(int row, int col, int end_row, int end_col) {
  if (row < 0 || row >= tbLineCount(TB))
    return;
  if (row == end_row) {
    erow *r = tbRowMut(TB, row);
    if (col < 0)
      col = 0;
    if (end_col > r->size)
      end_col = r->size;
    if (end_col <= col)
      return;
    charsMigrate(r);
    memmove(r->chars + col, r->chars + end_col,
            (size_t)r->size - end_col + 1);
    r->size -= end_col - col;
    tbStoreSize(&E, r->idx, r->size);
    editorUpdateRow(r);
    E.dirty++;
    return;
  }
  if (end_row >= tbLineCount(TB)) {
    /* The recorded span runs past the end of the buffer. Trim every row
     * after `row`, truncate `row` at `col`, and drop `row` itself if the
     * deletion swallowed it whole — this restores the original empty-file
     * state when we undo a "type into empty file" entry. */
    for (int j = tbLineCount(TB) - 1; j > row; j--)
      tbDeleteLine(TB, j);
    erow *r = tbRowMut(TB, row);
    if (col < r->size) {
      charsMigrate(r);
      r->chars[col] = '\0';
      r->size = col;
      tbStoreSize(&E, r->idx, r->size);
      editorUpdateRow(r);
    }
    if (col == 0 && end_row > row && row < tbLineCount(TB))
      tbDeleteLine(TB, row);
    E.dirty++;
    return;
  }
  erow *r = tbRowMut(TB, row);
  int end_size = tbLineLength(TB, end_row);
  int tail_len = end_size - end_col;
  if (tail_len < 0)
    tail_len = 0;
  charsMigrate(r);
  r->chars = realloc(r->chars, (size_t)col + tail_len + 1);
  if (tail_len > 0) {
    const char *end_chars = tbLineSlice(TB, end_row, NULL);
    memcpy(r->chars + col, end_chars + end_col, (size_t)tail_len);
  }
  r->size = col + tail_len;
  r->chars[r->size] = '\0';
  tbStoreSize(&E, r->idx, r->size);
  editorUpdateRow(r);
  for (int j = end_row; j > row; j--)
    tbDeleteLine(TB, j);
  E.dirty++;
}

static void undoRestoreCursor(undoOp *op);

void editorUndo(void) {
  if (U.size == 0) {
    editorSetStatusMessage("Nothing to undo");
    return;
  }
  if (U.txn_depth > 0) {
    /* Defensive: a caller forgot to end a txn. Close it before undoing. */
    U.txn_depth = 0;
    U.txn_started = 0;
  }

  U.suppress++;
  undoOp first_to_undo;
  int have_first = 0;

  while (U.size > 0) {
    undoOp *op = &U.ops[U.size - 1];
    if (op->op == UNDO_INSERT) {
      int end_row, end_col;
      undoEndPosition(op->row, op->col, op->text, op->len, &end_row, &end_col);
      undoDeleteSpan(op->row, op->col, end_row, end_col);
    } else {
      undoInsertTextAt(op->row, op->col, op->text, op->len);
    }
    first_to_undo = *op;
    have_first = 1;
    int was_first = op->starts_group;
    undoFreeOp(op);
    U.size--;
    if (was_first)
      break;
  }
  U.suppress--;

  if (have_first)
    undoRestoreCursor(&first_to_undo);

  U.last_kind = UNDO_KIND_NONE;
  undoInvalidateSavePointIfNeeded();
  undoUpdateDirty();
  editorSetStatusMessage("Undo");
}

static void undoRestoreCursor(undoOp *op) {
  E.cx = op->cx;
  E.cy = op->cy;
  E.coloff = op->coloff;
  E.rowoff = op->rowoff;
  if (E.cx < 0)
    E.cx = 0;
  if (E.cy < 0)
    E.cy = 0;
  if (E.coloff < 0)
    E.coloff = 0;
  if (E.rowoff < 0)
    E.rowoff = 0;
}

/* Insert a row at the specified position, shifting the other rows on the bottom
 * if required. */
void editorInsertRow(int at, char *s, size_t len) {
  if (at > E.numrows)
    return;
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  if (at != E.numrows) {
    memmove(E.row + at + 1, E.row + at, sizeof(E.row[0]) * (E.numrows - at));
    for (int j = at + 1; j <= E.numrows; j++)
      E.row[j].idx++;
  }
  E.row[at].size = len;
  /* Try the chars arena first; fall back to malloc for big lines or when
   * the arena is full. Arena-backed lines stay there until the line is
   * modified (then charsMigrate copies them out). */
  E.row[at].chars = arenaAlloc(&g_chars_arena, len + 1);
  if (!E.row[at].chars)
    E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len + 1);
  E.row[at].hl = NULL;
  E.row[at].hl_oc = 0;
  E.row[at].meta = 0;
  E.row[at].render = NULL;
  E.row[at].rsize = 0;
  E.row[at].idx = at;
  E.numrows++;
  /* Keep the SoA parallel array in lockstep: shift entries to make room,
   * then write the new size. */
  if (tbGrowLineSizes(&E, E.numrows)) {
    if (at < E.numrows - 1) {
      memmove(E.line_sizes + at + 1, E.line_sizes + at,
              sizeof(int) * (size_t)(E.numrows - at - 1));
    }
    E.line_sizes[at] = (int)len;
  }
  editorUpdateRow(E.row + at);
  /* Index shifts invalidated any pre-existing dirty tracking from `at`
   * onward; mark everything from here to EOF dirty. (editorUpdateRow
   * already marked row `at` dirty, but downstream rows shifted indices.) */
  dirtyMarkFromHere(at, E.numrows);
  E.dirty++;
}

/* Free row's heap allocated stuff. */
void editorFreeRow(erow *row) {
  free(row->render);
  charsFree(row->chars);
  free(row->hl);
}

void editorFreeRows(void) {
  for (int j = 0; j < E.numrows; j++)
    editorFreeRow(&E.row[j]);
  free(E.row);
  E.row = NULL;
  E.numrows = 0;
  free(E.line_sizes);
  E.line_sizes = NULL;
  E.line_sizes_cap = 0;
  /* All arena-backed lines are gone; drop their bytes too. */
  arenaReset(&g_chars_arena);
  /* No rows means no dirty syntax. */
  dirtyClear();
}

/* Remove the row at the specified position, shifting the remainign on the
 * top. */
void editorDelRow(int at) {
  erow *row;

  if (at >= E.numrows)
    return;
  row = E.row + at;
  editorFreeRow(row);
  memmove(E.row + at, E.row + at + 1, sizeof(E.row[0]) * (E.numrows - at - 1));
  for (int j = at; j < E.numrows - 1; j++)
    E.row[j].idx = j;
  E.numrows--;
  /* Shift the SoA size array to match. */
  if (E.line_sizes && at < E.numrows) {
    memmove(E.line_sizes + at, E.line_sizes + at + 1,
            sizeof(int) * (size_t)(E.numrows - at));
  }
  /* Index shifts: every row from `at` down is now a different row than
   * before. Mark them all dirty so syntax recomputes. */
  dirtyMarkFromHere(at, E.numrows);
  g_render_gen++;
  E.dirty++;
}

/* Turn the editor rows into a single heap-allocated string.
 * Returns the pointer to the heap-allocated string and populate the
 * integer pointed by 'buflen' with the size of the string, escluding
 * the final nulterm. */
char *editorRowsToString(int *buflen) {
  char *buf = NULL, *p;
  int totlen = 0;
  int j;
  int rowcount = tbLineCount(TB);

  /* Compute count of bytes */
  for (j = 0; j < rowcount; j++)
    totlen += tbLineLength(TB, j) + 1; /* +1 is for "\n" at end of every row */
  *buflen = totlen;
  totlen++; /* Also make space for nulterm */

  p = buf = malloc(totlen);
  for (j = 0; j < rowcount; j++) {
    int sz = 0;
    const char *chars = tbLineSlice(TB, j, &sz);
    memcpy(p, chars, sz);
    p += sz;
    *p = '\n';
    p++;
  }
  *p = '\0';
  return buf;
}

void editorRowInsertCharImpl(erow *row, int at, int c);
/* Insert a character at the specified position in a row, moving the remaining
 * chars on the right if needed. */
void editorRowInsertChar(erow *row, int at, int c) {
  PROF_BEGIN(rowInsertChar);
  editorRowInsertCharImpl(row, at, c);
  PROF_END(rowInsertChar);
}
void editorRowInsertCharImpl(erow *row, int at, int c) {
  charsMigrate(row);
  if (at > row->size) {
    /* Pad the string with spaces if the insert location is outside the
     * current length by more than a single character. */
    int padlen = at - row->size;
    /* In the next line +2 means: new char and null term. */
    row->chars = realloc(row->chars, row->size + padlen + 2);
    memset(row->chars + row->size, ' ', padlen);
    row->chars[row->size + padlen + 1] = '\0';
    row->size += padlen + 1;
  } else {
    /* If we are in the middle of the string just make space for 1 new
     * char plus the (already existing) null term. */
    row->chars = realloc(row->chars, row->size + 2);
    memmove(row->chars + at + 1, row->chars + at, row->size - at + 1);
    row->size++;
  }
  row->chars[at] = c;
  tbStoreSize(&E, row->idx, row->size);
  editorUpdateRow(row);
  E.dirty++;
}

/* Append the string 's' at the end of a row */
void editorRowAppendString(erow *row, char *s, size_t len) {
  charsMigrate(row);
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(row->chars + row->size, s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  tbStoreSize(&E, row->idx, row->size);
  editorUpdateRow(row);
  E.dirty++;
}

/* Delete the character at offset 'at' from the specified row. */
void editorRowDelChar(erow *row, int at) {
  charsMigrate(row);
  if (row->size <= at)
    return;
  row->size--;
  memmove(row->chars + at, row->chars + at + 1, row->size - at + 1);
  tbStoreSize(&E, row->idx, row->size);
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowReplace(erow *row, int at, int len, const char *replacement,
                      int replacement_len) {
  if (at < 0 || len < 0 || at > row->size || at + len > row->size)
    return;

  if (len > 0)
    undoRecordDelete(row->idx, at, row->chars + at, len, UNDO_KIND_BLOCK);
  if (replacement_len > 0)
    undoRecordInsert(row->idx, at, replacement, replacement_len,
                     UNDO_KIND_BLOCK);

  charsMigrate(row);
  row->chars = realloc(row->chars, row->size - len + replacement_len + 1);
  memmove(row->chars + at + replacement_len, row->chars + at + len,
          row->size - at - len + 1);
  memcpy(row->chars + at, replacement, replacement_len);
  row->size = row->size - len + replacement_len;
  tbStoreSize(&E, row->idx, row->size);
  editorUpdateRow(row);
  E.dirty++;
  undoUpdateDirty();
}

/* Insert the specified char at the current prompt position. */
void editorInsertChar(int c) {
  int filerow = E.rowoff + E.cy;
  int filecol = E.coloff + E.cx;
  int rowcount = tbLineCount(TB);
  int row_present = filerow < rowcount;
  int existing_size = row_present ? tbLineLength(TB, filerow) : 0;

  /* Record undo BEFORE mutation. The cursor coordinates saved on the entry
   * point back to the pre-edit state. If the insert needs to pad rows or
   * spaces, treat the whole padded segment as one BLOCK op. */
  {
    int orig_numrows = rowcount;
    int newline_count = 0, space_pad = 0;
    int start_row = filerow, start_col = filecol;

    if (filerow >= orig_numrows) {
      newline_count = filerow - orig_numrows + 1;
      space_pad = filecol;
      start_row = orig_numrows > 0 ? orig_numrows - 1 : 0;
      start_col = orig_numrows > 0 ? tbLineLength(TB, orig_numrows - 1) : 0;
    } else if (filecol > existing_size) {
      space_pad = filecol - existing_size;
      start_col = existing_size;
    }

    if (newline_count > 0 || space_pad > 0) {
      int total = newline_count + space_pad + 1;
      char *buf = malloc((size_t)total);
      if (buf) {
        int p = 0;
        for (int i = 0; i < newline_count; i++)
          buf[p++] = '\n';
        for (int i = 0; i < space_pad; i++)
          buf[p++] = ' ';
        buf[p] = (char)c;
        undoRecordInsert(start_row, start_col, buf, total, UNDO_KIND_BLOCK);
        free(buf);
      }
    } else {
      char ch = (char)c;
      undoRecordInsert(filerow, filecol, &ch, 1, UNDO_KIND_INSERT_CHAR);
    }
  }

  /* If the row where the cursor is currently located does not exist in our
   * logical representaion of the file, add enough empty rows as needed. */
  if (!row_present) {
    while (tbLineCount(TB) <= filerow)
      tbInsertLine(TB, tbLineCount(TB), "", 0);
  }
  tbInsertChar(TB, filerow, filecol, c);
  if (E.cx == E.screencols - 1)
    E.coloff++;
  else
    E.cx++;
  E.dirty++;
  undoUpdateDirty();
}

/* Inserting a newline is slightly complex as we have to handle inserting a
 * newline in the middle of a line, splitting the line as needed. */
void editorInsertNewline(void) {
  int filerow = E.rowoff + E.cy;
  int filecol = E.coloff + E.cx;
  int rowcount = tbLineCount(TB);
  int row_size = filerow < rowcount ? tbLineLength(TB, filerow) : -1;

  if (row_size < 0) {
    if (filerow == rowcount) {
      if (rowcount > 0) {
        int prev = rowcount - 1;
        undoRecordInsert(prev, tbLineLength(TB, prev), "\n", 1,
                         UNDO_KIND_INSERT_NL);
      }
      tbInsertLine(TB, filerow, "", 0);
      goto fixcursor;
    }
    return;
  }
  /* If the cursor is over the current line size, we want to conceptually
   * think it's just over the last character. */
  if (filecol >= row_size)
    filecol = row_size;
  undoRecordInsert(filerow, filecol, "\n", 1, UNDO_KIND_INSERT_NL);
  if (filecol == 0) {
    tbInsertLine(TB, filerow, "", 0);
  } else {
    /* We are in the middle of a line. Split it between two rows. */
    erow *row = tbRowMut(TB, filerow);
    tbInsertLine(TB, filerow + 1, row->chars + filecol, row->size - filecol);
    row = tbRowMut(TB, filerow);
    /* Truncate in place. row->chars may point into the arena; writing the
     * NUL inside the line's own region is safe — we're only stomping on
     * bytes that belong to this row. No realloc needed. */
    row->chars[filecol] = '\0';
    row->size = filecol;
    tbStoreSize(&E, row->idx, row->size);
    editorUpdateRow(row);
  }
fixcursor:
  if (E.cy == E.screenrows - 1) {
    E.rowoff++;
  } else {
    E.cy++;
  }
  E.cx = 0;
  E.coloff = 0;
  undoUpdateDirty();
}

/* Delete the char at the current prompt position. */
void editorDelChar(void) {
  int filerow = E.rowoff + E.cy;
  int filecol = E.coloff + E.cx;
  int row_present = filerow < tbLineCount(TB);

  if (!row_present || (filecol == 0 && filerow == 0))
    return;
  if (filecol == 0) {
    /* Handle the case of column 0, we need to move the current line
     * on the right of the previous one. */
    int prev_size = tbLineLength(TB, filerow - 1);
    int row_size = 0;
    const char *row_chars = tbLineSlice(TB, filerow, &row_size);
    undoRecordDelete(filerow - 1, prev_size, "\n", 1, UNDO_KIND_DELETE_NL);
    filecol = prev_size;
    tbAppendToLine(TB, filerow - 1, row_chars, row_size);
    tbDeleteLine(TB, filerow);
    row_present = 0;
    if (E.cy == 0)
      E.rowoff--;
    else
      E.cy--;
    E.cx = filecol;
    if (E.cx >= E.screencols) {
      int shift = (E.screencols - E.cx) + 1;
      E.cx -= shift;
      E.coloff += shift;
    }
  } else {
    int row_size = 0;
    const char *row_chars = tbLineSlice(TB, filerow, &row_size);
    char ch = row_chars[filecol - 1];
    undoKind kind = U.delete_is_forward ? UNDO_KIND_DELETE_FWD
                                        : UNDO_KIND_DELETE_BACK;
    undoRecordDelete(filerow, filecol - 1, &ch, 1, kind);
    tbDeleteChar(TB, filerow, filecol - 1);
    if (E.cx == 0 && E.coloff)
      E.coloff--;
    else
      E.cx--;
  }
  if (row_present)
    editorUpdateRow(tbRowMut(TB, filerow));
  E.dirty++;
  undoUpdateDirty();
}

static char *editorReadFileLine(FILE *fp, size_t *len_out) {
  size_t cap = 128;
  size_t len = 0;
  char *line = malloc(cap);
  int c;

  if (!line)
    return NULL;

  while ((c = fgetc(fp)) != EOF) {
    if (len + 1 >= cap) {
      char *new_line;

      cap *= 2;
      new_line = realloc(line, cap);
      if (!new_line) {
        free(line);
        return NULL;
      }
      line = new_line;
    }
    if (c == '\n')
      break;
    line[len++] = (char)c;
  }

  if (c == EOF && len == 0) {
    free(line);
    return NULL;
  }
  if (len && line[len - 1] == '\r')
    len--;
  line[len] = '\0';
  *len_out = len;
  return line;
}

static time_t editorPathMtime(const char *path) {
  struct stat st;
  if (!path || stat(path, &st) != 0)
    return 0;
  return st.st_mtime;
}

/* Load the specified program in the editor memory and returns 0 on success
 * or 1 on error. */
int editorOpen(char *filename) {
  FILE *fp;

  editorFreeRows();
  undoClear();
  E.cx = 0;
  E.cy = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.syntax = NULL;
  E.dirty = 0;
  E.file_mtime = 0;
  free(E.filename);
  size_t fnlen = strlen(filename) + 1;
  E.filename = malloc(fnlen);
  memcpy(E.filename, filename, fnlen);
  editorSelectSyntaxHighlight(filename);

  fp = fopen(filename, "r");
  if (!fp) {
    if (errno != ENOENT) {
      perror("Opening file");
      exit(1);
    }
    return 1;
  }

  while (1) {
    size_t linelen = 0;
    char *line = editorReadFileLine(fp, &linelen);

    if (!line)
      break;
    tbInsertLine(TB, tbLineCount(TB), line, linelen);
    free(line);
  }
  fclose(fp);
  E.dirty = 0;
  E.file_mtime = editorPathMtime(filename);
  return 0;
}

/* Save the current file on disk. Return 0 on success, 1 on error. */
int editorSave(void) {
  int len;
  char *buf = editorRowsToString(&len);
  FILE *fp = fopen(E.filename, "wb");

  if (!fp)
    goto writeerr;
  if (len > 0 && fwrite(buf, 1, len, fp) != (size_t)len)
    goto writeerr;
  if (fclose(fp) != 0) {
    fp = NULL;
    goto writeerr;
  }

  free(buf);
  E.dirty = 0;
  E.file_mtime = editorPathMtime(E.filename);
  undoMarkSaved();
  editorSetStatusMessage("%d bytes written on disk", len);
  return 0;

writeerr:
  free(buf);
  if (fp)
    fclose(fp);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
  return 1;
}

/* ============================= Terminal update ============================ */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
  char *b;
  int len;
};

#ifndef bento_RAYLIB

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL)
    return;
  memcpy(new + ab->len, s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) { free(ab->b); }

/* This function writes the whole screen using VT100 escape characters
 * starting from the logical state of the editor in the global state 'E'. */
void editorRefreshScreen(void) {
  int y;
  char buf[32];
  struct abuf ab = ABUF_INIT;
  int rowcount = tbLineCount(TB);

  /* Bring syntax highlights up-to-date for everything we're about to draw.
   * Off-screen dirty rows stay dirty until they scroll into view. */
  syntaxRefreshUpTo(E.rowoff + E.screenrows);

  abAppend(&ab, "\x1b[?25l", 6); /* Hide cursor. */
  abAppend(&ab, "\x1b[H", 3);    /* Go home. */
  for (y = 0; y < E.screenrows; y++) {
    int filerow = E.rowoff + y;

    if (filerow >= rowcount) {
      if (rowcount == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen =
            snprintf(welcome, sizeof(welcome),
                     "bento editor -- version %s\x1b[0K\r\n", bento_VERSION);
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(&ab, "~", 1);
          padding--;
        }
        while (padding--)
          abAppend(&ab, " ", 1);
        abAppend(&ab, welcome, welcomelen);
      } else {
        abAppend(&ab, "~\x1b[0K\r\n", 7);
      }
      continue;
    }

    int rsize = 0;
    const char *render = tbRenderSlice(TB, filerow, &rsize);
    const unsigned char *row_hl = tbHighlight(TB, filerow);

    int len = rsize - E.coloff;
    int current_color = -1;
    if (len > 0) {
      if (len > E.screencols)
        len = E.screencols;
      const char *c = render + E.coloff;
      const unsigned char *hl = row_hl + E.coloff;
      int j;
      for (j = 0; j < len; j++) {
        if (hl[j] == HL_NONPRINT) {
          char sym;
          abAppend(&ab, "\x1b[7m", 4);
          if (c[j] <= 26)
            sym = '@' + c[j];
          else
            sym = '?';
          abAppend(&ab, &sym, 1);
          abAppend(&ab, "\x1b[0m", 4);
        } else if (hl[j] == HL_NORMAL) {
          if (current_color != -1) {
            abAppend(&ab, "\x1b[39m", 5);
            current_color = -1;
          }
          abAppend(&ab, c + j, 1);
        } else {
          int color = editorSyntaxToColor(hl[j]);
          if (color != current_color) {
            char buf[16];
            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
            current_color = color;
            abAppend(&ab, buf, clen);
          }
          abAppend(&ab, c + j, 1);
        }
      }
    }
    abAppend(&ab, "\x1b[39m", 5);
    abAppend(&ab, "\x1b[0K", 4);
    abAppend(&ab, "\r\n", 2);
  }

  /* Create a two rows status. First row: */
  abAppend(&ab, "\x1b[0K", 4);
  abAppend(&ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename,
                     tbLineCount(TB), E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.rowoff + E.cy + 1,
                      tbLineCount(TB));
  if (len > E.screencols)
    len = E.screencols;
  abAppend(&ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(&ab, rstatus, rlen);
      break;
    } else {
      abAppend(&ab, " ", 1);
      len++;
    }
  }
  abAppend(&ab, "\x1b[0m\r\n", 6);

  /* Second row depends on E.statusmsg and the status message update time. */
  abAppend(&ab, "\x1b[0K", 4);
  int msglen = strlen(E.statusmsg);
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(&ab, E.statusmsg, msglen <= E.screencols ? msglen : E.screencols);

  /* Put cursor at its current position. Note that the horizontal position
   * at which the cursor is displayed may be different compared to 'E.cx'
   * because of TABs. */
  int j;
  int cx = 1;
  int filerow = E.rowoff + E.cy;
  int line_size = 0;
  const char *line_chars = tbLineSlice(TB, filerow, &line_size);
  if (line_chars) {
    for (j = E.coloff; j < (E.cx + E.coloff); j++) {
      if (j < line_size && line_chars[j] == TAB)
        cx += 7 - ((cx) % 8);
      cx++;
    }
  }
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, cx);
  abAppend(&ab, buf, strlen(buf));
  abAppend(&ab, "\x1b[?25h", 6); /* Show cursor. */
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

#endif /* bento_RAYLIB */

/* Set an editor status message for the second line of the status, at the
 * end of the screen. */
void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/* =============================== Find mode ================================ */

#define bento_QUERY_LEN 256

#ifndef bento_RAYLIB

void editorFind(int fd) {
  char query[bento_QUERY_LEN + 1] = {0};
  int qlen = 0;
  int last_match = -1;    /* Last line where a match was found. -1 for none. */
  int find_next = 0;      /* if 1 search next, if -1 search prev. */
  int saved_hl_line = -1; /* No saved HL */
  char *saved_hl = NULL;

#define FIND_RESTORE_HL                                                        \
  do {                                                                         \
    if (saved_hl) {                                                            \
      erow *_r = tbRowMut(TB, saved_hl_line);                                  \
      if (_r) memcpy(_r->hl, saved_hl, _r->rsize);                             \
      free(saved_hl);                                                          \
      saved_hl = NULL;                                                         \
    }                                                                          \
  } while (0)

  /* Save the cursor position in order to restore it later. */
  int saved_cx = E.cx, saved_cy = E.cy;
  int saved_coloff = E.coloff, saved_rowoff = E.rowoff;

  while (1) {
    editorSetStatusMessage("Search: %s (Use ESC/Arrows/Enter)", query);
    editorRefreshScreen();

    int c = editorReadKey(fd);
    if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
      if (qlen != 0)
        query[--qlen] = '\0';
      last_match = -1;
    } else if (c == ESC || c == ENTER) {
      if (c == ESC) {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
      }
      FIND_RESTORE_HL;
      editorSetStatusMessage("");
      return;
    } else if (c == ARROW_RIGHT || c == ARROW_DOWN) {
      find_next = 1;
    } else if (c == ARROW_LEFT || c == ARROW_UP) {
      find_next = -1;
    } else if (isprint(c)) {
      if (qlen < bento_QUERY_LEN) {
        query[qlen++] = c;
        query[qlen] = '\0';
        last_match = -1;
      }
    }

    /* Search occurrence. */
    if (last_match == -1)
      find_next = 1;
    if (find_next) {
      char *match = NULL;
      int match_offset = 0;
      int i, current = last_match;

      int rowcount = tbLineCount(TB);
      for (i = 0; i < rowcount; i++) {
        current += find_next;
        if (current == -1)
          current = rowcount - 1;
        else if (current == rowcount)
          current = 0;
        const char *render_cur = tbRenderSlice(TB, current, NULL);
        match = render_cur ? strstr(render_cur, query) : NULL;
        if (match) {
          match_offset = match - render_cur;
          break;
        }
      }
      find_next = 0;

      /* Highlight */
      FIND_RESTORE_HL;

      if (match) {
        /* Make sure the matched row has fresh hl before we overlay MATCH
         * bytes onto it (deferred-syntax model means hl may be stale). */
        syntaxRefreshUpTo(current + 1);
        erow *row = tbRowMut(TB, current);
        last_match = current;
        if (row && row->hl) {
          saved_hl_line = current;
          saved_hl = malloc(row->rsize);
          memcpy(saved_hl, row->hl, row->rsize);
          memset(row->hl + match_offset, HL_MATCH, qlen);
        }
        E.cy = 0;
        E.cx = match_offset;
        E.rowoff = current;
        E.coloff = 0;
        /* Scroll horizontally as needed. */
        if (E.cx > E.screencols) {
          int diff = E.cx - E.screencols;
          E.cx -= diff;
          E.coloff += diff;
        }
      }
    }
  }
}

#endif /* bento_RAYLIB */

/* ========================= Editor events handling  ======================== */

/* Handle cursor position change because arrow keys were pressed. */
void editorMoveCursor(int key) {
  int filerow = E.rowoff + E.cy;
  int filecol = E.coloff + E.cx;
  int rowlen;
  int rowcount = tbLineCount(TB);
  int row_size = tbLineLength(TB, filerow);
  int row_present = filerow < rowcount;

  switch (key) {
  case ARROW_LEFT:
    if (E.cx == 0) {
      if (E.coloff) {
        E.coloff--;
      } else {
        if (filerow > 0) {
          E.cy--;
          E.cx = tbLineLength(TB, filerow - 1);
          if (E.cx > E.screencols - 1) {
            E.coloff = E.cx - E.screencols + 1;
            E.cx = E.screencols - 1;
          }
        }
      }
    } else {
      E.cx -= 1;
    }
    break;
  case ARROW_RIGHT:
    if (row_present && filecol < row_size) {
      if (E.cx == E.screencols - 1) {
        E.coloff++;
      } else {
        E.cx += 1;
      }
    } else if (row_present && filecol == row_size) {
      E.cx = 0;
      E.coloff = 0;
      if (E.cy == E.screenrows - 1) {
        E.rowoff++;
      } else {
        E.cy += 1;
      }
    }
    break;
  case ARROW_UP:
    if (E.cy == 0) {
      if (E.rowoff)
        E.rowoff--;
    } else {
      E.cy -= 1;
    }
    break;
  case ARROW_DOWN:
    if (filerow < rowcount) {
      if (E.cy == E.screenrows - 1) {
        E.rowoff++;
      } else {
        E.cy += 1;
      }
    }
    break;
  }
  /* Fix cx if the current line has not enough chars. */
  filerow = E.rowoff + E.cy;
  filecol = E.coloff + E.cx;
  rowlen = tbLineLength(TB, filerow);
  if (filecol > rowlen) {
    E.cx -= filecol - rowlen;
    if (E.cx < 0) {
      E.coloff += E.cx;
      E.cx = 0;
    }
  }
}

/* Process events arriving from the standard input, which is, the user
 * is typing stuff on the terminal. */
#define bento_QUIT_TIMES 3

#ifndef bento_RAYLIB

void editorProcessKeypress(int fd) {
  /* When the file is modified, requires Ctrl-q to be pressed N times
   * before actually quitting. */
  static int quit_times = bento_QUIT_TIMES;

  int c = editorReadKey(fd);
  switch (c) {
  case ENTER: /* Enter */
    editorInsertNewline();
    break;
  case CTRL_C: /* Ctrl-c */
    /* We ignore ctrl-c, it can't be so simple to lose the changes
     * to the edited file. */
    break;
  case CTRL_Q: /* Ctrl-q */
    /* Quit if the file was already saved. */
    if (E.dirty && quit_times) {
      editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                             "Press Ctrl-Q %d more times to quit.",
                             quit_times);
      quit_times--;
      return;
    }
    exit(0);
    break;
  case CTRL_S: /* Ctrl-s */
    editorSave();
    break;
  case CTRL_F:
    editorFind(fd);
    break;
  case CTRL_Z: /* Ctrl-z (undo) */
    editorUndo();
    break;
  case BACKSPACE: /* Backspace */
  case CTRL_H:    /* Ctrl-h */
  case DEL_KEY:
    editorDelChar();
    break;
  case PAGE_UP:
  case PAGE_DOWN:
    if (c == PAGE_UP && E.cy != 0)
      E.cy = 0;
    else if (c == PAGE_DOWN && E.cy != E.screenrows - 1)
      E.cy = E.screenrows - 1;
    {
      int times = E.screenrows;
      while (times--)
        editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
    break;

  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    editorMoveCursor(c);
    break;
  case CTRL_L: /* ctrl+l, clear screen */
    /* Just refresht the line as side effect. */
    break;
  case ESC:
    /* Nothing to do for ESC in this mode. */
    break;
  default:
    editorInsertChar(c);
    break;
  }

  quit_times = bento_QUIT_TIMES; /* Reset it to the original value. */
}

#endif /* bento_RAYLIB */

int editorFileWasModified(void) { return E.dirty; }

#ifndef bento_RAYLIB

void updateWindowSize(void) {
  if (getWindowSize(STDIN_FILENO, STDOUT_FILENO, &E.screenrows,
                    &E.screencols) == -1) {
    perror("Unable to query the screen for size (columns / rows)");
    exit(1);
  }
  E.screenrows -= 2; /* Get room for status bar. */
}

void handleSigWinCh(int unused __attribute__((unused))) {
  updateWindowSize();
  if (E.cy > E.screenrows)
    E.cy = E.screenrows - 1;
  if (E.cx > E.screencols)
    E.cx = E.screencols - 1;
  editorRefreshScreen();
}

#endif /* bento_RAYLIB */

void initEditor(void) {
  E.cx = 0;
  E.cy = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.syntax = NULL;
  tbInit(TB, &E);
  undoInit();
#ifndef bento_RAYLIB
  updateWindowSize();
  signal(SIGWINCH, handleSigWinCh);
#else
  E.screenrows = 1;
  E.screencols = 1;
  E.rawmode = 0;
#endif
}

/* ============================== Benchmark ===============================
 *
 * Deterministic stress workload, invoked with `bento --bench <file>`. Runs
 * with no terminal/window — initializes the editor state directly with a
 * fixed viewport, then times: file load, hl_oc cascade trigger, full scroll,
 * 1000 keystrokes, full syntax recompute, full rowsToString save.
 *
 * Lets you measure before/after architectural changes from one binary.
 */

static double benchDiffMs(struct timespec a, struct timespec b) {
  return (b.tv_sec - a.tv_sec) * 1000.0 +
         (b.tv_nsec - a.tv_nsec) / 1.0e6;
}

static void benchInitEditor(void) {
  E.cx = 0;
  E.cy = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.syntax = NULL;
  tbInit(TB, &E);
  undoInit();
  /* Fixed viewport: 50 rows × 120 cols. Larger than typical so the lazy
   * refresh has visible work to do. */
  E.screenrows = 50;
  E.screencols = 120;
  E.rawmode = 0;
}

static void benchRun(const char *filename) {
  struct timespec t0, t1;

  benchInitEditor();

  clock_gettime(CLOCK_MONOTONIC, &t0);
  editorSelectSyntaxHighlight((char *)filename);
  editorOpen((char *)filename);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  int line_count = tbLineCount(TB);
  printf("load            : %6d lines in %8.3f ms\n", line_count,
         benchDiffMs(t0, t1));

  /* hl_oc cascade trigger: type an open-comment at row 0. Pre-Phase-5
   * code re-highlighted every line in the file synchronously; with
   * deferred dirty + lazy refresh, the cost is bounded to the viewport. */
  E.cx = E.cy = E.rowoff = E.coloff = 0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  editorInsertChar('/');
  editorInsertChar('*');
  syntaxRefreshUpTo(E.rowoff + E.screenrows);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  printf("cascade top     : %8.3f ms (mark + refresh viewport)\n",
         benchDiffMs(t0, t1));

  /* Walk the whole file viewport-by-viewport: dirty range propagates as
   * each row's hl_oc updates downstream. */
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (int i = 0; i < line_count; i++) {
    E.rowoff = i;
    syntaxRefreshUpTo(E.rowoff + E.screenrows);
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  printf("scroll EOF      : %8.3f ms (refresh %d viewports)\n",
         benchDiffMs(t0, t1), line_count);

  /* 1000 keystrokes near the middle of the file. */
  E.cx = E.cy = E.coloff = 0;
  E.rowoff = line_count / 2;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (int i = 0; i < 1000; i++)
    editorInsertChar((i % 26) + 'a');
  clock_gettime(CLOCK_MONOTONIC, &t1);
  printf("1000 keystrokes : %8.3f ms (%.3f us/key)\n",
         benchDiffMs(t0, t1), benchDiffMs(t0, t1));

  /* Force full syntax recompute (every row dirty, walk to EOF). */
  dirtyMarkFromHere(0, tbLineCount(TB));
  clock_gettime(CLOCK_MONOTONIC, &t0);
  syntaxRefreshAll();
  clock_gettime(CLOCK_MONOTONIC, &t1);
  printf("syntaxRefreshAll: %8.3f ms\n", benchDiffMs(t0, t1));

  /* File save path. */
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int len = 0;
  char *buf = editorRowsToString(&len);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  free(buf);
  printf("rowsToString    : %8.3f ms (%d bytes)\n", benchDiffMs(t0, t1), len);

  profDump();
}

#ifdef bento_RAYLIB

#define GUI_WINDOW_WIDTH 1120
#define GUI_WINDOW_HEIGHT 760
#define GUI_TAB_BAR_HEIGHT 36
#define GUI_FIND_BAR_HEIGHT 72
#define GUI_FIND_RESULTS_HEIGHT 132
#define GUI_STATUS_BAR_HEIGHT 24
#define GUI_TEXT_PADDING_X 12
#define GUI_TEXT_PADDING_Y 8
#define GUI_MIN_GUTTER_WIDTH 56
#define GUI_SIDEBAR_WIDTH 240
#define GUI_TREE_ROW_HEIGHT 22
#define GUI_FONT_SIZE 18.0f
#define GUI_SMALL_FONT_SIZE 14.0f
#define GUI_FONT_SPACING 0.0f
#define GUI_EDITOR_WHEEL_LINES 3.0f
#define GUI_MAX_FIND_RESULTS 2048
#define GUI_MAX_PROJECT_FILE_SIZE (4 * 1024 * 1024)
#define GUI_INPUT_LEN 256
#define GUI_SIGNATURE_NAME_LEN 64
#define GUI_SIGNATURE_TEXT_LEN 256
#define GUI_MAX_PROJECT_SIGNATURES 4096
#define GUI_RECENT_FILES_MAX 20
#define GUI_RECENT_PROJECTS_MAX 10
#define GUI_COMMANDS_MAX 64
#define GUI_SETTINGS_COUNT 6
#define GUI_CONTEXT_ITEMS_MAX 8

typedef struct guiFileEntry {
  char path[PATH_MAX];
  char name[256];
  int depth;
  int is_dir;
} guiFileEntry;

typedef struct guiMatch {
  int file_index;
  int row;
  int col;
  char preview[256];
} guiMatch;

typedef struct guiSignature {
  int language;
  char name[GUI_SIGNATURE_NAME_LEN];
  char signature[GUI_SIGNATURE_TEXT_LEN];
  char path[PATH_MAX];
  int row;
} guiSignature;

typedef struct guiTheme {
  Color bg;
  Color panel;
  Color panel_dark;
  Color current_line;
  Color gutter_text;
  Color text;
  Color dim_text;
  Color accent;
  Color border;
  Color match_bg;
  Color selected;
  Color text_selection;
  Color overlay_bg;
} guiTheme;

typedef struct guiContextItem {
  const char *label;
  int command;
  int enabled;
} guiContextItem;

enum guiOverlay {
  GUI_OVERLAY_NONE = 0,
  GUI_OVERLAY_QUICK_OPEN,
  GUI_OVERLAY_GOTO_LINE,
  GUI_OVERLAY_COMMAND_PALETTE,
  GUI_OVERLAY_RECENT,
  GUI_OVERLAY_SETTINGS,
  GUI_OVERLAY_PROMPT
};

enum guiPromptKind {
  GUI_PROMPT_NONE = 0,
  GUI_PROMPT_OPEN_PATH,
  GUI_PROMPT_NEW_FILE,
  GUI_PROMPT_NEW_FOLDER,
  GUI_PROMPT_RENAME,
  GUI_PROMPT_DELETE
};

enum guiContextMenuKind {
  GUI_CONTEXT_NONE = 0,
  GUI_CONTEXT_EDITOR,
  GUI_CONTEXT_TREE,
  GUI_CONTEXT_TAB
};

enum guiCommand {
  GUI_CMD_NONE = 0,
  GUI_CMD_NEW_FILE,
  GUI_CMD_NEW_WINDOW,
  GUI_CMD_CLOSE_WORKSPACE,
  GUI_CMD_OPEN_FILE,
  GUI_CMD_OPEN_FOLDER,
  GUI_CMD_OPEN_RECENT,
  GUI_CMD_QUICK_OPEN,
  GUI_CMD_GOTO_LINE,
  GUI_CMD_FIND,
  GUI_CMD_FIND_PROJECT,
  GUI_CMD_SAVE,
  GUI_CMD_SAVE_ALL,
  GUI_CMD_CLOSE_TAB,
  GUI_CMD_REOPEN_CLOSED,
  GUI_CMD_TOGGLE_SIDEBAR,
  GUI_CMD_TOGGLE_DEBUG_HUD,
  GUI_CMD_FONT_INCREASE,
  GUI_CMD_FONT_DECREASE,
  GUI_CMD_FONT_RESET,
  GUI_CMD_TOGGLE_THEME,
  GUI_CMD_TOGGLE_WHITESPACE,
  GUI_CMD_SETTINGS,
  GUI_CMD_SELECT_ALL,
  GUI_CMD_DUPLICATE_LINE,
  GUI_CMD_MOVE_LINE_UP,
  GUI_CMD_MOVE_LINE_DOWN,
  GUI_CMD_TOGGLE_COMMENT,
  GUI_CMD_INDENT,
  GUI_CMD_OUTDENT,
  GUI_CMD_TRIM_TRAILING_WS,
  GUI_CMD_SELECT_NEXT,
  GUI_CMD_ABOUT,
  GUI_CMD_FILE_NEW,
  GUI_CMD_FOLDER_NEW,
  GUI_CMD_RENAME,
  GUI_CMD_DELETE,
  GUI_CMD_REVEAL,
  GUI_CMD_REFRESH_TREE,
  GUI_CMD_PIN_TAB,
  GUI_CMD_CLOSE_OTHERS,
  GUI_CMD_CLOSE_SAVED
};

typedef struct guiCommandDef {
  int command;
  const char *name;
  const char *shortcut;
} guiCommandDef;

static const guiCommandDef GUI_COMMAND_DEFS[] = {
    {GUI_CMD_NEW_FILE, "New File", "Cmd/Ctrl+N"},
    {GUI_CMD_NEW_WINDOW, "New Bento Window", "Cmd/Ctrl+Shift+N"},
    {GUI_CMD_CLOSE_WORKSPACE, "Close Workspace", "Cmd/Ctrl+Shift+W"},
    {GUI_CMD_OPEN_FILE, "Open File...", "Cmd/Ctrl+O"},
    {GUI_CMD_OPEN_FOLDER, "Open Folder...", "Cmd/Ctrl+Shift+O"},
    {GUI_CMD_OPEN_RECENT, "Open Recent", "Cmd/Ctrl+Shift+R"},
    {GUI_CMD_QUICK_OPEN, "Quick Open In Project", "Cmd/Ctrl+P"},
    {GUI_CMD_GOTO_LINE, "Go To Line", "Cmd/Ctrl+G"},
    {GUI_CMD_FIND, "Find", "Cmd/Ctrl+F"},
    {GUI_CMD_FIND_PROJECT, "Find In Project", "Cmd/Ctrl+Shift+F"},
    {GUI_CMD_SAVE, "Save", "Cmd/Ctrl+S"},
    {GUI_CMD_SAVE_ALL, "Save All", "Alt+Cmd/Ctrl+S"},
    {GUI_CMD_CLOSE_TAB, "Close Tab", "Cmd/Ctrl+W"},
    {GUI_CMD_REOPEN_CLOSED, "Reopen Closed Tab", "Cmd/Ctrl+Shift+T"},
    {GUI_CMD_SETTINGS, "Settings", "Cmd/Ctrl+,"},
    {GUI_CMD_TOGGLE_SIDEBAR, "Toggle Sidebar", "Cmd/Ctrl+B"},
    {GUI_CMD_TOGGLE_DEBUG_HUD, "Toggle Performance HUD", "Cmd/Ctrl+Shift+D"},
    {GUI_CMD_FONT_INCREASE, "Increase Font Size", "Cmd/Ctrl+="},
    {GUI_CMD_FONT_DECREASE, "Decrease Font Size", "Cmd/Ctrl+-"},
    {GUI_CMD_FONT_RESET, "Reset Font Size", ""},
    {GUI_CMD_TOGGLE_THEME, "Switch Theme", ""},
    {GUI_CMD_TOGGLE_WHITESPACE, "Show Whitespace", ""},
    {GUI_CMD_SELECT_ALL, "Select All", "Cmd/Ctrl+A"},
    {GUI_CMD_DUPLICATE_LINE, "Duplicate Line", "Cmd/Ctrl+D"},
    {GUI_CMD_MOVE_LINE_UP, "Move Line Up", "Alt+Up"},
    {GUI_CMD_MOVE_LINE_DOWN, "Move Line Down", "Alt+Down"},
    {GUI_CMD_TOGGLE_COMMENT, "Toggle Comment", "Cmd/Ctrl+/"},
    {GUI_CMD_INDENT, "Indent Line", "Tab"},
    {GUI_CMD_OUTDENT, "Outdent Line", "Shift+Tab"},
    {GUI_CMD_TRIM_TRAILING_WS, "Trim Trailing Whitespace", ""},
    {GUI_CMD_SELECT_NEXT, "Select Next Occurrence", "Cmd/Ctrl+E"},
    {GUI_CMD_REFRESH_TREE, "Refresh Files", ""},
    {GUI_CMD_ABOUT, "About Bento", ""},
    {GUI_CMD_NONE, NULL, NULL}};

typedef struct guiConfig {
  Font font;
  int font_owned;
  guiTheme theme;
  char font_label[128];
  float font_size;
  float small_font_size;
  float char_width;
  /* Width of one glyph in the small (gutter / status-bar) font. Measured
   * once per layout pass so the gutter loop doesn't call MeasureTextEx
   * per visible row. */
  float small_char_width;
  float line_height;
  int find_active;
  int find_all_active;
  int find_project_active;
  int find_dirty;
  int find_focus;
  int find_editor_focus;
  int find_case_sensitive;
  int find_whole_word;
  char query[bento_QUERY_LEN + 1];
  int query_len;
  char replacement[bento_QUERY_LEN + 1];
  int replacement_len;
  guiMatch matches[GUI_MAX_FIND_RESULTS];
  int match_count;
  int match_overflow;
  int current_match;
  int match_scroll;
  guiFileEntry *files;
  int file_count;
  int file_cap;
  guiSignature *signatures;
  int signature_count;
  int signature_cap;
  int signature_dirty;
  int tree_scroll;
  int sidebar_width;
  char cwd[PATH_MAX];
  char launch_path[PATH_MAX];
  char last_closed_file[PATH_MAX];
  int project_open;
  int transient_session;
  int path_picker_active;
  int overlay;
  char overlay_query[GUI_INPUT_LEN + 1];
  int overlay_query_len;
  int overlay_selected;
  int overlay_scroll;
  int welcome_active;
  int settings_selected;
  int theme_index;
  int show_whitespace;
  int tab_width;
  int restore_session;
  char recent_files[GUI_RECENT_FILES_MAX][PATH_MAX];
  int recent_file_count;
  char recent_projects[GUI_RECENT_PROJECTS_MAX][PATH_MAX];
  int recent_project_count;
  int prompt_kind;
  char prompt_title[96];
  char prompt_value[GUI_INPUT_LEN + 1];
  int prompt_value_len;
  char prompt_base_path[PATH_MAX];
  char prompt_target_path[PATH_MAX];
  int context_menu;
  int context_x;
  int context_y;
  int context_target_index;
  int context_tab_index;
  guiContextItem context_items[GUI_CONTEXT_ITEMS_MAX];
  int context_item_count;
  int external_save_confirm;
  int selection_active;
  int selection_anchor_row;
  int selection_anchor_col;
  int selection_cursor_row;
  int selection_cursor_col;
  int mouse_selecting;
  int last_editor_click_row;
  int last_editor_click_col;
  double last_editor_click_time;
  int quit_times;
  int should_close;
  int repeat_key;
  double repeat_next;
  float wheel_row_accum;
  float wheel_col_accum;
  float wheel_tree_accum;
  float wheel_match_accum;
  float wheel_velocity;
  double wheel_last_time;
  float editor_scroll_y;
  /* Cursor blink: pulse like a heartbeat when idle, stay solid while moving. */
  int last_cursor_row;
  int last_cursor_col;
  double last_cursor_move_time;
  /* Event-driven render loop: track activity so we can sleep the thread
   * when nothing changes instead of busy-rendering at 60+ Hz. */
  double last_activity_time;
  float last_mouse_x, last_mouse_y;
  unsigned long long last_state_hash;
  int idle_mode; /* 0 = active rendering, 1 = blocked on glfwWaitEvents */
  int last_cursor_visible; /* whether the blink rendered visible last frame */
  /* Debug HUD: toggled with Ctrl-Shift-D. Per-frame counters get reset at
   * the top of guiDrawFrame and read by guiDrawDebugHud. */
  int debug_hud;
  double dbg_frame_ms;        /* last full-frame time */
  /* Editor-pass sub-buckets (inside guiDrawEditorRows): */
  double dbg_emit_us;
  double dbg_dispatch_us;
  double dbg_syntax_us;
  int dbg_draw_calls;
  int dbg_run_count;
  int dbg_glyph_count;
  /* guiDrawFrame top-level sub-buckets — these decompose "other" so we
   * can see whether time is in sidebar, status, EndDrawing, etc. */
  double dbg_layout_us;       /* guiUpdateLayout */
  double dbg_begin_us;        /* BeginDrawing */
  double dbg_clear_us;        /* ClearBackground */
  double dbg_sidebar_us;
  double dbg_topbar_us;
  double dbg_findbar_us;      /* guiDrawFindBar + guiDrawFindResults */
  double dbg_editor_us;       /* guiDrawEditorRows total (incl. emit/disp/syntax) */
  double dbg_cursor_us;
  double dbg_hint_us;
  double dbg_status_us;
  double dbg_overlay_us;
  double dbg_end_us;          /* EndDrawing (vsync + swap) */
  double dbg_gutter_us;       /* gutter loop inside editor pass */
  double dbg_edit_us;         /* (bench-set) time in editorInsertChar */
  double dbg_frame_window[64];
  int dbg_frame_window_idx;
  /* Function-hint cache. Resolve at most once per frame and reuse across
   * frames as long as the cursor's call-token is unchanged. */
  char hint_token[GUI_SIGNATURE_NAME_LEN];
  char hint_text[GUI_SIGNATURE_TEXT_LEN];
  int  hint_valid;
  int  hint_numrows_at;
  const char *hint_filename_at;
  /* Status-bar right-side cache: avoids MeasureTextEx every frame when the
   * (filerow, filecol, numrows) tuple hasn't moved. */
  int  statusbar_cached_row;
  int  statusbar_cached_col;
  int  statusbar_cached_numrows;
  char statusbar_cached_text[128];
  float statusbar_cached_width;
  /* Run-list cache. When the viewport (rowoff/coloff/scroll_y/screencols/
   * screenrows) and content (g_render_gen, defined outside G so the
   * editor code can bump it without seeing the GUI struct) are unchanged
   * from last frame, we skip guiEmitRowRuns and just re-dispatch the
   * existing g_runs. */
  unsigned long long runs_cache_gen;
  int runs_cache_rowoff;
  int runs_cache_coloff;
  int runs_cache_screencols;
  int runs_cache_screenrows;
  int runs_cache_scroll_y_q; /* scroll_y quantized to pixels */
  int runs_cache_rowcount;
  int runs_cache_valid;
} guiConfig;

static guiConfig G;

/* ============================== Tabs ====================================
 *
 * A tab owns one buffer worth of editor state. Only the currently active
 * tab's state lives in E / U; all the others sleep in `tabs[]`. Switching
 * tabs is a pointer-shuffle, not a copy.
 *
 * Preview / pin model (VS Code-style):
 *  - There is at most one "preview" tab at a time. Single-clicking a file
 *    in the sidebar replaces the preview tab's content with the new file.
 *  - Double-clicking a file in the sidebar (or a tab title) pins it. Pinned
 *    tabs stick around when other files are clicked.
 *  - Editing a preview tab auto-pins it (so unsaved work doesn't get
 *    silently replaced by the next single-click). */

#define BENTO_MAX_TABS 32

typedef struct guiTab {
  int in_use;
  int pinned;
  char filename[PATH_MAX];
  /* Saved state — meaningful only when this is NOT the active tab. */
  int cx, cy, rowoff, coloff;
  int numrows;
  erow *row;
  int dirty;
  struct editorSyntax *syntax;
  time_t file_mtime;
  /* Undo state, similarly only meaningful when inactive. */
  undoOp *undo_ops;
  int undo_size;
  int undo_cap;
  int undo_save_point;
  undoKind undo_last_kind;
  int undo_last_row, undo_last_col, undo_last_len;
  /* Layout cache for hit-testing — populated by the tab bar each draw. */
  int rect_x, rect_y, rect_w, rect_h;
  int close_x, close_y, close_size;
} guiTab;

typedef struct guiTabs {
  guiTab list[BENTO_MAX_TABS];
  int count;        /* number of in_use slots */
  int active;       /* index of currently active slot, or -1 if none */
  int preview;      /* index of the preview tab, or -1 if none */
  /* Double-click detection on sidebar entries. */
  int last_click_file_index;
  double last_click_time;
  /* Double-click detection on tab titles. */
  int last_tab_click_index;
  double last_tab_click_time;
  /* "Press Cmd-W again to close" dirty-tab confirmation. */
  int close_confirm_index;
  double close_confirm_time;
} guiTabs;

static guiTabs T;

static void guiClearSelection(void);
static void guiAddRecentFile(const char *path);
static void guiAddRecentProject(const char *path);
static void guiSaveSettings(void);
static void guiSaveRecents(void);
static void guiInitSettingsDefaults(void);
static void guiLoadSettings(void);
static void guiLoadRecents(void);
static void guiRefreshFindResults(void);
static int guiFindMatch(int direction, int include_current);
static int guiSaveActiveFile(void);

static int guiMaxInt(int a, int b) { return a > b ? a : b; }
static int guiMinInt(int a, int b) { return a < b ? a : b; }
static int guiAbsInt(int value) { return value < 0 ? -value : value; }

static float guiAbsFloat(float value) { return value < 0.0f ? -value : value; }

static Color guiHex(unsigned int rgb) {
  return (Color){(unsigned char)((rgb >> 16) & 0xff),
                 (unsigned char)((rgb >> 8) & 0xff),
                 (unsigned char)(rgb & 0xff), 255};
}

#define GUI_BG             (G.theme.bg)
#define GUI_PANEL          (G.theme.panel)
#define GUI_PANEL_DARK     (G.theme.panel_dark)
#define GUI_CURRENT_LINE   (G.theme.current_line)
#define GUI_GUTTER_TEXT    (G.theme.gutter_text)
#define GUI_TEXT           (G.theme.text)
#define GUI_DIM_TEXT       (G.theme.dim_text)
#define GUI_ACCENT         (G.theme.accent)
#define GUI_BORDER         (G.theme.border)
#define GUI_MATCH_BG       (G.theme.match_bg)
#define GUI_SELECTED       (G.theme.selected)
#define GUI_TEXT_SELECTION (G.theme.text_selection)
#define GUI_OVERLAY_BG     (G.theme.overlay_bg)

static const guiTheme GUI_THEMES[] = {
    {{39, 40, 34, 255},  {30, 31, 28, 255},  {25, 26, 24, 255},
     {52, 53, 48, 255},  {107, 108, 102, 255}, {248, 248, 242, 255},
     {180, 180, 170, 255}, {255, 158, 80, 255}, {70, 70, 64, 255},
     {73, 72, 62, 255}, {67, 69, 62, 255}, {77, 94, 120, 255},
     {22, 23, 21, 245}},
    {{250, 250, 248, 255}, {238, 238, 233, 255}, {228, 229, 224, 255},
     {235, 240, 246, 255}, {122, 125, 128, 255}, {34, 36, 38, 255},
     {92, 96, 101, 255}, {28, 118, 184, 255}, {198, 202, 206, 255},
     {255, 235, 157, 255}, {218, 231, 244, 255}, {184, 211, 239, 255},
     {252, 252, 250, 245}},
    {{5, 5, 5, 255}, {20, 20, 20, 255}, {0, 0, 0, 255},
     {35, 35, 35, 255}, {170, 170, 170, 255}, {255, 255, 255, 255},
     {210, 210, 210, 255}, {255, 210, 0, 255}, {120, 120, 120, 255},
     {90, 75, 0, 255}, {55, 55, 55, 255}, {0, 85, 170, 255},
     {0, 0, 0, 245}}};

static const char *GUI_THEME_NAMES[] = {"Bento Dark", "Light", "High Contrast"};

static void guiApplyTheme(int index) {
  int count = (int)(sizeof(GUI_THEMES) / sizeof(GUI_THEMES[0]));
  if (index < 0 || index >= count)
    index = 0;
  G.theme_index = index;
  G.theme = GUI_THEMES[index];
}

static int guiControlDown(void) {
  return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}

static int guiSuperDown(void) {
  return IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
}

static int guiModifierDown(void) { return guiControlDown() || guiSuperDown(); }

static int guiLineBoundaryModifierDown(void) {
#ifdef __APPLE__
  return guiSuperDown();
#else
  return guiControlDown();
#endif
}

static int guiShiftDown(void) {
  return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
}

static int guiAltDown(void) {
  return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
}

static int guiKeyPressedRepeat(int key) {
  double now = GetTime();
  if (IsKeyPressed(key)) {
    G.repeat_key = key;
    G.repeat_next = now + 0.32;
    return 1;
  }
  if (IsKeyDown(key) && G.repeat_key == key && now >= G.repeat_next) {
    G.repeat_next = now + 0.045;
    return 1;
  }
  if (G.repeat_key == key && !IsKeyDown(key))
    G.repeat_key = 0;
  return 0;
}

static const char *guiBaseName(const char *path) {
  const char *slash;
#ifdef _WIN32
  const char *backslash;
#endif

  if (!path || !path[0])
    return "[No Name]";
  slash = strrchr(path, '/');
#ifdef _WIN32
  backslash = strrchr(path, '\\');
  if (!slash || (backslash && backslash > slash))
    slash = backslash;
#endif
  return slash ? slash + 1 : path;
}

static int guiSidebarWidth(void) {
  return G.project_open && G.sidebar_width > 0 ? G.sidebar_width : 0;
}

static int guiAsciiLower(int c) {
  if (c >= 'A' && c <= 'Z')
    return c + ('a' - 'A');
  return c;
}

static int guiContainsCaseInsensitive(const char *text, const char *query) {
  int qlen = strlen(query);

  if (qlen == 0)
    return 1;
  for (int i = 0; text[i]; i++) {
    int j = 0;
    while (query[j] && text[i + j] &&
           guiAsciiLower((unsigned char)text[i + j]) ==
               guiAsciiLower((unsigned char)query[j]))
      j++;
    if (j == qlen)
      return 1;
  }
  return 0;
}

static void guiPathJoin(char *dst, size_t dst_len, const char *dir,
                        const char *name) {
  size_t dir_len = strlen(dir);
  const char *slash;
#ifdef _WIN32
  slash = dir_len && (dir[dir_len - 1] == '/' || dir[dir_len - 1] == '\\')
              ? ""
              : "\\";
#else
  slash = dir_len && dir[dir_len - 1] == '/' ? "" : "/";
#endif
  snprintf(dst, dst_len, "%s%s%s", dir, slash, name);
}

static int guiPathIsAbsolute(const char *path) {
  if (!path || !path[0])
    return 0;
#ifdef _WIN32
  if ((isalpha((unsigned char)path[0]) && path[1] == ':') ||
      path[0] == '/' || path[0] == '\\')
    return 1;
  return 0;
#else
  return path[0] == '/';
#endif
}

static int guiPathIsDir(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void guiParentDir(char *out, size_t out_len, const char *path) {
  snprintf(out, out_len, "%s", path && *path ? path : ".");
  char *slash = strrchr(out, '/');
#ifdef _WIN32
  char *bslash = strrchr(out, '\\');
  if (!slash || (bslash && bslash > slash))
    slash = bslash;
#endif
  if (slash && slash != out)
    *slash = '\0';
  else
    snprintf(out, out_len, ".");
}

static void guiUniqueUntitledPath(char *out, size_t out_len) {
  char base[PATH_MAX];
  if (G.cwd[0]) {
    snprintf(base, sizeof(base), "%s", G.cwd);
  } else if (!getcwd(base, sizeof(base))) {
    snprintf(base, sizeof(base), ".");
  }
  guiPathJoin(out, out_len, base, "untitled.txt");
  if (access(out, R_OK) != 0)
    return;
  for (int i = 2; i < 1000; i++) {
    char name[64];
    snprintf(name, sizeof(name), "untitled-%d.txt", i);
    guiPathJoin(out, out_len, base, name);
    if (access(out, R_OK) != 0)
      return;
  }
}

static int guiShouldSkipPath(const char *name) {
  if (!strcmp(name, ".") || !strcmp(name, ".."))
    return 1;
  if (!strcmp(name, ".git") || !strcmp(name, ".DS_Store"))
    return 1;
  return 0;
}

static void guiAddFileEntry(const char *path, const char *name, int depth,
                            int is_dir) {
  guiFileEntry *entry;

  if (G.file_count == G.file_cap) {
    int new_cap = G.file_cap ? G.file_cap * 2 : 128;
    guiFileEntry *new_files = realloc(G.files, sizeof(guiFileEntry) * new_cap);
    if (!new_files)
      return;
    G.files = new_files;
    G.file_cap = new_cap;
  }

  entry = &G.files[G.file_count++];
  snprintf(entry->path, sizeof(entry->path), "%s", path);
  snprintf(entry->name, sizeof(entry->name), "%s", name);
  entry->depth = depth;
  entry->is_dir = is_dir;
}

static void guiScanDirectory(const char *dir, int depth) {
#ifdef _WIN32
  char pattern[PATH_MAX];

  if (depth > 8 || G.file_count > 5000)
    return;

  guiPathJoin(pattern, sizeof(pattern), dir, "*");
  for (int pass = 0; pass < 2; pass++) {
    struct _finddata_t data;
    intptr_t handle = _findfirst(pattern, &data);

    if (handle == -1)
      return;
    do {
      char path[PATH_MAX];
      int is_dir;

      if (guiShouldSkipPath(data.name))
        continue;
      is_dir = (data.attrib & _A_SUBDIR) != 0;
      if ((pass == 0 && !is_dir) || (pass == 1 && is_dir))
        continue;

      guiPathJoin(path, sizeof(path), dir, data.name);
      guiAddFileEntry(path, data.name, depth, is_dir);
      if (is_dir)
        guiScanDirectory(path, depth + 1);
    } while (_findnext(handle, &data) == 0);
    _findclose(handle);
  }
#else
  struct dirent **names = NULL;
  int count;

  if (depth > 8 || G.file_count > 5000)
    return;

  count = scandir(dir, &names, NULL, alphasort);
  if (count < 0)
    return;

  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < count; i++) {
      char path[PATH_MAX];
      struct stat st;
      int is_dir;

      if (guiShouldSkipPath(names[i]->d_name))
        continue;
      guiPathJoin(path, sizeof(path), dir, names[i]->d_name);
      if (stat(path, &st) == -1)
        continue;

      is_dir = S_ISDIR(st.st_mode);
      if ((pass == 0 && !is_dir) || (pass == 1 && is_dir))
        continue;

      guiAddFileEntry(path, names[i]->d_name, depth, is_dir);
      if (is_dir)
        guiScanDirectory(path, depth + 1);
    }
  }

  for (int i = 0; i < count; i++)
    free(names[i]);
  free(names);
#endif
}

static void guiRebuildFileTree(void) {
  G.file_count = 0;
  if (!G.project_open) {
    G.tree_scroll = 0;
    G.signature_dirty = 1;
    return;
  }
  if (!getcwd(G.cwd, sizeof(G.cwd)))
    snprintf(G.cwd, sizeof(G.cwd), ".");
  guiScanDirectory(G.cwd, 0);
  G.signature_dirty = 1;
}

static void guiBuildGlyphCache(void);

static void guiLoadEditorFont(void) {
  static const char *font_paths[] = {
      "/System/Applications/Utilities/Terminal.app/Contents/Resources/Fonts/"
      "SF-Mono-Regular.otf",
      "/System/Library/Fonts/SFNSMono.ttf",
      "/System/Library/Fonts/Monaco.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf",
      "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
      "/usr/share/fonts/TTF/LiberationMono-Regular.ttf",
      NULL};

  G.font = GetFontDefault();
  snprintf(G.font_label, sizeof(G.font_label), "raylib default");

  for (int i = 0; font_paths[i]; i++) {
    Font font;

    if (access(font_paths[i], R_OK) != 0)
      continue;
    font = LoadFontEx(font_paths[i], 32, NULL, 0);
    if (font.texture.id == 0)
      continue;

    G.font = font;
    G.font_owned = 1;
    SetTextureFilter(G.font.texture, TEXTURE_FILTER_BILINEAR);
    snprintf(G.font_label, sizeof(G.font_label), "%s",
             guiBaseName(font_paths[i]));
    guiBuildGlyphCache();
    return;
  }
  guiBuildGlyphCache();
}

/* ====================== Fast text dispatch (raylib) ======================
 *
 * The editor's hot draw paths (the run-list dispatch and the gutter line
 * numbers) used to call DrawTextEx 150+ times per frame. Each call walked
 * a linear list of glyphs in the Font struct and submitted DrawTexturePro
 * with full matrix math per glyph. On the 36k-line bench that was the
 * dominant cost outside the measured emit/dispatch buckets (~1.4 ms).
 *
 * Replacement: cache each printable ASCII glyph's atlas UV + metrics once
 * at font-load, then in the hot path push raw quads via rlBegin(RL_QUADS)
 * with a precomputed scale. Stays inside raylib's default batcher, so a
 * full viewport of text becomes one (or very few) draw calls.
 */

#define GUI_GLYPH_CACHE_FIRST 32
#define GUI_GLYPH_CACHE_LAST  126
#define GUI_GLYPH_CACHE_COUNT (GUI_GLYPH_CACHE_LAST - GUI_GLYPH_CACHE_FIRST + 1)

typedef struct guiGlyphCache {
  float u0, v0, u1, v1;
  float offset_x, offset_y;
  float width, height;
  float advance_x;
} guiGlyphCache;

static guiGlyphCache g_glyph_cache[GUI_GLYPH_CACHE_COUNT];
static int   g_glyph_cache_built = 0;
static float g_glyph_base_size = 0.0f;
static unsigned int g_glyph_texture_id = 0;

static void guiBuildGlyphCache(void) {
  g_glyph_cache_built = 0;
  if (G.font.texture.id == 0 || !G.font.glyphs || !G.font.recs ||
      G.font.glyphCount <= 0 || G.font.baseSize <= 0)
    return;

  float atlas_w = (float)G.font.texture.width;
  float atlas_h = (float)G.font.texture.height;
  if (atlas_w <= 0 || atlas_h <= 0)
    return;

  for (int c = GUI_GLYPH_CACHE_FIRST; c <= GUI_GLYPH_CACHE_LAST; c++) {
    int idx = GetGlyphIndex(G.font, c);
    if (idx < 0 || idx >= G.font.glyphCount) continue;
    Rectangle r = G.font.recs[idx];
    GlyphInfo gi = G.font.glyphs[idx];
    guiGlyphCache *g = &g_glyph_cache[c - GUI_GLYPH_CACHE_FIRST];
    g->u0 = r.x / atlas_w;
    g->v0 = r.y / atlas_h;
    g->u1 = (r.x + r.width) / atlas_w;
    g->v1 = (r.y + r.height) / atlas_h;
    g->offset_x = (float)gi.offsetX;
    g->offset_y = (float)gi.offsetY;
    g->width = r.width;
    g->height = r.height;
    g->advance_x = gi.advanceX > 0 ? (float)gi.advanceX : r.width;
  }
  g_glyph_base_size = (float)G.font.baseSize;
  g_glyph_texture_id = G.font.texture.id;
  g_glyph_cache_built = 1;
}

/* "Open" the text batch: bind the atlas texture and begin a QUADS batch.
 * Caller must close with guiFastTextEnd(). Between begin/end the caller
 * MUST NOT call any other raylib draw function — those will flush our
 * pending vertices and switch textures. */
static int guiFastTextBegin(void) {
  if (!g_glyph_cache_built || g_glyph_texture_id != G.font.texture.id)
    return 0;
  rlSetTexture(g_glyph_texture_id);
  rlBegin(RL_QUADS);
  return 1;
}

static void guiFastTextEnd(void) {
  rlEnd();
  rlSetTexture(0);
}

/* Push a single string into an already-open batch. */
static void guiFastTextDraw(const char *text, int len, float x, float y,
                            float font_size, Color color) {
  float scale = font_size / g_glyph_base_size;
  rlColor4ub(color.r, color.g, color.b, color.a);

  float pen_x = x;
  for (int i = 0; i < len; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c < GUI_GLYPH_CACHE_FIRST || c > GUI_GLYPH_CACHE_LAST) {
      pen_x += g_glyph_cache[' ' - GUI_GLYPH_CACHE_FIRST].advance_x * scale;
      continue;
    }
    guiGlyphCache *g = &g_glyph_cache[c - GUI_GLYPH_CACHE_FIRST];
    if (g->width > 0 && g->height > 0) {
      float gx = pen_x + g->offset_x * scale;
      float gy = y + g->offset_y * scale;
      float gw = g->width * scale;
      float gh = g->height * scale;
      rlTexCoord2f(g->u0, g->v0); rlVertex2f(gx, gy);
      rlTexCoord2f(g->u0, g->v1); rlVertex2f(gx, gy + gh);
      rlTexCoord2f(g->u1, g->v1); rlVertex2f(gx + gw, gy + gh);
      rlTexCoord2f(g->u1, g->v0); rlVertex2f(gx + gw, gy);
    }
    pen_x += g->advance_x * scale;
  }
}

/* ================== Bento-box window icon ===========================
 *
 * Embedded PNG icon data keeps ./bento-gui self-contained while still giving
 * the window/dock/taskbar a recognizable icon. */


#ifdef __APPLE__
/* Implemented in bento_mac.m — sets [NSApp setApplicationIconImage:] so
 * `./bento-gui` shows the bento in the dock / Cmd-Tab even without a
 * surrounding .app bundle. */
extern void bentoMacSetDockIcon(const unsigned char *rgba, int width,
                                int height);
extern int bentoMacBeginChoosePath(int choose_dirs, const char *start_dir);
extern int bentoMacPollChosenPath(char *out, size_t out_len);
extern int bentoMacPathPickerActive(void);
#endif

#include "icon_data.h"
static void guiSetBentoWindowIcon(void) {
  Image base_img = LoadImageFromMemory(".png", icons_icon_512x512_png, icons_icon_512x512_png_len);
  ImageFormat(&base_img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
#ifdef __APPLE__
  bentoMacSetDockIcon((const unsigned char *)base_img.data, base_img.width, base_img.height);
  UnloadImage(base_img);
#else
  int sizes[] = {32, 64, 128};
  int count = 3;
  Image icons[3];
  for (int i = 0; i < count; i++) {
    icons[i] = ImageCopy(base_img);
    ImageResize(&icons[i], sizes[i], sizes[i]);
  }
  SetWindowIcons(icons, count);
  for (int i = 0; i < count; i++) {
    UnloadImage(icons[i]);
  }
  UnloadImage(base_img);
#endif
}

static int guiFileEntryIsCurrent(const guiFileEntry *entry) {
  return E.filename && !entry->is_dir && !strcmp(entry->path, E.filename);
}

static int guiFileIndexForPath(const char *path) {
  if (!path)
    return -1;
  for (int i = 0; i < G.file_count; i++) {
    if (!G.files[i].is_dir && !strcmp(G.files[i].path, path))
      return i;
  }
  return -1;
}

static const char *guiRelativePath(const char *path) {
  size_t cwd_len = strlen(G.cwd);

  if (!G.project_open)
    return path;
  if (cwd_len && !strncmp(path, G.cwd, cwd_len) &&
      (path[cwd_len] == '/'
#ifdef _WIN32
       || path[cwd_len] == '\\'
#endif
       ))
    return path + cwd_len + 1;
  return path;
}

/* ---------- Tab plumbing -------------------------------------------- */

static void bentoStashActiveTab(void) {
  if (T.active < 0)
    return;
  /* Flush deferred syntax work so the stashed tab carries fully-fresh hl
   * data. We don't track per-tab dirty ranges, so this keeps it simple. */
  syntaxRefreshAll();
  /* About to detach this buffer from "live" status: any of its lines that
   * are still arena-backed must move to their own heap allocations, because
   * the next tab activation may reset the arena. */
  for (int j = 0; j < E.numrows; j++)
    charsMigrate(&E.row[j]);
  guiTab *tab = &T.list[T.active];
  tab->cx = E.cx;
  tab->cy = E.cy;
  tab->rowoff = E.rowoff;
  tab->coloff = E.coloff;
  tab->numrows = E.numrows;
  tab->row = E.row;
  tab->dirty = E.dirty;
  tab->syntax = E.syntax;
  tab->file_mtime = E.file_mtime;
  if (E.filename)
    snprintf(tab->filename, sizeof(tab->filename), "%s", E.filename);
  else
    tab->filename[0] = '\0';
  tab->undo_ops = U.ops;
  tab->undo_size = U.size;
  tab->undo_cap = U.cap;
  tab->undo_save_point = U.save_point;
  tab->undo_last_kind = U.last_kind;
  tab->undo_last_row = U.last_row;
  tab->undo_last_col = U.last_col;
  tab->undo_last_len = U.last_len;
  /* No live row references the arena now; reclaim its bytes for the next
   * file load. */
  arenaReset(&g_chars_arena);
  /* Stashed tab has fresh hl for every row (we just refreshed). */
  dirtyClear();
}

static void bentoLoadActiveFromTab(int idx) {
  guiTab *tab = &T.list[idx];
  E.cx = tab->cx;
  E.cy = tab->cy;
  E.rowoff = tab->rowoff;
  E.coloff = tab->coloff;
  E.numrows = tab->numrows;
  E.row = tab->row;
  E.dirty = tab->dirty;
  E.syntax = tab->syntax;
  E.file_mtime = tab->file_mtime;
  free(E.filename);
  E.filename = tab->filename[0] ? strdup(tab->filename) : NULL;
  U.ops = tab->undo_ops;
  U.size = tab->undo_size;
  U.cap = tab->undo_cap;
  U.save_point = tab->undo_save_point;
  U.last_kind = tab->undo_last_kind;
  U.last_row = tab->undo_last_row;
  U.last_col = tab->undo_last_col;
  U.last_len = tab->undo_last_len;
  U.suppress = 0;
  U.txn_depth = 0;
  U.txn_started = 0;
  U.delete_is_forward = 0;
  /* Rebuild the SoA size array for the incoming tab's rows. It's not
   * persisted per-tab; cheaper to rebuild than to save/restore. */
  free(E.line_sizes);
  E.line_sizes = NULL;
  E.line_sizes_cap = 0;
  if (E.numrows > 0 && tbGrowLineSizes(&E, E.numrows)) {
    for (int j = 0; j < E.numrows; j++)
      E.line_sizes[j] = E.row[j].size;
  }
  /* Clear "active" copies of cold storage so we don't double-free later. */
  tab->row = NULL;
  tab->numrows = 0;
  tab->undo_ops = NULL;
  tab->undo_size = 0;
  tab->undo_cap = 0;
  T.active = idx;
  /* Reset cursor-blink + selection bookkeeping. */
  guiClearSelection();
  G.last_cursor_row = -1;
  G.last_cursor_col = -1;
  G.last_cursor_move_time = GetTime();
}

static void guiRememberClosedFile(const char *path) {
  if (path && *path)
    snprintf(G.last_closed_file, sizeof(G.last_closed_file), "%s", path);
}

static int bentoFindTabByPath(const char *path) {
  for (int i = 0; i < BENTO_MAX_TABS; i++) {
    if (!T.list[i].in_use)
      continue;
    if (i == T.active) {
      if (E.filename && !strcmp(E.filename, path))
        return i;
    } else if (!strcmp(T.list[i].filename, path)) {
      return i;
    }
  }
  return -1;
}

static int bentoAllocTabSlot(void) {
  for (int i = 0; i < BENTO_MAX_TABS; i++) {
    if (!T.list[i].in_use) {
      memset(&T.list[i], 0, sizeof(T.list[i]));
      T.list[i].in_use = 1;
      T.count++;
      return i;
    }
  }
  return -1;
}

static void bentoSwitchToTab(int idx) {
  if (idx == T.active || idx < 0 || !T.list[idx].in_use)
    return;
  bentoStashActiveTab();
  bentoLoadActiveFromTab(idx);
  G.current_match = -1;
  G.match_scroll = 0;
  G.find_dirty = 1;
}

static void bentoFreeTabSlot(int idx) {
  guiTab *tab = &T.list[idx];
  guiRememberClosedFile(idx == T.active ? E.filename : tab->filename);
  if (idx == T.active) {
    /* Active slot's cold pointers are stale — free the live ones from E/U. */
    editorFreeRows();
    undoClear();
    free(E.filename);
    E.filename = NULL;
    E.dirty = 0;
    E.cx = E.cy = E.rowoff = E.coloff = 0;
    E.syntax = NULL;
    E.file_mtime = 0;
  } else {
    if (tab->row) {
      for (int j = 0; j < tab->numrows; j++)
        editorFreeRow(&tab->row[j]);
      free(tab->row);
    }
    if (tab->undo_ops) {
      for (int j = 0; j < tab->undo_size; j++)
        free(tab->undo_ops[j].text);
      free(tab->undo_ops);
    }
  }
  memset(tab, 0, sizeof(*tab));
  T.count--;
  if (T.preview == idx)
    T.preview = -1;
}

static void bentoCloseTab(int idx) {
  if (idx < 0 || !T.list[idx].in_use)
    return;

  int was_active = (idx == T.active);
  /* Pick a neighbor to focus next if we're closing the active tab. */
  int next = -1;
  if (was_active) {
    for (int step = 1; step < BENTO_MAX_TABS && next < 0; step++) {
      int a = (idx + step) % BENTO_MAX_TABS;
      int b = (idx + BENTO_MAX_TABS - step) % BENTO_MAX_TABS;
      if (T.list[a].in_use && a != idx) {
        next = a;
        break;
      }
      if (T.list[b].in_use && b != idx) {
        next = b;
        break;
      }
    }
  }

  bentoFreeTabSlot(idx);

  if (was_active) {
    if (next >= 0) {
      T.active = -1; /* defeat the early-return guard in bentoSwitchToTab */
      bentoLoadActiveFromTab(next);
      G.current_match = -1;
      G.match_scroll = 0;
      G.find_dirty = 1;
    } else {
      T.active = -1;
    }
  }
}

/* Open `path` in the active tab slot, replacing whatever was there. */
static int bentoLoadFileInActive(const char *path) {
  if (T.active < 0)
    return 0;
  editorOpen((char *)path);
  /* editorOpen reset E.dirty, freed rows, set filename, etc. The active
   * slot's in_use stays 1; just update its filename. */
  snprintf(T.list[T.active].filename, sizeof(T.list[T.active].filename), "%s",
           path);
  guiClearSelection();
  G.current_match = -1;
  G.match_scroll = 0;
  G.find_dirty = 1;
  return 1;
}

/* Single source of truth for opening a file in response to a sidebar /
 * search-result / Cmd-P click. `pin_immediately` is set by double-clicks. */
static int bentoOpenFile(const char *path, int pin_immediately) {
  if (!path || !*path)
    return 0;

  /* If already open in some tab, just switch to it (and maybe pin). */
  int existing = bentoFindTabByPath(path);
  if (existing >= 0) {
    bentoSwitchToTab(existing);
    if (pin_immediately)
      T.list[existing].pinned = 1;
    if (T.preview == existing && pin_immediately)
      T.preview = -1;
    guiAddRecentFile(path);
    editorSetStatusMessage("%s%s", guiBaseName(path),
                           T.list[existing].pinned ? "" : " (preview)");
    return 1;
  }

  /* Not open yet. If there's a preview slot and the new file isn't being
   * pinned, replace the preview's contents in place. Otherwise grab a new
   * slot. */
  int target;
  if (!pin_immediately && T.preview >= 0 && T.list[T.preview].in_use) {
    target = T.preview;
    if (target != T.active)
      bentoSwitchToTab(target);
  } else {
    target = bentoAllocTabSlot();
    if (target < 0) {
      editorSetStatusMessage("Too many tabs open (%d). Close one first.",
                             BENTO_MAX_TABS);
      return 0;
    }
    bentoStashActiveTab();
    T.active = target;
    /* Fresh slot starts with no rows / no undo. */
    E.row = NULL;
    E.numrows = 0;
    E.cx = E.cy = E.rowoff = E.coloff = 0;
    E.dirty = 0;
    E.syntax = NULL;
    E.file_mtime = 0;
    free(E.filename);
    E.filename = NULL;
    U.ops = NULL;
    U.size = U.cap = 0;
    U.save_point = 0;
    U.last_kind = UNDO_KIND_NONE;
    U.last_row = U.last_col = U.last_len = 0;
    U.suppress = 0;
    U.txn_depth = 0;
    U.txn_started = 0;
    U.delete_is_forward = 0;
  }

  bentoLoadFileInActive(path);
  guiAddRecentFile(path);

  if (pin_immediately) {
    T.list[target].pinned = 1;
    if (T.preview == target)
      T.preview = -1;
  } else {
    T.list[target].pinned = 0;
    T.preview = target;
  }
  editorSetStatusMessage("%s%s", guiBaseName(path),
                         pin_immediately ? "" : " (preview)");
  return 1;
}

/* Edit ops auto-pin the active tab so a stray sidebar click can't
 * silently discard work-in-progress. */
static void bentoAutoPinIfDirty(void) {
  if (T.active < 0 || !E.dirty)
    return;
  T.list[T.active].pinned = 1;
  if (T.preview == T.active)
    T.preview = -1;
}

/* Compatibility wrapper for existing call sites (find-result clicks, etc.). */
static int guiOpenFile(const char *path) {
  int ok = bentoOpenFile(path, 1);
  if (ok) {
    G.welcome_active = 0;
    if (!G.project_open)
      guiParentDir(G.cwd, sizeof(G.cwd), path);
  }
  return ok;
}

static void guiNewFile(void) {
  char path[PATH_MAX];
  guiUniqueUntitledPath(path, sizeof(path));
  if (bentoOpenFile(path, 1)) {
    G.welcome_active = 0;
    editorSetStatusMessage("New file: %s", guiBaseName(path));
  }
}

static void guiOpenProject(const char *path) {
  if (!path || !guiPathIsDir(path)) {
    editorSetStatusMessage("Not a folder");
    return;
  }
  if (chdir(path) != 0) {
    editorSetStatusMessage("Can't open folder: %s", strerror(errno));
    return;
  }
  G.project_open = 1;
  guiRebuildFileTree();
  guiAddRecentProject(G.cwd);
  G.welcome_active = 0;
  editorSetStatusMessage("Opened folder: %s", G.cwd);
}

static void guiOpenPath(const char *path) {
  if (!path || !*path)
    return;
  if (access(path, 0) != 0) {
    editorSetStatusMessage("Path not found: %s", path);
    return;
  }
  if (guiPathIsDir(path))
    guiOpenProject(path);
  else
    guiOpenFile(path);
}

/* ---------- Session persistence ------------------------------------- *
 *
 * Remember the project folder, open files, and per-tab cursor position so
 * that launching `./bento-gui` with no args restores the previous workspace. */

static int bentoConfigPath(char *out, size_t out_len, const char *name) {
#ifdef _WIN32
  const char *base = getenv("APPDATA");
  if (!base || !*base)
    base = getenv("USERPROFILE");
  if (!base || !*base)
    return 0;
  snprintf(out, out_len, "%s\\Bento\\%s", base, name);
  return 1;
#elif defined(__APPLE__)
  const char *home = getenv("HOME");
  if (!home || !*home)
    return 0;
  snprintf(out, out_len,
           "%s/Library/Application Support/Bento/%s", home, name);
  return 1;
#else
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) {
    snprintf(out, out_len, "%s/bento/%s", xdg, name);
    return 1;
  }
  const char *home = getenv("HOME");
  if (!home || !*home)
    return 0;
  snprintf(out, out_len, "%s/.config/bento/%s", home, name);
  return 1;
#endif
}

static int bentoSessionPath(char *out, size_t out_len) {
  return bentoConfigPath(out, out_len, "session.txt");
}

static void bentoMkdirP(const char *path) {
  if (!path || !*path)
    return;
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);
  size_t len = strlen(tmp);
  while (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
    tmp[--len] = '\0';
  }
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/' || *p == '\\') {
      char saved = *p;
      *p = '\0';
#ifdef _WIN32
      _mkdir(tmp);
#else
      mkdir(tmp, 0755);
#endif
      *p = saved;
    }
  }
#ifdef _WIN32
  _mkdir(tmp);
#else
  mkdir(tmp, 0755);
#endif
}

static void bentoEnsureParentDir(const char *path) {
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s", path);
  char *slash = strrchr(dir, '/');
#ifdef _WIN32
  char *bslash = strrchr(dir, '\\');
  if (!slash || (bslash && bslash > slash))
    slash = bslash;
#endif
  if (slash) {
    *slash = '\0';
    bentoMkdirP(dir);
  }
}

static void guiInitSettingsDefaults(void) {
  G.font_size = GUI_FONT_SIZE;
  G.small_font_size = GUI_SMALL_FONT_SIZE;
  G.sidebar_width = GUI_SIDEBAR_WIDTH;
  G.tab_width = 8;
  g_tab_stop = G.tab_width;
  G.restore_session = 1;
  G.theme_index = 0;
  G.show_whitespace = 0;
  guiApplyTheme(0);
}

static void guiSaveSettings(void) {
  char path[PATH_MAX];
  if (!bentoConfigPath(path, sizeof(path), "settings.txt"))
    return;
  bentoEnsureParentDir(path);
  FILE *fp = fopen(path, "w");
  if (!fp)
    return;
  fprintf(fp, "font_size %.1f\n", G.font_size);
  fprintf(fp, "theme %d\n", G.theme_index);
  fprintf(fp, "show_whitespace %d\n", G.show_whitespace ? 1 : 0);
  fprintf(fp, "tab_width %d\n", G.tab_width);
  fprintf(fp, "restore_session %d\n", G.restore_session ? 1 : 0);
  fclose(fp);
}

static void guiLoadSettings(void) {
  char path[PATH_MAX];
  char key[64], value[128];
  if (!bentoConfigPath(path, sizeof(path), "settings.txt"))
    return;
  FILE *fp = fopen(path, "r");
  if (!fp)
    return;
  while (fscanf(fp, "%63s %127s", key, value) == 2) {
    if (!strcmp(key, "font_size")) {
      float v = (float)atof(value);
      if (v >= 11.0f && v <= 32.0f)
        G.font_size = v;
    } else if (!strcmp(key, "theme")) {
      guiApplyTheme(atoi(value));
    } else if (!strcmp(key, "show_whitespace")) {
      G.show_whitespace = atoi(value) != 0;
    } else if (!strcmp(key, "tab_width")) {
      int v = atoi(value);
      if (v >= 2 && v <= 12) {
        G.tab_width = v;
        g_tab_stop = v;
      }
    } else if (!strcmp(key, "restore_session")) {
      G.restore_session = atoi(value) != 0;
    }
  }
  fclose(fp);
}

static void guiMovePathToFront(char paths[][PATH_MAX], int *count, int max,
                               const char *path) {
  int found = -1;
  if (!path || !*path)
    return;
  for (int i = 0; i < *count; i++) {
    if (!strcmp(paths[i], path)) {
      found = i;
      break;
    }
  }
  if (found > 0)
    memmove(paths + 1, paths, sizeof(paths[0]) * (size_t)found);
  else if (found < 0) {
    if (*count < max)
      (*count)++;
    memmove(paths + 1, paths, sizeof(paths[0]) * (size_t)(*count - 1));
  }
  snprintf(paths[0], PATH_MAX, "%s", path);
}

static void guiAddRecentFile(const char *path) {
  guiMovePathToFront(G.recent_files, &G.recent_file_count,
                     GUI_RECENT_FILES_MAX, path);
  guiSaveRecents();
}

static void guiAddRecentProject(const char *path) {
  guiMovePathToFront(G.recent_projects, &G.recent_project_count,
                     GUI_RECENT_PROJECTS_MAX, path);
  guiSaveRecents();
}

static void guiSaveRecents(void) {
  char path[PATH_MAX];
  if (!bentoConfigPath(path, sizeof(path), "recent.txt"))
    return;
  bentoEnsureParentDir(path);
  FILE *fp = fopen(path, "w");
  if (!fp)
    return;
  for (int i = 0; i < G.recent_file_count; i++)
    fprintf(fp, "file %s\n", G.recent_files[i]);
  for (int i = 0; i < G.recent_project_count; i++)
    fprintf(fp, "project %s\n", G.recent_projects[i]);
  fclose(fp);
}

static void guiLoadRecents(void) {
  char path[PATH_MAX];
  char line[PATH_MAX + 16];
  if (!bentoConfigPath(path, sizeof(path), "recent.txt"))
    return;
  FILE *fp = fopen(path, "r");
  if (!fp)
    return;
  while (fgets(line, sizeof(line), fp)) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    if (!strncmp(line, "file ", 5) &&
        G.recent_file_count < GUI_RECENT_FILES_MAX) {
      snprintf(G.recent_files[G.recent_file_count++], PATH_MAX, "%s",
               line + 5);
    } else if (!strncmp(line, "project ", 8) &&
               G.recent_project_count < GUI_RECENT_PROJECTS_MAX) {
      snprintf(G.recent_projects[G.recent_project_count++], PATH_MAX, "%s",
               line + 8);
    }
  }
  fclose(fp);
}

static void bentoSaveSession(void) {
  char path[PATH_MAX];
  if (!bentoSessionPath(path, sizeof(path)))
    return;

  bentoEnsureParentDir(path);

  FILE *fp = fopen(path, "w");
  if (!fp)
    return;

  fprintf(fp, "# bento session v2\n");
  if (G.project_open && G.cwd[0])
    fprintf(fp, "project %s\n", G.cwd);

  /* Snapshot the active tab into its slot so saved cursor/path data is
   * uniform across all entries. */
  bentoStashActiveTab();

  int active_emit_index = -1;
  int emit_index = 0;
  for (int i = 0; i < BENTO_MAX_TABS; i++) {
    if (!T.list[i].in_use)
      continue;
    if (!T.list[i].filename[0])
      continue;
    int filerow = T.list[i].rowoff + T.list[i].cy;
    int filecol = T.list[i].coloff + T.list[i].cx;
    fprintf(fp, "tab %d %d %s\n", filerow, filecol, T.list[i].filename);
    if (i == T.active)
      active_emit_index = emit_index;
    emit_index++;
  }
  if (active_emit_index >= 0)
    fprintf(fp, "active %d\n", active_emit_index);

  fclose(fp);
}

static void bentoMoveCursorToFilePos(int row, int col) {
  int rowcount = tbLineCount(TB);
  if (row < 0)
    row = 0;
  if (row > rowcount)
    row = rowcount;
  if (col < 0)
    col = 0;
  if (row < rowcount) {
    int row_size = tbLineLength(TB, row);
    if (col > row_size)
      col = row_size;
  }

  E.rowoff = 0;
  E.coloff = 0;
  E.cy = row;
  E.cx = col;
  if (E.screenrows > 4 && E.cy >= E.screenrows) {
    E.rowoff = E.cy - E.screenrows / 2;
    E.cy -= E.rowoff;
  }
  if (E.screencols > 4 && E.cx >= E.screencols) {
    E.coloff = E.cx - E.screencols / 2;
    E.cx -= E.coloff;
  }
}

/* Returns the number of tabs successfully opened. */
static int bentoLoadSession(void) {
  char path[PATH_MAX];
  char original_cwd[PATH_MAX];
  if (!bentoSessionPath(path, sizeof(path)))
    return 0;
  FILE *fp = fopen(path, "r");
  if (!fp)
    return 0;
  if (!getcwd(original_cwd, sizeof(original_cwd)))
    snprintf(original_cwd, sizeof(original_cwd), ".");

  int opened = 0;
  int restored_project = 0;
  int active_emit_target = -1;
  int active_slot = -1;
  char line[PATH_MAX + 64];

  /* Restored tabs need to map "n-th tab line in file" -> "slot index" so we
   * can switch to the active one at the end. */
  int emit_to_slot[BENTO_MAX_TABS];
  int emit_count = 0;

  while (fgets(line, sizeof(line), fp)) {
    char *nl = strchr(line, '\n');
    if (nl)
      *nl = '\0';
    if (line[0] == '#' || line[0] == '\0')
      continue;

    if (!strncmp(line, "project ", 8)) {
      const char *p = line + 8;
      if (*p && chdir(p) == 0) {
        snprintf(G.cwd, sizeof(G.cwd), "%s", p);
        G.project_open = 1;
        restored_project = 1;
      }
    } else if (!strncmp(line, "cwd ", 4)) {
      const char *p = line + 4;
      /* Legacy v1 sessions used cwd for the process directory even when
       * no folder was intentionally open. Keep it as a base for relative
       * restored tabs, but do not restore it as a visible project. */
      if (*p && chdir(p) == 0)
        snprintf(G.cwd, sizeof(G.cwd), "%s", p);
    } else if (!strncmp(line, "active ", 7)) {
      active_emit_target = atoi(line + 7);
    } else if (!strncmp(line, "tab ", 4)) {
      const char *p = line + 4;
      char *end = NULL;
      long row = strtol(p, &end, 10);
      if (end == p)
        continue;
      p = end;
      while (*p == ' ')
        p++;
      long col = strtol(p, &end, 10);
      if (end == p)
        continue;
      p = end;
      while (*p == ' ')
        p++;
      if (!*p)
        continue;
      if (access(p, R_OK) != 0)
        continue;
      if (!bentoOpenFile(p, 1))
        continue;
      bentoMoveCursorToFilePos((int)row, (int)col);
      if (emit_count < BENTO_MAX_TABS)
        emit_to_slot[emit_count] = T.active;
      emit_count++;
      opened++;
    }
  }
  fclose(fp);

  if (active_emit_target >= 0 && active_emit_target < emit_count)
    active_slot = emit_to_slot[active_emit_target];
  if (active_slot >= 0 && active_slot != T.active)
    bentoSwitchToTab(active_slot);

  /* Mark all restored tabs as pinned so subsequent single-clicks don't
   * replace them. */
  for (int i = 0; i < BENTO_MAX_TABS; i++) {
    if (T.list[i].in_use)
      T.list[i].pinned = 1;
  }
  T.preview = -1;
  if (!restored_project && opened == 0) {
    chdir(original_cwd);
    snprintf(G.cwd, sizeof(G.cwd), "%s", original_cwd);
  }
  return restored_project || opened;
}

static int guiReadTextFile(const char *path, char **out, size_t *out_len) {
  FILE *fp;
  long size;
  char *buf;

  *out = NULL;
  *out_len = 0;

  fp = fopen(path, "rb");
  if (!fp)
    return 0;
  if (fseek(fp, 0, SEEK_END) != 0)
    goto fail;
  size = ftell(fp);
  if (size < 0 || size > GUI_MAX_PROJECT_FILE_SIZE)
    goto fail;
  if (fseek(fp, 0, SEEK_SET) != 0)
    goto fail;

  buf = malloc((size_t)size + 1);
  if (!buf)
    goto fail;
  if (size > 0 && fread(buf, 1, (size_t)size, fp) != (size_t)size) {
    free(buf);
    goto fail;
  }
  fclose(fp);

  for (long i = 0; i < size; i++) {
    if (buf[i] == '\0') {
      free(buf);
      return 0;
    }
  }
  buf[size] = '\0';
  *out = buf;
  *out_len = (size_t)size;
  return 1;

fail:
  fclose(fp);
  return 0;
}

static int guiWriteTextFile(const char *path, const char *buf, size_t len) {
  FILE *fp = fopen(path, "wb");
  int ok;

  if (!fp)
    return 0;
  ok = fwrite(buf, 1, len, fp) == len;
  if (fclose(fp) != 0)
    ok = 0;
  return ok;
}

static int guiLanguageForPath(const char *path) {
  struct editorSyntax *syntax = editorSyntaxForFilename(path);

  return syntax ? syntax->language : BENTO_LANG_NONE;
}

static int guiLanguageHasSignatures(int language) {
  return language == BENTO_LANG_C || language == BENTO_LANG_PYTHON ||
         language == BENTO_LANG_JS || language == BENTO_LANG_GO ||
         language == BENTO_LANG_RUST;
}

static int guiActiveLanguage(void) {
  return E.syntax ? E.syntax->language : BENTO_LANG_NONE;
}

static int guiLanguageCompatible(int wanted, int candidate) {
  if (wanted == BENTO_LANG_NONE)
    return 1;
  if (wanted == candidate)
    return 1;
  if (wanted == BENTO_LANG_C && candidate == BENTO_LANG_C)
    return 1;
  return 0;
}

static int guiStartsWithWordAt(const char *buf, size_t start, size_t end,
                               const char *word) {
  size_t len = strlen(word);

  if (start + len > end || memcmp(buf + start, word, len) != 0)
    return 0;
  return start + len == end ||
         !editorIsIdentChar((unsigned char)buf[start + len]);
}

static size_t guiSkipIdent(const char *buf, size_t pos, size_t end) {
  while (pos < end && editorIsIdentChar((unsigned char)buf[pos]))
    pos++;
  return pos;
}

static size_t guiFindLineEnd(const char *buf, size_t len, size_t pos) {
  while (pos < len && buf[pos] != '\n')
    pos++;
  return pos;
}

static size_t guiTrimLeft(const char *buf, size_t start, size_t end) {
  while (start < end && isspace((unsigned char)buf[start]))
    start++;
  return start;
}

static size_t guiTrimRight(const char *buf, size_t start, size_t end) {
  while (end > start && isspace((unsigned char)buf[end - 1]))
    end--;
  return end;
}

static int guiPreludeLooksLikeReturnType(const char *buf, size_t start,
                                         size_t end) {
  start = guiTrimLeft(buf, start, end);
  end = guiTrimRight(buf, start, end);
  if (start >= end)
    return 0;
  if (end - start == 6 && !memcmp(buf + start, "return", 6))
    return 0;
  if (end - start >= 7 && !memcmp(buf + start, "typedef", 7) &&
      (end - start == 7 ||
       !editorIsIdentChar((unsigned char)buf[start + 7])))
    return 0;

  for (size_t i = start; i < end; i++) {
    unsigned char c = (unsigned char)buf[i];
    if (isalnum(c) || c == '_' || isspace(c) || c == '*' || c == '&')
      continue;
    return 0;
  }
  return 1;
}

static size_t guiSignatureStatementStart(const char *buf, size_t name_start) {
  size_t start = name_start;
  size_t limit = name_start > 512 ? name_start - 512 : 0;

  while (start > limit) {
    char c = buf[start - 1];
    if (c == ';' || c == '{' || c == '}' || c == '#')
      break;
    start--;
  }
  return start;
}

static size_t guiSkipSignatureTrivia(const char *buf, size_t len, size_t pos) {
  for (;;) {
    while (pos < len && isspace((unsigned char)buf[pos]))
      pos++;
    if (pos + 1 < len && buf[pos] == '/' && buf[pos + 1] == '/') {
      pos += 2;
      while (pos < len && buf[pos] != '\n')
        pos++;
      continue;
    }
    if (pos + 1 < len && buf[pos] == '/' && buf[pos + 1] == '*') {
      pos += 2;
      while (pos + 1 < len && !(buf[pos] == '*' && buf[pos + 1] == '/'))
        pos++;
      if (pos + 1 < len)
        pos += 2;
      continue;
    }
    break;
  }
  return pos;
}

static int guiFindMatchingParen(const char *buf, size_t len, size_t open,
                                size_t *out_close) {
  int depth = 0;
  int in_string = 0;
  int in_line_comment = 0;
  int in_block_comment = 0;

  for (size_t i = open; i < len; i++) {
    char c = buf[i];
    char next = i + 1 < len ? buf[i + 1] : '\0';

    if (in_line_comment) {
      if (c == '\n')
        in_line_comment = 0;
      continue;
    }
    if (in_block_comment) {
      if (c == '*' && next == '/') {
        in_block_comment = 0;
        i++;
      }
      continue;
    }
    if (in_string) {
      if (c == '\\' && next) {
        i++;
        continue;
      }
      if (c == in_string)
        in_string = 0;
      continue;
    }
    if (c == '/' && next == '/') {
      in_line_comment = 1;
      i++;
      continue;
    }
    if (c == '/' && next == '*') {
      in_block_comment = 1;
      i++;
      continue;
    }
    if (c == '"' || c == '\'') {
      in_string = c;
      continue;
    }
    if (c == '(') {
      depth++;
      continue;
    }
    if (c == ')') {
      depth--;
      if (depth == 0) {
        *out_close = i;
        return 1;
      }
      if (depth < 0)
        return 0;
    }
  }
  return 0;
}

static void guiCompactSignatureText(const char *buf, size_t start, size_t end,
                                    char *out, size_t out_len) {
  size_t dst = 0;
  int pending_space = 0;

  start = guiTrimLeft(buf, start, end);
  end = guiTrimRight(buf, start, end);
  for (size_t i = start; i < end && dst + 1 < out_len; i++) {
    unsigned char c = (unsigned char)buf[i];

    if (isspace(c)) {
      pending_space = dst > 0;
      continue;
    }
    if (pending_space && dst + 1 < out_len &&
        !strchr("),;]", (char)c) && out[dst - 1] != '(' &&
        out[dst - 1] != '*' && out[dst - 1] != '&')
      out[dst++] = ' ';
    out[dst++] = (char)c;
    pending_space = 0;
  }
  out[dst] = '\0';
}

static int guiParseCSignatureAt(const char *buf, size_t len, size_t open,
                               char *name, size_t name_len, char *signature,
                               size_t signature_len) {
  size_t name_start, name_end, stmt_start, close, after;
  int nlen;

  if (buf[open] != '(')
    return 0;
  name_end = open;
  while (name_end > 0 && isspace((unsigned char)buf[name_end - 1]))
    name_end--;
  name_start = name_end;
  while (name_start > 0 &&
         editorIsIdentChar((unsigned char)buf[name_start - 1]))
    name_start--;
  nlen = (int)(name_end - name_start);
  if (nlen <= 0 || nlen >= (int)name_len)
    return 0;
  if (editorIsControlLikeToken(buf + name_start, nlen))
    return 0;

  stmt_start = guiSignatureStatementStart(buf, name_start);
  stmt_start = guiSkipSignatureTrivia(buf, name_start, stmt_start);
  if (!guiPreludeLooksLikeReturnType(buf, stmt_start, name_start))
    return 0;
  if (!guiFindMatchingParen(buf, len, open, &close))
    return 0;

  after = guiSkipSignatureTrivia(buf, len, close + 1);
  if (after >= len || (buf[after] != ';' && buf[after] != '{'))
    return 0;

  memcpy(name, buf + name_start, (size_t)nlen);
  name[nlen] = '\0';
  guiCompactSignatureText(buf, stmt_start, close + 1, signature, signature_len);
  return signature[0] != '\0';
}

static int guiLineForOffset(const char *buf, size_t offset) {
  int row = 0;

  for (size_t i = 0; i < offset; i++) {
    if (buf[i] == '\n')
      row++;
  }
  return row;
}

static void guiClearSignatureIndex(void) { G.signature_count = 0; }

static void guiAddSignature(int language, const char *name,
                            const char *signature, const char *path, int row) {
  guiSignature *sig;

  if (G.signature_count >= GUI_MAX_PROJECT_SIGNATURES)
    return;
  for (int i = 0; i < G.signature_count; i++) {
    if (G.signatures[i].language == language &&
        !strcmp(G.signatures[i].name, name) &&
        !strcmp(G.signatures[i].signature, signature))
      return;
  }
  if (G.signature_count == G.signature_cap) {
    int new_cap = G.signature_cap ? G.signature_cap * 2 : 128;
    guiSignature *new_sigs =
        realloc(G.signatures, sizeof(guiSignature) * new_cap);
    if (!new_sigs)
      return;
    G.signatures = new_sigs;
    G.signature_cap = new_cap;
  }

  sig = &G.signatures[G.signature_count++];
  sig->language = language;
  snprintf(sig->name, sizeof(sig->name), "%s", name);
  snprintf(sig->signature, sizeof(sig->signature), "%s", signature);
  snprintf(sig->path, sizeof(sig->path), "%s", path ? path : "");
  sig->row = row;
}

static void guiCollectCSignaturesFromText(const char *buf, size_t len,
                                         const char *path) {
  int in_string = 0;
  int in_line_comment = 0;
  int in_block_comment = 0;

  for (size_t i = 0; i < len; i++) {
    char c = buf[i];
    char next = i + 1 < len ? buf[i + 1] : '\0';

    if (in_line_comment) {
      if (c == '\n')
        in_line_comment = 0;
      continue;
    }
    if (in_block_comment) {
      if (c == '*' && next == '/') {
        in_block_comment = 0;
        i++;
      }
      continue;
    }
    if (in_string) {
      if (c == '\\' && next) {
        i++;
        continue;
      }
      if (c == in_string)
        in_string = 0;
      continue;
    }
    if (c == '/' && next == '/') {
      in_line_comment = 1;
      i++;
      continue;
    }
    if (c == '/' && next == '*') {
      in_block_comment = 1;
      i++;
      continue;
    }
    if (c == '"' || c == '\'') {
      in_string = c;
      continue;
    }
    if (c == '(') {
      char name[GUI_SIGNATURE_NAME_LEN];
      char signature[GUI_SIGNATURE_TEXT_LEN];

      if (guiParseCSignatureAt(buf, len, i, name, sizeof(name), signature,
                              sizeof(signature)))
        guiAddSignature(BENTO_LANG_C, name, signature, path,
                        guiLineForOffset(buf, i));
    }
  }
}

static int guiFindCSignatureInText(const char *buf, size_t len,
                                  const char *target, char *out,
                                  size_t out_len) {
  int in_string = 0;
  int in_line_comment = 0;
  int in_block_comment = 0;

  for (size_t i = 0; i < len; i++) {
    char c = buf[i];
    char next = i + 1 < len ? buf[i + 1] : '\0';

    if (in_line_comment) {
      if (c == '\n')
        in_line_comment = 0;
      continue;
    }
    if (in_block_comment) {
      if (c == '*' && next == '/') {
        in_block_comment = 0;
        i++;
      }
      continue;
    }
    if (in_string) {
      if (c == '\\' && next) {
        i++;
        continue;
      }
      if (c == in_string)
        in_string = 0;
      continue;
    }
    if (c == '/' && next == '/') {
      in_line_comment = 1;
      i++;
      continue;
    }
    if (c == '/' && next == '*') {
      in_block_comment = 1;
      i++;
      continue;
    }
    if (c == '"' || c == '\'') {
      in_string = c;
      continue;
    }
    if (buf[i] == '(') {
      char name[GUI_SIGNATURE_NAME_LEN];
      char signature[GUI_SIGNATURE_TEXT_LEN];

      if (guiParseCSignatureAt(buf, len, i, name, sizeof(name), signature,
                              sizeof(signature)) &&
          !strcmp(name, target)) {
        snprintf(out, out_len, "%s", signature);
        return 1;
      }
    }
  }
  return 0;
}

static int guiCopyIdentifierAt(const char *buf, size_t start, size_t end,
                               char *name, size_t name_len,
                               size_t *out_end) {
  size_t pos = start;

  if (pos >= end || !(isalpha((unsigned char)buf[pos]) || buf[pos] == '_'))
    return 0;
  pos++;
  while (pos < end && editorIsIdentChar((unsigned char)buf[pos]))
    pos++;
  if (pos - start >= name_len)
    return 0;
  memcpy(name, buf + start, pos - start);
  name[pos - start] = '\0';
  if (out_end)
    *out_end = pos;
  return 1;
}

static int guiFindCloseParenInRange(const char *buf, size_t open, size_t end,
                                    size_t *out_close) {
  int depth = 0;
  int in_string = 0;

  for (size_t i = open; i < end; i++) {
    char c = buf[i];
    char next = i + 1 < end ? buf[i + 1] : '\0';

    if (in_string) {
      if (c == '\\' && next) {
        i++;
        continue;
      }
      if (c == in_string)
        in_string = 0;
      continue;
    }
    if (c == '"' || c == '\'' || c == '`') {
      in_string = c;
      continue;
    }
    if (c == '(')
      depth++;
    else if (c == ')') {
      depth--;
      if (depth == 0) {
        *out_close = i;
        return 1;
      }
    }
  }
  return 0;
}

static size_t guiSignatureEndBefore(const char *buf, size_t start, size_t end,
                                    const char *stop_chars) {
  for (size_t pos = start; pos < end; pos++) {
    if (strchr(stop_chars, buf[pos]))
      return pos;
  }
  return end;
}

static int guiParsePythonSignatureLine(const char *buf, size_t start,
                                       size_t end, char *name,
                                       size_t name_len, char *signature,
                                       size_t signature_len) {
  size_t pos = guiTrimLeft(buf, start, end);
  size_t sig_start = pos;
  size_t name_end, open, close, sig_end;

  if (guiStartsWithWordAt(buf, pos, end, "async")) {
    pos = guiTrimLeft(buf, pos + 5, end);
  }
  if (!guiStartsWithWordAt(buf, pos, end, "def"))
    return 0;
  pos = guiTrimLeft(buf, pos + 3, end);
  if (!guiCopyIdentifierAt(buf, pos, end, name, name_len, &name_end))
    return 0;
  open = guiTrimLeft(buf, name_end, end);
  if (open >= end || buf[open] != '(')
    return 0;
  if (!guiFindCloseParenInRange(buf, open, end, &close))
    return 0;
  sig_end = close + 1;
  pos = guiTrimLeft(buf, sig_end, end);
  if (pos + 1 < end && buf[pos] == '-' && buf[pos + 1] == '>')
    sig_end = guiSignatureEndBefore(buf, pos, end, ":#");
  guiCompactSignatureText(buf, sig_start, sig_end, signature, signature_len);
  return signature[0] != '\0';
}

static int guiParseJsSignatureLine(const char *buf, size_t start, size_t end,
                                   char *name, size_t name_len,
                                   char *signature, size_t signature_len) {
  size_t pos = guiTrimLeft(buf, start, end);
  size_t sig_start = pos;
  size_t name_start, name_end, open, close, sig_end;

  if (guiStartsWithWordAt(buf, pos, end, "export")) {
    pos = guiTrimLeft(buf, pos + 6, end);
    if (guiStartsWithWordAt(buf, pos, end, "default"))
      pos = guiTrimLeft(buf, pos + 7, end);
    sig_start = pos;
  }
  if (guiStartsWithWordAt(buf, pos, end, "async"))
    pos = guiTrimLeft(buf, pos + 5, end);

  if (guiStartsWithWordAt(buf, pos, end, "function")) {
    pos = guiTrimLeft(buf, pos + 8, end);
    if (!guiCopyIdentifierAt(buf, pos, end, name, name_len, &name_end))
      return 0;
    open = guiTrimLeft(buf, name_end, end);
    if (open >= end || buf[open] != '(')
      return 0;
    if (!guiFindCloseParenInRange(buf, open, end, &close))
      return 0;
    sig_end = guiSignatureEndBefore(buf, close + 1, end, "{;");
    guiCompactSignatureText(buf, sig_start, sig_end, signature, signature_len);
    return signature[0] != '\0';
  }

  if (guiStartsWithWordAt(buf, pos, end, "const") ||
      guiStartsWithWordAt(buf, pos, end, "let") ||
      guiStartsWithWordAt(buf, pos, end, "var")) {
    if (guiStartsWithWordAt(buf, pos, end, "const"))
      pos += 5;
    else
      pos += 3;
    pos = guiTrimLeft(buf, pos, end);
    if (!guiCopyIdentifierAt(buf, pos, end, name, name_len, &name_end))
      return 0;
    pos = guiTrimLeft(buf, name_end, end);
    if (pos >= end || buf[pos] != '=')
      return 0;
    pos = guiTrimLeft(buf, pos + 1, end);
    if (guiStartsWithWordAt(buf, pos, end, "async"))
      pos = guiTrimLeft(buf, pos + 5, end);
    open = pos;
    if (open >= end || buf[open] != '(')
      return 0;
    if (!guiFindCloseParenInRange(buf, open, end, &close))
      return 0;
    sig_end = guiSignatureEndBefore(buf, close + 1, end, "={;");
    guiCompactSignatureText(buf, sig_start, sig_end, signature, signature_len);
    return signature[0] != '\0';
  }

  name_start = pos;
  if (!guiCopyIdentifierAt(buf, name_start, end, name, name_len, &name_end))
    return 0;
  if (editorIsControlLikeToken(name, (int)strlen(name)))
    return 0;
  open = guiTrimLeft(buf, name_end, end);
  if (open >= end || buf[open] != '(')
    return 0;
  if (!guiFindCloseParenInRange(buf, open, end, &close))
    return 0;
  sig_end = guiSignatureEndBefore(buf, close + 1, end, "{;");
  guiCompactSignatureText(buf, sig_start, sig_end, signature, signature_len);
  return signature[0] != '\0';
}

static int guiParseGoSignatureLine(const char *buf, size_t start, size_t end,
                                   char *name, size_t name_len,
                                   char *signature, size_t signature_len) {
  size_t pos = guiTrimLeft(buf, start, end);
  size_t name_end, open, close, sig_end;

  if (!guiStartsWithWordAt(buf, pos, end, "func"))
    return 0;
  pos = guiTrimLeft(buf, pos + 4, end);
  if (pos < end && buf[pos] == '(') {
    if (!guiFindCloseParenInRange(buf, pos, end, &close))
      return 0;
    pos = guiTrimLeft(buf, close + 1, end);
  }
  if (!guiCopyIdentifierAt(buf, pos, end, name, name_len, &name_end))
    return 0;
  open = guiTrimLeft(buf, name_end, end);
  if (open >= end || buf[open] != '(')
    return 0;
  if (!guiFindCloseParenInRange(buf, open, end, &close))
    return 0;
  sig_end = guiSignatureEndBefore(buf, close + 1, end, "{");
  guiCompactSignatureText(buf, start, sig_end, signature, signature_len);
  return signature[0] != '\0';
}

static int guiParseRustSignatureLine(const char *buf, size_t start, size_t end,
                                     char *name, size_t name_len,
                                     char *signature, size_t signature_len) {
  size_t pos = guiTrimLeft(buf, start, end);
  size_t fn_pos = end, name_end, open, close, sig_end;

  while (pos < end) {
    if (guiStartsWithWordAt(buf, pos, end, "fn")) {
      fn_pos = pos;
      break;
    }
    if (!editorIsIdentChar((unsigned char)buf[pos])) {
      pos++;
      continue;
    }
    pos = guiSkipIdent(buf, pos, end);
    pos = guiTrimLeft(buf, pos, end);
  }
  if (fn_pos == end)
    return 0;
  pos = guiTrimLeft(buf, fn_pos + 2, end);
  if (!guiCopyIdentifierAt(buf, pos, end, name, name_len, &name_end))
    return 0;
  open = guiTrimLeft(buf, name_end, end);
  if (open >= end || buf[open] != '(')
    return 0;
  if (!guiFindCloseParenInRange(buf, open, end, &close))
    return 0;
  sig_end = guiSignatureEndBefore(buf, close + 1, end, "{;");
  guiCompactSignatureText(buf, start, sig_end, signature, signature_len);
  return signature[0] != '\0';
}

static int guiParseLineSignature(int language, const char *buf, size_t start,
                                 size_t end, char *name, size_t name_len,
                                 char *signature, size_t signature_len) {
  switch (language) {
  case BENTO_LANG_PYTHON:
    return guiParsePythonSignatureLine(buf, start, end, name, name_len,
                                       signature, signature_len);
  case BENTO_LANG_JS:
    return guiParseJsSignatureLine(buf, start, end, name, name_len, signature,
                                   signature_len);
  case BENTO_LANG_GO:
    return guiParseGoSignatureLine(buf, start, end, name, name_len, signature,
                                   signature_len);
  case BENTO_LANG_RUST:
    return guiParseRustSignatureLine(buf, start, end, name, name_len, signature,
                                     signature_len);
  default:
    return 0;
  }
}

static void guiCollectLineSignaturesFromText(int language, const char *buf,
                                             size_t len, const char *path) {
  size_t pos = 0;
  int row = 0;

  while (pos < len) {
    size_t end = guiFindLineEnd(buf, len, pos);
    char name[GUI_SIGNATURE_NAME_LEN];
    char signature[GUI_SIGNATURE_TEXT_LEN];

    if (guiParseLineSignature(language, buf, pos, end, name, sizeof(name),
                              signature, sizeof(signature)))
      guiAddSignature(language, name, signature, path, row);
    pos = end < len ? end + 1 : end;
    row++;
  }
}

static int guiFindLineSignatureInText(int language, const char *buf,
                                      size_t len, const char *target,
                                      char *out, size_t out_len) {
  size_t pos = 0;

  while (pos < len) {
    size_t end = guiFindLineEnd(buf, len, pos);
    char name[GUI_SIGNATURE_NAME_LEN];
    char signature[GUI_SIGNATURE_TEXT_LEN];

    if (guiParseLineSignature(language, buf, pos, end, name, sizeof(name),
                              signature, sizeof(signature)) &&
        !strcmp(name, target)) {
      snprintf(out, out_len, "%s", signature);
      return 1;
    }
    pos = end < len ? end + 1 : end;
  }
  return 0;
}

static void guiCollectSignaturesFromText(int language, const char *buf,
                                         size_t len, const char *path) {
  if (language == BENTO_LANG_C)
    guiCollectCSignaturesFromText(buf, len, path);
  else
    guiCollectLineSignaturesFromText(language, buf, len, path);
}

static int guiFindSignatureInText(int language, const char *buf, size_t len,
                                  const char *target, char *out,
                                  size_t out_len) {
  if (language == BENTO_LANG_C)
    return guiFindCSignatureInText(buf, len, target, out, out_len);
  return guiFindLineSignatureInText(language, buf, len, target, out, out_len);
}

static void guiEnsureSignatureIndex(void) {
  if (!G.signature_dirty)
    return;

  guiClearSignatureIndex();
  for (int i = 0; i < G.file_count; i++) {
    char *buf = NULL;
    size_t len = 0;
    int language = guiLanguageForPath(G.files[i].path);

    if (G.files[i].is_dir || !guiLanguageHasSignatures(language))
      continue;
    if (guiReadTextFile(G.files[i].path, &buf, &len)) {
      guiCollectSignaturesFromText(language, buf, len, G.files[i].path);
      free(buf);
    }
  }
  G.signature_dirty = 0;
}

static const char *guiFindCurrentBufferSignature(const char *name) {
  static char signature[GUI_SIGNATURE_TEXT_LEN];
  int len = 0;
  int language = guiActiveLanguage();
  char *buf;

  if (!E.filename || !guiLanguageHasSignatures(language))
    return NULL;

  buf = editorRowsToString(&len);
  if (!buf)
    return NULL;
  if (guiFindSignatureInText(language, buf, (size_t)len, name, signature,
                             sizeof(signature))) {
    free(buf);
    return signature;
  }
  free(buf);
  return NULL;
}

static const char *guiFindProjectSignature(const char *name) {
  int language = guiActiveLanguage();

  guiEnsureSignatureIndex();
  for (int i = 0; i < G.signature_count; i++) {
    if (guiLanguageCompatible(language, G.signatures[i].language) &&
        !strcmp(G.signatures[i].name, name))
      return G.signatures[i].signature;
  }
  return NULL;
}

static int guiFindBoundaryOk(const char *text, int len, int start, int qlen) {
  int before = start > 0 ? (unsigned char)text[start - 1] : 0;
  int after = start + qlen < len ? (unsigned char)text[start + qlen] : 0;
  return (start == 0 || !editorIsIdentChar(before)) &&
         (start + qlen >= len || !editorIsIdentChar(after));
}

static int guiQueryMatchesAt(const char *text, int len, int start) {
  if (G.query_len == 0 || start < 0 || start + G.query_len > len)
    return 0;
  for (int i = 0; i < G.query_len; i++) {
    unsigned char a = (unsigned char)text[start + i];
    unsigned char b = (unsigned char)G.query[i];
    if (!G.find_case_sensitive) {
      a = (unsigned char)guiAsciiLower(a);
      b = (unsigned char)guiAsciiLower(b);
    }
    if (a != b)
      return 0;
  }
  if (G.find_whole_word &&
      !guiFindBoundaryOk(text, len, start, G.query_len))
    return 0;
  return 1;
}

static const char *guiFindQueryInRange(const char *text, int len, int start) {
  if (!text || G.query_len == 0 || len < G.query_len)
    return NULL;
  if (start < 0)
    start = 0;
  for (int i = start; i <= len - G.query_len; i++) {
    if (guiQueryMatchesAt(text, len, i))
      return text + i;
  }
  return NULL;
}

static int guiReplaceInTextBuffer(char **buf, size_t *len) {
  const char *scan;
  char *out;
  char *dst;
  size_t query_len = (size_t)G.query_len;
  size_t replacement_len = (size_t)G.replacement_len;
  size_t count = 0;
  size_t new_len;

  if (query_len == 0)
    return 0;

  scan = *buf;
  while ((scan = guiFindQueryInRange(*buf, (int)*len,
                                     (int)(scan - *buf))) != NULL) {
    count++;
    scan += query_len;
  }
  if (count == 0)
    return 0;

  new_len = *len + count * replacement_len - count * query_len;
  out = malloc(new_len + 1);
  if (!out)
    return 0;

  scan = *buf;
  dst = out;
  while (1) {
    const char *match = guiFindQueryInRange(*buf, (int)*len,
                                            (int)(scan - *buf));
    size_t prefix_len;

    if (!match)
      break;
    prefix_len = (size_t)(match - scan);
    memcpy(dst, scan, prefix_len);
    dst += prefix_len;
    memcpy(dst, G.replacement, replacement_len);
    dst += replacement_len;
    scan = match + query_len;
  }
  strcpy(dst, scan);

  free(*buf);
  *buf = out;
  *len = new_len;
  return (int)count;
}

static int guiDigits(int value) {
  int digits = 1;
  while (value >= 10) {
    value /= 10;
    digits++;
  }
  return digits;
}

static int guiGutterWidth(void) {
  int digits = guiDigits(guiMaxInt(tbLineCount(TB), 1));
  int width = (int)(digits * G.char_width) + 26;
  return guiMaxInt(GUI_MIN_GUTTER_WIDTH, width);
}

static int guiEditorTop(void) {
  return GUI_TAB_BAR_HEIGHT + (G.find_active ? GUI_FIND_BAR_HEIGHT : 0) +
         (G.find_active && G.find_all_active ? GUI_FIND_RESULTS_HEIGHT : 0);
}

static Color guiSyntaxColor(int hl) {
  switch (hl) {
  case HL_COMMENT:
  case HL_MLCOMMENT:
    return guiHex(0x8f908a);
  case HL_KEYWORD1:
    return guiHex(0xf92672);
  case HL_KEYWORD2:
    return guiHex(0x66d9ef);
  case HL_STRING:
    return guiHex(0xe6db74);
  case HL_NUMBER:
    return guiHex(0xae81ff);
  case HL_MATCH:
    return guiHex(0xf8f8f2);
  case HL_FUNCTION:
    return guiHex(0xa6e22e);
  case HL_NONPRINT:
    return guiHex(0xfd971f);
  default:
    return GUI_TEXT;
  }
}

static void guiClampEditorPosition(void) {
  if (E.rowoff < 0)
    E.rowoff = 0;
  if (E.coloff < 0)
    E.coloff = 0;
  if (E.screenrows < 1)
    E.screenrows = 1;
  if (E.screencols < 1)
    E.screencols = 1;
  if (E.cy < 0)
    E.cy = 0;
  if (E.cx < 0)
    E.cx = 0;
  if (E.cy >= E.screenrows)
    E.cy = E.screenrows - 1;
  if (E.cx >= E.screencols)
    E.cx = E.screencols - 1;
  if (E.rowoff > tbLineCount(TB))
    E.rowoff = tbLineCount(TB);
}

static void guiResetEditorSmoothScroll(void) { G.editor_scroll_y = 0.0f; }

static void guiUpdateLayout(void) {
  Vector2 measure;
  int editor_width, editor_height;

  measure = MeasureTextEx(G.font, "M", G.font_size, GUI_FONT_SPACING);
  G.char_width = measure.x > 1.0f ? measure.x : G.font_size * 0.6f;
  /* Measure once at small_font_size too — the gutter and status bar use
   * this size and run hot enough that per-row MeasureTextEx was a real
   * cost (≈1 ms/frame on a 50-row viewport). */
  measure = MeasureTextEx(G.font, "M", G.small_font_size, GUI_FONT_SPACING);
  G.small_char_width = measure.x > 1.0f ? measure.x : G.small_font_size * 0.6f;
  G.line_height = G.font_size + 6.0f;

  editor_width = GetScreenWidth() - guiSidebarWidth() - guiGutterWidth() -
                 GUI_TEXT_PADDING_X * 2;
  editor_height = GetScreenHeight() - guiEditorTop() - GUI_STATUS_BAR_HEIGHT -
                  GUI_TEXT_PADDING_Y * 2;
  E.screencols = guiMaxInt(1, (int)(editor_width / G.char_width));
  E.screenrows = guiMaxInt(1, (int)(editor_height / G.line_height));
  guiClampEditorPosition();
  if (G.editor_scroll_y < 0.0f || G.editor_scroll_y >= G.line_height)
    guiResetEditorSmoothScroll();
}

static void guiMoveCursorToFilePosition(int filerow, int filecol) {
  guiResetEditorSmoothScroll();

  if (filerow < 0)
    filerow = 0;
  if (filerow > tbLineCount(TB))
    filerow = tbLineCount(TB);
  if (filecol < 0)
    filecol = 0;

  if (filerow < E.rowoff) {
    E.rowoff = filerow;
    E.cy = 0;
  } else if (filerow >= E.rowoff + E.screenrows) {
    E.rowoff = filerow - E.screenrows + 1;
    E.cy = E.screenrows - 1;
  } else {
    E.cy = filerow - E.rowoff;
  }

  if (filecol < E.coloff) {
    E.coloff = filecol;
    E.cx = 0;
  } else if (filecol >= E.coloff + E.screencols) {
    E.coloff = filecol - E.screencols + 1;
    E.cx = E.screencols - 1;
  } else {
    E.cx = filecol - E.coloff;
  }
  guiClampEditorPosition();
}

static int guiCursorFileRow(void) { return E.rowoff + E.cy; }

static int guiCursorFileCol(void) { return E.coloff + E.cx; }

static int guiPositionCompare(int row_a, int col_a, int row_b, int col_b) {
  if (row_a != row_b)
    return row_a < row_b ? -1 : 1;
  if (col_a != col_b)
    return col_a < col_b ? -1 : 1;
  return 0;
}

static void guiClearSelection(void) {
  G.selection_active = 0;
  G.mouse_selecting = 0;
}

static void guiBeginSelectionIfNeeded(void) {
  if (G.selection_active)
    return;
  G.selection_active = 1;
  G.selection_anchor_row = guiCursorFileRow();
  G.selection_anchor_col = guiCursorFileCol();
  G.selection_cursor_row = G.selection_anchor_row;
  G.selection_cursor_col = G.selection_anchor_col;
}

static void guiUpdateSelectionToCursor(void) {
  if (!G.selection_active)
    return;
  G.selection_cursor_row = guiCursorFileRow();
  G.selection_cursor_col = guiCursorFileCol();
  if (G.selection_anchor_row == G.selection_cursor_row &&
      G.selection_anchor_col == G.selection_cursor_col)
    G.selection_active = 0;
}

static int guiHasSelection(void) {
  return G.selection_active &&
         (G.selection_anchor_row != G.selection_cursor_row ||
          G.selection_anchor_col != G.selection_cursor_col);
}

static void guiSelectionBounds(int *start_row, int *start_col, int *end_row,
                               int *end_col) {
  if (guiPositionCompare(G.selection_anchor_row, G.selection_anchor_col,
                         G.selection_cursor_row, G.selection_cursor_col) <= 0) {
    *start_row = G.selection_anchor_row;
    *start_col = G.selection_anchor_col;
    *end_row = G.selection_cursor_row;
    *end_col = G.selection_cursor_col;
  } else {
    *start_row = G.selection_cursor_row;
    *start_col = G.selection_cursor_col;
    *end_row = G.selection_anchor_row;
    *end_col = G.selection_anchor_col;
  }
}

static int guiRowCxToRx(erow *row, int cx) {
  int rx = 0;

  if (cx < 0)
    cx = 0;
  if (cx > row->size)
    cx = row->size;

  for (int j = 0; j < cx; j++) {
    if (row->chars[j] == TAB)
      rx += g_tab_stop - (rx % g_tab_stop);
    else
      rx++;
  }
  return rx;
}

static int guiSelectionRangeForRow(int filerow, int *start_rx, int *end_rx) {
  int start_row, start_col, end_row, end_col;
  int row_start_col, row_end_col;
  erow *row;
  int row_size;

  if (!guiHasSelection() || filerow < 0 || filerow >= tbLineCount(TB))
    return 0;

  guiSelectionBounds(&start_row, &start_col, &end_row, &end_col);
  if (filerow < start_row || filerow > end_row)
    return 0;

  row = tbRowMut(TB, filerow);
  row_size = tbLineLength(TB, filerow);
  row_start_col = filerow == start_row ? start_col : 0;
  row_end_col = filerow == end_row ? end_col : row_size;
  if (row_start_col < 0)
    row_start_col = 0;
  if (row_end_col < 0)
    row_end_col = 0;
  if (row_start_col > row_size)
    row_start_col = row_size;
  if (row_end_col > row_size)
    row_end_col = row_size;
  if (row_start_col == row_end_col)
    return 0;

  *start_rx = guiRowCxToRx(row, row_start_col);
  *end_rx = guiRowCxToRx(row, row_end_col);
  return *start_rx != *end_rx;
}

static void guiMoveCursorWithSelection(int key) {
  guiResetEditorSmoothScroll();

  if (guiShiftDown())
    guiBeginSelectionIfNeeded();
  else
    guiClearSelection();

  editorMoveCursor(key);

  if (guiShiftDown())
    guiUpdateSelectionToCursor();
}

static void guiMoveCursorByLinesWithSelection(int amount) {
  int key = amount > 0 ? ARROW_DOWN : ARROW_UP;
  int count = amount > 0 ? amount : -amount;

  if (count == 0)
    return;
  guiResetEditorSmoothScroll();

  if (guiShiftDown())
    guiBeginSelectionIfNeeded();
  else
    guiClearSelection();

  while (count--)
    editorMoveCursor(key);

  if (guiShiftDown())
    guiUpdateSelectionToCursor();
}

static void guiMoveCursorToPositionWithSelection(int filerow, int filecol,
                                                 int extend_selection) {
  if (extend_selection)
    guiBeginSelectionIfNeeded();
  else
    guiClearSelection();

  guiMoveCursorToFilePosition(filerow, filecol);

  if (extend_selection)
    guiUpdateSelectionToCursor();
}

static int guiCharClassInRow(erow *row, int col);

static int guiSelectWordAtPosition(int filerow, int filecol) {
  erow *row;
  int klass, start, end;

  if (filerow < 0 || filerow >= tbLineCount(TB))
    return 0;
  row = tbRowMut(TB, filerow);
  if (row->size == 0)
    return 0;
  if (filecol < 0)
    filecol = 0;
  if (filecol >= row->size)
    filecol = row->size - 1;

  klass = guiCharClassInRow(row, filecol);
  if (klass == 0 && filecol > 0) {
    filecol--;
    klass = guiCharClassInRow(row, filecol);
  }
  if (klass == 0)
    return 0;

  start = filecol;
  while (start > 0 && guiCharClassInRow(row, start - 1) == klass)
    start--;
  end = filecol + 1;
  while (end < row->size && guiCharClassInRow(row, end) == klass)
    end++;

  G.selection_active = 1;
  G.selection_anchor_row = filerow;
  G.selection_anchor_col = start;
  G.selection_cursor_row = filerow;
  G.selection_cursor_col = end;
  guiMoveCursorToFilePosition(filerow, end);
  return 1;
}

static int guiCharClass(int c) {
  if (isspace((unsigned char)c))
    return 0;
  if (isalpha((unsigned char)c) || c == '_')
    return 1;
  if (isdigit((unsigned char)c))
    return 2;
  return 3;
}

static int guiCharClassInRow(erow *row, int col) {
  int c;

  if (!row || col < 0 || col >= row->size)
    return 0;
  c = row->chars[col];
  if (c == '.') {
    int prev_digit =
        col > 0 && isdigit((unsigned char)row->chars[col - 1]);
    int next_digit =
        col + 1 < row->size && isdigit((unsigned char)row->chars[col + 1]);

    if (prev_digit || next_digit)
      return 2;
  }
  return guiCharClass(c);
}

static int guiPositionBeforeFileStart(int row, int col) {
  return row <= 0 && col <= 0;
}

static int guiPositionAtFileEnd(int row, int col) {
  int rowcount = tbLineCount(TB);
  return row >= rowcount ||
         (row == rowcount - 1 && col >= tbLineLength(TB, row));
}

static int guiMovePositionLeftOne(int *row, int *col) {
  int rowcount = tbLineCount(TB);
  if (guiPositionBeforeFileStart(*row, *col))
    return 0;
  if (*row >= rowcount)
    *row = rowcount - 1;
  if (*col > 0) {
    (*col)--;
    return 1;
  }
  (*row)--;
  *col = tbLineLength(TB, *row);
  return 1;
}

static int guiMovePositionRightOne(int *row, int *col) {
  if (guiPositionAtFileEnd(*row, *col))
    return 0;
  if (*col < tbLineLength(TB, *row)) {
    (*col)++;
    return 1;
  }
  (*row)++;
  *col = 0;
  return *row < tbLineCount(TB);
}

static int guiCharClassAtPosition(int row, int col) {
  if (row < 0 || row >= tbLineCount(TB))
    return 0;
  return guiCharClassInRow(tbRowMut(TB, row), col);
}

static int guiCharClassBeforePosition(int row, int col) {
  if (row < 0 || row >= tbLineCount(TB))
    return 0;
  if (col <= 0)
    return 0;
  return guiCharClassInRow(tbRowMut(TB, row), col - 1);
}

static void guiMoveClassBoundaryRight(int *row, int *col) {
  int start_class;
  int rowcount = tbLineCount(TB);

  if (rowcount == 0 || guiPositionAtFileEnd(*row, *col))
    return;
  if (*row < 0)
    *row = 0;
  if (*row >= rowcount) {
    *row = rowcount - 1;
    *col = tbLineLength(TB, *row);
  }
  if (*col < 0)
    *col = 0;
  if (*col > tbLineLength(TB, *row))
    *col = tbLineLength(TB, *row);

  start_class = guiCharClassAtPosition(*row, *col);
  while (!guiPositionAtFileEnd(*row, *col) &&
         guiCharClassAtPosition(*row, *col) == start_class)
    guiMovePositionRightOne(row, col);

  if (start_class == 0) {
    int next_class = guiCharClassAtPosition(*row, *col);

    while (!guiPositionAtFileEnd(*row, *col) &&
           guiCharClassAtPosition(*row, *col) == next_class)
      guiMovePositionRightOne(row, col);
  }
}

static void guiMoveClassBoundaryLeft(int *row, int *col) {
  int start_class;
  int rowcount = tbLineCount(TB);

  if (rowcount == 0 || guiPositionBeforeFileStart(*row, *col))
    return;
  if (*row >= rowcount) {
    *row = rowcount - 1;
    *col = tbLineLength(TB, *row);
  }
  if (*col < 0)
    *col = 0;
  if (*col > tbLineLength(TB, *row))
    *col = tbLineLength(TB, *row);

  start_class = guiCharClassBeforePosition(*row, *col);
  while (!guiPositionBeforeFileStart(*row, *col) &&
         guiCharClassBeforePosition(*row, *col) == start_class)
    guiMovePositionLeftOne(row, col);

  if (start_class == 0) {
    int next_class = guiCharClassBeforePosition(*row, *col);

    while (!guiPositionBeforeFileStart(*row, *col) &&
           guiCharClassBeforePosition(*row, *col) == next_class)
      guiMovePositionLeftOne(row, col);
  }
}

static void guiMoveClassBoundaryWithSelection(int direction) {
  int row = guiCursorFileRow();
  int col = guiCursorFileCol();

  if (guiShiftDown())
    guiBeginSelectionIfNeeded();
  else
    guiClearSelection();

  if (direction < 0)
    guiMoveClassBoundaryLeft(&row, &col);
  else
    guiMoveClassBoundaryRight(&row, &col);
  guiMoveCursorToFilePosition(row, col);

  if (guiShiftDown())
    guiUpdateSelectionToCursor();
}

static void guiScrollRowsByPixels(float pixels) {
  int filerow = E.rowoff + E.cy;
  float line_height =
      G.line_height > 1.0f ? G.line_height : GUI_FONT_SIZE + 6.0f;

  if (pixels == 0.0f)
    return;

  G.editor_scroll_y += pixels;

  int rowcount = tbLineCount(TB);
  while (G.editor_scroll_y >= line_height && E.rowoff < rowcount) {
    E.rowoff++;
    G.editor_scroll_y -= line_height;
  }
  while (G.editor_scroll_y < 0.0f && E.rowoff > 0) {
    E.rowoff--;
    G.editor_scroll_y += line_height;
  }

  if ((E.rowoff <= 0 && G.editor_scroll_y < 0.0f) ||
      (E.rowoff >= rowcount && G.editor_scroll_y > 0.0f))
    G.editor_scroll_y = 0.0f;

  if (filerow < E.rowoff)
    E.cy = 0;
  else if (filerow >= E.rowoff + E.screenrows)
    E.cy = E.screenrows - 1;
  else
    E.cy = filerow - E.rowoff;
  guiClampEditorPosition();
}

static void guiScrollRowsByWheel(float wheel) {
  guiScrollRowsByPixels(-wheel * G.line_height * GUI_EDITOR_WHEEL_LINES);
}

static void guiScrollColumns(int amount) {
  int filecol = E.coloff + E.cx;

  E.coloff += amount;
  if (E.coloff < 0)
    E.coloff = 0;

  if (filecol < E.coloff)
    E.cx = 0;
  else if (filecol >= E.coloff + E.screencols)
    E.cx = E.screencols - 1;
  else
    E.cx = filecol - E.coloff;
  guiClampEditorPosition();
}

static int guiWheelToLines(float wheel, float *accum, float base_speed) {
  double now = GetTime();
  double dt = G.wheel_last_time > 0.0 ? now - G.wheel_last_time : 1.0 / 60.0;
  float speed;
  float accel = 1.0f;
  int whole;

  if (dt > 0.18) {
    G.wheel_velocity = 0.0f;
    dt = 1.0 / 60.0;
  }
  if (dt < 0.001)
    dt = 0.001;

  speed = guiAbsFloat(wheel) / (float)dt;
  G.wheel_velocity = G.wheel_velocity * 0.62f + speed * 0.38f;
  G.wheel_last_time = now;

  if (G.wheel_velocity > 45.0f)
    accel = 3.0f;
  else if (G.wheel_velocity > 25.0f)
    accel = 2.25f;
  else if (G.wheel_velocity > 12.0f)
    accel = 1.55f;

  *accum += -wheel * base_speed * accel;
  whole = (int)*accum;
  if (whole != 0)
    *accum -= (float)whole;
  return whole;
}

static void guiScrollTreeByLines(int amount) {
  if (amount == 0)
    return;
  G.tree_scroll += amount;
  if (G.tree_scroll < 0)
    G.tree_scroll = 0;
  if (G.tree_scroll > G.file_count - 1)
    G.tree_scroll = guiMaxInt(0, G.file_count - 1);
}

static void guiScrollMatchesByLines(int amount) {
  int stored_count = guiMinInt(G.match_count, GUI_MAX_FIND_RESULTS);

  if (amount == 0)
    return;
  G.match_scroll += amount;
  if (G.match_scroll < 0)
    G.match_scroll = 0;
  if (G.match_scroll > stored_count - 1)
    G.match_scroll = guiMaxInt(0, stored_count - 1);
}

static void guiMoveLineStart(void) {
  E.coloff = 0;
  E.cx = 0;
}

static void guiMoveLineEnd(void) {
  int filerow = E.rowoff + E.cy;
  int rowlen = tbLineLength(TB, filerow);

  if (rowlen >= E.screencols) {
    E.coloff = rowlen - E.screencols + 1;
    E.cx = E.screencols - 1;
  } else {
    E.coloff = 0;
    E.cx = rowlen;
  }
  guiClampEditorPosition();
}

static void guiMoveLineBoundaryWithSelection(int to_end) {
  if (guiShiftDown())
    guiBeginSelectionIfNeeded();
  else
    guiClearSelection();

  if (to_end)
    guiMoveLineEnd();
  else
    guiMoveLineStart();

  if (guiShiftDown())
    guiUpdateSelectionToCursor();
}

static void guiMoveDocumentBoundaryWithSelection(int to_end) {
  if (guiShiftDown())
    guiBeginSelectionIfNeeded();
  else
    guiClearSelection();

  if (to_end) {
    int rowcount = tbLineCount(TB);
    if (rowcount > 0)
      guiMoveCursorToFilePosition(rowcount - 1, tbLineLength(TB, rowcount - 1));
    else
      guiMoveCursorToFilePosition(0, 0);
  } else {
    guiMoveCursorToFilePosition(0, 0);
  }

  if (guiShiftDown())
    guiUpdateSelectionToCursor();
}

static void guiDeleteForwardChar(void) {
  int old_cx = E.cx, old_cy = E.cy;
  int old_coloff = E.coloff, old_rowoff = E.rowoff;

  editorMoveCursor(ARROW_RIGHT);
  if (old_cx != E.cx || old_cy != E.cy || old_coloff != E.coloff ||
      old_rowoff != E.rowoff) {
    U.delete_is_forward = 1;
    editorDelChar();
    U.delete_is_forward = 0;
  }
}

static int guiAppendAsciiToBuffer(const char *text, char *buf, int *len,
                                  int max_len, int digits_only) {
  int appended = 0;

  if (!text)
    return 0;

  for (const unsigned char *p = (const unsigned char *)text;
       *p && *len < max_len; p++) {
    if (*p == '\r' || *p == '\n')
      continue;
    if (digits_only && !isdigit(*p))
      continue;
    if (*p >= 32 && *p < 127) {
      buf[(*len)++] = (char)*p;
      appended++;
    }
  }
  buf[*len] = '\0';
  return appended;
}

static int guiTextHasEditableChars(const char *text) {
  if (!text)
    return 0;

  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
    if (*p == '\r' || *p == '\n' || *p == TAB || (*p >= 32 && *p < 127))
      return 1;
  }
  return 0;
}

static void guiRefreshSyntaxFromRow(int start_row) {
  if (start_row < 0)
    start_row = 0;
  /* Just mark the tail dirty; lazy refresh in the renderer picks up only
   * what's visible. The pre-Phase-5 version walked every row down to EOF
   * synchronously, which was O(file_size) on every delete-selection. */
  dirtyMarkFromHere(start_row, tbLineCount(TB));
}

static void guiClampTextPosition(int *row, int *col) {
  int rowcount = tbLineCount(TB);
  if (*row < 0)
    *row = 0;
  if (*row > rowcount)
    *row = rowcount;
  if (*row >= rowcount) {
    *col = 0;
    return;
  }
  if (*col < 0)
    *col = 0;
  int row_size = tbLineLength(TB, *row);
  if (*col > row_size)
    *col = row_size;
}

static int guiSelectedRange(int *start_row, int *start_col, int *end_row,
                            int *end_col) {
  if (!guiHasSelection())
    return 0;

  guiSelectionBounds(start_row, start_col, end_row, end_col);
  guiClampTextPosition(start_row, start_col);
  guiClampTextPosition(end_row, end_col);
  return guiPositionCompare(*start_row, *start_col, *end_row, *end_col) < 0;
}

static char *guiSelectedText(size_t *len_out) {
  int start_row, start_col, end_row, end_col;
  size_t len = 0;
  char *text, *p;

  if (!guiSelectedRange(&start_row, &start_col, &end_row, &end_col))
    return NULL;

  int rowcount = tbLineCount(TB);
  if (start_row == end_row) {
    len = (size_t)(end_col - start_col);
  } else {
    len += (size_t)(tbLineLength(TB, start_row) - start_col) + 1;
    for (int row = start_row + 1; row < end_row; row++)
      len += (size_t)tbLineLength(TB, row) + 1;
    if (end_row < rowcount)
      len += (size_t)end_col;
  }

  text = malloc(len + 1);
  if (!text)
    return NULL;

  p = text;
  if (start_row == end_row) {
    int sz = 0;
    const char *chars = tbLineSlice(TB, start_row, &sz);
    memcpy(p, chars + start_col, len);
    p += len;
  } else {
    int start_size = 0;
    const char *start_chars = tbLineSlice(TB, start_row, &start_size);
    size_t chunk = (size_t)(start_size - start_col);

    memcpy(p, start_chars + start_col, chunk);
    p += chunk;
    *p++ = '\n';
    for (int row = start_row + 1; row < end_row; row++) {
      int sz = 0;
      const char *chars = tbLineSlice(TB, row, &sz);
      chunk = (size_t)sz;
      memcpy(p, chars, chunk);
      p += chunk;
      *p++ = '\n';
    }
    if (end_row < rowcount) {
      const char *end_chars = tbLineSlice(TB, end_row, NULL);
      memcpy(p, end_chars, (size_t)end_col);
      p += end_col;
    }
  }
  *p = '\0';
  if (len_out)
    *len_out = len;
  return text;
}

static char *guiExtractRangeText(int start_row, int start_col, int end_row,
                                 int end_col, size_t *len_out) {
  size_t len = 0;
  char *text, *p;

  if (guiPositionCompare(start_row, start_col, end_row, end_col) >= 0)
    return NULL;

  int rowcount = tbLineCount(TB);
  if (start_row == end_row) {
    len = (size_t)(end_col - start_col);
  } else {
    len += (size_t)(tbLineLength(TB, start_row) - start_col) + 1;
    for (int row = start_row + 1; row < end_row; row++)
      len += (size_t)tbLineLength(TB, row) + 1;
    if (end_row < rowcount)
      len += (size_t)end_col;
  }

  text = malloc(len + 1);
  if (!text)
    return NULL;

  p = text;
  if (start_row == end_row) {
    int sz = 0;
    const char *chars = tbLineSlice(TB, start_row, &sz);
    memcpy(p, chars + start_col, len);
    p += len;
  } else {
    int start_size = 0;
    const char *start_chars = tbLineSlice(TB, start_row, &start_size);
    size_t chunk = (size_t)(start_size - start_col);

    memcpy(p, start_chars + start_col, chunk);
    p += chunk;
    *p++ = '\n';
    for (int row = start_row + 1; row < end_row; row++) {
      int sz = 0;
      const char *chars = tbLineSlice(TB, row, &sz);
      chunk = (size_t)sz;
      memcpy(p, chars, chunk);
      p += chunk;
      *p++ = '\n';
    }
    if (end_row < rowcount) {
      const char *end_chars = tbLineSlice(TB, end_row, NULL);
      memcpy(p, end_chars, (size_t)end_col);
      p += end_col;
    }
  }
  *p = '\0';
  if (len_out)
    *len_out = len;
  return text;
}

static int guiDeleteRange(int start_row, int start_col, int end_row,
                          int end_col) {
  erow *row;
  int rowcount = tbLineCount(TB);

  guiClampTextPosition(&start_row, &start_col);
  guiClampTextPosition(&end_row, &end_col);
  if (guiPositionCompare(start_row, start_col, end_row, end_col) >= 0)
    return 0;
  if (start_row >= rowcount)
    return 0;

  /* Record what is about to be deleted so undo can restore it. */
  {
    size_t deleted_len = 0;
    char *deleted = guiExtractRangeText(start_row, start_col, end_row, end_col,
                                        &deleted_len);
    if (deleted) {
      if (deleted_len > 0)
        undoRecordDelete(start_row, start_col, deleted, (int)deleted_len,
                         UNDO_KIND_BLOCK);
      free(deleted);
    }
  }

  if (start_row == end_row) {
    row = tbRowMut(TB, start_row);
    charsMigrate(row);
    memmove(row->chars + start_col, row->chars + end_col,
            row->size - end_col + 1);
    row->size -= end_col - start_col;
    tbStoreSize(&E, row->idx, row->size);
    editorUpdateRow(row);
    E.dirty++;
    undoUpdateDirty();
    return 1;
  }

  if (end_row >= rowcount && start_col == 0) {
    for (int delete_row = rowcount - 1; delete_row >= start_row; delete_row--)
      tbDeleteLine(TB, delete_row);
    guiRefreshSyntaxFromRow(start_row - 1);
    undoUpdateDirty();
    return 1;
  }

  row = tbRowMut(TB, start_row);
  charsMigrate(row);
  if (end_row < rowcount) {
    int end_size = tbLineLength(TB, end_row);
    int tail_len = end_size - end_col;

    row->chars = realloc(row->chars, (size_t)start_col + tail_len + 1);
    if (tail_len > 0) {
      const char *end_chars = tbLineSlice(TB, end_row, NULL);
      memcpy(row->chars + start_col, end_chars + end_col, (size_t)tail_len);
    }
    row->size = start_col + tail_len;
  } else {
    row->chars = realloc(row->chars, (size_t)start_col + 1);
    row->size = start_col;
  }
  row->chars[row->size] = '\0';
  tbStoreSize(&E, row->idx, row->size);
  editorUpdateRow(row);
  for (int delete_row = (end_row < rowcount ? end_row : rowcount - 1);
       delete_row > start_row; delete_row--)
    tbDeleteLine(TB, delete_row);
  E.dirty++;
  guiRefreshSyntaxFromRow(start_row);
  undoUpdateDirty();
  return 1;
}

static int guiDeleteSelection(void) {
  int start_row, start_col, end_row, end_col;

  if (!guiSelectedRange(&start_row, &start_col, &end_row, &end_col))
    return 0;
  if (!guiDeleteRange(start_row, start_col, end_row, end_col))
    return 0;

  guiClearSelection();
  guiClampTextPosition(&start_row, &start_col);
  guiMoveCursorToFilePosition(start_row, start_col);
  return 1;
}

static int guiCopySelectionToClipboard(void) {
  size_t len = 0;
  char *text = guiSelectedText(&len);

  if (!text) {
    editorSetStatusMessage("No selection to copy");
    return 0;
  }
  SetClipboardText(text);
  free(text);
  editorSetStatusMessage("Copied %zu byte%s", len, len == 1 ? "" : "s");
  return 1;
}

static void guiCutSelectionToClipboard(void) {
  if (guiCopySelectionToClipboard()) {
    undoBeginTxn();
    guiDeleteSelection();
    undoEndTxn();
    editorSetStatusMessage("Cut selection");
  }
}

static void guiSelectAll(void) {
  int rowcount = tbLineCount(TB);
  if (rowcount == 0) {
    guiClearSelection();
    return;
  }

  G.selection_active = 1;
  G.selection_anchor_row = 0;
  G.selection_anchor_col = 0;
  G.selection_cursor_row = rowcount;
  G.selection_cursor_col = 0;
  guiMoveCursorToFilePosition(rowcount, 0);
  editorSetStatusMessage("Selected all");
}

static int guiInsertPlainText(const char *text) {
  int inserted = 0;

  for (const unsigned char *p = (const unsigned char *)text; p && *p; p++) {
    if (*p == '\r') {
      editorInsertNewline();
      if (p[1] == '\n')
        p++;
      inserted++;
    } else if (*p == '\n') {
      editorInsertNewline();
      inserted++;
    } else if (*p == TAB || (*p >= 32 && *p < 127)) {
      editorInsertChar(*p);
      inserted++;
    }
  }
  return inserted;
}

static void guiPasteClipboardText(void) {
  const char *text = GetClipboardText();
  int inserted;

  if (!guiTextHasEditableChars(text)) {
    editorSetStatusMessage("Clipboard has no editable text");
    return;
  }

  undoBeginTxn();
  guiDeleteSelection();
  inserted = guiInsertPlainText(text);
  undoEndTxn();
  editorSetStatusMessage("Pasted %d character%s", inserted,
                         inserted == 1 ? "" : "s");
}

static int guiEnsureEditableFile(void) {
  if (T.active >= 0)
    return 1;
  guiNewFile();
  return T.active >= 0;
}

static void guiMarkDirectEdit(void) {
  E.dirty = 1;
  U.save_point = -1;
  U.last_kind = UNDO_KIND_NONE;
  G.find_dirty = 1;
  G.signature_dirty = 1;
  bentoAutoPinIfDirty();
}

static void guiLineOperationRows(int *start_row, int *end_row) {
  int rowcount = tbLineCount(TB);

  if (rowcount <= 0) {
    *start_row = 0;
    *end_row = -1;
    return;
  }

  if (guiHasSelection()) {
    int start_col, end_col;
    guiSelectionBounds(start_row, &start_col, end_row, &end_col);
    guiClampTextPosition(start_row, &start_col);
    guiClampTextPosition(end_row, &end_col);
    if (*end_row >= rowcount)
      *end_row = rowcount - 1;
    else if (end_col == 0 && *end_row > *start_row)
      (*end_row)--;
  } else {
    *start_row = guiCursorFileRow();
    *end_row = *start_row;
  }

  if (*start_row < 0)
    *start_row = 0;
  if (*end_row >= rowcount)
    *end_row = rowcount - 1;
}

static void guiDuplicateLineOrSelection(void) {
  if (!guiEnsureEditableFile())
    return;

  if (guiHasSelection()) {
    int start_row, start_col, end_row, end_col;
    size_t len = 0;
    char *text = guiSelectedText(&len);

    if (!text)
      return;
    guiSelectionBounds(&start_row, &start_col, &end_row, &end_col);
    undoBeginTxn();
    guiMoveCursorToFilePosition(end_row, end_col);
    guiInsertPlainText(text);
    undoEndTxn();
    free(text);
    editorSetStatusMessage("Duplicated selection");
    return;
  }

  {
    int row = guiCursorFileRow();
    int col = guiCursorFileCol();
    int rowcount = tbLineCount(TB);
    int len = 0;
    const char *chars;
    char *copy;

    if (rowcount <= 0 || row >= rowcount) {
      editorInsertNewline();
      editorSetStatusMessage("Duplicated line");
      return;
    }

    chars = tbLineSlice(TB, row, &len);
    copy = malloc((size_t)len + 1);
    if (!copy)
      return;
    memcpy(copy, chars, (size_t)len);
    copy[len] = '\0';

    undoBeginTxn();
    guiMoveCursorToFilePosition(row, len);
    editorInsertNewline();
    guiInsertPlainText(copy);
    undoEndTxn();
    guiMoveCursorToFilePosition(row + 1, guiMinInt(col, len));
    free(copy);
    editorSetStatusMessage("Duplicated line");
  }
}

static void guiMoveCurrentLine(int direction) {
  int row = guiCursorFileRow();
  int other = row + direction;
  int rowcount = tbLineCount(TB);
  erow tmp;

  if (row < 0 || row >= rowcount || other < 0 || other >= rowcount)
    return;

  tmp = E.row[row];
  E.row[row] = E.row[other];
  E.row[other] = tmp;
  E.row[row].idx = row;
  E.row[other].idx = other;
  if (E.line_sizes && other < E.line_sizes_cap && row < E.line_sizes_cap) {
    int s = E.line_sizes[row];
    E.line_sizes[row] = E.line_sizes[other];
    E.line_sizes[other] = s;
  }
  dirtyMarkFromHere(guiMinInt(row, other), rowcount);
  g_render_gen++;
  guiMarkDirectEdit();
  guiMoveCursorToFilePosition(other, guiCursorFileCol());
  editorSetStatusMessage(direction < 0 ? "Moved line up" : "Moved line down");
}

static const char *guiCommentMarker(void) {
  if (E.syntax && E.syntax->singleline_comment_start[0])
    return E.syntax->singleline_comment_start;
  return "#";
}

static int guiLineCommentIndent(erow *row) {
  int indent = 0;
  while (indent < row->size &&
         (row->chars[indent] == ' ' || row->chars[indent] == TAB))
    indent++;
  return indent;
}

static void guiToggleLineComment(void) {
  int start_row, end_row;
  const char *marker = guiCommentMarker();
  int marker_len = (int)strlen(marker);
  int should_uncomment = 1;
  int nonblank = 0;
  char prefix[8];
  int prefix_len;

  if (!guiEnsureEditableFile() || marker_len <= 0)
    return;

  guiLineOperationRows(&start_row, &end_row);
  if (end_row < start_row)
    return;

  for (int row_index = start_row; row_index <= end_row; row_index++) {
    erow *row = tbRowMut(TB, row_index);
    int indent = guiLineCommentIndent(row);
    if (!row || indent >= row->size)
      continue;
    nonblank = 1;
    if (indent + marker_len > row->size ||
        memcmp(row->chars + indent, marker, (size_t)marker_len) != 0) {
      should_uncomment = 0;
      break;
    }
  }
  if (!nonblank)
    should_uncomment = 0;

  snprintf(prefix, sizeof(prefix), "%s ", marker);
  prefix_len = (int)strlen(prefix);
  undoBeginTxn();
  for (int row_index = start_row; row_index <= end_row; row_index++) {
    erow *row = tbRowMut(TB, row_index);
    int indent;
    if (!row)
      continue;
    indent = guiLineCommentIndent(row);
    if (should_uncomment) {
      int remove_len = marker_len;
      if (indent >= row->size ||
          memcmp(row->chars + indent, marker, (size_t)marker_len) != 0)
        continue;
      if (indent + remove_len < row->size &&
          row->chars[indent + remove_len] == ' ')
        remove_len++;
      editorRowReplace(row, indent, remove_len, "", 0);
    } else if (indent < row->size) {
      editorRowReplace(row, indent, 0, prefix, prefix_len);
    }
  }
  undoEndTxn();
  editorSetStatusMessage(should_uncomment ? "Uncommented line%s"
                                          : "Commented line%s",
                         start_row == end_row ? "" : "s");
}

static void guiIndentLines(int direction) {
  int start_row, end_row;
  char spaces[16];
  int count = guiMaxInt(1, guiMinInt(G.tab_width, (int)sizeof(spaces)));

  if (!guiEnsureEditableFile())
    return;

  guiLineOperationRows(&start_row, &end_row);
  if (end_row < start_row)
    return;

  memset(spaces, ' ', (size_t)count);
  undoBeginTxn();
  for (int row_index = start_row; row_index <= end_row; row_index++) {
    erow *row = tbRowMut(TB, row_index);
    if (!row)
      continue;
    if (direction > 0) {
      editorRowReplace(row, 0, 0, spaces, count);
    } else {
      int remove_len = 0;
      if (row->size > 0 && row->chars[0] == TAB) {
        remove_len = 1;
      } else {
        while (remove_len < count && remove_len < row->size &&
               row->chars[remove_len] == ' ')
          remove_len++;
      }
      if (remove_len > 0)
        editorRowReplace(row, 0, remove_len, "", 0);
    }
  }
  undoEndTxn();
  editorSetStatusMessage(direction > 0 ? "Indented line%s" : "Outdented line%s",
                         start_row == end_row ? "" : "s");
}

static void guiTrimTrailingWhitespace(void) {
  int changed = 0;
  int rowcount = tbLineCount(TB);

  if (!guiEnsureEditableFile())
    return;

  undoBeginTxn();
  for (int row_index = 0; row_index < rowcount; row_index++) {
    erow *row = tbRowMut(TB, row_index);
    int end;
    if (!row)
      continue;
    end = row->size;
    while (end > 0 &&
           (row->chars[end - 1] == ' ' || row->chars[end - 1] == TAB))
      end--;
    if (end < row->size) {
      editorRowReplace(row, end, row->size - end, "", 0);
      changed++;
    }
  }
  undoEndTxn();
  editorSetStatusMessage("Trimmed trailing whitespace on %d line%s", changed,
                         changed == 1 ? "" : "s");
}

static void guiSelectMatchAtCursor(void) {
  int row = guiCursorFileRow();
  int col = guiCursorFileCol();

  G.selection_active = 1;
  G.selection_anchor_row = row;
  G.selection_anchor_col = col;
  G.selection_cursor_row = row;
  G.selection_cursor_col = col + G.query_len;
  guiMoveCursorToFilePosition(row, col + G.query_len);
}

static void guiSelectNextOccurrence(void) {
  int row = guiCursorFileRow();
  int col = guiCursorFileCol();
  erow *erowp;

  if (guiHasSelection()) {
    size_t len = 0;
    char *text = guiSelectedText(&len);
    if (text && len > 0 && len <= bento_QUERY_LEN && !strchr(text, '\n')) {
      memcpy(G.query, text, len);
      G.query[len] = '\0';
      G.query_len = (int)len;
    }
    free(text);
  } else if ((erowp = tbRowMut(TB, row)) != NULL && erowp->size > 0) {
    int start = col;
    int end = col;
    if (start >= erowp->size)
      start = erowp->size - 1;
    if (!editorIsIdentChar((unsigned char)erowp->chars[start])) {
      editorSetStatusMessage("No word at cursor");
      return;
    }
    while (start > 0 &&
           editorIsIdentChar((unsigned char)erowp->chars[start - 1]))
      start--;
    end = start;
    while (end < erowp->size &&
           editorIsIdentChar((unsigned char)erowp->chars[end]))
      end++;
    if (end > start && end - start <= bento_QUERY_LEN) {
      memcpy(G.query, erowp->chars + start, (size_t)(end - start));
      G.query[end - start] = '\0';
      G.query_len = end - start;
      guiMoveCursorToFilePosition(row, start);
    }
  }

  if (G.query_len <= 0)
    return;
  G.find_active = 1;
  G.find_focus = 0;
  G.find_editor_focus = 1;
  G.find_dirty = 1;
  guiRefreshFindResults();
  if (guiFindMatch(1, 0))
    guiSelectMatchAtCursor();
}

static int guiFilePositionFromMouse(Vector2 mouse, int *out_row, int *out_col) {
  int sidebar = guiSidebarWidth();
  int gutter = guiGutterWidth();
  int text_left = sidebar + gutter + GUI_TEXT_PADDING_X;
  int text_top = guiEditorTop() + GUI_TEXT_PADDING_Y;
  int row = (int)((mouse.y - text_top + G.editor_scroll_y) / G.line_height);
  int col = (int)((mouse.x - text_left) / G.char_width);
  int filerow, filecol, rowlen;

  if (mouse.x < text_left)
    col = 0;
  if (row < 0)
    row = 0;
  if (row >= E.screenrows)
    row = E.screenrows - 1;
  if (col < 0)
    col = 0;
  if (col >= E.screencols)
    col = E.screencols - 1;

  filerow = E.rowoff + row;
  int rowcount = tbLineCount(TB);
  if (filerow > rowcount)
    filerow = rowcount;
  rowlen = tbLineLength(TB, filerow);
  filecol = E.coloff + col;
  if (filecol > rowlen)
    filecol = rowlen;
  *out_row = filerow;
  *out_col = filecol;
  return 1;
}

static void guiMoveCursorFromMouse(Vector2 mouse, int extend_selection) {
  int filerow, filecol;

  if (!guiFilePositionFromMouse(mouse, &filerow, &filecol))
    return;
  guiMoveCursorToPositionWithSelection(filerow, filecol, extend_selection);
}

static void guiRequestClose(void) {
  if (E.dirty && G.quit_times > 0) {
    editorSetStatusMessage("Unsaved changes. Press close or quit %d more time%s.",
                           G.quit_times, G.quit_times == 1 ? "" : "s");
    G.quit_times--;
    return;
  }
  G.should_close = 1;
}

static int guiSaveActiveFile(void) {
  if (T.active < 0 || !E.filename) {
    editorSetStatusMessage("No file to save");
    return 1;
  }
  time_t disk_mtime = editorPathMtime(E.filename);
  if (E.filename && E.dirty && E.file_mtime > 0 && disk_mtime > 0 &&
      disk_mtime != E.file_mtime && !G.external_save_confirm) {
    G.external_save_confirm = 1;
    editorSetStatusMessage(
        "File changed on disk. Save again to overwrite, or reopen to reload.");
    return 1;
  }
  G.external_save_confirm = 0;
  if (editorSave() == 0) {
    G.signature_dirty = 1;
    guiAddRecentFile(E.filename);
    return 0;
  }
  return 1;
}

static void guiStoreFindMatch(int file_index, int row, int col,
                              const char *line, int line_len) {
  if (G.match_count < GUI_MAX_FIND_RESULTS) {
    guiMatch *match = &G.matches[G.match_count];

    match->file_index = file_index;
    match->row = row;
    match->col = col;
    if (line_len < 0)
      line_len = 0;
    if (line_len >= (int)sizeof(match->preview))
      line_len = (int)sizeof(match->preview) - 1;
    memcpy(match->preview, line, line_len);
    match->preview[line_len] = '\0';
  } else {
    G.match_overflow = 1;
  }
  G.match_count++;
}

static void guiCollectBufferFindResults(int file_index) {
  int rowcount = tbLineCount(TB);
  for (int row_index = 0; row_index < rowcount; row_index++) {
    int row_size = 0;
    const char *row_chars = tbLineSlice(TB, row_index, &row_size);
    const char *scan = row_chars;

    while ((scan = guiFindQueryInRange(row_chars, row_size,
                                       (int)(scan - row_chars))) != NULL) {
      guiStoreFindMatch(file_index, row_index, (int)(scan - row_chars),
                        row_chars, row_size);
      scan += guiMaxInt(1, G.query_len);
    }
  }
}

static void guiCollectTextFindResults(int file_index, const char *text) {
  const char *line = text;
  int row = 0;

  while (*line) {
    const char *line_end = strchr(line, '\n');
    const char *scan = line;
    int line_len;

    if (!line_end)
      line_end = line + strlen(line);
    line_len = (int)(line_end - line);

    while (scan < line_end) {
      const char *match = guiFindQueryInRange(line, line_len,
                                              (int)(scan - line));
      if (!match || match >= line_end)
        break;
      guiStoreFindMatch(file_index, row, (int)(match - line), line, line_len);
      scan = match + guiMaxInt(1, G.query_len);
    }

    row++;
    line = *line_end == '\n' ? line_end + 1 : line_end;
  }
}

static void guiRefreshFindResults(void) {
  G.match_count = 0;
  G.match_overflow = 0;
  G.find_dirty = 0;

  if (G.query_len == 0)
    return;

  if (!G.find_project_active) {
    guiCollectBufferFindResults(guiFileIndexForPath(E.filename));
  } else {
    for (int i = 0; i < G.file_count; i++) {
      char *buf = NULL;
      size_t len = 0;

      if (G.files[i].is_dir)
        continue;
      if (E.filename && !strcmp(G.files[i].path, E.filename)) {
        guiCollectBufferFindResults(i);
        continue;
      }
      if (guiReadTextFile(G.files[i].path, &buf, &len)) {
        (void)len;
        guiCollectTextFindResults(i, buf);
        free(buf);
      }
    }
  }

  if (G.match_count == 0)
    G.current_match = -1;
  else if (G.current_match >= G.match_count)
    G.current_match = G.match_count - 1;
}

static void guiGoToMatchIndex(int index) {
  int stored_count = guiMinInt(G.match_count, GUI_MAX_FIND_RESULTS);
  guiMatch *match;

  if (stored_count == 0)
    return;
  if (index < 0)
    index = stored_count - 1;
  if (index >= stored_count)
    index = 0;

  match = &G.matches[index];
  if (G.find_project_active && match->file_index >= 0 &&
      match->file_index < G.file_count &&
      (!E.filename || strcmp(G.files[match->file_index].path, E.filename) != 0)) {
    if (!guiOpenFile(G.files[match->file_index].path))
      return;
  }

  G.current_match = index;
  guiMoveCursorToFilePosition(match->row, match->col);

  if (G.current_match < G.match_scroll)
    G.match_scroll = G.current_match;
  if (G.current_match >= G.match_scroll + 5)
    G.match_scroll = G.current_match - 4;
}

static int guiFindMatch(int direction, int include_current) {
  int current_row = E.rowoff + E.cy;
  int current_col = E.coloff + E.cx;
  int stored_count;
  int candidate = -1;

  if (G.find_dirty)
    guiRefreshFindResults();
  stored_count = guiMinInt(G.match_count, GUI_MAX_FIND_RESULTS);
  if (stored_count == 0) {
    if (G.query_len > 0)
      editorSetStatusMessage("No matches for \"%s\"", G.query);
    return 0;
  }

  if (G.find_project_active) {
    if (G.current_match < 0 || G.current_match >= stored_count)
      candidate = direction >= 0 ? 0 : stored_count - 1;
    else
      candidate = G.current_match + (direction >= 0 ? 1 : -1);
    if (candidate < 0)
      candidate = stored_count - 1;
    if (candidate >= stored_count)
      candidate = 0;
    guiGoToMatchIndex(candidate);
    editorSetStatusMessage("%d project match%s%s", G.match_count,
                           G.match_count == 1 ? "" : "es",
                           G.match_overflow ? " (showing first 2048)" : "");
    return 1;
  }

  if (direction >= 0) {
    for (int i = 0; i < stored_count; i++) {
      int after_row = G.matches[i].row > current_row;
      int same_row = G.matches[i].row == current_row;
      int after_col = include_current ? G.matches[i].col >= current_col
                                      : G.matches[i].col > current_col;
      if (after_row || (same_row && after_col)) {
        candidate = i;
        break;
      }
    }
    if (candidate == -1)
      candidate = 0;
  } else {
    for (int i = stored_count - 1; i >= 0; i--) {
      int before_row = G.matches[i].row < current_row;
      int same_row = G.matches[i].row == current_row;
      int before_col = include_current ? G.matches[i].col <= current_col
                                       : G.matches[i].col < current_col;
      if (before_row || (same_row && before_col)) {
        candidate = i;
        break;
      }
    }
    if (candidate == -1)
      candidate = stored_count - 1;
  }

  guiGoToMatchIndex(candidate);
  editorSetStatusMessage("%d match%s%s", G.match_count,
                         G.match_count == 1 ? "" : "es",
                         G.match_overflow ? " (showing first 2048)" : "");
  return 1;
}

static void guiReplaceCurrent(void) {
  int filerow = E.rowoff + E.cy;
  int filecol = E.coloff + E.cx;
  erow *row;

  if (G.query_len == 0 || filerow >= tbLineCount(TB))
    return;

  if (G.find_project_active) {
    if (G.current_match < 0 || G.current_match >=
                                guiMinInt(G.match_count, GUI_MAX_FIND_RESULTS)) {
      if (!guiFindMatch(1, 1))
        return;
    } else {
      guiGoToMatchIndex(G.current_match);
    }
    filerow = E.rowoff + E.cy;
    filecol = E.coloff + E.cx;
  }

  row = tbRowMut(TB, filerow);
  if (filecol > row->size ||
      !guiQueryMatchesAt(row->chars, row->size, filecol)) {
    if (!guiFindMatch(1, 1))
      return;
    filerow = E.rowoff + E.cy;
    filecol = E.coloff + E.cx;
    row = tbRowMut(TB, filerow);
  }

  undoBeginTxn();
  editorRowReplace(row, filecol, G.query_len, G.replacement,
                   G.replacement_len);
  undoEndTxn();
  guiMoveCursorToFilePosition(filerow, filecol + G.replacement_len);
  G.find_dirty = 1;
  guiRefreshFindResults();
  editorSetStatusMessage("Replaced current match%s",
                         G.find_project_active ? " in project" : "");
}

static void guiReplaceAll(void) {
  int replaced = 0;
  int changed_files = 0;

  if (G.query_len == 0)
    return;

  undoBeginTxn();
  if (G.find_project_active) {
    for (int i = 0; i < G.file_count; i++) {
      if (G.files[i].is_dir)
        continue;

      if (E.filename && !strcmp(G.files[i].path, E.filename)) {
        int file_replaced = 0;
        int rc = tbLineCount(TB);
        for (int row_index = 0; row_index < rc; row_index++) {
          erow *row = tbRowMut(TB, row_index);
          int at = 0;

          while (at <= row->size) {
            const char *match = guiFindQueryInRange(row->chars, row->size, at);
            int col;

            if (!match)
              break;
            col = (int)(match - row->chars);
            editorRowReplace(row, col, G.query_len, G.replacement,
                             G.replacement_len);
            at = col + G.replacement_len;
            replaced++;
            file_replaced++;
          }
        }
        if (file_replaced)
          changed_files++;
      } else {
        char *buf = NULL;
        size_t len = 0;
        int file_replaced;

        if (!guiReadTextFile(G.files[i].path, &buf, &len))
          continue;
        file_replaced = guiReplaceInTextBuffer(&buf, &len);
        if (file_replaced > 0 && guiWriteTextFile(G.files[i].path, buf, len)) {
          replaced += file_replaced;
          changed_files++;
        }
        free(buf);
      }
    }

    undoEndTxn();
    guiRefreshFindResults();
    editorSetStatusMessage("Replaced %d match%s in %d file%s", replaced,
                           replaced == 1 ? "" : "es", changed_files,
                           changed_files == 1 ? "" : "s");
    return;
  }

  {
    int rc = tbLineCount(TB);
    for (int row_index = 0; row_index < rc; row_index++) {
      erow *row = tbRowMut(TB, row_index);
      int at = 0;

      while (at <= row->size) {
        const char *match = guiFindQueryInRange(row->chars, row->size, at);
        int col;

        if (!match)
          break;
        col = (int)(match - row->chars);
        editorRowReplace(row, col, G.query_len, G.replacement,
                         G.replacement_len);
        at = col + G.replacement_len;
        replaced++;
      }
    }
  }
  undoEndTxn();

  G.find_dirty = 1;
  guiRefreshFindResults();
  editorSetStatusMessage("Replaced %d match%s", replaced,
                         replaced == 1 ? "" : "es");
}

static void guiStartFind(void) {
  G.find_active = 1;
  G.find_focus = 0;
  G.find_editor_focus = 0;
  G.repeat_key = 0;
  G.find_dirty = 1;
  guiRefreshFindResults();
  if (G.query_len > 0)
    guiFindMatch(1, 1);
}

static void guiCloseFind(void) {
  G.find_active = 0;
  G.find_all_active = 0;
  G.find_editor_focus = 0;
  G.find_focus = 0;
  editorSetStatusMessage("");
}

static void guiFocusedFindFieldChanged(void) {
  if (G.find_focus == 0) {
    G.find_dirty = 1;
    if (G.find_project_active)
      guiRefreshFindResults();
    else
      guiFindMatch(1, 1);
  }
}

static void guiAppendToFocusedFindField(int c) {
  char *buf = G.find_focus == 0 ? G.query : G.replacement;
  int *len = G.find_focus == 0 ? &G.query_len : &G.replacement_len;

  if (c >= 32 && c < 127 && *len < bento_QUERY_LEN) {
    buf[(*len)++] = (char)c;
    buf[*len] = '\0';
    guiFocusedFindFieldChanged();
  }
}

static void guiBackspaceFocusedFindField(void) {
  char *buf = G.find_focus == 0 ? G.query : G.replacement;
  int *len = G.find_focus == 0 ? &G.query_len : &G.replacement_len;

  if (*len > 0) {
    buf[--(*len)] = '\0';
    guiFocusedFindFieldChanged();
  }
}

static void guiPasteIntoFocusedFindField(void) {
  char *buf = G.find_focus == 0 ? G.query : G.replacement;
  int *len = G.find_focus == 0 ? &G.query_len : &G.replacement_len;

  if (guiAppendAsciiToBuffer(GetClipboardText(), buf, len, bento_QUERY_LEN, 0))
    guiFocusedFindFieldChanged();
  else
    editorSetStatusMessage("Clipboard has no editable text");
}

static int guiHandleFindMouseClick(Vector2 mouse, int allow_editor_focus) {
  int sidebar = guiSidebarWidth();
  int y = GUI_TAB_BAR_HEIGHT;
  int results_y = GUI_TAB_BAR_HEIGHT + GUI_FIND_BAR_HEIGHT;
  int editor_top = guiEditorTop();
  int editor_bottom = GetScreenHeight() - GUI_STATUS_BAR_HEIGHT;
  int query_x = sidebar + 58;
  int query_w = guiMaxInt(140, (GetScreenWidth() - sidebar - 390) / 2);
  int replace_x = query_x + query_w + 78;
  int replace_w = guiMaxInt(120, GetScreenWidth() - replace_x - 448);
  int button_x = GetScreenWidth() - 430;

  if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    return 0;

  if (mouse.y >= y + 8 && mouse.y <= y + 31) {
    if (mouse.x >= query_x && mouse.x <= query_x + query_w) {
      G.find_focus = 0;
      G.find_editor_focus = 0;
      return 1;
    } else if (mouse.x >= button_x && mouse.x <= button_x + 34) {
      guiFindMatch(-1, 0);
      return 1;
    } else if (mouse.x >= button_x + 40 && mouse.x <= button_x + 74) {
      guiFindMatch(1, 0);
      return 1;
    } else if (mouse.x >= button_x + 82 && mouse.x <= button_x + 138) {
      G.find_all_active = !G.find_all_active;
      return 1;
    } else if (mouse.x >= button_x + 144 && mouse.x <= button_x + 202) {
      G.find_project_active = !G.find_project_active;
      G.find_all_active = 1;
      G.current_match = -1;
      G.match_scroll = 0;
      G.find_dirty = 1;
      guiRefreshFindResults();
      return 1;
    } else if (mouse.x >= button_x + 208 && mouse.x <= button_x + 256) {
      G.find_case_sensitive = !G.find_case_sensitive;
      G.current_match = -1;
      G.match_scroll = 0;
      G.find_dirty = 1;
      guiRefreshFindResults();
      return 1;
    } else if (mouse.x >= button_x + 262 && mouse.x <= button_x + 316) {
      G.find_whole_word = !G.find_whole_word;
      G.current_match = -1;
      G.match_scroll = 0;
      G.find_dirty = 1;
      guiRefreshFindResults();
      return 1;
    }
  } else if (mouse.y >= y + 40 && mouse.y <= y + 63) {
    if (mouse.x >= replace_x && mouse.x <= replace_x + replace_w) {
      G.find_focus = 1;
      G.find_editor_focus = 0;
      return 1;
    } else if (mouse.x >= button_x && mouse.x <= button_x + 78) {
      guiReplaceCurrent();
      return 1;
    } else if (mouse.x >= button_x + 86 && mouse.x <= button_x + 158) {
      guiReplaceAll();
      return 1;
    }
  } else if (G.find_all_active && mouse.y >= results_y &&
             mouse.y < results_y + GUI_FIND_RESULTS_HEIGHT) {
    int row = (int)((mouse.y - results_y - 8) / GUI_TREE_ROW_HEIGHT);
    int index = G.match_scroll + row;
    if (row >= 0 && index < guiMinInt(G.match_count, GUI_MAX_FIND_RESULTS)) {
      guiGoToMatchIndex(index);
      return 1;
    }
  } else if (allow_editor_focus && mouse.x >= sidebar && mouse.y >= editor_top &&
             mouse.y < editor_bottom) {
    int filerow, filecol;

    if (!guiFilePositionFromMouse(mouse, &filerow, &filecol))
      return 1;
    guiMoveCursorToPositionWithSelection(filerow, filecol, guiShiftDown());
    G.last_editor_click_row = filerow;
    G.last_editor_click_col = filecol;
    G.last_editor_click_time = GetTime();
    G.mouse_selecting = 1;
    G.find_editor_focus = 1;
    editorSetStatusMessage("");
    return 1;
  }

  return 0;
}

static void guiProcessFindInput(void) {
  float wheel = GetMouseWheelMove();
  Vector2 mouse = GetMousePosition();
  int sidebar = guiSidebarWidth();
  int results_y = GUI_TAB_BAR_HEIGHT + GUI_FIND_BAR_HEIGHT;
  int stored_count;

  if (wheel != 0.0f && G.find_all_active && mouse.y >= results_y &&
      mouse.y < results_y + GUI_FIND_RESULTS_HEIGHT) {
    guiScrollMatchesByLines(guiWheelToLines(wheel, &G.wheel_match_accum,
                                            3.0f));
  } else if (wheel != 0.0f) {
    if (mouse.x < sidebar && guiSidebarWidth() > 0) {
      guiScrollTreeByLines(guiWheelToLines(wheel, &G.wheel_tree_accum, 3.0f));
    } else if (guiAltDown()) {
      guiScrollColumns(guiWheelToLines(wheel, &G.wheel_col_accum, 6.0f));
    } else if (guiShiftDown()) {
      guiMoveCursorByLinesWithSelection(
          guiWheelToLines(wheel, &G.wheel_row_accum, 3.0f));
    } else {
      guiScrollRowsByWheel(wheel);
    }
  }

  if (guiHandleFindMouseClick(mouse, 1))
    return;

  if (IsKeyPressed(KEY_ESCAPE)) {
    guiCloseFind();
    return;
  }
  if (IsKeyPressed(KEY_TAB)) {
    G.find_focus = !G.find_focus;
    return;
  }
  if (guiModifierDown() && IsKeyPressed(KEY_R)) {
    if (guiShiftDown())
      guiReplaceAll();
    else
      guiReplaceCurrent();
    return;
  }
  if (guiModifierDown() && IsKeyPressed(KEY_ENTER)) {
    G.find_all_active = !G.find_all_active;
    return;
  }
  if (guiModifierDown() && guiShiftDown() && IsKeyPressed(KEY_F)) {
    G.find_project_active = !G.find_project_active;
    G.find_all_active = 1;
    G.current_match = -1;
    G.match_scroll = 0;
    G.find_dirty = 1;
    guiRefreshFindResults();
    return;
  }
  if (guiAltDown() && IsKeyPressed(KEY_C)) {
    G.find_case_sensitive = !G.find_case_sensitive;
    G.current_match = -1;
    G.match_scroll = 0;
    G.find_dirty = 1;
    guiRefreshFindResults();
    return;
  }
  if (guiAltDown() && IsKeyPressed(KEY_W)) {
    G.find_whole_word = !G.find_whole_word;
    G.current_match = -1;
    G.match_scroll = 0;
    G.find_dirty = 1;
    guiRefreshFindResults();
    return;
  }
  if (guiModifierDown() && IsKeyPressed(KEY_V)) {
    guiPasteIntoFocusedFindField();
    return;
  }
  if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
    if (G.find_focus == 1)
      guiReplaceCurrent();
    else
      guiFindMatch(guiShiftDown() ? -1 : 1, 0);
    return;
  }
  if (IsKeyPressed(KEY_F3)) {
    guiFindMatch(guiShiftDown() ? -1 : 1, 0);
    return;
  }
  if (guiKeyPressedRepeat(KEY_BACKSPACE))
    guiBackspaceFocusedFindField();

  for (int c = GetCharPressed(); c > 0; c = GetCharPressed()) {
    if (!guiModifierDown())
      guiAppendToFocusedFindField(c);
  }

  if (G.find_dirty)
    guiRefreshFindResults();
  stored_count = guiMinInt(G.match_count, GUI_MAX_FIND_RESULTS);
  if (G.match_scroll > stored_count - 1)
    G.match_scroll = guiMaxInt(0, stored_count - 1);
}

static int guiTreeEntryAtMouse(Vector2 mouse) {
  int sidebar = guiSidebarWidth();
  int list_y = GUI_TAB_BAR_HEIGHT + 8;
  int row;

  if (sidebar <= 0 || mouse.x < 0 || mouse.x >= sidebar || mouse.y < list_y)
    return -1;
  row = (int)((mouse.y - list_y) / GUI_TREE_ROW_HEIGHT) + G.tree_scroll;
  if (row < 0 || row >= G.file_count)
    return -1;
  return row;
}

static int guiFilteredFileIndex(int filtered_index) {
  int seen = 0;

  for (int i = 0; i < G.file_count; i++) {
    if (G.files[i].is_dir)
      continue;
    if (!guiContainsCaseInsensitive(G.files[i].path, G.overlay_query) &&
        !guiContainsCaseInsensitive(G.files[i].name, G.overlay_query))
      continue;
    if (seen == filtered_index)
      return i;
    seen++;
  }
  return -1;
}

static int guiFilteredFileCount(void) {
  int count = 0;

  for (int i = 0; i < G.file_count; i++) {
    if (!G.files[i].is_dir &&
        (guiContainsCaseInsensitive(G.files[i].path, G.overlay_query) ||
         guiContainsCaseInsensitive(G.files[i].name, G.overlay_query)))
      count++;
  }
  return count;
}

static void guiOpenOverlay(int overlay) {
  G.overlay = overlay;
  G.overlay_query[0] = '\0';
  G.overlay_query_len = 0;
  G.overlay_selected = 0;
  G.overlay_scroll = 0;
  G.repeat_key = 0;
}

static void guiCloseOverlay(void) {
  G.overlay = GUI_OVERLAY_NONE;
  G.overlay_query[0] = '\0';
  G.overlay_query_len = 0;
  G.overlay_selected = 0;
  G.overlay_scroll = 0;
}

static void guiOpenQuickSelectedFile(void) {
  int file_index = guiFilteredFileIndex(G.overlay_selected);

  if (file_index >= 0 && guiOpenFile(G.files[file_index].path))
    guiCloseOverlay();
}

static void guiGoToRequestedLine(void) {
  int line = atoi(G.overlay_query);

  int rowcount = tbLineCount(TB);
  if (line < 1)
    line = 1;
  if (line > rowcount)
    line = rowcount ? rowcount : 1;
  guiMoveCursorToFilePosition(line - 1, 0);
  editorSetStatusMessage("Jumped to line %d", line);
  guiCloseOverlay();
}

static int guiFilteredCommandIndex(int filtered_index) {
  int seen = 0;

  for (int i = 0; GUI_COMMAND_DEFS[i].name; i++) {
    const guiCommandDef *def = &GUI_COMMAND_DEFS[i];
    if (!guiContainsCaseInsensitive(def->name, G.overlay_query) &&
        !guiContainsCaseInsensitive(def->shortcut, G.overlay_query))
      continue;
    if (seen == filtered_index)
      return i;
    seen++;
  }
  return -1;
}

static int guiFilteredCommandCount(void) {
  int count = 0;

  for (int i = 0; GUI_COMMAND_DEFS[i].name; i++) {
    const guiCommandDef *def = &GUI_COMMAND_DEFS[i];
    if (guiContainsCaseInsensitive(def->name, G.overlay_query) ||
        guiContainsCaseInsensitive(def->shortcut, G.overlay_query))
      count++;
  }
  return count;
}

static int guiFilteredRecent(int filtered_index, char *out, size_t out_len,
                             int *is_project) {
  int seen = 0;

  for (int pass = 0; pass < 2; pass++) {
    int count = pass == 0 ? G.recent_project_count : G.recent_file_count;
    for (int i = 0; i < count; i++) {
      const char *path = pass == 0 ? G.recent_projects[i] : G.recent_files[i];
      if (!guiContainsCaseInsensitive(path, G.overlay_query) &&
          !guiContainsCaseInsensitive(guiBaseName(path), G.overlay_query))
        continue;
      if (seen == filtered_index) {
        snprintf(out, out_len, "%s", path);
        *is_project = pass == 0;
        return 1;
      }
      seen++;
    }
  }
  return 0;
}

static int guiFilteredRecentCount(void) {
  int count = 0;

  for (int pass = 0; pass < 2; pass++) {
    int n = pass == 0 ? G.recent_project_count : G.recent_file_count;
    for (int i = 0; i < n; i++) {
      const char *path = pass == 0 ? G.recent_projects[i] : G.recent_files[i];
      if (guiContainsCaseInsensitive(path, G.overlay_query) ||
          guiContainsCaseInsensitive(guiBaseName(path), G.overlay_query))
        count++;
    }
  }
  return count;
}

static void guiOpenSelectedRecent(void) {
  char path[PATH_MAX];
  int is_project = 0;

  if (!guiFilteredRecent(G.overlay_selected, path, sizeof(path), &is_project))
    return;
  if (is_project)
    guiOpenProject(path);
  else
    guiOpenFile(path);
  guiCloseOverlay();
}

static void guiStartPrompt(int kind, const char *title, const char *base_path,
                           const char *target_path, const char *initial) {
  guiOpenOverlay(GUI_OVERLAY_PROMPT);
  G.prompt_kind = kind;
  snprintf(G.prompt_title, sizeof(G.prompt_title), "%s", title ? title : "");
  snprintf(G.prompt_base_path, sizeof(G.prompt_base_path), "%s",
           base_path ? base_path : G.cwd);
  snprintf(G.prompt_target_path, sizeof(G.prompt_target_path), "%s",
           target_path ? target_path : "");
  snprintf(G.prompt_value, sizeof(G.prompt_value), "%s", initial ? initial : "");
  G.prompt_value_len = (int)strlen(G.prompt_value);
}

static int guiPathExists(const char *path) {
  return path && access(path, 0) == 0;
}

static void guiResolveEnteredPath(char *out, size_t out_len,
                                  const char *entered) {
  char clean[PATH_MAX];
  const char *start = entered ? entered : "";
  size_t len;

  while (*start && isspace((unsigned char)*start))
    start++;
  snprintf(clean, sizeof(clean), "%s", start);
  len = strlen(clean);
  while (len > 0 && isspace((unsigned char)clean[len - 1]))
    clean[--len] = '\0';
  if (len >= 2 && ((clean[0] == '"' && clean[len - 1] == '"') ||
                   (clean[0] == '\'' && clean[len - 1] == '\''))) {
    memmove(clean, clean + 1, len - 2);
    clean[len - 2] = '\0';
    len -= 2;
  }

  if (clean[0] == '~' && (clean[1] == '\0' || clean[1] == '/')) {
    const char *home = getenv("HOME");
    if (home && *home)
      snprintf(out, out_len, "%s%s", home, clean + 1);
    else
      snprintf(out, out_len, "%s", clean);
  } else if (guiPathIsAbsolute(clean)) {
    snprintf(out, out_len, "%s", clean);
  } else {
    char base[PATH_MAX];
    if (G.cwd[0])
      snprintf(base, sizeof(base), "%s", G.cwd);
    else if (!getcwd(base, sizeof(base)))
      snprintf(base, sizeof(base), ".");
    guiPathJoin(out, out_len, base, clean);
  }
}

static void guiPickerStartDir(char *out, size_t out_len) {
  const char *home = getenv("HOME");

  if (G.project_open && G.cwd[0]) {
    snprintf(out, out_len, "%s", G.cwd);
    return;
  }

  if (T.active >= 0 && E.filename && E.filename[0]) {
    guiParentDir(out, out_len, E.filename);
    if (guiPathIsDir(out))
      return;
  }

  if (!G.welcome_active && G.cwd[0] && guiPathIsDir(G.cwd)) {
    snprintf(out, out_len, "%s", G.cwd);
    return;
  }

  if (home && home[0] && guiPathIsDir(home)) {
    snprintf(out, out_len, "%s", home);
    return;
  }

  snprintf(out, out_len, "%s", G.cwd[0] ? G.cwd : ".");
}

static void guiOpenPathPicker(int choose_dir) {
#ifdef __APPLE__
  char start_dir[PATH_MAX];

  if (bentoMacPathPickerActive() || G.path_picker_active) {
    editorSetStatusMessage("File picker is already open");
    g_render_gen++;
    return;
  }

  guiPickerStartDir(start_dir, sizeof(start_dir));
  if (bentoMacBeginChoosePath(choose_dir, start_dir)) {
    G.path_picker_active = 1;
    editorSetStatusMessage(choose_dir ? "Opening folder picker..."
                                      : "Opening file picker...");
    g_render_gen++;
  } else {
    editorSetStatusMessage("Could not open macOS file picker");
    g_render_gen++;
  }
  return;
#else
  guiStartPrompt(GUI_PROMPT_OPEN_PATH,
                 choose_dir ? "Open Folder Path" : "Open File Path",
                 G.cwd, NULL, "");
#endif
}

static void guiPollPathPicker(void) {
#ifdef __APPLE__
  char path[PATH_MAX];
  int result;

  if (!G.path_picker_active)
    return;

  result = bentoMacPollChosenPath(path, sizeof(path));
  if (result == 0)
    return;

  G.path_picker_active = 0;
  G.last_activity_time = GetTime();
  g_render_gen++;

  if (result > 0)
    guiOpenPath(path);
  else
    editorSetStatusMessage("Open canceled");
#endif
}

static int guiCreateEmptyFile(const char *path) {
  FILE *fp;

  if (!path || !*path || guiPathExists(path))
    return 0;
  fp = fopen(path, "ab");
  if (!fp)
    return 0;
  fclose(fp);
  return 1;
}

static int guiCreateFolderPath(const char *path) {
  if (!path || !*path || guiPathExists(path))
    return 0;
#ifdef _WIN32
  return _mkdir(path) == 0;
#else
  return mkdir(path, 0755) == 0;
#endif
}

static void guiUpdateOpenTabPath(const char *old_path, const char *new_path) {
  int tab = bentoFindTabByPath(old_path);

  if (tab < 0)
    return;
  if (tab == T.active) {
    free(E.filename);
    E.filename = strdup(new_path);
    editorSelectSyntaxHighlight((char *)new_path);
    E.file_mtime = editorPathMtime(new_path);
  } else {
    T.list[tab].file_mtime = editorPathMtime(new_path);
  }
  snprintf(T.list[tab].filename, sizeof(T.list[tab].filename), "%s", new_path);
}

static void guiDeletePathAndCloseTab(const char *path) {
  int tab = bentoFindTabByPath(path);
  int is_dir = guiPathIsDir(path);

  if (tab >= 0 &&
      ((tab == T.active && E.dirty) || (tab != T.active && T.list[tab].dirty))) {
    editorSetStatusMessage("Save or close dirty tab before deleting");
    return;
  }

  if ((is_dir ? rmdir(path) : remove(path)) != 0) {
    editorSetStatusMessage("Delete failed: %s", strerror(errno));
    return;
  }
  if (tab >= 0)
    bentoCloseTab(tab);
  guiRebuildFileTree();
  editorSetStatusMessage("Deleted %s", guiBaseName(path));
}

static int guiAnyDirtyTabs(void) {
  for (int i = 0; i < BENTO_MAX_TABS; i++) {
    if (!T.list[i].in_use)
      continue;
    if (i == T.active ? E.dirty : T.list[i].dirty)
      return 1;
  }
  return 0;
}

static void guiCloseWorkspace(void) {
  if (guiAnyDirtyTabs()) {
    editorSetStatusMessage("Save or close dirty tabs before closing workspace");
    return;
  }

  for (int i = 0; i < BENTO_MAX_TABS; i++) {
    if (T.list[i].in_use)
      bentoCloseTab(i);
  }
  G.project_open = 0;
  G.file_count = 0;
  G.tree_scroll = 0;
  G.find_active = 0;
  G.find_all_active = 0;
  G.find_project_active = 0;
  G.match_count = 0;
  G.current_match = -1;
  G.overlay = GUI_OVERLAY_NONE;
  G.context_menu = GUI_CONTEXT_NONE;
  G.welcome_active = 1;
  guiClearSelection();
  editorSetStatusMessage("Fresh workspace");
}

static void guiSaveAll(void) {
  int original = T.active;
  int saved = 0;
  int failed = 0;

  for (int i = 0; i < BENTO_MAX_TABS; i++) {
    int dirty;
    if (!T.list[i].in_use)
      continue;
    dirty = (i == T.active) ? E.dirty : T.list[i].dirty;
    if (!dirty)
      continue;
    if (i != T.active)
      bentoSwitchToTab(i);
    if (guiSaveActiveFile() == 0)
      saved++;
    else
      failed++;
  }

  if (original >= 0 && T.list[original].in_use && T.active != original)
    bentoSwitchToTab(original);
  editorSetStatusMessage("Saved %d file%s%s", saved, saved == 1 ? "" : "s",
                         failed ? " (some failed)" : "");
}

static void guiLaunchFreshWindow(void) {
#ifdef _WIN32
  editorSetStatusMessage("New window is not supported on this build yet");
#else
  const char *exe = G.launch_path[0] ? G.launch_path : "./bento-gui";
  pid_t pid = fork();
  if (pid < 0) {
    editorSetStatusMessage("Could not start new window: %s", strerror(errno));
    return;
  }
  if (pid == 0) {
    execl(exe, exe, "--fresh-window", (char *)NULL);
    execlp(exe, exe, "--fresh-window", (char *)NULL);
    _exit(127);
  }
  editorSetStatusMessage("Opened new Bento window");
#endif
}

static void guiReopenClosedTab(void) {
  if (!G.last_closed_file[0]) {
    editorSetStatusMessage("No recently closed tab");
    return;
  }
  guiOpenFile(G.last_closed_file);
}

static void guiCompletePrompt(void) {
  char path[PATH_MAX];
  char parent[PATH_MAX];

  if (G.prompt_kind == GUI_PROMPT_NONE)
    return;
  if (G.prompt_kind != GUI_PROMPT_DELETE && G.prompt_value_len == 0) {
    editorSetStatusMessage("Name required");
    return;
  }

  if (G.prompt_kind == GUI_PROMPT_OPEN_PATH) {
    guiResolveEnteredPath(path, sizeof(path), G.prompt_value);
    if (!guiPathExists(path)) {
      editorSetStatusMessage("Path not found: %s", path);
      return;
    }
    guiCloseOverlay();
    guiOpenPath(path);
  } else if (G.prompt_kind == GUI_PROMPT_NEW_FILE) {
    guiPathJoin(path, sizeof(path), G.prompt_base_path, G.prompt_value);
    if (!guiCreateEmptyFile(path)) {
      editorSetStatusMessage("Could not create file");
      return;
    }
    guiRebuildFileTree();
    guiOpenFile(path);
    guiCloseOverlay();
    editorSetStatusMessage("Created %s", guiBaseName(path));
  } else if (G.prompt_kind == GUI_PROMPT_NEW_FOLDER) {
    guiPathJoin(path, sizeof(path), G.prompt_base_path, G.prompt_value);
    if (!guiCreateFolderPath(path)) {
      editorSetStatusMessage("Could not create folder");
      return;
    }
    guiRebuildFileTree();
    guiCloseOverlay();
    editorSetStatusMessage("Created folder %s", guiBaseName(path));
  } else if (G.prompt_kind == GUI_PROMPT_RENAME) {
    guiParentDir(parent, sizeof(parent), G.prompt_target_path);
    guiPathJoin(path, sizeof(path), parent, G.prompt_value);
    if (rename(G.prompt_target_path, path) != 0) {
      editorSetStatusMessage("Rename failed: %s", strerror(errno));
      return;
    }
    guiUpdateOpenTabPath(G.prompt_target_path, path);
    guiRebuildFileTree();
    guiCloseOverlay();
    editorSetStatusMessage("Renamed to %s", guiBaseName(path));
  } else if (G.prompt_kind == GUI_PROMPT_DELETE) {
    if (strcmp(G.prompt_value, "delete") != 0) {
      editorSetStatusMessage("Type delete to confirm");
      return;
    }
    snprintf(path, sizeof(path), "%s", G.prompt_target_path);
    guiCloseOverlay();
    guiDeletePathAndCloseTab(path);
  }
}

static void guiActivateSetting(int direction) {
  int theme_count = (int)(sizeof(GUI_THEMES) / sizeof(GUI_THEMES[0]));

  switch (G.settings_selected) {
  case 0:
    guiApplyTheme((G.theme_index + direction + theme_count) % theme_count);
    break;
  case 1:
    G.font_size += direction > 0 ? 1.0f : -1.0f;
    if (G.font_size < 11.0f) G.font_size = 11.0f;
    if (G.font_size > 32.0f) G.font_size = 32.0f;
    G.small_font_size = guiMaxInt(10, (int)(G.font_size - 3.0f));
    break;
  case 2:
    G.show_whitespace = !G.show_whitespace;
    break;
  case 3:
    G.tab_width += direction > 0 ? 1 : -1;
    if (G.tab_width < 2) G.tab_width = 2;
    if (G.tab_width > 12) G.tab_width = 12;
    g_tab_stop = G.tab_width;
    for (int row = 0; row < tbLineCount(TB); row++)
      editorUpdateRow(tbRowMut(TB, row));
    break;
  case 4:
    G.restore_session = !G.restore_session;
    break;
  case 5:
    if (G.sidebar_width > 0)
      G.sidebar_width = 0;
    else
      G.sidebar_width = GUI_SIDEBAR_WIDTH;
    break;
  }
  guiSaveSettings();
}

static void guiRunCommand(int command) {
  switch (command) {
  case GUI_CMD_NEW_FILE:
    guiNewFile();
    break;
  case GUI_CMD_NEW_WINDOW:
    guiLaunchFreshWindow();
    break;
  case GUI_CMD_CLOSE_WORKSPACE:
    guiCloseWorkspace();
    break;
  case GUI_CMD_OPEN_FILE:
    guiOpenPathPicker(0);
    break;
  case GUI_CMD_OPEN_FOLDER:
    guiOpenPathPicker(1);
    break;
  case GUI_CMD_OPEN_RECENT:
    guiOpenOverlay(GUI_OVERLAY_RECENT);
    break;
  case GUI_CMD_QUICK_OPEN:
    if (!G.project_open) {
      editorSetStatusMessage("Open a folder first for project quick open");
      break;
    }
    guiOpenOverlay(GUI_OVERLAY_QUICK_OPEN);
    break;
  case GUI_CMD_GOTO_LINE:
    guiOpenOverlay(GUI_OVERLAY_GOTO_LINE);
    break;
  case GUI_CMD_FIND:
    guiStartFind();
    break;
  case GUI_CMD_FIND_PROJECT:
    guiStartFind();
    G.find_project_active = 1;
    G.find_all_active = 1;
    G.find_dirty = 1;
    guiRefreshFindResults();
    break;
  case GUI_CMD_SAVE:
    guiSaveActiveFile();
    break;
  case GUI_CMD_SAVE_ALL:
    guiSaveAll();
    break;
  case GUI_CMD_CLOSE_TAB:
    {
      int idx = (G.context_menu == GUI_CONTEXT_TAB &&
                 G.context_tab_index >= 0)
                    ? G.context_tab_index
                    : T.active;
      if (idx >= 0)
        bentoCloseTab(idx);
    }
    break;
  case GUI_CMD_REOPEN_CLOSED:
    guiReopenClosedTab();
    break;
  case GUI_CMD_TOGGLE_SIDEBAR:
    G.sidebar_width = G.sidebar_width > 0 ? 0 : GUI_SIDEBAR_WIDTH;
    guiSaveSettings();
    break;
  case GUI_CMD_TOGGLE_DEBUG_HUD:
    G.debug_hud = !G.debug_hud;
    break;
  case GUI_CMD_FONT_INCREASE:
    G.font_size += 1.0f;
    if (G.font_size > 32.0f) G.font_size = 32.0f;
    G.small_font_size = guiMaxInt(10, (int)(G.font_size - 3.0f));
    guiSaveSettings();
    break;
  case GUI_CMD_FONT_DECREASE:
    G.font_size -= 1.0f;
    if (G.font_size < 11.0f) G.font_size = 11.0f;
    G.small_font_size = guiMaxInt(10, (int)(G.font_size - 3.0f));
    guiSaveSettings();
    break;
  case GUI_CMD_FONT_RESET:
    G.font_size = GUI_FONT_SIZE;
    G.small_font_size = GUI_SMALL_FONT_SIZE;
    guiSaveSettings();
    break;
  case GUI_CMD_TOGGLE_THEME: {
    int theme_count = (int)(sizeof(GUI_THEMES) / sizeof(GUI_THEMES[0]));
    guiApplyTheme((G.theme_index + 1) % theme_count);
    guiSaveSettings();
    break;
  }
  case GUI_CMD_TOGGLE_WHITESPACE:
    G.show_whitespace = !G.show_whitespace;
    guiSaveSettings();
    break;
  case GUI_CMD_SETTINGS:
    guiOpenOverlay(GUI_OVERLAY_SETTINGS);
    break;
  case GUI_CMD_SELECT_ALL:
    guiSelectAll();
    break;
  case GUI_CMD_DUPLICATE_LINE:
    guiDuplicateLineOrSelection();
    break;
  case GUI_CMD_MOVE_LINE_UP:
    guiMoveCurrentLine(-1);
    break;
  case GUI_CMD_MOVE_LINE_DOWN:
    guiMoveCurrentLine(1);
    break;
  case GUI_CMD_TOGGLE_COMMENT:
    guiToggleLineComment();
    break;
  case GUI_CMD_INDENT:
    guiIndentLines(1);
    break;
  case GUI_CMD_OUTDENT:
    guiIndentLines(-1);
    break;
  case GUI_CMD_TRIM_TRAILING_WS:
    guiTrimTrailingWhitespace();
    break;
  case GUI_CMD_SELECT_NEXT:
    guiSelectNextOccurrence();
    break;
  case GUI_CMD_FILE_NEW: {
    char base[PATH_MAX];
    if (G.context_target_index >= 0 &&
        G.context_target_index < G.file_count) {
      guiFileEntry *entry = &G.files[G.context_target_index];
      if (entry->is_dir)
        snprintf(base, sizeof(base), "%s", entry->path);
      else
        guiParentDir(base, sizeof(base), entry->path);
    } else {
      snprintf(base, sizeof(base), "%s", G.cwd);
    }
    guiStartPrompt(GUI_PROMPT_NEW_FILE, "New File", base, NULL, "");
    break;
  }
  case GUI_CMD_FOLDER_NEW: {
    char base[PATH_MAX];
    if (G.context_target_index >= 0 &&
        G.context_target_index < G.file_count) {
      guiFileEntry *entry = &G.files[G.context_target_index];
      if (entry->is_dir)
        snprintf(base, sizeof(base), "%s", entry->path);
      else
        guiParentDir(base, sizeof(base), entry->path);
    } else {
      snprintf(base, sizeof(base), "%s", G.cwd);
    }
    guiStartPrompt(GUI_PROMPT_NEW_FOLDER, "New Folder", base, NULL, "");
    break;
  }
  case GUI_CMD_RENAME:
    if (G.context_target_index >= 0 && G.context_target_index < G.file_count)
      guiStartPrompt(GUI_PROMPT_RENAME, "Rename",
                     G.cwd, G.files[G.context_target_index].path,
                     G.files[G.context_target_index].name);
    break;
  case GUI_CMD_DELETE:
    if (G.context_target_index >= 0 && G.context_target_index < G.file_count)
      guiStartPrompt(GUI_PROMPT_DELETE, "Delete",
                     G.cwd, G.files[G.context_target_index].path, "");
    break;
  case GUI_CMD_REVEAL:
    if (G.context_target_index >= 0 && G.context_target_index < G.file_count) {
      SetClipboardText(G.files[G.context_target_index].path);
      editorSetStatusMessage("Path copied");
    }
    break;
  case GUI_CMD_REFRESH_TREE:
    guiRebuildFileTree();
    editorSetStatusMessage("Files refreshed");
    break;
  case GUI_CMD_PIN_TAB:
    if (G.context_tab_index >= 0 && G.context_tab_index < BENTO_MAX_TABS) {
      T.list[G.context_tab_index].pinned = 1;
      if (T.preview == G.context_tab_index)
        T.preview = -1;
    }
    break;
  case GUI_CMD_CLOSE_OTHERS:
    {
      int keep = (G.context_menu == GUI_CONTEXT_TAB &&
                  G.context_tab_index >= 0)
                     ? G.context_tab_index
                     : T.active;
    for (int i = 0; i < BENTO_MAX_TABS; i++) {
      if (i != keep && T.list[i].in_use && !T.list[i].dirty)
        bentoCloseTab(i);
    }
    }
    break;
  case GUI_CMD_CLOSE_SAVED:
    for (int i = 0; i < BENTO_MAX_TABS; i++) {
      int dirty = i == T.active ? E.dirty : T.list[i].dirty;
      if (T.list[i].in_use && !dirty)
        bentoCloseTab(i);
    }
    break;
  case GUI_CMD_ABOUT:
    editorSetStatusMessage("Bento %s - small, fast text editor", bento_VERSION);
    break;
  default:
    break;
  }
}

static void guiRunSelectedCommand(void) {
  int def_index = guiFilteredCommandIndex(G.overlay_selected);
  if (def_index < 0)
    return;
  int command = GUI_COMMAND_DEFS[def_index].command;
  guiCloseOverlay();
  guiRunCommand(command);
}

static void guiProcessOverlayInput(void) {
  int count = 0;
  int list_overlay = G.overlay == GUI_OVERLAY_QUICK_OPEN ||
                     G.overlay == GUI_OVERLAY_COMMAND_PALETTE ||
                     G.overlay == GUI_OVERLAY_RECENT;

  if (IsKeyPressed(KEY_ESCAPE)) {
    guiCloseOverlay();
    return;
  }

  if (G.overlay == GUI_OVERLAY_SETTINGS) {
    if (guiKeyPressedRepeat(KEY_UP))
      G.settings_selected =
          (G.settings_selected - 1 + GUI_SETTINGS_COUNT) % GUI_SETTINGS_COUNT;
    if (guiKeyPressedRepeat(KEY_DOWN))
      G.settings_selected = (G.settings_selected + 1) % GUI_SETTINGS_COUNT;
    if (IsKeyPressed(KEY_LEFT))
      guiActivateSetting(-1);
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_ENTER) ||
        IsKeyPressed(KEY_KP_ENTER))
      guiActivateSetting(1);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      Vector2 mouse = GetMousePosition();
      int box_w = guiMinInt(720, GetScreenWidth() - guiSidebarWidth() - 80);
      int box_x;
      int row;
      if (box_w < 320)
        box_w = guiMaxInt(220, GetScreenWidth() - guiSidebarWidth() - 24);
      box_x = guiSidebarWidth() +
              (GetScreenWidth() - guiSidebarWidth() - box_w) / 2;
      row = (int)((mouse.y - 140) / 30);
      if (mouse.x >= box_x && mouse.x <= box_x + box_w && row >= 0 &&
          row < GUI_SETTINGS_COUNT) {
        G.settings_selected = row;
        guiActivateSetting(1);
      }
    }
    return;
  }

  if (G.overlay == GUI_OVERLAY_PROMPT) {
    if (guiKeyPressedRepeat(KEY_BACKSPACE) && G.prompt_value_len > 0)
      G.prompt_value[--G.prompt_value_len] = '\0';
    if (guiModifierDown() && IsKeyPressed(KEY_V)) {
      if (!guiAppendAsciiToBuffer(GetClipboardText(), G.prompt_value,
                                  &G.prompt_value_len, GUI_INPUT_LEN, 0))
        editorSetStatusMessage("Clipboard has no editable text");
      return;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
      guiCompletePrompt();
      return;
    }
    for (int c = GetCharPressed(); c > 0; c = GetCharPressed()) {
      if (!guiModifierDown() && c >= 32 && c < 127 &&
          G.prompt_value_len < GUI_INPUT_LEN) {
        G.prompt_value[G.prompt_value_len++] = (char)c;
        G.prompt_value[G.prompt_value_len] = '\0';
      }
    }
    return;
  }

  if (guiKeyPressedRepeat(KEY_BACKSPACE) && G.overlay_query_len > 0) {
    G.overlay_query[--G.overlay_query_len] = '\0';
    G.overlay_selected = 0;
    G.overlay_scroll = 0;
  }
  if (guiModifierDown() && IsKeyPressed(KEY_V)) {
    if (guiAppendAsciiToBuffer(GetClipboardText(), G.overlay_query,
                               &G.overlay_query_len, GUI_INPUT_LEN,
                               G.overlay == GUI_OVERLAY_GOTO_LINE)) {
      G.overlay_selected = 0;
      G.overlay_scroll = 0;
    } else {
      editorSetStatusMessage("Clipboard has no editable text");
    }
    return;
  }

  if (list_overlay) {
    if (G.overlay == GUI_OVERLAY_QUICK_OPEN)
      count = guiFilteredFileCount();
    else if (G.overlay == GUI_OVERLAY_COMMAND_PALETTE)
      count = guiFilteredCommandCount();
    else
      count = guiFilteredRecentCount();
    if (guiKeyPressedRepeat(KEY_UP) && count > 0)
      G.overlay_selected =
          (G.overlay_selected - 1 + count) % guiMaxInt(1, count);
    if (guiKeyPressedRepeat(KEY_DOWN) && count > 0)
      G.overlay_selected = (G.overlay_selected + 1) % guiMaxInt(1, count);
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
      if (G.overlay == GUI_OVERLAY_QUICK_OPEN)
        guiOpenQuickSelectedFile();
      else if (G.overlay == GUI_OVERLAY_COMMAND_PALETTE)
        guiRunSelectedCommand();
      else
        guiOpenSelectedRecent();
    }
  } else if (G.overlay == GUI_OVERLAY_GOTO_LINE) {
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))
      guiGoToRequestedLine();
  }

  for (int c = GetCharPressed(); c > 0; c = GetCharPressed()) {
    if (guiModifierDown() || G.overlay_query_len >= GUI_INPUT_LEN)
      continue;
    if (G.overlay == GUI_OVERLAY_GOTO_LINE && !isdigit(c))
      continue;
    if (c >= 32 && c < 127) {
      G.overlay_query[G.overlay_query_len++] = (char)c;
      G.overlay_query[G.overlay_query_len] = '\0';
      G.overlay_selected = 0;
      G.overlay_scroll = 0;
    }
  }

  if (list_overlay) {
    if (G.overlay == GUI_OVERLAY_QUICK_OPEN)
      count = guiFilteredFileCount();
    else if (G.overlay == GUI_OVERLAY_COMMAND_PALETTE)
      count = guiFilteredCommandCount();
    else
      count = guiFilteredRecentCount();
    if (G.overlay_selected >= count)
      G.overlay_selected = guiMaxInt(0, count - 1);
    if (G.overlay_selected < G.overlay_scroll)
      G.overlay_scroll = G.overlay_selected;
    if (G.overlay_selected >= G.overlay_scroll + 8)
      G.overlay_scroll = G.overlay_selected - 7;

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      Vector2 mouse = GetMousePosition();
      int box_w = guiMinInt(720, GetScreenWidth() - guiSidebarWidth() - 80);
      int box_x;
      int row;

      if (box_w < 320)
        box_w = guiMaxInt(220, GetScreenWidth() - guiSidebarWidth() - 24);
      box_x = guiSidebarWidth() +
              (GetScreenWidth() - guiSidebarWidth() - box_w) / 2;
      row = (int)((mouse.y - 160) / GUI_TREE_ROW_HEIGHT);
      if (mouse.x >= box_x && mouse.x <= box_x + box_w && row >= 0 &&
          row < 8) {
        int index = G.overlay_scroll + row;
        if (index < count) {
          G.overlay_selected = index;
          if (G.overlay == GUI_OVERLAY_QUICK_OPEN)
            guiOpenQuickSelectedFile();
          else if (G.overlay == GUI_OVERLAY_COMMAND_PALETTE)
            guiRunSelectedCommand();
          else
            guiOpenSelectedRecent();
        }
      }
    }
  }
}

typedef struct cFunctionHint {
  const char *name;
  const char *signature;
} cFunctionHint;

static const cFunctionHint C_FUNCTION_HINTS[] = {
    {"printf", "printf(const char *format, ...)"},
    {"fprintf", "fprintf(FILE *stream, const char *format, ...)"},
    {"snprintf", "snprintf(char *str, size_t size, const char *format, ...)"},
    {"sprintf", "sprintf(char *str, const char *format, ...)"},
    {"scanf", "scanf(const char *format, ...)"},
    {"fscanf", "fscanf(FILE *stream, const char *format, ...)"},
    {"malloc", "malloc(size_t size)"},
    {"calloc", "calloc(size_t count, size_t size)"},
    {"realloc", "realloc(void *ptr, size_t size)"},
    {"free", "free(void *ptr)"},
    {"fopen", "fopen(const char *path, const char *mode)"},
    {"fclose", "fclose(FILE *stream)"},
    {"fread", "fread(void *ptr, size_t size, size_t count, FILE *stream)"},
    {"fwrite", "fwrite(const void *ptr, size_t size, size_t count, FILE *stream)"},
    {"getline", "getline(char **lineptr, size_t *n, FILE *stream)"},
    {"strlen", "strlen(const char *s)"},
    {"strcmp", "strcmp(const char *s1, const char *s2)"},
    {"strncmp", "strncmp(const char *s1, const char *s2, size_t n)"},
    {"strcpy", "strcpy(char *dest, const char *src)"},
    {"strncpy", "strncpy(char *dest, const char *src, size_t n)"},
    {"strstr", "strstr(const char *haystack, const char *needle)"},
    {"strchr", "strchr(const char *s, int c)"},
    {"memcpy", "memcpy(void *dest, const void *src, size_t n)"},
    {"memmove", "memmove(void *dest, const void *src, size_t n)"},
    {"memset", "memset(void *s, int c, size_t n)"},
    {"open", "open(const char *path, int flags, mode_t mode)"},
    {"read", "read(int fd, void *buf, size_t count)"},
    {"write", "write(int fd, const void *buf, size_t count)"},
    {"close", "close(int fd)"},
    {"ftruncate", "ftruncate(int fd, off_t length)"},
    {"exit", "exit(int status)"},
    {"perror", "perror(const char *s)"},
    {NULL, NULL}};

static int guiIsIdentChar(int c) { return isalnum(c) || c == '_'; }

static int guiCopyTokenBeforePosition(erow *row, int pos, char *token,
                                      size_t token_len) {
  int end, start, len;

  if (!row || pos <= 0)
    return 0;
  if (pos > row->size)
    pos = row->size;
  end = pos;
  while (end > 0 && isspace((unsigned char)row->chars[end - 1]))
    end--;
  start = end;
  while (start > 0 && guiIsIdentChar((unsigned char)row->chars[start - 1]))
    start--;
  len = end - start;
  if (len <= 0 || len >= (int)token_len)
    return 0;
  memcpy(token, row->chars + start, (size_t)len);
  token[len] = '\0';
  return 1;
}

static int guiCurrentCallToken(char *token, size_t token_len) {
  int filerow = E.rowoff + E.cy;
  int filecol = E.coloff + E.cx;
  erow *row = tbRowMut(TB, filerow);
  int pos, depth = 0;

  if (!row)
    return 0;
  pos = guiMinInt(filecol, row->size);
  for (int i = pos - 1; i >= 0; i--) {
    char c = row->chars[i];

    if (c == ')') {
      depth++;
    } else if (c == '(') {
      if (depth == 0)
        return guiCopyTokenBeforePosition(row, i, token, token_len);
      depth--;
    } else if (depth == 0 && (c == ';' || c == '{' || c == '}')) {
      break;
    }
  }
  /* No enclosing `(` found before the cursor — we're not in a function-call
   * argument list, so there's no useful hint to show. Returning the bare
   * identifier under the cursor here used to trigger a full-buffer
   * signature scan on every keystroke in unrelated code (e.g. typing a
   * variable name), with no upside since nothing matches anyway. */
  return 0;
}

/* Internal: resolve a function-name token to a signature string. Expensive
 * on big files because guiFindCurrentBufferSignature flattens the whole
 * buffer. Call sites should go through guiCachedFunctionHint() instead;
 * this stays here so we can keep one place that owns the lookup logic. */
static const char *guiResolveHintForToken(const char *token) {
  const char *hint;
  int language = guiActiveLanguage();

  hint = guiFindCurrentBufferSignature(token);
  if (hint)
    return hint;
  hint = guiFindProjectSignature(token);
  if (hint)
    return hint;

  if (language == BENTO_LANG_C || language == BENTO_LANG_NONE) {
    for (int i = 0; C_FUNCTION_HINTS[i].name; i++) {
      if (!strcmp(C_FUNCTION_HINTS[i].name, token))
        return C_FUNCTION_HINTS[i].signature;
    }
  }
  return NULL;
}

/* Per-frame cached function hint. Recomputes only when the cursor's
 * call-token, filename, or numrows changes — so steady-state typing
 * within a call body is a no-op after the first frame. */
static const char *guiCachedFunctionHint(void) {
  return G.hint_valid ? G.hint_text : NULL;
}

static void guiUpdateFrameHint(void) {
  char token[GUI_SIGNATURE_NAME_LEN];
  if (!guiCurrentCallToken(token, sizeof(token))) {
    G.hint_valid = 0;
    G.hint_token[0] = '\0';
    return;
  }

  /* Cache hit: same token, same file identity, same row count. Typing
   * inside a call body keeps all three stable. */
  if (G.hint_valid &&
      strcmp(token, G.hint_token) == 0 &&
      G.hint_filename_at == E.filename &&
      G.hint_numrows_at == E.numrows) {
    return;
  }

  /* Cache miss: recompute. */
  snprintf(G.hint_token, sizeof(G.hint_token), "%s", token);
  G.hint_filename_at = E.filename;
  G.hint_numrows_at = E.numrows;

  const char *hint = guiResolveHintForToken(token);
  if (hint) {
    snprintf(G.hint_text, sizeof(G.hint_text), "%s", hint);
    G.hint_valid = 1;
  } else {
    G.hint_text[0] = '\0';
    G.hint_valid = 0;
  }
}

static int guiTabAtMouse(Vector2 mouse) {
  int sidebar = guiSidebarWidth();

  if (mouse.y >= GUI_TAB_BAR_HEIGHT || mouse.x < sidebar)
    return -1;
  for (int i = 0; i < BENTO_MAX_TABS; i++) {
    guiTab *tab = &T.list[i];
    if (!tab->in_use || tab->rect_w == 0)
      continue;
    if (mouse.x >= tab->rect_x && mouse.x < tab->rect_x + tab->rect_w &&
        mouse.y >= tab->rect_y && mouse.y < tab->rect_y + tab->rect_h)
      return i;
  }
  return -1;
}

static void guiContextAdd(const char *label, int command, int enabled) {
  if (G.context_item_count >= GUI_CONTEXT_ITEMS_MAX)
    return;
  G.context_items[G.context_item_count].label = label;
  G.context_items[G.context_item_count].command = command;
  G.context_items[G.context_item_count].enabled = enabled;
  G.context_item_count++;
}

static void guiOpenContextMenu(int kind, int x, int y, int target_index,
                               int tab_index) {
  G.context_menu = kind;
  G.context_x = x;
  G.context_y = y;
  G.context_target_index = target_index;
  G.context_tab_index = tab_index;
  G.context_item_count = 0;

  if (kind == GUI_CONTEXT_TREE) {
    guiContextAdd("New File", GUI_CMD_FILE_NEW, 1);
    guiContextAdd("New Folder", GUI_CMD_FOLDER_NEW, 1);
    guiContextAdd("Rename", GUI_CMD_RENAME, target_index >= 0);
    guiContextAdd("Delete", GUI_CMD_DELETE, target_index >= 0);
    guiContextAdd("Copy Path", GUI_CMD_REVEAL, target_index >= 0);
    guiContextAdd("Refresh", GUI_CMD_REFRESH_TREE, 1);
  } else if (kind == GUI_CONTEXT_TAB) {
    guiContextAdd("Pin Tab", GUI_CMD_PIN_TAB, tab_index >= 0);
    guiContextAdd("Close Tab", GUI_CMD_CLOSE_TAB, tab_index >= 0);
    guiContextAdd("Reopen Closed", GUI_CMD_REOPEN_CLOSED,
                  G.last_closed_file[0] != '\0');
    guiContextAdd("Close Others", GUI_CMD_CLOSE_OTHERS, T.count > 1);
    guiContextAdd("Close Saved", GUI_CMD_CLOSE_SAVED, T.count > 0);
  } else if (kind == GUI_CONTEXT_EDITOR) {
    guiContextAdd("New Window", GUI_CMD_NEW_WINDOW, 1);
    guiContextAdd("Close Workspace", GUI_CMD_CLOSE_WORKSPACE, 1);
    guiContextAdd("Command Palette", GUI_CMD_NONE, 1);
    guiContextAdd("Select All", GUI_CMD_SELECT_ALL, T.active >= 0);
    guiContextAdd("Duplicate", GUI_CMD_DUPLICATE_LINE, T.active >= 0);
    guiContextAdd("Toggle Comment", GUI_CMD_TOGGLE_COMMENT, T.active >= 0);
    guiContextAdd("Trim Whitespace", GUI_CMD_TRIM_TRAILING_WS, T.active >= 0);
  }
}

static int guiProcessContextMenu(void) {
  if (G.context_menu == GUI_CONTEXT_NONE)
    return 0;
  if (IsKeyPressed(KEY_ESCAPE)) {
    G.context_menu = GUI_CONTEXT_NONE;
    return 1;
  }
  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
    Vector2 mouse = GetMousePosition();
    int w = 180;
    int h = G.context_item_count * 24 + 8;
    int x = G.context_x;
    int y = G.context_y;
    int row;

    if (x + w > GetScreenWidth())
      x = GetScreenWidth() - w - 4;
    if (y + h > GetScreenHeight())
      y = GetScreenHeight() - h - 4;
    row = (int)((mouse.y - y - 4) / 24);
    if (mouse.x >= x && mouse.x <= x + w && row >= 0 &&
        row < G.context_item_count && G.context_items[row].enabled) {
      int command = G.context_items[row].command;
      G.context_x = x;
      G.context_y = y;
      if (command == GUI_CMD_NONE)
        guiOpenOverlay(GUI_OVERLAY_COMMAND_PALETTE);
      else
        guiRunCommand(command);
      G.context_menu = GUI_CONTEXT_NONE;
      return 1;
    }
    G.context_menu = GUI_CONTEXT_NONE;
    return 1;
  }
  return 1;
}

static int guiHandleWelcomeInput(void) {
  int sidebar = guiSidebarWidth();
  int x = sidebar + guiMaxInt(32, (GetScreenWidth() - sidebar - 760) / 2);
  int y = 94 + 84;
  int button_w = 112;
  int button_h = 48;

  if (!G.welcome_active || T.count > 0)
    return 0;

  if (guiModifierDown() && guiShiftDown() && IsKeyPressed(KEY_N)) {
    guiRunCommand(GUI_CMD_NEW_WINDOW);
    return 1;
  }
  if (IsKeyPressed(KEY_ENTER) || (guiModifierDown() && IsKeyPressed(KEY_N))) {
    guiNewFile();
    return 1;
  }
  if (guiModifierDown() && IsKeyPressed(KEY_P)) {
    guiRunCommand(GUI_CMD_QUICK_OPEN);
    return 1;
  }
  if (guiModifierDown() && guiShiftDown() && IsKeyPressed(KEY_O)) {
    guiRunCommand(GUI_CMD_OPEN_FOLDER);
    return 1;
  }
  if (guiModifierDown() && IsKeyPressed(KEY_O)) {
    guiRunCommand(GUI_CMD_OPEN_FILE);
    return 1;
  }
  if (guiModifierDown() && guiShiftDown() && IsKeyPressed(KEY_R)) {
    guiRunCommand(GUI_CMD_OPEN_RECENT);
    return 1;
  }
  if (guiModifierDown() && guiShiftDown() && IsKeyPressed(KEY_W)) {
    guiRunCommand(GUI_CMD_CLOSE_WORKSPACE);
    return 1;
  }

  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
    Vector2 mouse = GetMousePosition();
    for (int i = 0; i < 6; i++) {
      int bx = x + i * (button_w + 8);
      if (mouse.x >= bx && mouse.x <= bx + button_w &&
          mouse.y >= y && mouse.y <= y + button_h) {
        if (i == 0)
          guiNewFile();
        else if (i == 1)
          guiRunCommand(GUI_CMD_NEW_WINDOW);
        else if (i == 2)
          guiRunCommand(GUI_CMD_OPEN_FILE);
        else if (i == 3)
          guiRunCommand(GUI_CMD_OPEN_FOLDER);
        else if (i == 4)
          guiOpenOverlay(GUI_OVERLAY_RECENT);
        else
          guiOpenOverlay(GUI_OVERLAY_SETTINGS);
        return 1;
      }
    }

    int recent_y = y + 86;
    for (int i = 0; i < G.recent_file_count && i < 6; i++) {
      int row_y = recent_y + i * GUI_TREE_ROW_HEIGHT;
      if (mouse.x >= x && mouse.x <= x + 590 &&
          mouse.y >= row_y && mouse.y <= row_y + GUI_TREE_ROW_HEIGHT) {
        guiOpenFile(G.recent_files[i]);
        return 1;
      }
    }
  }
  return 0;
}

static void guiProcessMouseInput(void) {
  Vector2 mouse = GetMousePosition();
  float wheel = GetMouseWheelMove();
  int editor_top = guiEditorTop();
  int editor_bottom = GetScreenHeight() - GUI_STATUS_BAR_HEIGHT;
  int sidebar = guiSidebarWidth();

  if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
    int tab = guiTabAtMouse(mouse);
    int tree = guiTreeEntryAtMouse(mouse);
    if (tab >= 0)
      guiOpenContextMenu(GUI_CONTEXT_TAB, (int)mouse.x, (int)mouse.y, -1, tab);
    else if (tree >= 0 || (sidebar > 0 && mouse.x < sidebar))
      guiOpenContextMenu(GUI_CONTEXT_TREE, (int)mouse.x, (int)mouse.y, tree,
                         -1);
    else if (mouse.y >= editor_top && mouse.y < editor_bottom)
      guiOpenContextMenu(GUI_CONTEXT_EDITOR, (int)mouse.x, (int)mouse.y, -1,
                         -1);
    return;
  }

  if (guiProcessContextMenu())
    return;

  if (wheel != 0.0f) {
    if (sidebar > 0 && mouse.x < sidebar) {
      guiScrollTreeByLines(guiWheelToLines(wheel, &G.wheel_tree_accum, 3.0f));
    } else if (guiAltDown()) {
      guiScrollColumns(guiWheelToLines(wheel, &G.wheel_col_accum, 6.0f));
    } else if (guiShiftDown()) {
      guiMoveCursorByLinesWithSelection(
          guiWheelToLines(wheel, &G.wheel_row_accum, 3.0f));
    } else {
      guiScrollRowsByWheel(wheel);
    }
  }

  /* Tab bar input: click body to switch, double-click to pin, click × to
   * close. Handled before the editor-area mouse fall-through. */
  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && mouse.y < GUI_TAB_BAR_HEIGHT &&
      mouse.x >= sidebar) {
    int handled = 0;
    for (int i = 0; i < BENTO_MAX_TABS && !handled; i++) {
      guiTab *tab = &T.list[i];
      if (!tab->in_use || tab->rect_w == 0)
        continue;
      if (mouse.x >= tab->close_x && mouse.x < tab->close_x + tab->close_size &&
          mouse.y >= tab->close_y && mouse.y < tab->close_y + tab->close_size) {
        if ((i == T.active && E.dirty) || (i != T.active && tab->dirty)) {
          double now = GetTime();
          if (T.close_confirm_index == i &&
              (now - T.close_confirm_time) < 2.0) {
            bentoCloseTab(i);
            T.close_confirm_index = -1;
          } else {
            T.close_confirm_index = i;
            T.close_confirm_time = now;
            editorSetStatusMessage(
                "Unsaved changes. Click × again to discard.");
          }
        } else {
          bentoCloseTab(i);
        }
        handled = 1;
        break;
      }
      if (mouse.x >= tab->rect_x && mouse.x < tab->rect_x + tab->rect_w &&
          mouse.y >= tab->rect_y && mouse.y < tab->rect_y + tab->rect_h) {
        double now = GetTime();
        int is_double = (T.last_tab_click_index == i &&
                         (now - T.last_tab_click_time) < 0.35);
        if (i != T.active)
          bentoSwitchToTab(i);
        if (is_double) {
          tab->pinned = 1;
          if (T.preview == i)
            T.preview = -1;
        }
        T.last_tab_click_index = i;
        T.last_tab_click_time = now;
        handled = 1;
        break;
      }
    }
    if (handled)
      return;
  }

  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && sidebar > 0 &&
      mouse.x < sidebar) {
    int index = guiTreeEntryAtMouse(mouse);
    guiClearSelection();
    if (index >= 0 && !G.files[index].is_dir) {
      double now = GetTime();
      int is_double = (T.last_click_file_index == index &&
                       (now - T.last_click_time) < 0.35);
      bentoOpenFile(G.files[index].path, is_double);
      T.last_click_file_index = index;
      T.last_click_time = now;
    }
  } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && mouse.y >= editor_top &&
             mouse.y < editor_bottom) {
    int filerow, filecol;

    if (!guiFilePositionFromMouse(mouse, &filerow, &filecol))
      return;
    if (!guiShiftDown()) {
      double now = GetTime();
      int is_double = (G.last_editor_click_row == filerow &&
                       guiAbsInt(G.last_editor_click_col - filecol) <= 1 &&
                       (now - G.last_editor_click_time) < 0.35);

      G.last_editor_click_row = filerow;
      G.last_editor_click_col = filecol;
      G.last_editor_click_time = now;
      if (is_double && guiSelectWordAtPosition(filerow, filecol)) {
        G.mouse_selecting = 0;
        return;
      }
      guiMoveCursorToPositionWithSelection(filerow, filecol, 0);
      guiBeginSelectionIfNeeded();
    } else {
      guiMoveCursorToPositionWithSelection(filerow, filecol, 1);
    }
    G.mouse_selecting = 1;
  } else if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && G.mouse_selecting &&
             mouse.y >= editor_top && mouse.y < editor_bottom) {
    guiMoveCursorFromMouse(mouse, 1);
  }

  if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && G.mouse_selecting) {
    G.mouse_selecting = 0;
    if (!guiHasSelection())
      guiClearSelection();
  }
}

static void guiProcessEditorInput(void) {
  int command_handled = 0;

  guiUpdateLayout();

  if (G.overlay != GUI_OVERLAY_NONE) {
    guiProcessOverlayInput();
    return;
  }

  if (guiHandleWelcomeInput())
    return;

  if (guiModifierDown() && !guiAltDown() && IsKeyPressed(KEY_S)) {
    guiSaveActiveFile();
    command_handled = 1;
  }
  if (guiModifierDown() && IsKeyPressed(KEY_Q)) {
    guiRequestClose();
    return;
  }
  if (guiModifierDown() && guiShiftDown() && IsKeyPressed(KEY_F)) {
    guiStartFind();
    G.find_project_active = 1;
    G.find_all_active = 1;
    G.current_match = -1;
    G.match_scroll = 0;
    G.find_dirty = 1;
    guiRefreshFindResults();
    command_handled = 1;
  } else if (guiModifierDown() && IsKeyPressed(KEY_F)) {
    guiStartFind();
    command_handled = 1;
  }
  if (guiModifierDown() && guiShiftDown() && IsKeyPressed(KEY_N)) {
    guiRunCommand(GUI_CMD_NEW_WINDOW);
    command_handled = 1;
  }
  if (guiModifierDown() && guiShiftDown() && IsKeyPressed(KEY_R)) {
    guiRunCommand(GUI_CMD_OPEN_RECENT);
    command_handled = 1;
  }
  if (guiModifierDown() && guiShiftDown() && IsKeyPressed(KEY_P)) {
    guiOpenOverlay(GUI_OVERLAY_COMMAND_PALETTE);
    command_handled = 1;
  } else if (guiModifierDown() && IsKeyPressed(KEY_P)) {
    guiRunCommand(GUI_CMD_QUICK_OPEN);
    command_handled = 1;
  }
  if (guiModifierDown() && !guiShiftDown() && IsKeyPressed(KEY_N)) {
    guiRunCommand(GUI_CMD_NEW_FILE);
    command_handled = 1;
  }
  if (guiModifierDown() && guiShiftDown() && IsKeyPressed(KEY_W)) {
    guiRunCommand(GUI_CMD_CLOSE_WORKSPACE);
    command_handled = 1;
  }
  if (guiModifierDown() && guiShiftDown() && IsKeyPressed(KEY_T)) {
    guiRunCommand(GUI_CMD_REOPEN_CLOSED);
    command_handled = 1;
  }
  if (guiModifierDown() && guiAltDown() && IsKeyPressed(KEY_S)) {
    guiRunCommand(GUI_CMD_SAVE_ALL);
    command_handled = 1;
  }
  if (guiModifierDown() && guiShiftDown() && IsKeyPressed(KEY_O)) {
    guiRunCommand(GUI_CMD_OPEN_FOLDER);
    command_handled = 1;
  } else if (guiModifierDown() && IsKeyPressed(KEY_O)) {
    guiRunCommand(GUI_CMD_OPEN_FILE);
    command_handled = 1;
  }
  if (guiModifierDown() && IsKeyPressed(KEY_G)) {
    guiOpenOverlay(GUI_OVERLAY_GOTO_LINE);
    command_handled = 1;
  }
  if (guiModifierDown() && IsKeyPressed(KEY_COMMA)) {
    guiRunCommand(GUI_CMD_SETTINGS);
    command_handled = 1;
  }
  if (guiModifierDown() && IsKeyPressed(KEY_B)) {
    guiRunCommand(GUI_CMD_TOGGLE_SIDEBAR);
    command_handled = 1;
  }
  if (guiModifierDown() && IsKeyPressed(KEY_EQUAL)) {
    guiRunCommand(GUI_CMD_FONT_INCREASE);
    command_handled = 1;
  }
  if (guiModifierDown() && IsKeyPressed(KEY_MINUS)) {
    guiRunCommand(GUI_CMD_FONT_DECREASE);
    command_handled = 1;
  }
  if (guiModifierDown() && !guiShiftDown() && IsKeyPressed(KEY_D)) {
    guiRunCommand(GUI_CMD_DUPLICATE_LINE);
    command_handled = 1;
  }
  if (guiModifierDown() && IsKeyPressed(KEY_E)) {
    guiRunCommand(GUI_CMD_SELECT_NEXT);
    command_handled = 1;
  }
  if (guiModifierDown() && IsKeyPressed(KEY_SLASH)) {
    guiRunCommand(GUI_CMD_TOGGLE_COMMENT);
    command_handled = 1;
  }
  if (guiModifierDown() && !guiShiftDown() && IsKeyPressed(KEY_W)) {
    /* Cmd/Ctrl+W closes the current tab; dirty tabs need a second press. */
    if (T.active >= 0) {
      if (E.dirty) {
        double now = GetTime();
        if (T.close_confirm_index == T.active &&
            (now - T.close_confirm_time) < 2.0) {
          bentoCloseTab(T.active);
          T.close_confirm_index = -1;
        } else {
          T.close_confirm_index = T.active;
          T.close_confirm_time = now;
          editorSetStatusMessage(
              "Unsaved changes. Press Cmd/Ctrl+W again to discard.");
        }
      } else {
        bentoCloseTab(T.active);
      }
    }
    command_handled = 1;
  }
  if (guiModifierDown() && IsKeyPressed(KEY_TAB)) {
    /* Cycle to the next tab (Shift reverses direction). */
    if (T.count > 1 && T.active >= 0) {
      int step = guiShiftDown() ? -1 : 1;
      int next = T.active;
      for (int tries = 0; tries < BENTO_MAX_TABS; tries++) {
        next = (next + step + BENTO_MAX_TABS) % BENTO_MAX_TABS;
        if (T.list[next].in_use && next != T.active) {
          bentoSwitchToTab(next);
          break;
        }
      }
    }
    command_handled = 1;
  }

  if (command_handled) {
    G.quit_times = bento_QUIT_TIMES;
    return;
  }

  if (G.find_active && !G.find_editor_focus) {
    guiProcessFindInput();
    return;
  }
  if (G.find_active && G.find_editor_focus) {
    if (IsKeyPressed(KEY_ESCAPE)) {
      guiCloseFind();
      return;
    }
    if (guiHandleFindMouseClick(GetMousePosition(), 0))
      return;
  }

  if (guiModifierDown() && IsKeyPressed(KEY_A)) {
    guiSelectAll();
    command_handled = 1;
  } else if (guiModifierDown() && IsKeyPressed(KEY_C)) {
    guiCopySelectionToClipboard();
    command_handled = 1;
  } else if (guiModifierDown() && IsKeyPressed(KEY_X)) {
    guiCutSelectionToClipboard();
    command_handled = 1;
  } else if (guiModifierDown() && IsKeyPressed(KEY_V)) {
    guiPasteClipboardText();
    command_handled = 1;
  } else if (guiModifierDown() && IsKeyPressed(KEY_Z)) {
    editorUndo();
    command_handled = 1;
  }

  if (command_handled) {
    G.quit_times = bento_QUIT_TIMES;
    return;
  }

  guiProcessMouseInput();

  if (guiKeyPressedRepeat(KEY_BACKSPACE)) {
    if (!guiDeleteSelection())
      editorDelChar();
  } else if (guiKeyPressedRepeat(KEY_DELETE)) {
    if (!guiDeleteSelection())
      guiDeleteForwardChar();
  } else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
    if (!guiEnsureEditableFile())
      return;
    int wrap = guiHasSelection();
    if (wrap) undoBeginTxn();
    guiDeleteSelection();
    editorInsertNewline();
    if (wrap) undoEndTxn();
  } else if (IsKeyPressed(KEY_TAB)) {
    if (!guiEnsureEditableFile())
      return;
    if (guiShiftDown())
      guiIndentLines(-1);
    else if (guiHasSelection())
      guiIndentLines(1);
    else
      editorInsertChar(TAB);
  } else if (guiAltDown() && guiKeyPressedRepeat(KEY_LEFT)) {
    guiMoveClassBoundaryWithSelection(-1);
  } else if (guiAltDown() && guiKeyPressedRepeat(KEY_RIGHT)) {
    guiMoveClassBoundaryWithSelection(1);
  } else if (guiAltDown() && guiKeyPressedRepeat(KEY_UP)) {
    guiMoveCurrentLine(-1);
  } else if (guiAltDown() && guiKeyPressedRepeat(KEY_DOWN)) {
    guiMoveCurrentLine(1);
  } else if (guiLineBoundaryModifierDown() && guiKeyPressedRepeat(KEY_LEFT)) {
    guiMoveLineBoundaryWithSelection(0);
  } else if (guiLineBoundaryModifierDown() && guiKeyPressedRepeat(KEY_RIGHT)) {
    guiMoveLineBoundaryWithSelection(1);
  } else if (guiLineBoundaryModifierDown() && guiKeyPressedRepeat(KEY_UP)) {
    /* Cmd/Ctrl + Up: jump to start of document (Shift extends selection). */
    guiMoveDocumentBoundaryWithSelection(0);
  } else if (guiLineBoundaryModifierDown() && guiKeyPressedRepeat(KEY_DOWN)) {
    /* Cmd/Ctrl + Down: jump to end of document. */
    guiMoveDocumentBoundaryWithSelection(1);
  } else if (guiKeyPressedRepeat(KEY_LEFT)) {
    guiMoveCursorWithSelection(ARROW_LEFT);
  } else if (guiKeyPressedRepeat(KEY_RIGHT)) {
    guiMoveCursorWithSelection(ARROW_RIGHT);
  } else if (guiKeyPressedRepeat(KEY_UP)) {
    guiMoveCursorWithSelection(ARROW_UP);
  } else if (guiKeyPressedRepeat(KEY_DOWN)) {
    guiMoveCursorWithSelection(ARROW_DOWN);
  } else if (IsKeyPressed(KEY_HOME)) {
    guiMoveLineBoundaryWithSelection(0);
  } else if (IsKeyPressed(KEY_END)) {
    guiMoveLineBoundaryWithSelection(1);
  } else if (IsKeyPressed(KEY_PAGE_UP)) {
    guiResetEditorSmoothScroll();
    if (guiShiftDown())
      guiBeginSelectionIfNeeded();
    else
      guiClearSelection();
    E.cy = 0;
    for (int i = 0; i < E.screenrows; i++)
      editorMoveCursor(ARROW_UP);
    if (guiShiftDown())
      guiUpdateSelectionToCursor();
  } else if (IsKeyPressed(KEY_PAGE_DOWN)) {
    guiResetEditorSmoothScroll();
    if (guiShiftDown())
      guiBeginSelectionIfNeeded();
    else
      guiClearSelection();
    E.cy = E.screenrows - 1;
    for (int i = 0; i < E.screenrows; i++)
      editorMoveCursor(ARROW_DOWN);
    if (guiShiftDown())
      guiUpdateSelectionToCursor();
  }

  if (!guiModifierDown()) {
    int deleted_selection = 0;
    int started_txn = 0;

    for (int c = GetCharPressed(); c > 0; c = GetCharPressed()) {
      if (c >= 32 && c < 127) {
        if (!guiEnsureEditableFile())
          break;
        if (!deleted_selection) {
          if (guiHasSelection()) {
            undoBeginTxn();
            started_txn = 1;
          }
          guiDeleteSelection();
          deleted_selection = 1;
        }
        editorInsertChar(c);
      }
    }
    if (started_txn)
      undoEndTxn();
  }

  /* Editing the preview tab implicitly pins it, matching VS Code's
   * "don't silently throw away the user's work" behavior. */
  bentoAutoPinIfDirty();

  G.quit_times = bento_QUIT_TIMES;
}

static void guiDrawTopBar(void) {
  int sidebar = guiSidebarWidth();
  int width = GetScreenWidth() - sidebar;

  DrawRectangle(sidebar, 0, width, GUI_TAB_BAR_HEIGHT, GUI_PANEL_DARK);
  DrawLine(sidebar, GUI_TAB_BAR_HEIGHT - 1, sidebar + width,
           GUI_TAB_BAR_HEIGHT - 1, GUI_BORDER);

  if (T.count == 0)
    return;

  /* Each tab is sized to its title within sensible bounds. They all share
   * the same available width and shrink uniformly if the total overflows. */
  int min_tab_w = 96;
  int max_tab_w = 220;
  int spacing = 4;
  int avail = width - 24;
  int total_desired = 0;

  int desired_w[BENTO_MAX_TABS];
  for (int i = 0; i < BENTO_MAX_TABS; i++)
    desired_w[i] = 0;

  for (int i = 0; i < BENTO_MAX_TABS; i++) {
    guiTab *tab = &T.list[i];
    if (!tab->in_use)
      continue;
    const char *name = (i == T.active && E.filename)
                           ? guiBaseName(E.filename)
                           : guiBaseName(tab->filename);
    int dirty = (i == T.active) ? E.dirty : tab->dirty;
    char label[280];
    snprintf(label, sizeof(label), "%s%s", name, dirty ? " *" : "");
    Vector2 sz = MeasureTextEx(G.font, label, G.small_font_size,
                               GUI_FONT_SPACING);
    int w = (int)sz.x + 56; /* room for italic preview, close ×, padding */
    if (w < min_tab_w)
      w = min_tab_w;
    if (w > max_tab_w)
      w = max_tab_w;
    desired_w[i] = w;
    total_desired += w + spacing;
  }
  if (total_desired > avail && total_desired > 0) {
    /* Uniformly shrink each tab. */
    for (int i = 0; i < BENTO_MAX_TABS; i++) {
      if (!T.list[i].in_use)
        continue;
      desired_w[i] = (desired_w[i] * avail) / total_desired;
      if (desired_w[i] < 40)
        desired_w[i] = 40;
    }
  }

  int x = sidebar + 12;
  for (int i = 0; i < BENTO_MAX_TABS; i++) {
    guiTab *tab = &T.list[i];
    if (!tab->in_use)
      continue;

    int tw = desired_w[i];
    int ty = 6;
    int th = GUI_TAB_BAR_HEIGHT - 6;
    int active = (i == T.active);
    int preview = (i == T.preview);

    Color bg = active ? GUI_PANEL : GUI_PANEL_DARK;
    DrawRectangle(x, ty, tw, th, bg);
    if (active)
      DrawRectangle(x, GUI_TAB_BAR_HEIGHT - 2, tw, 2, GUI_ACCENT);

    const char *name = (active && E.filename) ? guiBaseName(E.filename)
                                              : guiBaseName(tab->filename);
    int dirty = active ? E.dirty : tab->dirty;
    char label[280];
    snprintf(label, sizeof(label), "%s%s", name, dirty ? " *" : "");

    /* Preview tabs render in the dim text color (mimics "italic"). */
    Color title_color = preview ? GUI_DIM_TEXT : GUI_TEXT;
    DrawTextEx(G.font, label, (Vector2){x + 12, ty + 7}, G.small_font_size,
               GUI_FONT_SPACING, title_color);

    /* Close button × on the right. */
    int cs = 14;
    int cx = x + tw - cs - 8;
    int cy = ty + (th - cs) / 2;
    Vector2 mouse = GetMousePosition();
    int hover = mouse.x >= cx && mouse.x < cx + cs && mouse.y >= cy &&
                mouse.y < cy + cs;
    if (hover)
      DrawRectangle(cx - 2, cy - 2, cs + 4, cs + 4, GUI_SELECTED);
    DrawLine(cx + 3, cy + 3, cx + cs - 3, cy + cs - 3,
             hover ? GUI_TEXT : GUI_DIM_TEXT);
    DrawLine(cx + cs - 3, cy + 3, cx + 3, cy + cs - 3,
             hover ? GUI_TEXT : GUI_DIM_TEXT);

    tab->rect_x = x;
    tab->rect_y = ty;
    tab->rect_w = tw;
    tab->rect_h = th;
    tab->close_x = cx;
    tab->close_y = cy;
    tab->close_size = cs;

    x += tw + spacing;
    if (x > sidebar + avail)
      break;
  }
}

static void guiDrawFindBar(void) {
  int sidebar = guiSidebarWidth();
  int width = GetScreenWidth() - sidebar;
  int y = GUI_TAB_BAR_HEIGHT;
  int query_x = sidebar + 58;
  int query_w = guiMaxInt(140, (GetScreenWidth() - sidebar - 390) / 2);
  int replace_x = query_x + query_w + 78;
  int replace_w = guiMaxInt(120, GetScreenWidth() - replace_x - 448);
  int button_x = GetScreenWidth() - 430;
  char summary[64];
  Vector2 cursor_size;

  if (G.find_dirty)
    guiRefreshFindResults();
  snprintf(summary, sizeof(summary), "%d%s%s", G.match_count,
           G.match_overflow ? "+" : "",
           G.find_project_active ? " files" : "");

  DrawRectangle(sidebar, y, width, GUI_FIND_BAR_HEIGHT, GUI_PANEL);
  DrawTextEx(G.font, "Find", (Vector2){sidebar + 14, y + 13},
             G.small_font_size,
             GUI_FONT_SPACING, GUI_DIM_TEXT);
  DrawRectangle(query_x, y + 8, query_w, 23, GUI_PANEL_DARK);
  DrawRectangleLines(query_x, y + 8, query_w, 23,
                     (!G.find_editor_focus && G.find_focus == 0) ? GUI_ACCENT
                                                                 : GUI_BORDER);
  DrawTextEx(G.font, G.query, (Vector2){query_x + 8, y + 13},
             G.small_font_size, GUI_FONT_SPACING, GUI_TEXT);

  DrawTextEx(G.font, "Replace", (Vector2){sidebar + 14, y + 45},
             G.small_font_size, GUI_FONT_SPACING, GUI_DIM_TEXT);
  DrawRectangle(replace_x, y + 40, replace_w, 23, GUI_PANEL_DARK);
  DrawRectangleLines(replace_x, y + 40, replace_w, 23,
                     (!G.find_editor_focus && G.find_focus == 1) ? GUI_ACCENT
                                                                 : GUI_BORDER);
  DrawTextEx(G.font, G.replacement, (Vector2){replace_x + 8, y + 45},
             G.small_font_size, GUI_FONT_SPACING, GUI_TEXT);

  DrawRectangle(button_x, y + 8, 34, 23, GUI_PANEL_DARK);
  DrawRectangle(button_x + 40, y + 8, 34, 23, GUI_PANEL_DARK);
  DrawRectangle(button_x + 82, y + 8, 56, 23,
                G.find_all_active ? GUI_SELECTED : GUI_PANEL_DARK);
  DrawRectangle(button_x + 144, y + 8, 58, 23,
                G.find_project_active ? GUI_SELECTED : GUI_PANEL_DARK);
  DrawRectangle(button_x + 208, y + 8, 48, 23,
                G.find_case_sensitive ? GUI_SELECTED : GUI_PANEL_DARK);
  DrawRectangle(button_x + 262, y + 8, 54, 23,
                G.find_whole_word ? GUI_SELECTED : GUI_PANEL_DARK);
  DrawRectangle(button_x + 322, y + 8, 52, 23, GUI_PANEL_DARK);
  DrawTextEx(G.font, "<", (Vector2){button_x + 13, y + 13},
             G.small_font_size, GUI_FONT_SPACING, GUI_TEXT);
  DrawTextEx(G.font, ">", (Vector2){button_x + 53, y + 13},
             G.small_font_size, GUI_FONT_SPACING, GUI_TEXT);
  DrawTextEx(G.font, "All", (Vector2){button_x + 98, y + 13},
             G.small_font_size, GUI_FONT_SPACING, GUI_TEXT);
  DrawTextEx(G.font, "Files", (Vector2){button_x + 157, y + 13},
             G.small_font_size, GUI_FONT_SPACING, GUI_TEXT);
  DrawTextEx(G.font, "Case", (Vector2){button_x + 216, y + 13},
             G.small_font_size, GUI_FONT_SPACING, GUI_TEXT);
  DrawTextEx(G.font, "Word", (Vector2){button_x + 270, y + 13},
             G.small_font_size, GUI_FONT_SPACING, GUI_TEXT);
  DrawTextEx(G.font, "Regex", (Vector2){button_x + 330, y + 13},
             G.small_font_size, GUI_FONT_SPACING, GUI_DIM_TEXT);

  DrawRectangle(button_x, y + 40, 78, 23, GUI_PANEL_DARK);
  DrawRectangle(button_x + 86, y + 40, 72, 23, GUI_PANEL_DARK);
  DrawTextEx(G.font, "Replace", (Vector2){button_x + 8, y + 45},
             G.small_font_size, GUI_FONT_SPACING, GUI_TEXT);
  DrawTextEx(G.font, "All", (Vector2){button_x + 109, y + 45},
             G.small_font_size, GUI_FONT_SPACING, GUI_TEXT);
  DrawTextEx(G.font, summary, (Vector2){button_x + 166, y + 45},
             G.small_font_size, GUI_FONT_SPACING, GUI_DIM_TEXT);

  if (!G.find_editor_focus && ((int)(GetTime() * 2.0)) % 2 == 0) {
    const char *text = G.find_focus == 0 ? G.query : G.replacement;
    int x = G.find_focus == 0 ? query_x : replace_x;
    int row_y = G.find_focus == 0 ? y + 13 : y + 45;
    cursor_size =
        MeasureTextEx(G.font, G.query, G.small_font_size, GUI_FONT_SPACING);
    if (G.find_focus == 1)
      cursor_size = MeasureTextEx(G.font, text, G.small_font_size,
                                  GUI_FONT_SPACING);
    DrawRectangle(x + 8 + (int)cursor_size.x + 1, row_y, 2, 14,
                  GUI_ACCENT);
  }
  DrawLine(sidebar, y + GUI_FIND_BAR_HEIGHT - 1, sidebar + width,
           y + GUI_FIND_BAR_HEIGHT - 1, GUI_BORDER);
}

static void guiDrawFindResults(void) {
  int sidebar = guiSidebarWidth();
  int y = GUI_TAB_BAR_HEIGHT + GUI_FIND_BAR_HEIGHT;
  int width = GetScreenWidth() - sidebar;
  int stored_count = guiMinInt(G.match_count, GUI_MAX_FIND_RESULTS);
  int max_rows = (GUI_FIND_RESULTS_HEIGHT - 12) / GUI_TREE_ROW_HEIGHT;

  if (!G.find_active || !G.find_all_active)
    return;

  DrawRectangle(sidebar, y, width, GUI_FIND_RESULTS_HEIGHT, GUI_PANEL_DARK);
  for (int i = 0; i < max_rows; i++) {
    int match_index = G.match_scroll + i;
    guiMatch *match;
    char line[512];
    int row_y = y + 7 + i * GUI_TREE_ROW_HEIGHT;

    if (match_index >= stored_count)
      break;

    match = &G.matches[match_index];
    if (G.find_project_active && match->file_index >= 0 &&
        match->file_index < G.file_count) {
      snprintf(line, sizeof(line), "%s:%d:%d  %s",
               guiRelativePath(G.files[match->file_index].path),
               match->row + 1, match->col + 1, match->preview);
    } else {
      snprintf(line, sizeof(line), "%d:%d  %s", match->row + 1,
               match->col + 1, match->preview);
    }

    if (match_index == G.current_match)
      DrawRectangle(sidebar + 8, row_y - 1, width - 16, GUI_TREE_ROW_HEIGHT,
                    GUI_SELECTED);
    DrawTextEx(G.font, line, (Vector2){sidebar + 14, row_y + 3},
               G.small_font_size, GUI_FONT_SPACING,
               match_index == G.current_match ? GUI_TEXT : GUI_DIM_TEXT);
  }
  DrawLine(sidebar, y + GUI_FIND_RESULTS_HEIGHT - 1, sidebar + width,
           y + GUI_FIND_RESULTS_HEIGHT - 1, GUI_BORDER);
}

static void guiDrawSearchMatches(erow *row, float row_y, int text_left) {
  const char *scan;
  int visible_start = E.coloff;
  int visible_end = E.coloff + E.screencols;

  if (!G.find_active || G.query_len == 0)
    return;

  scan = row->render;
  while ((scan = guiFindQueryInRange(row->render, row->rsize,
                                     (int)(scan - row->render))) != NULL) {
    int match_start = (int)(scan - row->render);
    int match_end = match_start + G.query_len;
    int draw_start = guiMaxInt(match_start, visible_start);
    int draw_end = guiMinInt(match_end, visible_end);

    if (draw_start < draw_end) {
      DrawRectangle(text_left + (int)((draw_start - visible_start) *
                                      G.char_width),
                    (int)row_y + 2,
                    (int)((draw_end - draw_start) * G.char_width),
                    (int)G.line_height - 3, GUI_MATCH_BG);
    }
    scan += G.query_len;
  }
}

static void guiDrawSelectionForRow(int filerow, float row_y, int text_left) {
  int selection_start, selection_end;
  int visible_start = E.coloff;
  int visible_end = E.coloff + E.screencols;
  int draw_start, draw_end;

  if (!guiSelectionRangeForRow(filerow, &selection_start, &selection_end))
    return;

  draw_start = guiMaxInt(selection_start, visible_start);
  draw_end = guiMinInt(selection_end, visible_end);
  if (draw_start >= draw_end)
    return;

  DrawRectangle(text_left + (int)((draw_start - visible_start) * G.char_width),
                (int)row_y + 2,
                (int)((draw_end - draw_start) * G.char_width),
                (int)G.line_height - 3, GUI_TEXT_SELECTION);
}

static void guiDrawWhitespaceForRow(erow *row, float row_y, int text_left) {
  int visible_start = E.coloff;
  int visible_end = E.coloff + E.screencols;
  Color color = GUI_GUTTER_TEXT;

  if (!G.show_whitespace || !row || !row->render)
    return;
  for (int col = visible_start; col < visible_end && col < row->rsize; col++) {
    if (row->render[col] == ' ') {
      int x = text_left + (int)((col - visible_start) * G.char_width +
                                G.char_width * 0.45f);
      int y = (int)(row_y + G.line_height * 0.55f);
      DrawRectangle(x, y, 2, 2, color);
    }
  }
}

/* ============================ Render Runs ===============================
 *
 * Per-frame batched render data for the visible editor window. The render
 * loop is split in two passes:
 *
 *   1. Layout pass — walks visible rows, draws backgrounds (current-line
 *      highlight, gutter numbers, selection, search-match), and emits one
 *      RenderRun per same-hl glyph span into the frame-local g_runs array.
 *      Text bytes are copied into g_run_text (a frame-local arena) so the
 *      dispatch pass doesn't need to revisit row->render.
 *
 *   2. Dispatch pass — single linear walk over g_runs, one DrawTextEx per
 *      run. No per-run row lookups, no per-run hl walking.
 *
 * Separating layout from dispatch makes it cheap to reorder draws for overdraw
 * avoidance and instrument the run count for profiling.
 *
 * Capacities are static and sized for the practical worst case (every
 * cell a different hl on a full screen): screencols × screenrows runs,
 * which on a 200×100 layout is 20k runs and ~20k text bytes. The static
 * caps are conservative.
 */

typedef struct guiRenderRun {
  float x, y;         /* top-left pixel position */
  int run_len;        /* glyph count */
  int hl;             /* syntax-highlight class */
  int text_off;       /* offset into g_run_text */
  unsigned char nonprint;  /* draw HL_NONPRINT rectangle overlay */
  unsigned char slow;      /* use DrawTextEx; text contains non-direct glyphs */
} guiRenderRun;

#define GUI_RUNS_CAP        (1 << 15)   /* 32k runs */
#define GUI_RUN_TEXT_CAP    (1 << 16)   /* 64 KiB of glyph bytes */

static guiRenderRun g_runs[GUI_RUNS_CAP];
static int g_run_count = 0;
static char g_run_text[GUI_RUN_TEXT_CAP];
static int g_run_text_used = 0;

static inline void guiRunsReset(void) {
  g_run_count = 0;
  g_run_text_used = 0;
}

/* Append `len` bytes to the run-text arena and NUL-terminate. Returns the
 * offset, or -1 if the arena is full. */
static int guiRunTextAppend(const char *src, int len) {
  if (g_run_text_used + len + 1 > GUI_RUN_TEXT_CAP)
    return -1;
  int off = g_run_text_used;
  memcpy(g_run_text + off, src, (size_t)len);
  g_run_text[off + len] = '\0';
  g_run_text_used += len + 1;
  return off;
}

/* Like guiRunTextAppend but fills with a repeated character. */
static int guiRunTextFill(char ch, int len) {
  if (g_run_text_used + len + 1 > GUI_RUN_TEXT_CAP)
    return -1;
  int off = g_run_text_used;
  memset(g_run_text + off, ch, (size_t)len);
  g_run_text[off + len] = '\0';
  g_run_text_used += len + 1;
  return off;
}

static unsigned long long guiHashBytes(const char *s, int len) {
  unsigned long long h = 1469598103934665603ULL;
  if (!s || len <= 0)
    return h;
  for (int i = 0; i < len; i++) {
    h ^= (unsigned char)s[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static int guiDirectGlyph(unsigned char c, int hl) {
  return c >= GUI_GLYPH_CACHE_FIRST && c <= GUI_GLYPH_CACHE_LAST &&
         hl != HL_NONPRINT;
}

/* Walk the visible portion of one row and emit RenderRuns for same-highlight
 * spans. Tabs are already expanded inside row->render. */
static void guiEmitRowRuns(int filerow, float row_y, int text_left) {
  if (filerow < 0 || filerow >= tbLineCount(TB))
    return;

  int rsize = 0;
  const char *render = tbRenderSlice(TB, filerow, &rsize);
  const unsigned char *row_hl = tbHighlight(TB, filerow);
  int len = rsize - E.coloff;
  if (len <= 0 || !render)
    return;
  if (len > E.screencols)
    len = E.screencols;

  int j = 0;
  while (j < len) {
    int render_col = E.coloff + j;
    int hl = row_hl ? row_hl[render_col] : HL_NORMAL;
    int direct = guiDirectGlyph((unsigned char)render[render_col], hl);
    int k = j + 1;
    while (k < len) {
      int hl2 = row_hl ? row_hl[E.coloff + k] : HL_NORMAL;
      int direct2 = guiDirectGlyph((unsigned char)render[E.coloff + k], hl2);
      if (hl2 != hl || direct2 != direct)
        break;
      k++;
    }
    int run_len = k - j;
    if (g_run_count < GUI_RUNS_CAP) {
      guiRenderRun *r = &g_runs[g_run_count];
      r->x = text_left + j * G.char_width;
      r->y = row_y + 2;
      r->run_len = run_len;
      r->hl = hl;
      r->nonprint = (hl == HL_NONPRINT) ? 1 : 0;
      r->slow = (!direct && !r->nonprint) ? 1 : 0;
      r->text_off = r->nonprint
                        ? guiRunTextFill('?', run_len)
                        : guiRunTextAppend(render + render_col, run_len);
      if (r->text_off >= 0)
        g_run_count++;
    }
    j = k;
  }
}

static void guiDispatchRuns(int prefer_batch) {
  /* Non-print background rectangles first so text draws on top. */
  for (int i = 0; i < g_run_count; i++) {
    const guiRenderRun *r = &g_runs[i];
    if (r->nonprint) {
      for (int m = 0; m < r->run_len; m++) {
        DrawRectangle((int)(r->x + m * G.char_width), (int)r->y,
                      (int)G.char_width, (int)G.line_height - 3, GUI_BORDER);
      }
    }
  }

  int batched = prefer_batch ? guiFastTextBegin() : 0;
  for (int i = 0; i < g_run_count; i++) {
    const guiRenderRun *r = &g_runs[i];
    if (batched && r->slow) {
      guiFastTextEnd();
      batched = 0;
    }

    if (batched) {
      guiFastTextDraw(g_run_text + r->text_off, r->run_len, r->x, r->y,
                      G.font_size, guiSyntaxColor(r->hl));
    } else {
      DrawTextEx(G.font, g_run_text + r->text_off,
                 (Vector2){r->x, r->y}, G.font_size, GUI_FONT_SPACING,
                 guiSyntaxColor(r->hl));
      if (prefer_batch && !r->slow && i + 1 < g_run_count)
        batched = guiFastTextBegin();
    }
    G.dbg_draw_calls++;
    G.dbg_glyph_count += r->run_len;
  }
  if (batched)
    guiFastTextEnd();
  G.dbg_run_count += g_run_count;
}

static void guiDrawEditorRows(void) {
  int width = GetScreenWidth();
  int height = GetScreenHeight();
  int sidebar = guiSidebarWidth();
  int editor_top = guiEditorTop();
  int editor_bottom = height - GUI_STATUS_BAR_HEIGHT;
  int gutter = guiGutterWidth();
  int text_left = sidebar + gutter + GUI_TEXT_PADDING_X;
  int text_top = editor_top + GUI_TEXT_PADDING_Y;
  float scroll_y = G.editor_scroll_y;
  int rowcount = tbLineCount(TB);

  /* Bring syntax up-to-date for every row we're about to draw. The +2 is
   * slack for partial rows at the top/bottom of the viewport. */
  {
    struct timespec _s0, _s1;
    clock_gettime(CLOCK_MONOTONIC, &_s0);
    syntaxRefreshUpTo(E.rowoff + E.screenrows + 2);
    clock_gettime(CLOCK_MONOTONIC, &_s1);
    G.dbg_syntax_us += (_s1.tv_sec - _s0.tv_sec) * 1.0e6 +
                       (_s1.tv_nsec - _s0.tv_nsec) / 1000.0;
  }

  DrawRectangle(sidebar, editor_top, width - sidebar, editor_bottom - editor_top,
                GUI_BG);
  DrawRectangle(sidebar, editor_top, gutter, editor_bottom - editor_top,
                GUI_PANEL);
  DrawLine(sidebar + gutter, editor_top, sidebar + gutter, editor_bottom,
           GUI_BORDER);

  /* Run-list cache: if the viewport AND content haven't changed since
   * last frame, skip the emit pass entirely and re-dispatch the existing
   * g_runs. */
  int scroll_y_q = (int)(scroll_y * 4.0f);
  int runs_cache_hit = G.runs_cache_valid &&
                       G.runs_cache_gen == g_render_gen &&
                       G.runs_cache_rowoff == E.rowoff &&
                       G.runs_cache_coloff == E.coloff &&
                       G.runs_cache_screencols == E.screencols &&
                       G.runs_cache_screenrows == E.screenrows &&
                       G.runs_cache_scroll_y_q == scroll_y_q &&
                       G.runs_cache_rowcount == rowcount;

  /* ---- Pass 1: gutter + per-row backgrounds + build text runs. ---- */
  if (!runs_cache_hit)
    guiRunsReset();
  struct timespec _gutter_t0, _gutter_t1;
  clock_gettime(CLOCK_MONOTONIC, &_gutter_t0);
  /* One scissor covering the whole editor (gutter + text). Used to be
   * two separate scissors but each Begin/End pair forces a raylib batch
   * flush + GL state change; on macOS GL-via-Metal that was ~700 µs of
   * the editor pass cost. The emit pass already trims runs to E.screencols
   * so we don't need the tighter text-only scissor. */
  BeginScissorMode(sidebar, editor_top, width - sidebar,
                   editor_bottom - editor_top);
  /* Pass 1a: rectangles only (current-line highlights). Doing all of
   * these before any text avoids per-row texture-state thrash in raylib's
   * batcher. */
  for (int y = 0; y <= E.screenrows; y++) {
    float row_y = text_top + y * G.line_height - scroll_y;
    if (row_y + G.line_height <= editor_top || row_y >= editor_bottom)
      continue;
    if (y == E.cy)
      DrawRectangle(sidebar + gutter + 1, (int)row_y,
                    width - sidebar - gutter - 1,
                    (int)G.line_height, GUI_CURRENT_LINE);
  }
  /* Pass 1b: gutter line numbers, all in a single open text batch. */
  int gutter_batched = guiFastTextBegin();
  for (int y = 0; y <= E.screenrows; y++) {
    int filerow = E.rowoff + y;
    float row_y = text_top + y * G.line_height - scroll_y;

    if (row_y + G.line_height <= editor_top || row_y >= editor_bottom)
      continue;

    if (filerow < rowcount) {
      char number[32];
      Color number_color = y == E.cy ? GUI_DIM_TEXT : GUI_GUTTER_TEXT;
      int n = snprintf(number, sizeof(number), "%d", filerow + 1);
      float number_w = n * G.small_char_width;
      float number_x = sidebar + gutter - GUI_TEXT_PADDING_X - number_w;
      if (gutter_batched) {
        guiFastTextDraw(number, n, number_x, row_y + 3,
                        G.small_font_size, number_color);
      } else {
        DrawTextEx(G.font, number, (Vector2){number_x, row_y + 3},
                   G.small_font_size, GUI_FONT_SPACING, number_color);
      }
    } else {
      float tilde_x = sidebar + gutter - GUI_TEXT_PADDING_X - 10;
      if (gutter_batched) {
        guiFastTextDraw("~", 1, tilde_x, row_y + 3,
                        G.small_font_size, GUI_GUTTER_TEXT);
      } else {
        DrawTextEx(G.font, "~", (Vector2){tilde_x, row_y + 3},
                   G.small_font_size, GUI_FONT_SPACING, GUI_GUTTER_TEXT);
      }
    }
  }
  if (gutter_batched) guiFastTextEnd();
  /* Keep scissor open through the editor text pass below. */
  clock_gettime(CLOCK_MONOTONIC, &_gutter_t1);
  G.dbg_gutter_us = (_gutter_t1.tv_sec - _gutter_t0.tv_sec) * 1.0e6 +
                    (_gutter_t1.tv_nsec - _gutter_t0.tv_nsec) / 1000.0;

  /* We're already inside the outer editor scissor; no second Begin needed. */

  /* Pass 1 inner loop: draw per-row backgrounds and emit text runs. */
  struct timespec _emit_t0, _emit_t1;
  clock_gettime(CLOCK_MONOTONIC, &_emit_t0);
  for (int y = 0; y <= E.screenrows; y++) {
    int filerow = E.rowoff + y;
    float row_y = text_top + y * G.line_height - scroll_y;

    if (row_y + G.line_height <= text_top || row_y >= editor_bottom)
      continue;
    if (filerow >= rowcount)
      continue;

    guiDrawSearchMatches(tbRowMut(TB, filerow), row_y, text_left);
    guiDrawSelectionForRow(filerow, row_y, text_left);
    guiDrawWhitespaceForRow(tbRowMut(TB, filerow), row_y, text_left);
    if (!runs_cache_hit)
      guiEmitRowRuns(filerow, row_y, text_left);
  }
  clock_gettime(CLOCK_MONOTONIC, &_emit_t1);
  G.dbg_emit_us += (_emit_t1.tv_sec - _emit_t0.tv_sec) * 1.0e6 +
                   (_emit_t1.tv_nsec - _emit_t0.tv_nsec) / 1000.0;

  /* Capture cache key after a miss-rebuild so the next frame can match. */
  if (!runs_cache_hit) {
    G.runs_cache_gen = g_render_gen;
    G.runs_cache_rowoff = E.rowoff;
    G.runs_cache_coloff = E.coloff;
    G.runs_cache_screencols = E.screencols;
    G.runs_cache_screenrows = E.screenrows;
    G.runs_cache_scroll_y_q = scroll_y_q;
    G.runs_cache_rowcount = rowcount;
    G.runs_cache_valid = 1;
  }

  /* ---- Pass 2: dispatch text runs. ---- */
  struct timespec _disp_t0, _disp_t1;
  clock_gettime(CLOCK_MONOTONIC, &_disp_t0);
  guiDispatchRuns(1);
  clock_gettime(CLOCK_MONOTONIC, &_disp_t1);
  G.dbg_dispatch_us += (_disp_t1.tv_sec - _disp_t0.tv_sec) * 1.0e6 +
                       (_disp_t1.tv_nsec - _disp_t0.tv_nsec) / 1000.0;
  EndScissorMode();
}

static int guiCursorRenderX(void) {
  int filerow = E.rowoff + E.cy;
  int line_size = 0;
  const char *line_chars = tbLineSlice(TB, filerow, &line_size);
  int rx = 0;

  if (!line_chars)
    return E.cx;

  for (int j = E.coloff; j < E.coloff + E.cx; j++) {
    if (j < line_size && line_chars[j] == TAB)
      rx += g_tab_stop - (rx % g_tab_stop);
    else
      rx++;
  }
  return rx;
}

static int bentoCursorVisibleNow(void) {
  double now = GetTime();
  double idle = now - G.last_cursor_move_time;
  double overall_idle = now - G.last_activity_time;
  if (overall_idle > 3.0)
    return 1; /* solid in deep idle so the frozen frame looks right */
  if (idle < 0.5)
    return 1; /* solid while moving */
  int phase_ms = (int)((idle - 0.5) * 1000.0) % 730;
  return phase_ms < 130;
}

static void guiDrawCursor(void) {
  int sidebar = guiSidebarWidth();
  int gutter = guiGutterWidth();
  int text_left = sidebar + gutter + GUI_TEXT_PADDING_X;
  int text_top = guiEditorTop() + GUI_TEXT_PADDING_Y;
  int editor_bottom = GetScreenHeight() - GUI_STATUS_BAR_HEIGHT;
  int rx = guiCursorRenderX();
  int x = text_left + (int)(rx * G.char_width);
  int y = text_top + (int)(E.cy * G.line_height - G.editor_scroll_y);

  if (T.active < 0)
    return;
  if (y + G.line_height <= text_top || y >= editor_bottom)
    return;

  if (bentoCursorVisibleNow())
    DrawRectangle(x, y + 3, 2, (int)G.line_height - 5, GUI_ACCENT);
}

static void guiDrawFunctionHint(void) {
  const char *hint = guiCachedFunctionHint();
  int sidebar = guiSidebarWidth();
  int gutter = guiGutterWidth();
  int text_left = sidebar + gutter + GUI_TEXT_PADDING_X;
  int text_top = guiEditorTop() + GUI_TEXT_PADDING_Y;
  int rx = guiCursorRenderX();
  int x = text_left + (int)(rx * G.char_width);
  int y = text_top + (int)((E.cy + 1) * G.line_height - G.editor_scroll_y) + 2;
  Vector2 size;
  int w, h;

  if (!hint || G.overlay != GUI_OVERLAY_NONE ||
      (G.find_active && !G.find_editor_focus))
    return;

  size = MeasureTextEx(G.font, hint, G.small_font_size, GUI_FONT_SPACING);
  w = (int)size.x + 18;
  h = (int)size.y + 10;
  if (x + w > GetScreenWidth() - 8)
    x = GetScreenWidth() - w - 8;
  if (y + h > GetScreenHeight() - GUI_STATUS_BAR_HEIGHT - 8)
    y = text_top + (int)((E.cy - 1) * G.line_height) - h;
  if (x < sidebar + gutter + 8)
    x = sidebar + gutter + 8;
  if (y < guiEditorTop() + 4)
    y = guiEditorTop() + 4;

  DrawRectangle(x, y, w, h, GUI_OVERLAY_BG);
  DrawRectangleLines(x, y, w, h, GUI_BORDER);
  DrawTextEx(G.font, hint, (Vector2){x + 9, y + 5}, G.small_font_size,
             GUI_FONT_SPACING, GUI_TEXT);
}

static void guiDrawSidebar(void) {
  int sidebar = guiSidebarWidth();
  int height = GetScreenHeight() - GUI_STATUS_BAR_HEIGHT;
  int list_y = GUI_TAB_BAR_HEIGHT + 8;
  int visible_rows;

  if (sidebar <= 0)
    return;

  visible_rows = guiMaxInt(0, (height - list_y - 6) / GUI_TREE_ROW_HEIGHT);
  DrawRectangle(0, 0, sidebar, height, GUI_PANEL_DARK);
  DrawTextEx(G.font, guiBaseName(G.cwd), (Vector2){12, 13}, G.small_font_size,
             GUI_FONT_SPACING, GUI_TEXT);
  DrawLine(sidebar - 1, 0, sidebar - 1, height, GUI_BORDER);
  DrawLine(0, GUI_TAB_BAR_HEIGHT - 1, sidebar, GUI_TAB_BAR_HEIGHT - 1,
           GUI_BORDER);

  BeginScissorMode(0, list_y, sidebar - 1, height - list_y);
  for (int i = 0; i < visible_rows; i++) {
    int index = G.tree_scroll + i;
    guiFileEntry *entry;
    int y = list_y + i * GUI_TREE_ROW_HEIGHT;
    int x;
    char label[320];
    Color color;

    if (index >= G.file_count)
      break;
    entry = &G.files[index];
    x = 12 + entry->depth * 14;
    color = entry->is_dir ? GUI_DIM_TEXT : GUI_TEXT;
    snprintf(label, sizeof(label), "%s %s", entry->is_dir ? ">" : " ",
             entry->name);

    if (guiFileEntryIsCurrent(entry))
      DrawRectangle(6, y - 1, sidebar - 12, GUI_TREE_ROW_HEIGHT, GUI_SELECTED);
    DrawTextEx(G.font, label, (Vector2){x, y + 4}, G.small_font_size,
               GUI_FONT_SPACING, color);
  }
  EndScissorMode();
}

static void guiDrawOverlay(void) {
  int sidebar = guiSidebarWidth();
  int screen_w = GetScreenWidth();
  int box_w = guiMinInt(720, screen_w - sidebar - 80);
  int box_x;
  int box_y = 88;
  int list_overlay = G.overlay == GUI_OVERLAY_QUICK_OPEN ||
                     G.overlay == GUI_OVERLAY_COMMAND_PALETTE ||
                     G.overlay == GUI_OVERLAY_RECENT;
  int box_h = list_overlay ? 272 :
              G.overlay == GUI_OVERLAY_SETTINGS ? 268 :
              G.overlay == GUI_OVERLAY_PROMPT ? 122 : 104;
  const char *title = "Go To Line";
  int count = 0;

  if (G.overlay == GUI_OVERLAY_NONE)
    return;
  if (G.overlay == GUI_OVERLAY_QUICK_OPEN)
    title = "Quick Open";
  else if (G.overlay == GUI_OVERLAY_COMMAND_PALETTE)
    title = "Command Palette";
  else if (G.overlay == GUI_OVERLAY_RECENT)
    title = "Open Recent";
  else if (G.overlay == GUI_OVERLAY_SETTINGS)
    title = "Settings";
  else if (G.overlay == GUI_OVERLAY_PROMPT)
    title = G.prompt_title;

  if (box_w < 320)
    box_w = guiMaxInt(220, screen_w - sidebar - 24);
  box_x = sidebar + (screen_w - sidebar - box_w) / 2;

  DrawRectangle(0, 0, screen_w, GetScreenHeight(), (Color){0, 0, 0, 82});
  DrawRectangle(box_x, box_y, box_w, box_h, GUI_OVERLAY_BG);
  DrawRectangleLines(box_x, box_y, box_w, box_h, GUI_BORDER);
  DrawTextEx(G.font, title, (Vector2){box_x + 16, box_y + 14},
             G.small_font_size, GUI_FONT_SPACING, GUI_DIM_TEXT);

  if (G.overlay == GUI_OVERLAY_SETTINGS) {
    char value[GUI_SETTINGS_COUNT][64];
    const char *label[GUI_SETTINGS_COUNT] = {
        "Theme", "Font Size", "Whitespace", "Tab Width", "Restore Session",
        "Sidebar"};

    snprintf(value[0], sizeof(value[0]), "%s", GUI_THEME_NAMES[G.theme_index]);
    snprintf(value[1], sizeof(value[1]), "%.0f", G.font_size);
    snprintf(value[2], sizeof(value[2]), "%s", G.show_whitespace ? "On" : "Off");
    snprintf(value[3], sizeof(value[3]), "%d", G.tab_width);
    snprintf(value[4], sizeof(value[4]), "%s", G.restore_session ? "On" : "Off");
    snprintf(value[5], sizeof(value[5]), "%s", G.sidebar_width > 0 ? "On" : "Off");

    for (int i = 0; i < GUI_SETTINGS_COUNT; i++) {
      int row_y = box_y + 52 + i * 30;
      Vector2 value_size =
          MeasureTextEx(G.font, value[i], G.small_font_size, GUI_FONT_SPACING);
      if (i == G.settings_selected)
        DrawRectangle(box_x + 12, row_y - 4, box_w - 24, 26, GUI_SELECTED);
      DrawTextEx(G.font, label[i], (Vector2){box_x + 20, row_y},
                 G.small_font_size, GUI_FONT_SPACING, GUI_TEXT);
      DrawTextEx(G.font, value[i],
                 (Vector2){box_x + box_w - 20 - value_size.x, row_y},
                 G.small_font_size, GUI_FONT_SPACING, GUI_DIM_TEXT);
    }
    return;
  }

  if (G.overlay == GUI_OVERLAY_PROMPT) {
    Vector2 cursor_size;
    DrawTextEx(G.font, guiRelativePath(G.prompt_base_path),
               (Vector2){box_x + 16, box_y + 38}, G.small_font_size,
               GUI_FONT_SPACING, GUI_DIM_TEXT);
    DrawRectangle(box_x + 16, box_y + 66, box_w - 32, 28, GUI_PANEL_DARK);
    DrawRectangleLines(box_x + 16, box_y + 66, box_w - 32, 28, GUI_ACCENT);
    DrawTextEx(G.font, G.prompt_value, (Vector2){box_x + 26, box_y + 73},
               G.small_font_size, GUI_FONT_SPACING, GUI_TEXT);
    if (((int)(GetTime() * 2.0)) % 2 == 0) {
      cursor_size = MeasureTextEx(G.font, G.prompt_value, G.small_font_size,
                                  GUI_FONT_SPACING);
      DrawRectangle(box_x + 26 + (int)cursor_size.x + 1, box_y + 73, 2, 15,
                    GUI_ACCENT);
    }
    return;
  }

  DrawRectangle(box_x + 16, box_y + 40, box_w - 32, 28, GUI_PANEL_DARK);
  DrawRectangleLines(box_x + 16, box_y + 40, box_w - 32, 28, GUI_ACCENT);
  DrawTextEx(G.font, G.overlay_query, (Vector2){box_x + 26, box_y + 47},
             G.small_font_size, GUI_FONT_SPACING, GUI_TEXT);

  if (((int)(GetTime() * 2.0)) % 2 == 0) {
    Vector2 size = MeasureTextEx(G.font, G.overlay_query, G.small_font_size,
                                 GUI_FONT_SPACING);
    DrawRectangle(box_x + 26 + (int)size.x + 1, box_y + 47, 2, 15,
                  GUI_ACCENT);
  }

  if (!list_overlay)
    return;

  if (G.overlay == GUI_OVERLAY_QUICK_OPEN)
    count = guiFilteredFileCount();
  else if (G.overlay == GUI_OVERLAY_COMMAND_PALETTE)
    count = guiFilteredCommandCount();
  else
    count = guiFilteredRecentCount();

  for (int i = 0; i < 8; i++) {
    int filtered_index = G.overlay_scroll + i;
    char label[PATH_MAX + 32];
    char detail[128];
    int row_y = box_y + 72 + i * GUI_TREE_ROW_HEIGHT;
    Vector2 detail_size;

    if (filtered_index >= count)
      break;

    detail[0] = '\0';
    if (G.overlay == GUI_OVERLAY_QUICK_OPEN) {
      int file_index = guiFilteredFileIndex(filtered_index);
      if (file_index < 0)
        break;
      snprintf(label, sizeof(label), "%s",
               guiRelativePath(G.files[file_index].path));
    } else if (G.overlay == GUI_OVERLAY_COMMAND_PALETTE) {
      int def_index = guiFilteredCommandIndex(filtered_index);
      if (def_index < 0)
        break;
      snprintf(label, sizeof(label), "%s", GUI_COMMAND_DEFS[def_index].name);
    } else {
      char path[PATH_MAX];
      int is_project = 0;
      if (!guiFilteredRecent(filtered_index, path, sizeof(path), &is_project))
        break;
      snprintf(label, sizeof(label), "%s", guiRelativePath(path));
      snprintf(detail, sizeof(detail), "%s", is_project ? "Folder" : "File");
    }

    if (filtered_index == G.overlay_selected)
      DrawRectangle(box_x + 12, row_y, box_w - 24, GUI_TREE_ROW_HEIGHT,
                    GUI_SELECTED);
    DrawTextEx(G.font, label, (Vector2){box_x + 20, row_y + 4},
               G.small_font_size, GUI_FONT_SPACING, GUI_TEXT);
    if (detail[0]) {
      detail_size =
          MeasureTextEx(G.font, detail, G.small_font_size, GUI_FONT_SPACING);
      DrawTextEx(G.font, detail,
                 (Vector2){box_x + box_w - 20 - detail_size.x, row_y + 4},
                 G.small_font_size, GUI_FONT_SPACING, GUI_DIM_TEXT);
    }
  }
}

static void guiDrawWelcome(void) {
  int sidebar = guiSidebarWidth();
  int x = sidebar + guiMaxInt(32, (GetScreenWidth() - sidebar - 760) / 2);
  int y = 94;
  int button_w = 112;
  int button_h = 48;
  const char *buttons[] = {"New File", "New Window", "Open File",
                           "Open Folder", "Recent", "Settings"};
  const char *shortcuts[] = {"Cmd+N", "Cmd+Shift+N", "Cmd+O",
                             "Cmd+Shift+O", "Cmd+Shift+R", "Cmd+,"};

  if (!G.welcome_active || T.count > 0)
    return;

  DrawTextEx(G.font, "Bento", (Vector2){x, y}, G.font_size + 10.0f,
             GUI_FONT_SPACING, GUI_TEXT);

  y += 84;
  for (int i = 0; i < 6; i++) {
    int bx = x + i * (button_w + 8);
    DrawRectangle(bx, y, button_w, button_h,
                  i == 0 ? GUI_ACCENT : GUI_PANEL_DARK);
    DrawRectangleLines(bx, y, button_w, button_h, GUI_BORDER);
    DrawTextEx(G.font, buttons[i], (Vector2){bx + 12, y + 7},
               G.small_font_size, GUI_FONT_SPACING,
               i == 0 ? GUI_BG : GUI_TEXT);
    DrawTextEx(G.font, shortcuts[i], (Vector2){bx + 12, y + 27},
               G.small_font_size - 2.0f, GUI_FONT_SPACING,
               i == 0 ? GUI_BG : GUI_DIM_TEXT);
  }

  y += 86;
  DrawTextEx(G.font, "Recent Files", (Vector2){x, y - 24},
             G.small_font_size, GUI_FONT_SPACING, GUI_DIM_TEXT);
  for (int i = 0; i < G.recent_file_count && i < 6; i++) {
    int row_y = y + i * GUI_TREE_ROW_HEIGHT;
    DrawRectangle(x - 6, row_y - 1, 602, GUI_TREE_ROW_HEIGHT, GUI_PANEL_DARK);
    DrawTextEx(G.font, guiRelativePath(G.recent_files[i]),
               (Vector2){x + 4, row_y + 4}, G.small_font_size,
               GUI_FONT_SPACING, GUI_TEXT);
  }
}

static void guiDrawContextMenu(void) {
  int w = 180;
  int h = G.context_item_count * 24 + 8;
  int x = G.context_x;
  int y = G.context_y;
  Vector2 mouse = GetMousePosition();

  if (G.context_menu == GUI_CONTEXT_NONE)
    return;
  if (x + w > GetScreenWidth())
    x = GetScreenWidth() - w - 4;
  if (y + h > GetScreenHeight())
    y = GetScreenHeight() - h - 4;

  DrawRectangle(x, y, w, h, GUI_OVERLAY_BG);
  DrawRectangleLines(x, y, w, h, GUI_BORDER);
  for (int i = 0; i < G.context_item_count; i++) {
    int row_y = y + 4 + i * 24;
    int hover = mouse.x >= x && mouse.x <= x + w &&
                mouse.y >= row_y && mouse.y <= row_y + 24;
    Color color = G.context_items[i].enabled ? GUI_TEXT : GUI_GUTTER_TEXT;
    if (hover && G.context_items[i].enabled)
      DrawRectangle(x + 4, row_y, w - 8, 24, GUI_SELECTED);
    DrawTextEx(G.font, G.context_items[i].label,
               (Vector2){x + 12, row_y + 5}, G.small_font_size,
               GUI_FONT_SPACING, color);
  }
}

static void guiDrawStatusBar(void) {
  int width = GetScreenWidth();
  int height = GetScreenHeight();
  int y = height - GUI_STATUS_BAR_HEIGHT;
  int filerow = E.rowoff + E.cy;
  int filecol = E.coloff + E.cx;
  const char *hint = guiCachedFunctionHint();
  char left[256];
  char right[128];
  Vector2 right_size;
  int left_clip_width;

  if (hint && G.overlay == GUI_OVERLAY_NONE &&
      (!G.find_active || G.find_editor_focus))
    snprintf(left, sizeof(left), "Hint: %s", hint);
  else if (E.statusmsg[0] && time(NULL) - E.statusmsg_time < 5)
    snprintf(left, sizeof(left), "%s", E.statusmsg);
  else
    snprintf(left, sizeof(left), "%s", E.syntax ? E.syntax->name : "Plain Text");

  /* Cache the right-side "Ln X, Col Y | N lines" text + its measured
   * width. Skip the snprintf + MeasureTextEx when (filerow, filecol,
   * numrows) are unchanged — which is the common case during idle and
   * when typing inside a single line without moving cursor row. */
  int rowcount = tbLineCount(TB);
  if (G.statusbar_cached_row == filerow &&
      G.statusbar_cached_col == filecol &&
      G.statusbar_cached_numrows == rowcount &&
      G.statusbar_cached_text[0] != '\0') {
    snprintf(right, sizeof(right), "%s", G.statusbar_cached_text);
    right_size = (Vector2){G.statusbar_cached_width, G.small_font_size};
  } else {
    snprintf(right, sizeof(right), "Ln %d, Col %d | %d lines",
             guiMaxInt(1, filerow + 1), guiMaxInt(1, filecol + 1),
             rowcount);
    right_size =
        MeasureTextEx(G.font, right, G.small_font_size, GUI_FONT_SPACING);
    G.statusbar_cached_row = filerow;
    G.statusbar_cached_col = filecol;
    G.statusbar_cached_numrows = rowcount;
    G.statusbar_cached_width = right_size.x;
    snprintf(G.statusbar_cached_text, sizeof(G.statusbar_cached_text),
             "%s", right);
  }

  DrawRectangle(0, y, width, GUI_STATUS_BAR_HEIGHT, GUI_PANEL_DARK);
  DrawLine(0, y, width, y, GUI_BORDER);
  left_clip_width = guiMaxInt(1, width - (int)right_size.x - 36);
  BeginScissorMode(12, y, left_clip_width, GUI_STATUS_BAR_HEIGHT);
  DrawTextEx(G.font, left, (Vector2){12, y + 5}, G.small_font_size,
             GUI_FONT_SPACING, GUI_DIM_TEXT);
  EndScissorMode();
  DrawTextEx(G.font, right,
             (Vector2){guiMaxInt(12, width - (int)right_size.x - 12), y + 5},
             G.small_font_size, GUI_FONT_SPACING, GUI_DIM_TEXT);
}

static void guiDrawDebugHud(void);

/* Microsecond elapsed between two clock_gettime readings. */
static inline double guiUsBetween(struct timespec a, struct timespec b) {
  return (b.tv_sec - a.tv_sec) * 1.0e6 +
         (b.tv_nsec - a.tv_nsec) / 1000.0;
}

#define GUI_TIME_BEGIN(slot) \
  struct timespec _gt_##slot##_0; \
  clock_gettime(CLOCK_MONOTONIC, &_gt_##slot##_0)
#define GUI_TIME_END(slot) do {                                                 \
    struct timespec _gt_t1;                                                    \
    clock_gettime(CLOCK_MONOTONIC, &_gt_t1);                                   \
    G.dbg_##slot##_us = guiUsBetween(_gt_##slot##_0, _gt_t1);                  \
  } while (0)

static void guiDrawFrame(void) {
  struct timespec frame_t0;
  clock_gettime(CLOCK_MONOTONIC, &frame_t0);

  /* Ctrl-Shift-D toggles the HUD. macOS reserves F-keys by default; this
   * shortcut works regardless of the system Fn-key setting. */
  if (guiModifierDown() && guiShiftDown() && IsKeyPressed(KEY_D)) {
    G.debug_hud = !G.debug_hud;
    editorSetStatusMessage("Debug HUD %s", G.debug_hud ? "on" : "off");
  }

  /* Reset per-frame debug accumulators. */
  G.dbg_emit_us = 0.0;
  G.dbg_dispatch_us = 0.0;
  G.dbg_syntax_us = 0.0;
  G.dbg_draw_calls = 0;
  G.dbg_run_count = 0;
  G.dbg_glyph_count = 0;
  G.dbg_layout_us = 0.0;
  G.dbg_begin_us = 0.0;
  G.dbg_clear_us = 0.0;
  G.dbg_sidebar_us = 0.0;
  G.dbg_topbar_us = 0.0;
  G.dbg_findbar_us = 0.0;
  G.dbg_editor_us = 0.0;
  G.dbg_cursor_us = 0.0;
  G.dbg_hint_us = 0.0;
  G.dbg_status_us = 0.0;
  G.dbg_overlay_us = 0.0;
  G.dbg_end_us = 0.0;

  GUI_TIME_BEGIN(layout);  guiUpdateLayout();             GUI_TIME_END(layout);
  /* Resolve the function hint once per frame and stash it in G.hint_text.
   * guiDrawFunctionHint and guiDrawStatusBar both read the cached value.
   * Time it under the same bucket as guiDrawFunctionHint so the bench
   * attributes the cost where the work is. */
  GUI_TIME_BEGIN(hint);    guiUpdateFrameHint();          GUI_TIME_END(hint);
  GUI_TIME_BEGIN(begin);   BeginDrawing();                GUI_TIME_END(begin);
  GUI_TIME_BEGIN(clear);   ClearBackground(GUI_BG);       GUI_TIME_END(clear);
  GUI_TIME_BEGIN(sidebar); guiDrawSidebar();              GUI_TIME_END(sidebar);
  GUI_TIME_BEGIN(topbar);  guiDrawTopBar();               GUI_TIME_END(topbar);
  if (G.find_active) {
    GUI_TIME_BEGIN(findbar);
    guiDrawFindBar();
    guiDrawFindResults();
    GUI_TIME_END(findbar);
  }
  GUI_TIME_BEGIN(editor);  guiDrawEditorRows();           GUI_TIME_END(editor);
  guiDrawWelcome();
  GUI_TIME_BEGIN(cursor);  guiDrawCursor();               GUI_TIME_END(cursor);
  /* guiDrawFunctionHint just reads G.hint_text — cheap now. */
  guiDrawFunctionHint();
  GUI_TIME_BEGIN(status);  guiDrawStatusBar();            GUI_TIME_END(status);
  GUI_TIME_BEGIN(overlay);
  guiDrawContextMenu();
  guiDrawOverlay();
  GUI_TIME_END(overlay);

  /* Frame time measured BEFORE EndDrawing so vsync wait isn't lumped in
   * with our render work. EndDrawing has its own timing bucket. */
  struct timespec frame_t1;
  clock_gettime(CLOCK_MONOTONIC, &frame_t1);
  double frame_ms = (frame_t1.tv_sec - frame_t0.tv_sec) * 1000.0 +
                    (frame_t1.tv_nsec - frame_t0.tv_nsec) / 1.0e6;
  G.dbg_frame_window[G.dbg_frame_window_idx] = frame_ms;
  G.dbg_frame_window_idx = (G.dbg_frame_window_idx + 1) & 63;
  G.dbg_frame_ms = frame_ms;

  if (G.debug_hud)
    guiDrawDebugHud();

  GUI_TIME_BEGIN(end);     EndDrawing();                  GUI_TIME_END(end);
}

static void guiDrawDebugHud(void) {
  /* Smoothed frame time = mean of the recent window. */
  double sum = 0.0;
  int n = 0;
  for (int i = 0; i < 64; i++) {
    if (G.dbg_frame_window[i] > 0.0) { sum += G.dbg_frame_window[i]; n++; }
  }
  double mean_ms = n > 0 ? sum / n : 0.0;
  double fps = mean_ms > 0.001 ? 1000.0 / mean_ms : 0.0;

  /* Decompose into render-phase categories. "other" = setup, gutter,
   * selection backgrounds, search match drawing, sidebar, status bar,
   * VSync, etc — anything outside the three measured phases. */
  double measured_ms = (G.dbg_emit_us + G.dbg_dispatch_us + G.dbg_syntax_us) /
                       1000.0;
  double other_ms = G.dbg_frame_ms - measured_ms;
  if (other_ms < 0.0) other_ms = 0.0;

  char lines[12][96];
  int line_count = 0;
  snprintf(lines[line_count++], 96, "frame   %6.2f ms  %.1f fps", mean_ms, fps);
  snprintf(lines[line_count++], 96, "emit    %6.1f us  layout pass", G.dbg_emit_us);
  snprintf(lines[line_count++], 96, "disp    %6.1f us  %d DrawTextEx", G.dbg_dispatch_us, G.dbg_draw_calls);
  snprintf(lines[line_count++], 96, "syntax  %6.1f us  refresh", G.dbg_syntax_us);
  snprintf(lines[line_count++], 96, "other   %6.2f ms  bg/vsync", other_ms);
  snprintf(lines[line_count++], 96, "runs    %6d   glyphs %d", G.dbg_run_count, G.dbg_glyph_count);
  snprintf(lines[line_count++], 96, "rows    %6d   rowoff %d", tbLineCount(TB), E.rowoff);

  int pad = 8;
  int line_h = 16;
  int w = 280;
  int h = pad * 2 + line_count * line_h;
  int x = GetScreenWidth() - w - 12;
  int y = 12;

  DrawRectangle(x, y, w, h, (Color){0, 0, 0, 200});
  DrawRectangleLines(x, y, w, h, (Color){255, 255, 255, 80});
  for (int i = 0; i < line_count; i++) {
    DrawTextEx(G.font, lines[i], (Vector2){x + pad, y + pad + i * line_h},
               13.0f, 0.0f, (Color){240, 240, 240, 255});
  }
}

/* ============================ GUI Benchmark =============================
 *
 * Headless GUI benchmark. Opens a hidden raylib window, loads the file,
 * and synthetically scrolls/types while recording per-frame numbers from
 * the same instrumentation the F12 / Ctrl-Shift-D HUD uses. Prints CSV
 * to stdout so the operator (or me, reading the output) can see whether
 * the dispatch loop, the layout pass, or "other" dominates frame time.
 *
 * Invoked with: `bento-gui --bench-gui <file>`. Window is hidden via
 * FLAG_WINDOW_HIDDEN and VSync is disabled so we measure real render
 * cost rather than monitor-locked rate.
 */

static void benchGuiPrintHeader(void) {
  printf("phase,frame,rowoff,frame_ms,"
         "edit_us,layout_us,begin_us,clear_us,sidebar_us,topbar_us,"
         "findbar_us,editor_us,cursor_us,hint_us,status_us,overlay_us,end_us,"
         "emit_us,disp_us,syntax_us,draws,runs,glyphs\n");
}

static void benchGuiPrintRow(const char *phase, int frame) {
  printf("%s,%d,%d,%.3f,"
         "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
         "%.1f,%.1f,%.1f,%d,%d,%d\n",
         phase, frame, E.rowoff, G.dbg_frame_ms,
         G.dbg_edit_us, G.dbg_layout_us, G.dbg_begin_us, G.dbg_clear_us,
         G.dbg_sidebar_us, G.dbg_topbar_us, G.dbg_findbar_us, G.dbg_editor_us,
         G.dbg_cursor_us, G.dbg_hint_us, G.dbg_status_us, G.dbg_overlay_us,
         G.dbg_end_us,
         G.dbg_emit_us, G.dbg_dispatch_us, G.dbg_syntax_us,
         G.dbg_draw_calls, G.dbg_run_count, G.dbg_glyph_count);
}

/* Per-phase aggregate captured across a run. */
typedef struct guiBenchAgg {
  double frame_ms[1024];
  double edit_us[1024];
  double sub_us[1024][12]; /* layout, begin, clear, sidebar, topbar, findbar,
                              editor, cursor, hint, status, overlay, end */
  int n;
} guiBenchAgg;

static const char *guiBenchSubNames[12] = {
  "layout", "begin", "clear", "sidebar", "topbar", "findbar",
  "editor", "cursor", "hint", "status", "overlay", "end"};

static void benchGuiCapture(guiBenchAgg *agg) {
  if (agg->n >= 1024) return;
  int i = agg->n++;
  agg->frame_ms[i] = G.dbg_frame_ms;
  agg->edit_us[i] = G.dbg_edit_us;
  agg->sub_us[i][0]  = G.dbg_layout_us;
  agg->sub_us[i][1]  = G.dbg_begin_us;
  agg->sub_us[i][2]  = G.dbg_clear_us;
  agg->sub_us[i][3]  = G.dbg_sidebar_us;
  agg->sub_us[i][4]  = G.dbg_topbar_us;
  agg->sub_us[i][5]  = G.dbg_findbar_us;
  agg->sub_us[i][6]  = G.dbg_editor_us;
  agg->sub_us[i][7]  = G.dbg_cursor_us;
  agg->sub_us[i][8]  = G.dbg_hint_us;
  agg->sub_us[i][9]  = G.dbg_status_us;
  agg->sub_us[i][10] = G.dbg_overlay_us;
  agg->sub_us[i][11] = G.dbg_end_us;
}

/* Median of a copied buffer (small, insertion sort is fine). */
static double benchMedian(const double *src, int n) {
  if (n <= 0) return 0.0;
  double buf[1024];
  int m = n < 1024 ? n : 1024;
  for (int i = 0; i < m; i++) buf[i] = src[i];
  for (int i = 1; i < m; i++) {
    double k = buf[i];
    int j = i - 1;
    while (j >= 0 && buf[j] > k) { buf[j + 1] = buf[j]; j--; }
    buf[j + 1] = k;
  }
  return buf[m / 2];
}

static void benchGuiPhaseSummary(const char *phase, const guiBenchAgg *agg) {
  if (agg->n <= 0) return;
  int n = agg->n;
  double frame_sum = 0, frame_max = 0, edit_sum = 0;
  for (int i = 0; i < n; i++) {
    frame_sum += agg->frame_ms[i];
    if (agg->frame_ms[i] > frame_max) frame_max = agg->frame_ms[i];
    edit_sum += agg->edit_us[i];
  }
  double frame_median = benchMedian(agg->frame_ms, n);
  fprintf(stderr,
          "%-8s n=%4d  frame mean=%7.3fms median=%7.3fms max=%7.3fms  "
          "edit mean=%6.1fus\n",
          phase, n, frame_sum / n, frame_median, frame_max, edit_sum / n);
  /* Per-sub-call means + max. Sorted by mean descending so the worst
   * offender shows at the top. */
  struct slot_stat { const char *name; double mean; double max; };
  struct slot_stat slots[12];
  for (int s = 0; s < 12; s++) {
    double sum = 0, mx = 0;
    for (int i = 0; i < n; i++) {
      sum += agg->sub_us[i][s];
      if (agg->sub_us[i][s] > mx) mx = agg->sub_us[i][s];
    }
    slots[s].name = guiBenchSubNames[s];
    slots[s].mean = sum / n;
    slots[s].max = mx;
  }
  /* Insertion-sort slots by mean desc. */
  for (int i = 1; i < 12; i++) {
    struct slot_stat k = slots[i];
    int j = i - 1;
    while (j >= 0 && slots[j].mean < k.mean) { slots[j + 1] = slots[j]; j--; }
    slots[j + 1] = k;
  }
  for (int s = 0; s < 12; s++) {
    fprintf(stderr, "    %-8s mean=%7.1fus  max=%8.1fus\n",
            slots[s].name, slots[s].mean, slots[s].max);
  }
}

static void benchGuiRun(const char *filename) {
  char fbuf[PATH_MAX];
  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), ".");
  if (guiPathIsAbsolute(filename))
    snprintf(fbuf, sizeof(fbuf), "%s", filename);
  else
    guiPathJoin(fbuf, sizeof(fbuf), cwd, filename);

  SetTraceLogLevel(LOG_WARNING);
  /* Hidden window so we don't disturb anything visually; no VSync so we
   * measure raw render cost. */
  SetConfigFlags(FLAG_WINDOW_HIDDEN);
  InitWindow(GUI_WINDOW_WIDTH, GUI_WINDOW_HEIGHT, "bento-bench-gui");
  SetExitKey(KEY_NULL);

  G.font_size = GUI_FONT_SIZE;
  G.small_font_size = GUI_SMALL_FONT_SIZE;
  G.quit_times = bento_QUIT_TIMES;
  G.current_match = -1;
  G.sidebar_width = GUI_SIDEBAR_WIDTH;
  G.debug_hud = 0; /* we print our own numbers */
  guiLoadEditorFont();

  initEditor();
  T.active = -1;
  T.preview = -1;
  T.last_click_file_index = -1;
  T.last_tab_click_index = -1;
  T.close_confirm_index = -1;

  T.active = bentoAllocTabSlot();
  editorOpen(fbuf);
  snprintf(T.list[T.active].filename, sizeof(T.list[T.active].filename),
           "%s", fbuf);
  T.list[T.active].pinned = 1;
  G.project_open = 1;
  guiRebuildFileTree();
  G.last_activity_time = GetTime();

  int lc = tbLineCount(TB);
  fprintf(stderr, "loaded %d lines from %s\n", lc, fbuf);

  /* Warmup: 10 frames at top of file so caches/atlas settle. */
  for (int f = 0; f < 10; f++) {
    E.rowoff = 0;
    guiDrawFrame();
  }

  benchGuiPrintHeader();

  guiBenchAgg idle_agg = {0}, scroll_agg = {0}, type_agg = {0}, jump_agg = {0};

  /* --- Phase 1: idle on top of file. --- */
  E.rowoff = 0;
  G.dbg_edit_us = 0;
  for (int f = 0; f < 20; f++) {
    guiDrawFrame();
    benchGuiPrintRow("idle", f);
    benchGuiCapture(&idle_agg);
  }

  /* --- Phase 2: scroll forward 1 line per frame. --- */
  int n_scroll = lc > 1024 ? 1024 : lc;
  for (int f = 0; f < n_scroll; f++) {
    E.rowoff = f;
    G.dbg_edit_us = 0;
    guiDrawFrame();
    benchGuiPrintRow("scroll", f);
    benchGuiCapture(&scroll_agg);
  }

  /* --- Phase 3: typing at row lc/2. Each frame inserts one char then
   * renders. Times editorInsertChar separately. --- */
  E.cx = 0; E.cy = 0; E.coloff = 0; E.rowoff = lc / 2;
  for (int f = 0; f < 100; f++) {
    struct timespec et0, et1;
    clock_gettime(CLOCK_MONOTONIC, &et0);
    editorInsertChar('a' + (f % 26));
    clock_gettime(CLOCK_MONOTONIC, &et1);
    G.dbg_edit_us = guiUsBetween(et0, et1);
    guiDrawFrame();
    benchGuiPrintRow("type", f);
    benchGuiCapture(&type_agg);
  }

  /* --- Phase 4: page jumps. Sample 16 far-apart positions. --- */
  for (int f = 0; f < 16; f++) {
    E.rowoff = (f * lc) / 16;
    G.dbg_edit_us = 0;
    guiDrawFrame();
    benchGuiPrintRow("jump", f);
    benchGuiCapture(&jump_agg);
  }

  benchGuiPhaseSummary("idle",   &idle_agg);
  benchGuiPhaseSummary("scroll", &scroll_agg);
  benchGuiPhaseSummary("type",   &type_agg);
  benchGuiPhaseSummary("jump",   &jump_agg);

  if (G.font_owned)
    UnloadFont(G.font);
  CloseWindow();
}

#ifndef bento_NO_MAIN
int main(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "--bench")) {
    if (argc < 3) {
      fprintf(stderr, "Usage: bento --bench <file>\n");
      return 2;
    }
    benchRun(argv[2]);
    return 0;
  }
  if (argc >= 2 && !strcmp(argv[1], "--bench-gui")) {
    if (argc < 3) {
      fprintf(stderr, "Usage: bento-gui --bench-gui <file>\n");
      return 2;
    }
    benchGuiRun(argv[2]);
    return 0;
  }

  char filename[PATH_MAX];
  char cwd[PATH_MAX];
  char title[512];
  int fresh_window = 0;
  int file_arg = 0;

  if (!getcwd(cwd, sizeof(cwd)))
    snprintf(cwd, sizeof(cwd), ".");
  snprintf(G.cwd, sizeof(G.cwd), "%s", cwd);

  if (guiPathIsAbsolute(argv[0]))
    snprintf(G.launch_path, sizeof(G.launch_path), "%s", argv[0]);
  else if (strchr(argv[0], '/')
#ifdef _WIN32
           || strchr(argv[0], '\\')
#endif
           )
    guiPathJoin(G.launch_path, sizeof(G.launch_path), cwd, argv[0]);
  else
    snprintf(G.launch_path, sizeof(G.launch_path), "%s", argv[0]);

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--fresh-window") || !strcmp(argv[i], "--no-restore")) {
      fresh_window = 1;
      continue;
    }
    if (!file_arg)
      file_arg = i;
  }

  if (file_arg && guiPathIsAbsolute(argv[file_arg]))
    snprintf(filename, sizeof(filename), "%s", argv[file_arg]);
  else
    guiPathJoin(filename, sizeof(filename), cwd,
                file_arg ? argv[file_arg] : "untitled.txt");

  /* Silence raylib's startup INFO log (module list, GL extensions, etc.).
   * Real problems still surface — anything LOG_WARNING and above prints. */
  SetTraceLogLevel(LOG_WARNING);

  /* Let the display drive the frame rate (VSync). On a 120Hz panel we render
   * at 120Hz, on 60Hz we render at 60Hz. No artificial cap. */
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
  snprintf(title, sizeof(title), "Bento %s", bento_VERSION);
  InitWindow(GUI_WINDOW_WIDTH, GUI_WINDOW_HEIGHT, title);
  SetExitKey(KEY_NULL);
  guiSetBentoWindowIcon();

  G.quit_times = bento_QUIT_TIMES;
  G.current_match = -1;
  G.transient_session = fresh_window;
  guiInitSettingsDefaults();
  guiLoadSettings();
  guiLoadRecents();
  guiLoadEditorFont();

  initEditor();
  T.active = -1;
  T.preview = -1;
  T.last_click_file_index = -1;
  T.last_tab_click_index = -1;
  T.close_confirm_index = -1;

  int restored = 0;
  if (!file_arg && !fresh_window && G.restore_session) {
    /* No file argument — try to restore the previous session. */
    restored = bentoLoadSession();
  }
  if (!restored && file_arg) {
    if (guiPathIsDir(filename)) {
      guiOpenProject(filename);
    } else {
      /* We were given a filename. Open it without inventing a project
       * folder; users can open a folder explicitly when they want a tree. */
      guiOpenFile(filename);
    }
  } else if (!restored) {
    G.welcome_active = 1;
  }
  if (G.project_open) {
    guiRebuildFileTree();
    guiAddRecentProject(G.cwd);
  }
  if (restored) {
    if (G.project_open)
      editorSetStatusMessage("Bento %s - restored workspace - %s",
                             bento_VERSION, G.font_label);
    else
      editorSetStatusMessage("Bento %s - restored %d tab%s - %s",
                             bento_VERSION, T.count,
                             T.count == 1 ? "" : "s", G.font_label);
  } else {
    editorSetStatusMessage("Bento %s - ready - %s",
                           bento_VERSION, G.font_label);
  }

  G.last_activity_time = GetTime();
  G.last_mouse_x = -1;
  G.last_mouse_y = -1;
  G.last_editor_click_row = -1;
  G.last_editor_click_col = -1;
  G.last_state_hash = 0;
  G.idle_mode = 0;

  while (!G.should_close) {
    if (T.active >= 0 && E.filename)
      snprintf(title, sizeof(title), "Bento - %s%s", guiBaseName(E.filename),
               E.dirty ? " *" : "");
    else
      snprintf(title, sizeof(title), "Bento");
    SetWindowTitle(title);

    /* Manual event polling lets us decide whether to render this frame. */
    PollInputEvents();
    guiPollPathPicker();

    if (IsFileDropped()) {
      FilePathList dropped = LoadDroppedFiles();
      for (unsigned int i = 0; i < dropped.count; i++) {
        const char *path = dropped.paths[i];
        if (guiPathIsDir(path))
          guiOpenProject(path);
        else
          guiOpenFile(path);
      }
      UnloadDroppedFiles(dropped);
    }

    if (WindowShouldClose())
      guiRequestClose();
    else
      guiProcessEditorInput();

    /* Track the user's cursor motion (used to be inside guiDrawCursor). It
     * has to run every iteration regardless of frame-skip so the blink
     * resets the moment text/navigation moves the caret. */
    int file_row = E.rowoff + E.cy;
    int file_col = E.coloff + E.cx;
    if (file_row != G.last_cursor_row || file_col != G.last_cursor_col) {
      G.last_cursor_row = file_row;
      G.last_cursor_col = file_col;
      G.last_cursor_move_time = GetTime();
    }

    /* Detect a hover on any tab's × button — the only mouse-hover effect
     * the UI has — so we only repaint when *that* hover state actually
     * flips, not on every single mouse-motion sample. */
    Vector2 mp = GetMousePosition();
    int hover_close = -1;
    for (int i = 0; i < BENTO_MAX_TABS; i++) {
      guiTab *t = &T.list[i];
      if (!t->in_use || t->close_size == 0)
        continue;
      if (mp.x >= t->close_x && mp.x < t->close_x + t->close_size &&
          mp.y >= t->close_y && mp.y < t->close_y + t->close_size) {
        hover_close = i;
        break;
      }
    }

    /* Content hash: everything that, if it changes, requires a repaint. */
	    unsigned long long state_hash =
	        (unsigned long long)E.cx * 1u +
	        (unsigned long long)E.cy * 1009u +
	        (unsigned long long)E.rowoff * 1000003u +
	        (unsigned long long)E.coloff * 31u +
        (unsigned long long)E.screenrows * 257u +
        (unsigned long long)E.screencols * 263u +
        (unsigned long long)E.dirty * 7u +
        (unsigned long long)tbLineCount(TB) * 131u +
        (unsigned long long)T.active * 17u +
        (unsigned long long)T.count * 199u +
	        (unsigned long long)G.find_active * 53u +
	        (unsigned long long)G.find_focus * 59u +
	        (unsigned long long)G.find_editor_focus * 61u +
	        (unsigned long long)G.find_all_active * 67u +
	        (unsigned long long)G.find_project_active * 71u +
	        (unsigned long long)G.find_case_sensitive * 9073u +
	        (unsigned long long)G.find_whole_word * 9079u +
	        guiHashBytes(G.query, G.query_len) * 79u +
	        guiHashBytes(G.replacement, G.replacement_len) * 83u +
	        (unsigned long long)G.overlay * 211u +
	        (unsigned long long)G.overlay_selected * 223u +
	        (unsigned long long)G.overlay_scroll * 227u +
	        guiHashBytes(G.overlay_query, G.overlay_query_len) * 229u +
	        guiHashBytes(G.prompt_value, G.prompt_value_len) * 233u +
	        (unsigned long long)G.prompt_kind * 239u +
	        (unsigned long long)G.settings_selected * 241u +
	        (unsigned long long)G.theme_index * 251u +
	        (unsigned long long)G.show_whitespace * 269u +
	        (unsigned long long)G.tab_width * 271u +
	        (unsigned long long)G.restore_session * 277u +
	        (unsigned long long)G.sidebar_width * 279u +
	        (unsigned long long)G.project_open * 280u +
	        (unsigned long long)G.welcome_active * 281u +
	        (unsigned long long)G.path_picker_active * 282u +
	        (unsigned long long)G.context_menu * 283u +
	        (unsigned long long)G.context_x * 293u +
	        (unsigned long long)G.context_y * 307u +
	        (unsigned long long)G.context_target_index * 311u +
	        (unsigned long long)G.context_tab_index * 313u +
	        (unsigned long long)G.selection_active * 73u +
	        (unsigned long long)G.selection_anchor_row * 503u +
	        (unsigned long long)G.selection_anchor_col * 509u +
	        (unsigned long long)G.selection_cursor_row * 521u +
	        (unsigned long long)G.selection_cursor_col * 523u +
	        (unsigned long long)G.tree_scroll * 9001u +
	        (unsigned long long)G.match_scroll * 9043u +
	        (unsigned long long)G.current_match * 9061u +
	        (unsigned long long)(G.editor_scroll_y * 1024.0f) * 9067u +
	        (unsigned long long)E.statusmsg_time * 41u +
	        g_render_gen * 9277u +
	        (unsigned long long)(hover_close + 1) * 911u;

    int mouse_moved =
        (mp.x != G.last_mouse_x) || (mp.y != G.last_mouse_y);
    int state_changed = (state_hash != G.last_state_hash);
    G.last_mouse_x = mp.x;
    G.last_mouse_y = mp.y;
    G.last_state_hash = state_hash;

    /* Mouse motion (even pure hover) counts as activity so we don't drop
     * into deep idle while the user is using the trackpad — but it does
     * NOT force a repaint. State changes drive repaints. */
    if (mouse_moved || state_changed)
      G.last_activity_time = GetTime();

    int blink_now = bentoCursorVisibleNow();
    int blink_changed = blink_now != G.last_cursor_visible;
    G.last_cursor_visible = blink_now;

    double idle = GetTime() - G.last_activity_time;
    if (idle > 3.0 && !G.idle_mode) {
      EnableEventWaiting();
      G.idle_mode = 1;
    } else if (idle <= 3.0 && G.idle_mode) {
      DisableEventWaiting();
      G.idle_mode = 0;
    }

    int needs_redraw = state_changed || blink_changed;

    if (needs_redraw) {
      /* guiDrawFrame's EndDrawing handles VSync; in idle_mode it blocks on
       * glfwWaitEvents until the next OS event arrives. */
      guiDrawFrame();
    } else {
      /* Nothing to draw. Sleep instead of busy-spinning at refresh rate.
       * Short sleep in active mode so cursor blink and incoming input feel
       * snappy; longer sleep in deep idle. */
      WaitTime(G.idle_mode ? 0.25 : 0.008);
    }
  }

  if (!G.transient_session)
    bentoSaveSession();

  if (G.font_owned)
    UnloadFont(G.font);
  free(G.signatures);
  free(G.files);
  CloseWindow();
  return 0;
}
#endif

#else

#ifndef bento_NO_MAIN
int main(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "--bench")) {
    if (argc < 3) {
      fprintf(stderr, "Usage: bento --bench <file>\n");
      return 2;
    }
    benchRun(argv[2]);
    return 0;
  }

  if (argc != 2) {
    fprintf(stderr, "Usage: bento <filename>\n");
    exit(1);
  }

  initEditor();
  editorSelectSyntaxHighlight(argv[1]);
  editorOpen(argv[1]);
  enableRawMode(STDIN_FILENO);
  editorSetStatusMessage(
      "HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find | Ctrl-Z = undo");
  while (1) {
    editorRefreshScreen();
    editorProcessKeypress(STDIN_FILENO);
  }
  return 0;
}
#endif

#endif /* bento_RAYLIB */

#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  /* isatty */
#include <stdint.h>

#include "buf.h"

#define MAX_FILES DIAG_MAX_FILES
#define MAX_NOTES 8
#define MAX_SECONDARY_SPANS 4

static const SourceFile *files_[MAX_FILES];
static Span              origins_[MAX_FILES];  /* see diag_set_file_origin */
static Span              expansion_site_;      /* see diag_set_expansion_site */
static const char       *expansion_macro_;
static size_t            file_count_;
static bool              had_error_;
static uint64_t          error_serial_;   /* count of SHOWN (uncaptured) errors */
static bool              use_color_ = false;
static bool              json_output_ = false;  /* Phase 8: JSON diagnostics mode */

/* ANSI color codes for diagnostics */
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_DIM     "\033[2m"

void diag_init(bool use_color) {
    use_color_ = use_color;
}

bool diag_use_color(void) {
    return use_color_;
}

void diag_register_file(const SourceFile *file) {
    if (file->file_id >= MAX_FILES) {
        fprintf(stderr, "tur: too many source files\n");
        abort();
    }
    files_[file->file_id] = file;
    origins_[file->file_id] = SPAN_UNKNOWN;
    if (file->file_id >= file_count_) file_count_ = (size_t)file->file_id + 1;
}

void diag_set_file_origin(uint16_t file_id, Span origin) {
    if (file_id >= MAX_FILES || origin.line == 0 ||
        origin.file_id >= MAX_FILES || origin.file_id == file_id)
        return;
    origins_[file_id] = origin;
}

Span diag_set_expansion_site(Span site, const char *macro_name,
                             const char **prev_name) {
    Span prev = expansion_site_;
    if (prev_name) *prev_name = expansion_macro_;
    expansion_site_  = site;
    expansion_macro_ = site.line > 0 ? macro_name : NULL;
    return prev;
}

uint16_t diag_alloc_file_id(void) {
    static uint16_t next_high = MAX_FILES - 1;
    if (next_high == 0 || files_[next_high] != NULL) {
        fprintf(stderr, "tur: too many source files\n");
        abort();
    }
    return next_high--;
}

bool diag_had_error(void) { return had_error_; }

/* Re-mark the error flag after a nested evaluation's diag_reset cleared it.
 * Used by macro-time evaluation (src/turi/macro_env.c, read-string): the
 * nested eval resets the diagnostic slate for its own parse/elaborate, which
 * would otherwise let a compile that already reported errors exit 0. */
void diag_force_had_error(void) { had_error_ = true; }

uint64_t diag_error_serial(void) { return error_serial_; }

/* Speculative-elaboration capture frames (see diag.h). */
#define MAX_CAPTURE_DEPTH 16
static int      capture_depth_;
static uint32_t capture_err_[MAX_CAPTURE_DEPTH];
static bool     capture_saved_had_[MAX_CAPTURE_DEPTH];

void diag_push_capture(void) {
    if (capture_depth_ < MAX_CAPTURE_DEPTH) {
        capture_saved_had_[capture_depth_] = had_error_;
        capture_err_[capture_depth_] = 0;
    }
    capture_depth_++;
}

uint32_t diag_pop_capture(void) {
    if (capture_depth_ <= 0) return 0;
    capture_depth_--;
    if (capture_depth_ < MAX_CAPTURE_DEPTH) {
        had_error_ = capture_saved_had_[capture_depth_];
        return capture_err_[capture_depth_];
    }
    return 0;
}

/* Returns true when the current emission must be suppressed (a capture frame
 * is active); also tallies suppressed errors into the innermost frame.
 *
 * Warnings pass through untouched: the capture frame's job is to detect (and
 * swallow) the *errors* that a speculative elaboration produces, not to mute
 * a body's warnings.  A body that captures cleanly (no errors) is kept as-is,
 * so letting its warnings out keeps that common case byte-identical to the
 * pre-capture behavior.  Errors and their subordinate notes/help are
 * suppressed together so a swallowed error never leaves orphaned notes. */
static bool diag_intercept(DiagLevel level) {
    if (capture_depth_ <= 0) {
        /* An error that reaches the user.  Counted here -- the one gate every
         * emit entry point passes through -- so callers can bracket a window
         * and ask "did this elaboration surface an error?" without treating a
         * captured-and-swallowed speculative error as one.  Consumed by the
         * macro-expansion provenance note (elab_call.c). */
        if (level == DIAG_ERROR) error_serial_++;
        return false;
    }
    if (level == DIAG_WARNING) return false;
    if (level == DIAG_ERROR && capture_depth_ <= MAX_CAPTURE_DEPTH)
        capture_err_[capture_depth_ - 1]++;
    return true;
}

const char *diag_file_path(uint16_t file_id) {
    if (file_id < MAX_FILES && files_[file_id])
        return files_[file_id]->path;
    return NULL;
}

/* r7rs-lang-plan R7: the first file id past the auto-loaded stdlib band the
 * compiled driver numbers from 1 (0 when no band was prepended). */
static uint16_t autoload_file_ids_end_ = 0;
void diag_note_autoload_file_ids(uint16_t end) {
    if (end > autoload_file_ids_end_) autoload_file_ids_end_ = end;
}
uint16_t diag_autoload_file_ids_end(void) { return autoload_file_ids_end_; }

const SourceFile *diag_source_file(uint16_t file_id) {
    if (file_id < MAX_FILES) return files_[file_id];
    return NULL;
}

void diag_reset(void) {
    had_error_ = false;
    file_count_ = 0;
    for (size_t i = 0; i < MAX_FILES; i++) files_[i] = NULL;
}

size_t diag_files_capacity(void) { return MAX_FILES; }

size_t diag_files_save(const SourceFile **out, size_t cap) {
    size_t n = (cap < MAX_FILES) ? cap : MAX_FILES;
    for (size_t i = 0; i < n; i++) out[i] = files_[i];
    return n;
}

void diag_files_replace(const SourceFile **in, size_t n) {
    if (n > MAX_FILES) n = MAX_FILES;
    file_count_ = 0;
    for (size_t i = 0; i < MAX_FILES; i++) {
        files_[i] = (i < n) ? in[i] : NULL;
        if (files_[i]) file_count_ = i + 1;
    }
}

void diag_files_restore(const SourceFile **in, size_t n) {
    if (n > MAX_FILES) n = MAX_FILES;
    /* Skip id 0: the caller has just registered THIS turn's source blob there,
     * and the saved entry is the previous turn's, which must not clobber it. */
    for (size_t i = 1; i < n; i++) {
        if (!in[i] || files_[i]) continue;
        files_[i] = in[i];
        if (i >= file_count_) file_count_ = i + 1;
    }
}

/* Check if stderr is a TTY (for auto-color detection) */
bool stderr_is_tty(void) {
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 1
    return isatty(fileno(stderr));
#else
    return false; /* Default to no color on non-POSIX systems */
#endif
}

/* Get the color code for a diagnostic level */
static const char *color_for_level(DiagLevel level) {
    if (!use_color_) return "";
    switch (level) {
        case DIAG_ERROR:   return COLOR_RED;
        case DIAG_WARNING: return COLOR_YELLOW;
        case DIAG_NOTE:    return COLOR_CYAN;
        case DIAG_HELP:    return COLOR_GREEN;
    }
    return "";
}

static const char *level_name(DiagLevel l) {
    switch (l) {
        case DIAG_ERROR:   return "error";
        case DIAG_WARNING: return "warning";
        case DIAG_NOTE:    return "note";
        case DIAG_HELP:    return "help";
    }
    return "?";
}

/* Get the underline character for a style */
static char underline_char(UnderlineStyle style) {
    switch (style) {
        case UNDERLINE_PRIMARY:   return '^';
        case UNDERLINE_SECONDARY: return '~';
        case UNDERLINE_GAP:       return '-';
    }
    return '^';
}

/* Get error code string for display */
const char *diag_code_to_string(DiagCode code) {
    switch (code) {
        case TUR_E0001_TYPE_MISMATCH:     return "TUR-E0001";
        case TUR_E0002_ARITY_MISMATCH:    return "TUR-E0002";
        case TUR_E0003_UNBOUND_SYMBOL:   return "TUR-E0003";
        case TUR_E0004_INVALID_SCOPE:    return "TUR-E0004";
        case TUR_E0005_USE_AFTER_MOVE:    return "TUR-E0005";
        case TUR_E0006_OPERATOR_LOOKUP_FAILED: return "TUR-E0006";
        case TUR_E0007_CAPTURE_ERROR:     return "TUR-E0007";
        case TUR_E0009_EFFECT_ROW_MISMATCH: return "TUR-E0009";
        case TUR_E0010_NOT_SEND:            return "TUR-E0010";
        case TUR_E0011_NOT_SYNC:            return "TUR-E0011";
        case TUR_E0012_KIND_MISMATCH:       return "TUR-E0012";
        case TUR_E0013_ORPHAN_INSTANCE:     return "TUR-E0013";
        case TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED: return "TUR-E0015";
        case TUR_E0014_NOT_CLONE:                        return "TUR-E0014";
        case TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET:    return "TUR-E0016";
        case TUR_E0017_CONT_ESCAPE_ASYNC:                return "TUR-E0017";
        case TUR_E0018_NOT_SERIALIZABLE:                 return "TUR-E0018";
        case TUR_E0019_SERIAL_SHIFT_OUTSIDE_RESET:       return "TUR-E0019";
        /* Was missing from this table, so every ambiguous-dispatch error
         * printed a bare `error []` and could not be grepped for or matched by
         * an `errors/` fixture's expected.diag.  Both pre-existing call sites
         * in elab_typeclasses.c were affected, not just the new one. */
        case TUR_E0020_AMBIGUOUS_DISPATCH:               return "TUR-E0020";
        case TUR_E0021_PRIVATE_EFFECT:                   return "TUR-E0021";
        /* CF6: async Send-across-await */
        case TUR_E0022_AWAIT_LIVE_NOT_SEND:              return "TUR-E0022";
        case TUR_E0025_DUPLICATE_INSTANCE:               return "TUR-E0025";
        case TUR_E0026_UNKNOWN_EFFECT_IN_ROW:            return "TUR-E0026";
        /* Phase B: mixed-width numeric arithmetic */
        case TUR_E0042_MIXED_WIDTH_ARITH:                return "TUR-E0042";
        case TUR_E0254_INFINITE_EFFECT_ROW:              return "TUR-E0254";
        /* SZ7: static size checking */
        case TUR_E0260_SIZED_TYPE_MISMATCH:              return "TUR-E0260";
        /* ET3: handler typing errors */
        case TUR_E0251_HANDLER_OVERLAP:                  return "TUR-E0251";
        case TUR_E0252_HANDLER_RESULT_MISMATCH:          return "TUR-E0252";
        /* CF4: gated call/cc / escape */
        case TUR_E0700_CALLCC_GATED:                     return "TUR-E0700";
        case TUR_E0701_ESCAPE_GATED:                     return "TUR-E0701";
        /* CF5: always-on generator limitation diagnostics */
        case TUR_E0702_YIELD_IN_MATCH_ARM:               return "TUR-E0702";
        case TUR_E0703_YIELD_IN_RECURSIVE_GEN:           return "TUR-E0703";
        /* CF3: gated first-class handler composition */
        case TUR_E0704_HANDLER_COMPOSE_UNIMPL:           return "TUR-E0704";
        case TUR_E0705_POLY_CLOSURE_RESULT_TYVAR:        return "TUR-E0705";
        case TUR_E0706_SERIAL_CONTEXT_NOT_CAPTURABLE:    return "TUR-E0706";
        case TUR_E0707_RETURN_REGISTER_CLASS_MISMATCH:   return "TUR-E0707";
        case TUR_E0708_RETURN_POINTER_SCALAR_MISMATCH:   return "TUR-E0708";
        case TUR_E0709_RETURN_TYPE_MISMATCH:             return "TUR-E0709";
        case TUR_E0710_CLONEABLE_CONTEXT_NOT_CAPTURABLE: return "TUR-E0710";
        case TUR_E0711_MODULE_TOPLEVEL_EXPR:             return "TUR-E0711";
        case TUR_E0712_EXPR_NESTING_TOO_DEEP:            return "TUR-E0712";
        case TUR_E0713_DEFINITION_IN_TAIL_POSITION:      return "TUR-E0713";
        case TUR_E0714_NICHE_ELEMENT_ERASED_STORE:       return "TUR-E0714";
        case TUR_E0715_NICHE_ELEMENT_ERASED_COMPARATOR:  return "TUR-E0715";
        /* ET4: effect scope errors */
        case TUR_E0250_ROW_VAR_ESCAPES_SCOPE:            return "TUR-E0250";
        case TUR_E0253_EFFECT_NOT_IN_SCOPE:              return "TUR-E0253";
        case TUR_W0030_STRICT_EFFECTS_UNANNOTATED: return "TUR-W0030";
        case TUR_W0031_EFFECT_OVER_ANNOTATED:      return "TUR-W0031";
        case TUR_W0032_ROW_VAR_ALWAYS_CONCRETE:    return "TUR-W0032";
        case TUR_W0033_UNREACHABLE_HANDLER:        return "TUR-W0033";
        case TUR_W0034_ROW_VAR_GENERALISED:        return "TUR-W0034";
        /* U6: inline-C outside Unsafe annotation */
        case TUR_W0036_INLINE_C_MISSING_UNSAFE:    return "TUR-W0036";
        /* Phase C: narrow-width param in inline-C body */
        case TUR_W0037_INLINE_C_NARROW_PARAM:      return "TUR-W0037";
        case TUR_W0038_LINT_PANIC_SITE:            return "TUR-W0038";
        case TUR_W0039_METHOD_DEFN_CLASH:          return "TUR-W0039";
        case TUR_W0040_EVAL_UNKNOWN_CALL_RUNTIME_DISPATCH: return "TUR-W0040";
        case TUR_W0041_HIGH_ARITY:                 return "TUR-W0041";
        case TUR_W0042_SHADOWS_SPECIAL_FORM:       return "TUR-W0042";
        case TUR_W0043_SESSION_OP_IN_ASYNC:        return "TUR-W0043";
        /* LT1: Linear type errors */
        case TUR_E0100_LINEAR_DROPPED:             return "TUR-E0100";
        case TUR_E0101_LINEAR_USE_AFTER_CONSUME:   return "TUR-E0101";
        case TUR_E0102_LINEAR_COPY:                return "TUR-E0102";
        case TUR_E0103_LINEAR_IN_RC:               return "TUR-E0103";
        case TUR_E0104_LINEAR_BRANCH_MISMATCH:     return "TUR-E0104";
        case TUR_E0105_BORROW_ESCAPES_SCOPE:       return "TUR-E0105";
        case TUR_E0106_CYCLIC_LIFETIME:            return "TUR-E0106";
        case TUR_E0107_CAPTURED_FIELD_CONSUMED_IN_HANDLER: return "TUR-E0107";
        case TUR_E0108_REF_FIELD_MOVED_OUT_OF_BORROW: return "TUR-E0108";
        /* ST0: Substructural type errors */
        case TUR_E0150_AFFINE_USED_TWICE:          return "TUR-E0150";
        case TUR_E0151_RELEVANT_DROPPED:           return "TUR-E0151";
        /* UT1: Uniqueness type errors */
        case TUR_E0200_UNIQUE_ALIASED:             return "TUR-E0200";
        case TUR_E0201_UNIQUE_COPY:                return "TUR-E0201";
        case TUR_E0202_UNIQUE_IN_RC:               return "TUR-E0202";
        /* CONV-S6: product-shape construction errors */
        case TUR_E0292_MISSING_FIELD:             return "TUR-E0292";
        case TUR_E0293_DUPLICATE_FIELD:           return "TUR-E0293";
        case TUR_E0294_UNKNOWN_FIELD:             return "TUR-E0294";
        case TUR_E0295_BYVALUE_CARRIER_CAST:      return "TUR-E0295";
        case TUR_E0296_WITH_NOT_COPY:             return "TUR-E0296";
        case TUR_E0297_WITH_UNKNOWN_FIELD:        return "TUR-E0297";
        case TUR_E0298_WITH_DUPLICATE_FIELD:      return "TUR-E0298";
        case TUR_E0299_MIXED_POS_KW:              return "TUR-E0299";
        /* IT1: Union type errors */
        case TUR_E0300_UNION_TYPE_MISMATCH:        return "TUR-E0300";
        case TUR_E0301_NON_EXHAUSTIVE_UNION_MATCH: return "TUR-E0301";
        case TUR_E0302_SEALED_OPAQUE_CAST:         return "TUR-E0302";
        case TUR_E0303_NON_NULL_OPAQUE_ZERO:       return "TUR-E0303";
        /* IT3: Intersection type errors */
        case TUR_E0350_INTERSECTION_UNSATISFIABLE:   return "TUR-E0350";
        case TUR_E0351_INTERSECTION_MEMBER_MISMATCH: return "TUR-E0351";
        /* RT3: refinement-type discharge */
        case TUR_E0370_REFINE_ILL_TYPED:          return "TUR-E0370";
        case TUR_E0371_REFINE_NOT_PROVED:         return "TUR-E0371";
        case TUR_W0372_REFINE_UNKNOWN:            return "TUR-W0372";
        case TUR_W0373_REFINE_NONLINEAR:          return "TUR-W0373";
        case TUR_E0374_REFINE_INSTANCE_STRONGER:  return "TUR-E0374";
        case TUR_E0375_REFINE_EFFECTFUL:          return "TUR-E0375";
        case TUR_E0376_REFINE_TYPE_PARAM:         return "TUR-E0376";
        case TUR_W0377_REFINE_INSTANCE_LENIENCY:  return "TUR-W0377";
        case TUR_E0378_REFINE_IN_FN_TYPE:         return "TUR-E0378";
        case TUR_I0379_REFINE_ORACLE_MISMATCH:    return "TUR-I0379";
        case TUR_W0380_REFINE_TYPE_ARG_UNENFORCED: return "TUR-W0380";
        case TUR_E0381_WRITES_FRAME_INVALID:       return "TUR-E0381";
        case TUR_E0382_WRITES_FRAME_EXCEEDED:      return "TUR-E0382";
        case TUR_W0383_READS_FRAME_OMITS_MUTABLE:  return "TUR-W0383";
        case TUR_E0384_REFLECT_NOT_TOTAL:          return "TUR-E0384";
        case TUR_W0385_REFLECT_FUEL_EXHAUSTED:     return "TUR-W0385";
        case TUR_E0390_CLASS_SUPERCLASS_PREAMBLE:         return "TUR-E0390";
        case TUR_E0391_CLASS_SUPERCLASS_UNRESOLVED:       return "TUR-E0391";
        case TUR_E0392_CLASS_SUPERCLASS_CYCLE:            return "TUR-E0392";
        case TUR_E0393_CLASS_SUPERCLASS_INSTANCE_MISSING: return "TUR-E0393";
        /* MS2: Multi-shot continuation capture analysis */
        case TUR_E0500_MULTISHOT_UNIQUE_CAPTURE:      return "TUR-E0500";
        case TUR_E0501_MULTISHOT_ANN_OUTSIDE_HANDLER: return "TUR-E0501";
        case TUR_E0502_MULTISHOT_RESUME_IN_ATOMIC:    return "TUR-E0502";
        /* SS0b: Session type errors */
        case TUR_E0210_SESSION_NOT_DUAL:         return "TUR-E0210";
        case TUR_E0211_SESSION_DROPPED:          return "TUR-E0211";
        case TUR_E0212_SESSION_PROTO_MISMATCH:   return "TUR-E0212";
        /* SS5: Global protocol type errors */
        case TUR_E0220_GLOBAL_NOT_PROJECTABLE:   return "TUR-E0220";
        case TUR_E0221_ROLE_NOT_DECLARED:        return "TUR-E0221";
        case TUR_E0222_ROLE_IMPL_MISMATCH:       return "TUR-E0222";
        case TUR_E0223_GLOBAL_NOT_WELLFORMED:    return "TUR-E0223";
        /* DV0-DV1: Dynamic var errors */
        case TUR_E0600_DYNVAR_SET_NOT_DYNAMIC:    return "TUR-E0600";
        case TUR_E0601_DYNVAR_SET_NO_BINDING:     return "TUR-E0601";
        case TUR_E0602_DYNVAR_TYPE_MISMATCH:      return "TUR-E0602";
        case TUR_E0603_DYNVAR_SUBSTRUCTURAL_TYPE: return "TUR-E0603";
        case TUR_E0604_DYNVAR_NOT_TOPLEVEL:       return "TUR-E0604";
        case TUR_E0605_DYNVAR_SET_IN_ATOMIC:      return "TUR-E0605";
        /* DV0: Dynamic var naming warning */
        case TUR_W0600_DYNVAR_NO_EARMUFFS:        return "TUR-W0600";
        /* Deprecation band */
        case TUR_D0001_FN_TYPE_COLON:             return "TUR-D0001";
        case TUR_D0002_FX_ROW_LEGACY_HASH:        return "TUR-D0002";
        case TUR_D0003_FX_ROW_LEGACY_AT:          return "TUR-D0003";
        case TUR_D0004_NONEXHAUSTIVE_FX_MARKER:   return "TUR-D0004";
        /* XF: experimental-flag mechanism */
        case TUR_E0310_UNKNOWN_EXPERIMENT:        return "TUR-E0310";
        case TUR_E0311_UNKNOWN_ENGINE:            return "TUR-E0311";
        case TUR_E0312_SAFFRON_STATIC_ONLY:       return "TUR-E0312";
        case TUR_W0060_EXPERIMENTAL_PROTOTYPE:    return "TUR-W0060";
        case TUR_W0061_EXPERIMENTAL_BETA:         return "TUR-W0061";
        case TUR_E0023_BIND_VOID_EXPRESSION:      return "TUR-E0023";
        case TUR_E0024_READS_FRAME_INVALID:       return "TUR-E0024";
        case TUR_E0620_EXPORTS_FX_ROW:            return "TUR-E0620";
        case TUR_E0621_TUR_VERSION_BELOW_FLOOR:   return "TUR-E0621";
        case TUR_E0622_TUR_VERSION_MALFORMED:     return "TUR-E0622";
        case TUR_W0623_TUR_VERSION_ABOVE_CEILING: return "TUR-W0623";
        case TUR_W0624_NO_ENTRY_POINT_NEAR_MISS:   return "TUR-W0624";
        case TUR_W0706_IMAGE_GLOBAL_UNREGISTERED:  return "TUR-W0706";
        case TUR_E0332_SWEET_DOLLAR_IN_BRACKETS:   return "TUR-E0332";
        case TUR_E0716_TAILCALL_NOT_TAIL:          return "TUR-E0716";
        default:                          return "";
    }
}

/* Phase HKT-P5: Return true if `s` looks like a diagnostic code string
 * of the form "TUR-E" (or W or D) followed by one or more decimal digits. */
bool diag_looks_like_code(const char *s) {
    if (!s) return false;
    /* Accept TUR-E####, TUR-W#### and TUR-D#### (deprecation band) */
    if (strncmp(s, "TUR-", 4) != 0) return false;
    const char *p = s + 4;
    if (*p != 'E' && *p != 'W' && *p != 'D') return false;
    p++;
    if (*p == '\0') return false;   /* need at least one digit */
    while (*p) {
        if (*p < '0' || *p > '9') return false;
        p++;
    }
    return true;
}

/* Phase HKT-P5: DiagCode lookup by string (inverse of diag_code_to_string). */
DiagCode diag_code_from_string(const char *s) {
    if (!s) return DIAG_CODE_NONE;
    if (strcmp(s, "TUR-E0001") == 0) return TUR_E0001_TYPE_MISMATCH;
    if (strcmp(s, "TUR-E0002") == 0) return TUR_E0002_ARITY_MISMATCH;
    if (strcmp(s, "TUR-E0003") == 0) return TUR_E0003_UNBOUND_SYMBOL;
    if (strcmp(s, "TUR-E0004") == 0) return TUR_E0004_INVALID_SCOPE;
    if (strcmp(s, "TUR-E0005") == 0) return TUR_E0005_USE_AFTER_MOVE;
    if (strcmp(s, "TUR-E0006") == 0) return TUR_E0006_OPERATOR_LOOKUP_FAILED;
    if (strcmp(s, "TUR-E0007") == 0) return TUR_E0007_CAPTURE_ERROR;
    if (strcmp(s, "TUR-E0009") == 0) return TUR_E0009_EFFECT_ROW_MISMATCH;
    if (strcmp(s, "TUR-E0010") == 0) return TUR_E0010_NOT_SEND;
    if (strcmp(s, "TUR-E0011") == 0) return TUR_E0011_NOT_SYNC;
    if (strcmp(s, "TUR-E0012") == 0) return TUR_E0012_KIND_MISMATCH;
    if (strcmp(s, "TUR-E0013") == 0) return TUR_E0013_ORPHAN_INSTANCE;
    if (strcmp(s, "TUR-E0015") == 0) return TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED;
    if (strcmp(s, "TUR-E0014") == 0) return TUR_E0014_NOT_CLONE;
    if (strcmp(s, "TUR-E0016") == 0) return TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET;
    if (strcmp(s, "TUR-E0017") == 0) return TUR_E0017_CONT_ESCAPE_ASYNC;
    if (strcmp(s, "TUR-E0018") == 0) return TUR_E0018_NOT_SERIALIZABLE;
    if (strcmp(s, "TUR-E0019") == 0) return TUR_E0019_SERIAL_SHIFT_OUTSIDE_RESET;
    if (strcmp(s, "TUR-E0021") == 0) return TUR_E0021_PRIVATE_EFFECT;
    if (strcmp(s, "TUR-E0022") == 0) return TUR_E0022_AWAIT_LIVE_NOT_SEND;
    if (strcmp(s, "TUR-E0025") == 0) return TUR_E0025_DUPLICATE_INSTANCE;
    if (strcmp(s, "TUR-E0026") == 0) return TUR_E0026_UNKNOWN_EFFECT_IN_ROW;
    if (strcmp(s, "TUR-E0042") == 0) return TUR_E0042_MIXED_WIDTH_ARITH;
    if (strcmp(s, "TUR-E0254") == 0) return TUR_E0254_INFINITE_EFFECT_ROW;
    if (strcmp(s, "TUR-E0260") == 0) return TUR_E0260_SIZED_TYPE_MISMATCH;
    /* ET3: handler typing errors */
    if (strcmp(s, "TUR-E0251") == 0) return TUR_E0251_HANDLER_OVERLAP;
    if (strcmp(s, "TUR-E0252") == 0) return TUR_E0252_HANDLER_RESULT_MISMATCH;
    /* CF4: gated call/cc / escape */
    if (strcmp(s, "TUR-E0700") == 0) return TUR_E0700_CALLCC_GATED;
    if (strcmp(s, "TUR-E0701") == 0) return TUR_E0701_ESCAPE_GATED;
    /* CF5: always-on generator limitation diagnostics */
    if (strcmp(s, "TUR-E0702") == 0) return TUR_E0702_YIELD_IN_MATCH_ARM;
    if (strcmp(s, "TUR-E0703") == 0) return TUR_E0703_YIELD_IN_RECURSIVE_GEN;
    /* CF3: gated first-class handler composition */
    if (strcmp(s, "TUR-E0704") == 0) return TUR_E0704_HANDLER_COMPOSE_UNIMPL;
    if (strcmp(s, "TUR-E0705") == 0) return TUR_E0705_POLY_CLOSURE_RESULT_TYVAR;
    if (strcmp(s, "TUR-E0706") == 0) return TUR_E0706_SERIAL_CONTEXT_NOT_CAPTURABLE;
    if (strcmp(s, "TUR-E0707") == 0) return TUR_E0707_RETURN_REGISTER_CLASS_MISMATCH;
    if (strcmp(s, "TUR-E0708") == 0) return TUR_E0708_RETURN_POINTER_SCALAR_MISMATCH;
    if (strcmp(s, "TUR-E0709") == 0) return TUR_E0709_RETURN_TYPE_MISMATCH;
    if (strcmp(s, "TUR-E0710") == 0) return TUR_E0710_CLONEABLE_CONTEXT_NOT_CAPTURABLE;
    if (strcmp(s, "TUR-E0711") == 0) return TUR_E0711_MODULE_TOPLEVEL_EXPR;
    if (strcmp(s, "TUR-E0712") == 0) return TUR_E0712_EXPR_NESTING_TOO_DEEP;
    if (strcmp(s, "TUR-E0713") == 0) return TUR_E0713_DEFINITION_IN_TAIL_POSITION;
    if (strcmp(s, "TUR-E0714") == 0) return TUR_E0714_NICHE_ELEMENT_ERASED_STORE;
    if (strcmp(s, "TUR-E0715") == 0) return TUR_E0715_NICHE_ELEMENT_ERASED_COMPARATOR;
    /* ET4: effect scope errors */
    if (strcmp(s, "TUR-E0250") == 0) return TUR_E0250_ROW_VAR_ESCAPES_SCOPE;
    if (strcmp(s, "TUR-E0253") == 0) return TUR_E0253_EFFECT_NOT_IN_SCOPE;
    if (strcmp(s, "TUR-W0030") == 0) return TUR_W0030_STRICT_EFFECTS_UNANNOTATED;
    if (strcmp(s, "TUR-W0031") == 0) return TUR_W0031_EFFECT_OVER_ANNOTATED;
    if (strcmp(s, "TUR-W0032") == 0) return TUR_W0032_ROW_VAR_ALWAYS_CONCRETE;
    if (strcmp(s, "TUR-W0033") == 0) return TUR_W0033_UNREACHABLE_HANDLER;
    if (strcmp(s, "TUR-W0034") == 0) return TUR_W0034_ROW_VAR_GENERALISED;
    /* U6: inline-C outside Unsafe annotation */
    if (strcmp(s, "TUR-W0036") == 0) return TUR_W0036_INLINE_C_MISSING_UNSAFE;
    if (strcmp(s, "TUR-W0040") == 0) return TUR_W0040_EVAL_UNKNOWN_CALL_RUNTIME_DISPATCH;
    if (strcmp(s, "TUR-W0041") == 0) return TUR_W0041_HIGH_ARITY;
    if (strcmp(s, "TUR-W0042") == 0) return TUR_W0042_SHADOWS_SPECIAL_FORM;
    if (strcmp(s, "TUR-W0043") == 0) return TUR_W0043_SESSION_OP_IN_ASYNC;
    /* LT1: Linear type errors */
    if (strcmp(s, "TUR-E0100") == 0) return TUR_E0100_LINEAR_DROPPED;
    if (strcmp(s, "TUR-E0101") == 0) return TUR_E0101_LINEAR_USE_AFTER_CONSUME;
    if (strcmp(s, "TUR-E0102") == 0) return TUR_E0102_LINEAR_COPY;
    if (strcmp(s, "TUR-E0103") == 0) return TUR_E0103_LINEAR_IN_RC;
    if (strcmp(s, "TUR-E0104") == 0) return TUR_E0104_LINEAR_BRANCH_MISMATCH;
    if (strcmp(s, "TUR-E0105") == 0) return TUR_E0105_BORROW_ESCAPES_SCOPE;
    if (strcmp(s, "TUR-E0106") == 0) return TUR_E0106_CYCLIC_LIFETIME;
    if (strcmp(s, "TUR-E0107") == 0) return TUR_E0107_CAPTURED_FIELD_CONSUMED_IN_HANDLER;
    if (strcmp(s, "TUR-E0108") == 0) return TUR_E0108_REF_FIELD_MOVED_OUT_OF_BORROW;
    /* ST0: Substructural type errors */
    if (strcmp(s, "TUR-E0150") == 0) return TUR_E0150_AFFINE_USED_TWICE;
    if (strcmp(s, "TUR-E0151") == 0) return TUR_E0151_RELEVANT_DROPPED;
    /* UT1: Uniqueness type errors */
    if (strcmp(s, "TUR-E0200") == 0) return TUR_E0200_UNIQUE_ALIASED;
    if (strcmp(s, "TUR-E0201") == 0) return TUR_E0201_UNIQUE_COPY;
    if (strcmp(s, "TUR-E0202") == 0) return TUR_E0202_UNIQUE_IN_RC;
    /* CONV-S6: product-shape construction errors */
    if (strcmp(s, "TUR-E0292") == 0) return TUR_E0292_MISSING_FIELD;
    if (strcmp(s, "TUR-E0293") == 0) return TUR_E0293_DUPLICATE_FIELD;
    if (strcmp(s, "TUR-E0294") == 0) return TUR_E0294_UNKNOWN_FIELD;
    if (strcmp(s, "TUR-E0295") == 0) return TUR_E0295_BYVALUE_CARRIER_CAST;
    if (strcmp(s, "TUR-E0296") == 0) return TUR_E0296_WITH_NOT_COPY;
    if (strcmp(s, "TUR-E0297") == 0) return TUR_E0297_WITH_UNKNOWN_FIELD;
    if (strcmp(s, "TUR-E0298") == 0) return TUR_E0298_WITH_DUPLICATE_FIELD;
    if (strcmp(s, "TUR-E0299") == 0) return TUR_E0299_MIXED_POS_KW;
    /* IT1: Union type errors */
    if (strcmp(s, "TUR-E0300") == 0) return TUR_E0300_UNION_TYPE_MISMATCH;
    if (strcmp(s, "TUR-E0301") == 0) return TUR_E0301_NON_EXHAUSTIVE_UNION_MATCH;
    if (strcmp(s, "TUR-E0302") == 0) return TUR_E0302_SEALED_OPAQUE_CAST;
    if (strcmp(s, "TUR-E0303") == 0) return TUR_E0303_NON_NULL_OPAQUE_ZERO;
    /* IT3: Intersection type errors */
    if (strcmp(s, "TUR-E0350") == 0) return TUR_E0350_INTERSECTION_UNSATISFIABLE;
    if (strcmp(s, "TUR-E0351") == 0) return TUR_E0351_INTERSECTION_MEMBER_MISMATCH;
    /* RT3: refinement-type discharge */
    if (strcmp(s, "TUR-E0370") == 0) return TUR_E0370_REFINE_ILL_TYPED;
    if (strcmp(s, "TUR-E0371") == 0) return TUR_E0371_REFINE_NOT_PROVED;
    if (strcmp(s, "TUR-W0372") == 0) return TUR_W0372_REFINE_UNKNOWN;
    if (strcmp(s, "TUR-W0373") == 0) return TUR_W0373_REFINE_NONLINEAR;
    if (strcmp(s, "TUR-E0374") == 0) return TUR_E0374_REFINE_INSTANCE_STRONGER;
    if (strcmp(s, "TUR-E0375") == 0) return TUR_E0375_REFINE_EFFECTFUL;
    if (strcmp(s, "TUR-E0376") == 0) return TUR_E0376_REFINE_TYPE_PARAM;
    if (strcmp(s, "TUR-W0377") == 0) return TUR_W0377_REFINE_INSTANCE_LENIENCY;
    if (strcmp(s, "TUR-E0378") == 0) return TUR_E0378_REFINE_IN_FN_TYPE;
    if (strcmp(s, "TUR-I0379") == 0) return TUR_I0379_REFINE_ORACLE_MISMATCH;
    if (strcmp(s, "TUR-W0380") == 0) return TUR_W0380_REFINE_TYPE_ARG_UNENFORCED;
    if (strcmp(s, "TUR-E0381") == 0) return TUR_E0381_WRITES_FRAME_INVALID;
    if (strcmp(s, "TUR-E0382") == 0) return TUR_E0382_WRITES_FRAME_EXCEEDED;
    if (strcmp(s, "TUR-W0383") == 0) return TUR_W0383_READS_FRAME_OMITS_MUTABLE;
    if (strcmp(s, "TUR-E0384") == 0) return TUR_E0384_REFLECT_NOT_TOTAL;
    if (strcmp(s, "TUR-W0385") == 0) return TUR_W0385_REFLECT_FUEL_EXHAUSTED;
    if (strcmp(s, "TUR-E0390") == 0) return TUR_E0390_CLASS_SUPERCLASS_PREAMBLE;
    if (strcmp(s, "TUR-E0391") == 0) return TUR_E0391_CLASS_SUPERCLASS_UNRESOLVED;
    if (strcmp(s, "TUR-E0392") == 0) return TUR_E0392_CLASS_SUPERCLASS_CYCLE;
    if (strcmp(s, "TUR-E0393") == 0) return TUR_E0393_CLASS_SUPERCLASS_INSTANCE_MISSING;
    /* MS2: Multi-shot continuation capture analysis */
    if (strcmp(s, "TUR-E0500") == 0) return TUR_E0500_MULTISHOT_UNIQUE_CAPTURE;
    if (strcmp(s, "TUR-E0501") == 0) return TUR_E0501_MULTISHOT_ANN_OUTSIDE_HANDLER;
    if (strcmp(s, "TUR-E0502") == 0) return TUR_E0502_MULTISHOT_RESUME_IN_ATOMIC;
    /* SS0b: Session type errors */
    if (strcmp(s, "TUR-E0210") == 0) return TUR_E0210_SESSION_NOT_DUAL;
    if (strcmp(s, "TUR-E0211") == 0) return TUR_E0211_SESSION_DROPPED;
    if (strcmp(s, "TUR-E0212") == 0) return TUR_E0212_SESSION_PROTO_MISMATCH;
    /* SS5: Global protocol type errors */
    if (strcmp(s, "TUR-E0220") == 0) return TUR_E0220_GLOBAL_NOT_PROJECTABLE;
    if (strcmp(s, "TUR-E0221") == 0) return TUR_E0221_ROLE_NOT_DECLARED;
    if (strcmp(s, "TUR-E0222") == 0) return TUR_E0222_ROLE_IMPL_MISMATCH;
    if (strcmp(s, "TUR-E0223") == 0) return TUR_E0223_GLOBAL_NOT_WELLFORMED;
    /* DV0-DV1: Dynamic var errors */
    if (strcmp(s, "TUR-E0600") == 0) return TUR_E0600_DYNVAR_SET_NOT_DYNAMIC;
    if (strcmp(s, "TUR-E0601") == 0) return TUR_E0601_DYNVAR_SET_NO_BINDING;
    if (strcmp(s, "TUR-E0602") == 0) return TUR_E0602_DYNVAR_TYPE_MISMATCH;
    if (strcmp(s, "TUR-E0603") == 0) return TUR_E0603_DYNVAR_SUBSTRUCTURAL_TYPE;
    if (strcmp(s, "TUR-E0604") == 0) return TUR_E0604_DYNVAR_NOT_TOPLEVEL;
    if (strcmp(s, "TUR-E0605") == 0) return TUR_E0605_DYNVAR_SET_IN_ATOMIC;
    /* DV0: Dynamic var naming warning */
    if (strcmp(s, "TUR-W0600") == 0) return TUR_W0600_DYNVAR_NO_EARMUFFS;
    /* Deprecation band */
    if (strcmp(s, "TUR-D0001") == 0) return TUR_D0001_FN_TYPE_COLON;
    if (strcmp(s, "TUR-D0002") == 0) return TUR_D0002_FX_ROW_LEGACY_HASH;
    if (strcmp(s, "TUR-D0003") == 0) return TUR_D0003_FX_ROW_LEGACY_AT;
    if (strcmp(s, "TUR-D0004") == 0) return TUR_D0004_NONEXHAUSTIVE_FX_MARKER;
    /* XF: experimental-flag mechanism */
    if (strcmp(s, "TUR-E0310") == 0) return TUR_E0310_UNKNOWN_EXPERIMENT;
    if (strcmp(s, "TUR-E0311") == 0) return TUR_E0311_UNKNOWN_ENGINE;
    if (strcmp(s, "TUR-E0312") == 0) return TUR_E0312_SAFFRON_STATIC_ONLY;
    if (strcmp(s, "TUR-W0060") == 0) return TUR_W0060_EXPERIMENTAL_PROTOTYPE;
    if (strcmp(s, "TUR-W0061") == 0) return TUR_W0061_EXPERIMENTAL_BETA;
    if (strcmp(s, "TUR-E0023") == 0) return TUR_E0023_BIND_VOID_EXPRESSION;
    if (strcmp(s, "TUR-E0024") == 0) return TUR_E0024_READS_FRAME_INVALID;
    if (strcmp(s, "TUR-E0620") == 0) return TUR_E0620_EXPORTS_FX_ROW;
    if (strcmp(s, "TUR-E0621") == 0) return TUR_E0621_TUR_VERSION_BELOW_FLOOR;
    if (strcmp(s, "TUR-E0622") == 0) return TUR_E0622_TUR_VERSION_MALFORMED;
    if (strcmp(s, "TUR-W0623") == 0) return TUR_W0623_TUR_VERSION_ABOVE_CEILING;
    if (strcmp(s, "TUR-W0624") == 0) return TUR_W0624_NO_ENTRY_POINT_NEAR_MISS;
    if (strcmp(s, "TUR-W0706") == 0) return TUR_W0706_IMAGE_GLOBAL_UNREGISTERED;
    if (strcmp(s, "TUR-E0332") == 0) return TUR_E0332_SWEET_DOLLAR_IN_BRACKETS;
    if (strcmp(s, "TUR-E0716") == 0) return TUR_E0716_TAILCALL_NOT_TAIL;
    return DIAG_CODE_NONE;
}

/* Phase HKT-P5: Long-form explanation table (modelled on rustc --explain).
 * Each entry maps a DiagCode to a multi-paragraph explanation string. */
typedef struct DiagExplanation {
    DiagCode    code;
    const char *text;
} DiagExplanation;

static const DiagExplanation diag_explanations_[] = {
    { TUR_E0001_TYPE_MISMATCH,
      "TUR-E0001: Type mismatch\n"
      "\n"
      "The type of an expression does not match the type expected in that\n"
      "position.\n"
      "\n"
      "Example:\n"
      "  (defn add [x :int y :int] :int (+ x y))\n"
      "  (add 1 true)   ; error: expected :int, got :bool\n"
      "\n"
      "Ensure that argument types, return types, and binding types are\n"
      "consistent.  Explicit type annotations help the compiler pinpoint the\n"
      "mismatch.\n"
    },
    { TUR_E0042_MIXED_WIDTH_ARITH,
      "TUR-E0042: Mixed-width numeric arithmetic\n"
      "\n"
      "An arithmetic or comparison operator received operands of distinct\n"
      "numeric kinds.  Turmeric does not implicitly widen or narrow numeric\n"
      "values -- every conversion must be written explicitly with (as ...).\n"
      "\n"
      "Example:\n"
      "  (+ 1i8 2i16)    ; error: int8 and int16 cannot be mixed\n"
      "  (+ 1i8 (as int8 2i16))  ; ok: explicit narrowing cast\n"
      "  (+ (as int16 1i8) 2i16) ; ok: explicit widening cast\n"
      "\n"
      "  (+ 1i32 2)      ; error: int32 and int (64-bit) cannot be mixed\n"
      "  (+ 1i32 2i32)   ; ok: same kind on both sides\n"
      "\n"
      "Rationale: implicit numeric coercion hides ABI-level representation\n"
      "changes and makes it easy to accidentally widen to a 64-bit carrier\n"
      "where a 32-bit computation was intended.  Explicit casts make the\n"
      "intent clear and keep the emitted C free of silent int64 round-trips.\n"
    },
    { TUR_E0260_SIZED_TYPE_MISMATCH,
      "TUR-E0260: Sized type mismatch\n"
      "\n"
      "Two size indices that are both statically known reduce to different\n"
      "natural numbers, so a sized-types check that requires them to be equal\n"
      "(or compatible) can never hold.  This is reported at compile time --\n"
      "no runtime assertion is emitted -- by the -Xsized-types static checker.\n"
      "\n"
      "Example:\n"
      "  (size-assert-eq! (size-static 4) (size-static 5))\n"
      "    ; error TUR-E0260: sized type mismatch: size 4 is not 5\n"
      "\n"
      "  (size-assert-eq! (size-static 4) (size-add (size-static 2)\n"
      "                                             (size-static 2)))\n"
      "    ; ok: both sides reduce to 4\n"
      "\n"
      "The same check reconciles a WRITTEN size claim with the value it\n"
      "describes, so a wrong annotation cannot launder a mismatch past the\n"
      "argument checks that trust it:\n"
      "\n"
      "  (defn bogus [] : (LaMatN (Static 5) (Static 5)) (mk23))\n"
      "    ; error TUR-E0260: 'bogus' declares return size 5 but its body\n"
      "    ; has size 2\n"
      "\n"
      "  (:: (mk4) (SizedBuf (Static 3)))\n"
      "    ; error TUR-E0260: ascription declares size 3 but the expression\n"
      "    ; has size 4\n"
      "\n"
      "An ascription whose inner expression has an OPEN index (for example\n"
      "`(:: (sized-buf-new-zeroed 4) (SizedBuf (Static 4)))`, where the\n"
      "constructor's `n` is polymorphic) is the legitimate pin-by-ascription\n"
      "idiom: it is accepted, and the pinned index then takes part in\n"
      "cross-parameter unification like a declared return type would.\n"
      "\n"
      "Fallback: when at least one size is NOT statically known (for example a\n"
      "dimension derived from a runtime length), the checker cannot decide the\n"
      "equation and emits the existing runtime assertion instead of this error\n"
      "-- it never silently accepts a possibly-wrong size.\n"
      "\n"
      "Fix: make the two sizes agree, or, if they genuinely cannot be known to\n"
      "match until run time, route the value through a size whose index is a\n"
      "variable rather than a literal so the check falls back to run time.\n"
    },
    { TUR_E0002_ARITY_MISMATCH,
      "TUR-E0002: Arity mismatch\n"
      "\n"
      "A function call provides a different number of arguments than the\n"
      "function declares parameters.\n"
      "\n"
      "Example:\n"
      "  (defn add [x :int y :int] :int (+ x y))\n"
      "  (add 1)   ; error: expected 2 args, got 1\n"
      "\n"
      "Count the parameters in the function definition and match the number\n"
      "of arguments at the call site.\n"
    },
    { TUR_E0003_UNBOUND_SYMBOL,
      "TUR-E0003: Unbound symbol\n"
      "\n"
      "A name is used that has not been defined in any enclosing scope.\n"
      "\n"
      "Common causes:\n"
      "  - Typo in the name.\n"
      "  - Forgetting to import or require the file that defines the name.\n"
      "  - Using a name before it is defined (Turmeric is sequential, not\n"
      "    mutually recursive by default).\n"
    },
    { TUR_E0004_INVALID_SCOPE,
      "TUR-E0004: Invalid scope\n"
      "\n"
      "A form is used in a position where it is not allowed.  For example,\n"
      "using a top-level-only form (defn, defstruct) inside an expression\n"
      "context, or a control-flow form outside a function body.\n"
    },
    { TUR_E0714_NICHE_ELEMENT_ERASED_STORE,
      "TUR-E0714: Niche-represented element stored through an erased container access\n"
      "\n"
      "By default (the Option niche; TUR_OPTION_NICHE=0 restores the tagged\n"
      "form) an eligible (Option P) is carried as its\n"
      "payload pointer, and a Vec stores such an element in its slot as that\n"
      "one word (container-element-form-plan, CE2).  The slot convention is\n"
      "decided per element monomorph, so every store and read must be able to\n"
      "name the element type.  This store cannot: the receiver's element type\n"
      "is still erased here (a raw :int-typed receiver, or an unresolved type\n"
      "variable), so it would put a second convention into the same vec that\n"
      "no reader can tell apart from the first.\n"
      "\n"
      "Ascribe the receiver to its concrete element type at this site, e.g.\n"
      "  (vec-push! (:: v (Vec (Option String))) x)\n"
      "or give the helper a typed (Vec A) parameter so the call specializes.\n"
    },
    { TUR_E0715_NICHE_ELEMENT_ERASED_COMPARATOR,
      "TUR-E0715: Erased comparator over a niche-represented Vec element\n"
      "\n"
      "`vec-eq?` hands its comparator RAW Vec slot words -- its inline-C loop\n"
      "passes a->data[i] straight through -- rather than values that have\n"
      "crossed the carrier boundary.  (So do list-eq?, option-eq?, result-eq?\n"
      "and pair-eq?, from their own payload slots; map-eq? and set-eq-cmp?\n"
      "are the exception and hand over a HAMT-stored box.)  A niche\n"
      "(Option P) element rides its slot as the payload pointer itself, not\n"
      "as a carrier box (container-element-form-plan, CE2), so a comparator\n"
      "that ascribes a parameter back to the element type has to reinterpret\n"
      "that word.  Unboxing it instead reads the payload's own first words as\n"
      "a tag and a value: a silent wrong answer, not a crash.\n"
      "\n"
      "A comparator written INLINE at the call is marked as receiving slot\n"
      "words and compiles correctly, and so does one whose parameters are\n"
      "declared as the element type.  A NAMED function with ERASED parameters\n"
      "cannot be: it is elaborated once and the same function may also be\n"
      "called with genuine boxes, so it has no decidable convention here.\n"
      "\n"
      "Declare the parameter types, which bridges at the closure boundary:\n"
      "  (vec-eq? v w (fn [a : (Option String) b : (Option String)] : bool\n"
      "                 (eq? a b)))\n"
      "or write the comparator inline at the call site.\n"
    },
    { TUR_E0005_USE_AFTER_MOVE,
      "TUR-E0005: Use after move\n"
      "\n"
      "A value was moved (transferred to another binding or passed to a\n"
      "function that consumes it) and then used again after the move.\n"
      "\n"
      "Example:\n"
      "  (let [r (ref 42)]\n"
      "    (let [s r]     ; r is moved into s\n"
      "      (deref r)))  ; error: r was already moved\n"
      "\n"
      "Clone or copy the value before moving if you need to use it again,\n"
      "or restructure the code so ownership is only held in one place at a\n"
      "time.\n"
    },
    { TUR_E0007_CAPTURE_ERROR,
      "TUR-E0007: Capture error\n"
      "\n"
      "A closure or effect-handler case attempts to capture a variable that\n"
      "cannot safely be captured in that context.\n"
      "\n"
      "Effect handler case bodies are emitted as separate C functions with\n"
      "no access to the enclosing stack frame, so borrow-typed (&T / &mut T)\n"
      "variables from the enclosing scope cannot be captured by them.\n"
      "\n"
      "A capturing closure also cannot be passed to an effect-annotated\n"
      "(fn ...) parameter that is cfnptr, variadic, or has more than 5\n"
      "parameters: those shapes keep the thin calling convention, which\n"
      "has no slot for a closure environment.  Reduce the signature, or\n"
      "pass the captured state as explicit arguments.  (Ordinary\n"
      "effect-annotated parameters take capturing closures as of the\n"
      "2026-08-16 fat-normalization increment.)\n"
    },
    { TUR_E0009_EFFECT_ROW_MISMATCH,
      "TUR-E0009: Effect-row mismatch\n"
      "\n"
      "The effect row of a function call or expression does not match the\n"
      "effect row expected by the enclosing context.\n"
      "\n"
      "Ensure that any algebraic effects used inside a function are listed\n"
      "in its effect annotation (e.g., #{IO Write}) and that `handle`\n"
      "blocks cover all effects that are performed inside them.\n"
    },
    { TUR_E0010_NOT_SEND,
      "TUR-E0010: Type is not Send\n"
      "\n"
      "An attempt was made to transfer a value of a non-Send type across a\n"
      "thread boundary (e.g., passing it to (spawn ...) or storing it in a\n"
      "channel).\n"
      "\n"
      "Non-Send types include:\n"
      "  - rc<T>  (reference-counted; not thread-safe)\n"
      "  - weak<T> (weak reference into an rc; not thread-safe)\n"
      "  - Borrow types (&T, &mut T) whose lifetimes do not cross threads.\n"
      "\n"
      "Use arc<T> (atomic reference count) or plain values for data that\n"
      "must cross thread boundaries.\n"
    },
    { TUR_E0011_NOT_SYNC,
      "TUR-E0011: Type is not Sync\n"
      "\n"
      "An attempt was made to share a reference to a non-Sync type across\n"
      "thread boundaries (e.g., placing it behind a shared atomic).\n"
      "\n"
      "Non-Sync types include types with interior mutability that is not\n"
      "protected by a synchronization primitive such as mutex<T> or\n"
      "rwlock<T>.\n"
      "\n"
      "Wrap the value in mutex<T> or rwlock<T> to make shared mutable access\n"
      "thread-safe.\n"
    },
    { TUR_E0012_KIND_MISMATCH,
      "TUR-E0012: Kind mismatch\n"
      "\n"
      "A type expression is used where a type constructor of a different\n"
      "kind is expected.\n"
      "\n"
      "Kinds describe the 'type of a type':\n"
      "  *        — a fully applied, concrete type  (e.g., int, bool, vec<int>)\n"
      "  * -> *   — a unary type constructor        (e.g., vec, option, rc)\n"
      "  * -> * -> *  — a binary type constructor   (e.g., result, either)\n"
      "\n"
      "Example:\n"
      "  (defclass Functor [^f]\n"
      "    (fmap [container fn] :int))\n"
      "  (definstance Functor [int])\n"
      "  ; error: Functor expects a type constructor (kind * -> *),\n"
      "  ; but int has kind *.\n"
      "\n"
      "Pass a type constructor (vec, option, rc, …) where kind '* -> *' is\n"
      "required, and a concrete type (int, bool, …) where kind '*' is\n"
      "required.\n"
    },
    { TUR_E0013_ORPHAN_INSTANCE,
      "TUR-E0013: Orphan instance\n"
      "\n"
      "A typeclass instance is defined in a file that owns neither the\n"
      "typeclass nor any of the type arguments of the instance.\n"
      "\n"
      "This mirrors the Rust / Haskell 'orphan rule': to avoid conflicting\n"
      "instances when multiple modules are combined, every instance must be\n"
      "defined in the same module as either the typeclass or at least one\n"
      "of the concrete types it is instantiated for.\n"
      "\n"
      "In Turmeric v1 (single-file compilation) this diagnostic is advisory\n"
      "(a warning, not an error) because all definitions share a single file.\n"
      "It will be promoted to a hard error once the module system lands\n"
      "(see P19-6).\n"
      "\n"
      "To silence the warning, move the instance definition into the file\n"
      "that defines the typeclass or the file that defines one of the types\n"
      "used as type arguments.\n"
    },
    { TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED,
      "TUR-E0015: Typeclass constraint not satisfied\n"
      "\n"
      "A typeclass instance declaration includes a constraint that cannot be\n"
      "satisfied because no matching typeclass instance exists.\n"
      "\n"
      "Example:\n"
      "  (defclass Clone [a] (clone [x : a] : a))\n"
      "  (definstance Clone [int] (clone [x] x))\n"
      "  (definstance Clone [Pair a b] [Clone a] ...)  ; error if Clone[a] missing\n"
      "\n"
      "In Phase PTC2 (v1), constraints are only validated for primitive types\n"
      "(int, bool, cstr, nil, float, ptr<void>). Constraints on user-defined\n"
      "types are stored but not validated until PTC3 lands.\n"
      "\n"
      "Define an instance of the required typeclass for the constrained type,\n"
      "or remove the constraint if it is not needed.\n"
    },
    { TUR_E0025_DUPLICATE_INSTANCE,
      "TUR-E0025: Instance already defined for this class and type\n"
      "\n"
      "A definstance names a (typeclass, type) pair that already has an\n"
      "instance -- usually one the autoloaded stdlib supplies, such as\n"
      "Eq [int] or Show [cstr].  Instances are unique per class and type:\n"
      "there is exactly one dictionary to dispatch to, so a second definition\n"
      "cannot coexist with the first, and it is rejected rather than silently\n"
      "ignored (which is what used to happen, with the first definition\n"
      "winning and the second having no effect).\n"
      "\n"
      "Example:\n"
      "  (definstance Eq [int]\n"
      "    (eq? [a b] : bool false))     ;; error: stdlib already defines Eq [int]\n"
      "\n"
      "To give a primitive different behaviour under a class, wrap it in a\n"
      "newtype and define the instance for the newtype:\n"
      "  (defopaque Loose :int)\n"
      "  (definstance Eq [Loose] (eq? [a b] : bool false))\n"
      "\n"
      "A stdlib file loaded twice (an explicit (load \"stdlib/...\") beside the\n"
      "autoload) is not a duplicate in this sense: the same definition arriving\n"
      "through two load paths stays a silent no-op.\n"
    },
    { TUR_E0026_UNKNOWN_EFFECT_IN_ROW,
      "TUR-E0026: Unknown effect in an effect row\n"
      "\n"
      "An uppercase name inside #fx{...} must be an effect that some\n"
      "defeffect in the program declares.  (A lowercase name is a row\n"
      "variable and is not looked up.)  This one is not declared anywhere\n"
      "the compiler can see.\n"
      "\n"
      "Example:\n"
      "  (defn log-line [] #fx{Write} : int ...)   ;; error: no defeffect Write\n"
      "\n"
      "The usual cause is an effect from a module that is not loaded: Write,\n"
      "Read, Log, Fail and the rest of stdlib/effects.tur are not\n"
      "autoloaded.  Load it -- (load \"stdlib/effects.tur\") -- or declare the\n"
      "effect yourself.  (The capability tags IO, FS, Net, Proc and Rand are\n"
      "compiler-known and need no declaration; Unsafe likewise.)\n"
      "\n"
      "Compiler attributes that used to be written in the row are not\n"
      "effects: Construct is ^construct, ByVal is ^byval, and NonExhaustive\n"
      "is (match ^non-exhaustive ...).\n"
      "\n"
      "This used to be silent: the unknown name was dropped, so #fx{FS}\n"
      "checked as #fx{}, and a caller's #fx{} -- which should have failed\n"
      "with TUR-E0009 for calling an FS function -- passed a check that had\n"
      "quietly stopped happening.\n"
    },
    { TUR_E0014_NOT_CLONE,
      "TUR-E0014: Captured binding does not implement Clone\n"
      "\n"
      "A cloneable-shift captures a local variable whose type does not have a\n"
      "Clone instance.  Because cloneable continuations can be resumed multiple\n"
      "times, every value captured across the shift boundary must be deep-\n"
      "cloneable so that each clone of the continuation gets an independent copy.\n"
      "\n"
      "Example:\n"
      "  (let [x (make-non-cloneable)]\n"
      "    (cloneable-reset\n"
      "      (cloneable-shift (fn [k] k) x)))  ; error: x is not Clone\n"
      "\n"
      "Add a (definstance Clone [YourType] ...) to make the type cloneable,\n"
      "or restructure the code so the non-cloneable value is not captured.\n"
    },
    { TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET,
      "TUR-E0016: cloneable-shift outside cloneable-reset\n"
      "\n"
      "A cloneable-shift form was used outside of any enclosing cloneable-reset\n"
      "boundary.  cloneable-shift captures the current continuation up to the\n"
      "nearest enclosing cloneable-reset; without one, there is no continuation\n"
      "to capture.\n"
      "\n"
      "Example:\n"
      "  (defn bad [] :int\n"
      "    (cloneable-shift (fn [k] 42) 0))  ; error: no enclosing cloneable-reset\n"
      "\n"
      "Wrap the cloneable-shift in a cloneable-reset:\n"
      "  (cloneable-reset (cloneable-shift (fn [k] 42) 0))\n"
    },
    { TUR_E0017_CONT_ESCAPE_ASYNC,
      "TUR-E0017: Continuation escape into async scope\n"
      "\n"
      "An effect-handler continuation `k` was captured by an async block.  Effect\n"
      "handler continuations are bound to the fiber that performed the effect; if `k`\n"
      "is resumed from a different fiber (spawned by async), the fiber identity check\n"
      "in tur_cont_resume will fail at runtime with a fatal error.\n"
      "\n"
      "Example of the problem:\n"
      "  (defeffect GetK [] :int)\n"
      "  (handle (perform (GetK))\n"
      "    (GetK [] k)\n"
      "      (await (async (fn [] :int (resume k 42)))))  ; ERROR: k escapes\n"
      "\n"
      "Instead, resume the continuation synchronously inside the handler body:\n"
      "  (defeffect GetK [] :int)\n"
      "  (handle (perform (GetK))\n"
      "    (GetK [] k) (resume k 42))\n"
      "\n"
      "Or, if you need async work before resuming, perform the async computation and\n"
      "pass the result back synchronously:\n"
      "  (handle (perform (GetK))\n"
      "    (GetK [] k)\n"
      "      (let [v (await (async (fn [] :int 42)))]\n"
      "        (resume k v)))\n"
    },
    /* Phase 21: Serializable continuations */
    { TUR_E0018_NOT_SERIALIZABLE,
      "TUR-E0018: captured binding does not implement Serializable\n"
      "\n"
      "A serial-shift form captured a binding whose type does not implement the\n"
      "Serializable typeclass.  serial-shift requires every captured value to be\n"
      "round-trippable through bytes so the continuation can be marshalled to disk\n"
      "or sent over a network and resumed in a fresh process.\n"
      "\n"
      "Example:\n"
      "  (let [handle (open-file \"data.txt\")]\n"
      "    (serial-reset\n"
      "      (serial-shift (fn [k] k) 0)))  ; error: handle : file-handle is not Serializable\n"
      "\n"
      "Solutions:\n"
      "  1. Move non-serializable resources outside the serial-reset boundary.\n"
      "  2. Implement Serializable for the type with custom marshal/unmarshal hooks\n"
      "     (e.g., serialise a file path and re-open on resume).\n"
    },
    { TUR_E0019_SERIAL_SHIFT_OUTSIDE_RESET,
      "TUR-E0019: serial-shift used outside serial-reset boundary\n"
      "\n"
      "A serial-shift form appeared outside of any enclosing serial-reset.\n"
      "serial-shift captures the current continuation up to the nearest enclosing\n"
      "serial-reset; without one there is no delimited continuation to capture.\n"
      "\n"
      "Wrap the serial-shift in a serial-reset:\n"
      "  (serial-reset\n"
      "    (serial-shift (fn [k] (save-cont! k) 0) 42))\n"
    },
    { TUR_E0021_PRIVATE_EFFECT,
      "TUR-E0021: Private effect accessed outside its defining module\n"
      "\n"
      "An effect declared with ^private is only visible within the module that\n"
      "defined it.  Attempting to perform or handle a private effect from another\n"
      "module is a compile-time error.\n"
      "\n"
      "Example of the problem:\n"
      "  ; module mymod\n"
      "  (defmodule mymod\n"
      "    (defeffect ^private Ask [] :int)\n"
      "    (defn run [] :int (perform (Ask))))\n"
      "\n"
      "  ; module consumer -- ERROR\n"
      "  (handle (perform (Ask))       ; TUR-E0021: Ask is private to mymod\n"
      "    (Ask [] k) (resume k 42))\n"
      "\n"
      "Solutions:\n"
      "  1. Remove ^private from the defeffect declaration to make the effect\n"
      "     public and importable by other modules.\n"
      "  2. Expose a public function in mymod that encapsulates the effect and\n"
      "     provides the handler, so consumers never touch Ask directly.\n"
    },
    { TUR_E0254_INFINITE_EFFECT_ROW,
      "TUR-E0254: Infinite effect row (occurs-check failure)\n"
      "\n"
      "Unifying an effect row variable with a row that contains the same variable\n"
      "would produce an infinite effect row. This usually means a function with an\n"
      "open row variable is being passed to a parameter that constrains the same\n"
      "variable.\n"
      "\n"
      "Fix: ensure the actual argument's effect row does not contain the same row\n"
      "variable as the parameter's declared row, e.g.:\n"
      "  (defn run-e [f :(fn [] #{e} :nil)] #{e} :nil (f))\n"
      "  ;; pass a concrete-row function, not one with the same variable:\n"
      "  (defn pure-f [] #{} :nil nil)\n"
      "  (run-e pure-f)  ; ok\n",
    },
    { TUR_W0030_STRICT_EFFECTS_UNANNOTATED,
      "TUR-W0030: Unannotated effectful function (--strict-effects)\n"
      "\n"
      "Under --strict-effects, every function whose inferred effect row is non-empty\n"
      "should carry an explicit #fx{...} annotation. This warning fires when an\n"
      "unannotated function performs one or more effects -- a defn, a fn literal\n"
      "(named by where it is: \"anonymous function in 'f'\"), or an instance\n"
      "method (whose row is its defclass method's).\n"
      "\n"
      "Fix: add the effect-row annotation the message spells, e.g.:\n"
      "  (defn my-fn [] #fx{Write} :nil ...)\n"
      "  (fn [k] #fx{Bt} (bt-set! c 1))\n"
      "Or handle the effect inside the function so its row is empty.\n"
      "\n"
      "-Werror=strict-effects makes this an error (and implies --strict-effects).\n"
      "--lint-effects is a deprecated alias for --strict-effects.\n",
    },
    { TUR_W0031_EFFECT_OVER_ANNOTATED,
      "TUR-W0031: Over-annotated effect row\n"
      "\n"
      "The declared effect row contains an effect that the function never actually\n"
      "performs. This may indicate a stale annotation after refactoring.\n"
      "\n"
      "Fix: remove the unused effect from the #fx{...} annotation, e.g.:\n"
      "  (defn my-fn [] #fx{Write} :nil ...)  ; remove Log if it is never performed\n"
      "\n"
      "Not reported for a capability effect (^capability), which the annotation\n"
      "alone justifies, nor for an instance method, whose row is its class\n"
      "method's: an instance that performs less than its class allows is normal.\n",
    },
    { TUR_W0032_ROW_VAR_ALWAYS_CONCRETE,
      "TUR-W0032: Row variable is always instantiated to a concrete row\n"
      "\n"
      "A function declares a row variable (e.g., #{e}) but at every call site the\n"
      "variable is always bound to the same concrete effect set. Replacing the row\n"
      "variable with the concrete row makes the annotation more informative and\n"
      "allows the compiler to enforce it strictly.\n"
      "\n"
      "Fix: replace the row variable with the concrete effect set, e.g.:\n"
      "  (defn run-twice [f :(fn [] #fx{Ask} :int)] #fx{Ask} :int ...)\n"
      "\n"
      "Reported under --strict-effects; -Werror=strict-effects makes it an error.\n",
    },
    { TUR_W0033_UNREACHABLE_HANDLER,
      "TUR-W0033: Handler clause is unreachable\n"
      "\n"
      "A (handle ...) expression contains a handler clause for an effect that the\n"
      "handled body never actually performs. The clause will never be triggered.\n"
      "\n"
      "This usually means the effect was already handled by an inner (handle ...) or\n"
      "the body was refactored and the handler was not updated.\n"
      "\n"
      "Fix: remove the unreachable handler clause, or ensure the body performs the\n"
      "effect, e.g.:\n"
      "  (handle (perform (Write \"hi\"))         ; body actually performs Write\n"
      "    (Write [s] k) (do (println s) (resume k nil)))\n",
    },
    { TUR_W0034_ROW_VAR_GENERALISED,
      "TUR-W0034: Row variable was auto-generalised\n"
      "\n"
      "Under --strict-effects, a function whose effect-row annotation uses a bare\n"
      "row variable (e.g. #{e}) has that variable implicitly generalised as if\n"
      "you had written (forall [e] ...). This warning suggests making the\n"
      "quantification explicit for clarity.\n"
      "\n"
      "Fix: add an explicit forall binder:\n"
      "  ; Before (implicit generalisation triggers TUR-W0034):\n"
      "  (defn run [f :(fn [] #{e} :int)] #{e} :int (f))\n"
      "\n"
      "  ; After (explicit forall, no warning):\n"
      "  (defn run [f :(forall [e] fn [] #{e} :int)] #{e} :int (f))\n",
    },
    /* U6: inline-C outside Unsafe annotation */
    { TUR_W0036_INLINE_C_MISSING_UNSAFE,
      "TUR-W0036: Inline-C block in function not annotated #{Unsafe}\n"
      "\n"
      "A ```c ... ``` inline-C block appears in a function that does not declare the\n"
      "#{Unsafe} effect. Inline C bypasses the type system, the borrow checker, and\n"
      "all other safety guarantees -- it is inherently unsafe code.\n"
      "\n"
      "Fix: annotate the enclosing function with #{Unsafe}:\n"
      "  (defn my-fn [] #{Unsafe} :int\n"
      "    ```c return 42; ```)\n"
      "\n"
      "Alternatively, wrap the inline-C call site in (unsafe ...) if the function\n"
      "is a safe abstraction over an unsafe implementation:\n"
      "  (defn safe-fn [] :int (unsafe (raw-c-helper)))\n",
    },
    { TUR_W0043_SESSION_OP_IN_ASYNC,
      "TUR-W0043: Session op inside an async body may deadlock the compiled program\n"
      "\n"
      "An (async ...) body spells a session op (send, recv, offer, choose-left,\n"
      "choose-right, recv-timeout, send-to, recv-from, recv-timeout-from) on\n"
      "endpoints it makes itself, e.g.\n"
      "  (async (fn [] (let [[s r] (make-session (Send int Close))] ... (recv r))))\n"
      "\n"
      "An async body that CAPTURES an endpoint made outside it does not warn: it\n"
      "runs on its own OS thread, so its peer -- the spawner, or anything else --\n"
      "can run while it waits.  A body that makes both endpoints has both ends of\n"
      "the protocol in one straight-line body; a session op then blocks it until\n"
      "the peer arrives, and if the peer is later in the same body nothing can\n"
      "ever run it: the program hangs with no further diagnostic.  The same\n"
      "program runs under `tur --interpret`, whose rendezvous is cooperative,\n"
      "which is why the warning is not emitted there.\n"
      "\n"
      "Fix: run the peer with session-spawn / session-join from stdlib/session.tur,\n"
      "which is an OS thread compiled and a scheduler fiber under --interpret:\n"
      "  (load \"stdlib/session.tur\")\n"
      "  (let [t (session-spawn (fn [] (let [[n r] (recv r)] (println n) (close r))))]\n"
      "    ...\n"
      "    (session-join t))\n"
      "\n"
      "The warning is a heuristic: a body that does hand one end to a peer on\n"
      "another thread works and still warns.\n"
      "See docs/archive/compiled-async-fiber-deadlocks-on-a-session-op.md.\n",
    },
    { TUR_W0042_SHADOWS_SPECIAL_FORM,
      "TUR-W0042: Definition shadows a special form\n"
      "\n"
      "A defn or defmacro was given the name of a reserved special form, e.g.\n"
      "  (defn return [x :int] : (fn [] int) ...)\n"
      "\n"
      "Call heads are matched against the special forms by name BEFORE any\n"
      "binding, macro, or typeclass-method lookup, so the definition is accepted\n"
      "but never consulted: a bare (return 1) elaborates as the early-return\n"
      "form, not as a call to your function. The resulting type error -- if there\n"
      "is one at all -- lands on the caller's argument and never mentions the\n"
      "name collision.\n"
      "\n"
      "Fix: rename the definition. `pure` is the conventional name for a monadic\n"
      "unit, which is the usual reason `return` gets reached for:\n"
      "  (defn pure [x :int] : (fn [] int) ...)\n"
      "\n"
      "Inside a defmodule the definition is still reachable through its qualified\n"
      "name ((mymod/return 1)), since a qualified head symbol never matches a\n"
      "special form -- but the bare name stays shadowed, so renaming is better.\n"
      "\n"
      "Names that are deliberately shadowable (handler, with, default-of, and the\n"
      "session ops send/recv/close/...) do not trigger this warning: a user\n"
      "definition of those genuinely wins over the form.\n",
    },
    /* LT1: Linear type errors */
    { TUR_E0100_LINEAR_DROPPED,
      "TUR-E0100: Linear value dropped without being consumed\n"
      "\n"
      "A value of linear type (lref<T>, annotated with ^linear, or a ref<T> under\n"
      "-Xsubstructural) went out of scope without being consumed exactly once.\n"
      "Linear values must be explicitly consumed -- passing them to a function,\n"
      "returning them, or binding them to another name.\n"
      "\n"
      "This error is part of the unified substructural type system (-Xsubstructural),\n"
      "which enforces three disciplines:\n"
      "  ^linear   -- must be used exactly once (no weakening, no contraction)\n"
      "  ^affine   -- may be discarded; may not be duplicated (no contraction)\n"
      "  ^relevant -- must be used at least once; may be duplicated (no weakening)\n"
      "\n"
      "Example of the error:\n"
      "  (let [fh (open-file \"data.txt\")]\n"
      "    42)           ; ERROR: fh is dropped without being consumed\n"
      "\n"
      "Fix: consume the value before the scope ends:\n"
      "  (let [fh (open-file \"data.txt\")]\n"
      "    (close-file fh))\n"
      "\n"
      "Enable with: tur -Xlinear myfile.tur\n"
      "         or: tur -Xsubstructural myfile.tur\n"
      "\n"
      "In effect handler clauses:\n"
      "A ^linear continuation k must be resumed or discontinued exactly once.\n"
      "If the handler body exits without calling (resume k ...) or\n"
      "(discontinue k ...), TUR-E0100 is emitted at the k binding site.\n"
      "\n"
      "  (handle body\n"
      "    (Ask [] ^linear k)\n"
      "    42)   ; ERROR: ^linear k dropped without resume or discontinue\n"
      "\n"
      "Fix: ensure every code path through the handler body calls resume or\n"
      "discontinue exactly once:\n"
      "  (handle body\n"
      "    (Ask [] ^linear k)\n"
      "    (resume k 42))  ; OK\n",
    },
    { TUR_E0101_LINEAR_USE_AFTER_CONSUME,
      "TUR-E0101: Linear value used after being moved or consumed\n"
      "\n"
      "A value of linear type was used a second time after it had already been\n"
      "consumed. Each linear value may be used exactly once.\n"
      "\n"
      "This is enforced by the substructural type system (-Xsubstructural).\n"
      "^linear is the strictest discipline: no weakening (must use) and no\n"
      "contraction (cannot duplicate). See also TUR-E0150 for ^affine violations.\n"
      "\n"
      "Example of the error:\n"
      "  (let [fh (open-file \"data.txt\")]\n"
      "    (read-file fh)   ; fh consumed here\n"
      "    (close-file fh)) ; ERROR: fh already consumed\n"
      "\n"
      "Fix: restructure so each linear value flows through exactly one code path.\n"
      "\n"
      "Enable with: tur -Xlinear myfile.tur\n"
      "         or: tur -Xsubstructural myfile.tur\n"
      "\n"
      "In effect handler clauses:\n"
      "A ^linear continuation k may be resumed or discontinued at most once.\n"
      "Calling (resume k ...) or (discontinue k ...) a second time emits\n"
      "TUR-E0101 at the second call site.\n"
      "\n"
      "  (handle body\n"
      "    (Ask [] ^linear k)\n"
      "    (+ (resume k 1) (resume k 2)))  ; ERROR: ^linear k used twice\n"
      "\n"
      "Fix: ensure at most one resume or discontinue is reachable per handler\n"
      "invocation. For multi-shot continuations, use ^multishot k\n"
      "(see TUR-E0500).\n",
    },
    { TUR_E0102_LINEAR_COPY,
      "TUR-E0102: Cannot copy a linear value\n"
      "\n"
      "Linear values (lref<T>, ^linear, or ref<T> under -Xsubstructural) cannot\n"
      "be implicitly duplicated. Copying would allow the value to be consumed\n"
      "twice, violating the exactly-once guarantee.\n"
      "\n"
      "This is enforced by the substructural type system (-Xsubstructural).\n"
      "The three disciplines and their duplication rules:\n"
      "  ^linear   -- no contraction: cannot duplicate\n"
      "  ^affine   -- no contraction: cannot duplicate (see TUR-E0150)\n"
      "  ^relevant -- contraction allowed: may duplicate freely\n"
      "\n"
      "If you need to share the underlying resource, consider wrapping it in an\n"
      "abstraction that explicitly transfers ownership (such as passing it through\n"
      "a channel or splitting it into sub-resources).\n"
      "\n"
      "Enable with: tur -Xlinear myfile.tur\n"
      "         or: tur -Xsubstructural myfile.tur\n",
    },
    { TUR_E0103_LINEAR_IN_RC,
      "TUR-E0103: Cannot wrap a linear value in rc<T>\n"
      "\n"
      "Placing a linear value (lref<T> or ^linear) inside rc<T> is forbidden.\n"
      "Shared reference-counting allows multiple owners, which would break the\n"
      "exactly-once consumption guarantee of linear types.\n"
      "\n"
      "If the resource must be shared, it must first be made non-linear (e.g., by\n"
      "consuming it and producing a shared representation).\n"
      "\n"
      "Enable with: turc -Xlinear myfile.tur\n",
    },
    { TUR_E0104_LINEAR_BRANCH_MISMATCH,
      "TUR-E0104: Linear value consumed in one branch but not another\n"
      "\n"
      "A linear value (lref<T> or ^linear) was consumed in some branches of an\n"
      "if/match expression but not others. Linear types require exactly-once\n"
      "consumption: every branch must consume the same set of linear values.\n"
      "\n"
      "Example of the error:\n"
      "  (let [^linear x 42]\n"
      "    (if cond\n"
      "      (consume x)   ; x consumed here\n"
      "      unit))        ; ERROR: x not consumed in else branch\n"
      "\n"
      "Fix: consume the linear value in all branches, or in none:\n"
      "  (let [^linear x 42]\n"
      "    (if cond\n"
      "      (consume x)   ; consumed in then\n"
      "      (consume x))) ; consumed in else -- OK\n"
      "\n"
      "Alternatively, consume the value after the if expression:\n"
      "  (let [^linear x 42]\n"
      "    (if cond do-something do-something-else)\n"
      "    (consume x))    ; consumed after if -- OK\n"
      "\n"
      "Enable with: turc -Xlinear myfile.tur\n",
    },
    /* UT1: Uniqueness type explanations */
    { TUR_E0200_UNIQUE_ALIASED,
      "TUR-E0200: Value is not unique -- it has been aliased\n"
      "\n"
      "A value was expected to have at most one live reference (^unique), but\n"
      "another binding that refers to the same value is still in scope. A unique\n"
      "value cannot be passed to a ^unique parameter when aliases exist.\n"
      "\n"
      "Example of the error:\n"
      "  (defn consume-unique [^unique v] :int v)\n"
      "  (let [x 42]\n"
      "    (let [y x]             ; y is an alias of x\n"
      "      (consume-unique x))) ; ERROR: x is aliased by y\n"
      "\n"
      "Fix: ensure no other binding refers to x before passing it as ^unique:\n"
      "  (let [x 42]\n"
      "    (consume-unique x))    ; OK: x has no aliases\n"
      "\n"
      "Enable with: turc -Xunique-types myfile.tur\n",
    },
    { TUR_E0201_UNIQUE_COPY,
      "TUR-E0201: Cannot copy unique value\n"
      "\n"
      "A value annotated with ^unique has already been consumed (moved or passed\n"
      "to a function). Unique values have at-most-one-use semantics for consumption\n"
      "-- once transferred, the original binding cannot be used again.\n"
      "\n"
      "Example of the error:\n"
      "  (let [^unique x 42]\n"
      "    (println x)   ; x consumed here\n"
      "    (println x))  ; ERROR: x already consumed\n"
      "\n"
      "Fix: ensure each ^unique value is used at most once:\n"
      "  (let [^unique x 42]\n"
      "    (println x))  ; OK: single use\n"
      "\n"
      "Enable with: turc -Xunique-types myfile.tur\n"
      "\n"
      "In effect handler clauses:\n"
      "Every handler continuation k is one-shot (unique) by default. Calling\n"
      "(resume k ...) or (discontinue k ...) a second time emits TUR-E0201.\n"
      "\n"
      "  (handle body\n"
      "    (Ask [] k)\n"
      "    (+ (resume k 1) (resume k 2)))  ; ERROR: k already resumed\n"
      "\n"
      "Fix: call resume or discontinue at most once per handler invocation.\n"
      "For continuations that must be called exactly once, use ^linear k\n"
      "(emits TUR-E0100 if dropped). For multi-shot continuations, use\n"
      "^multishot k (see TUR-E0500).\n",
    },
    { TUR_E0202_UNIQUE_IN_RC,
      "TUR-E0202: Cannot wrap a unique value in rc<T>\n"
      "\n"
      "Placing a ^unique value inside rc<T> is forbidden. Shared reference-counting\n"
      "creates multiple owners of the same value, which violates the at-most-one-\n"
      "reference guarantee of uniqueness types.\n"
      "\n"
      "Example of the error:\n"
      "  (let [^unique x 42]\n"
      "    (rc/of x))   ; ERROR: cannot wrap unique value in rc\n"
      "\n"
      "Fix: if shared ownership is needed, remove the ^unique annotation.\n"
      "\n"
      "Enable with: tur -Xunique-types myfile.tur\n",
    },
    /* TY4: captured owning field consumed in a handler case */
    { TUR_E0107_CAPTURED_FIELD_CONSUMED_IN_HANDLER,
      "TUR-E0107: Handler case consumes an owning field of a captured value\n"
      "\n"
      "A handler case dropped an owning (rc/ref) field of a by-value struct/record\n"
      "that it captured from an enclosing scope -- e.g. (rc/drop (.r o)) where o is\n"
      "a `let`-bound aggregate outside the `handle`. This is rejected because the\n"
      "field would be released twice: once by the handler case and once by o's\n"
      "scope-exit auto-drop. A handler case also runs once PER perform (0..N times),\n"
      "so even suppressing the auto-drop cannot make the counts balance -- the field\n"
      "would be under-dropped when the case never runs and over-dropped when it runs\n"
      "more than once.\n"
      "\n"
      "Example of the error:\n"
      "  (let [o (make-struct Own :r (rc/of 7) :tag 9)]\n"
      "    (handle (g)\n"
      "      (E [] k) (do (rc/drop (.r o)) (resume k 0))))  ; ERROR: case drops o.r\n"
      "\n"
      "Fix: do not consume the captured aggregate's owning field inside the handler.\n"
      "Read it (borrow) instead -- (.tag o), (rc/strong-count (.r o)) -- and let o's\n"
      "scope-exit auto-drop release the field once; or move ownership out of the\n"
      "aggregate before the handle so the enclosing scope no longer owns it.\n",
    },
    /* byvalue-recursive-shared-copies-leak: an owning ref field moved out of a borrow */
    { TUR_E0108_REF_FIELD_MOVED_OUT_OF_BORROW,
      "TUR-E0108: A struct with an owning ref field is returned out of a borrow\n"
      "\n"
      "A function returned, by value, a struct that owns a `ref` field -- but the\n"
      "value was a copy of one it does not own: a ^borrow parameter, a global, a\n"
      "container element, or a match binder of one of those.  The caller owns a\n"
      "function's result and frees its `ref` field when it is done, while the\n"
      "value's real owner frees the same box: a double free.  An `rc` field is\n"
      "cloned at this point (the result takes its own count), but a `ref` is a\n"
      "unique owner and has no count to take.\n"
      "\n"
      "Example of the error:\n"
      "  (defstruct R [p : ref<int> n : int])\n"
      "  (defn id-b [^borrow x : R] : R x)     ; ERROR: returns x's ref field\n"
      "\n"
      "Fix: return a new value -- (R (ref (deref (.p x))) (.n x)) copies the box --\n"
      "or take the parameter by value (without ^borrow) so the function owns what\n"
      "it returns, or return only the fields the caller needs.\n",
    },
    /* ST1: Substructural type explanations */
    { TUR_E0150_AFFINE_USED_TWICE,
      "TUR-E0150: Affine value used more than once\n"
      "\n"
      "A value annotated with ^affine was used more than once. Affine values\n"
      "implement no-contraction: they may be discarded (weakening is allowed),\n"
      "but they may not be duplicated or used a second time.\n"
      "\n"
      "This is part of the unified substructural type system (-Xsubstructural).\n"
      "The three disciplines and their usage rules:\n"
      "  ^linear   -- must use exactly once (no weakening, no contraction)\n"
      "  ^affine   -- may discard; may not duplicate (no contraction)\n"
      "  ^relevant -- must use at least once; may duplicate (no weakening)\n"
      "\n"
      "Example of the error:\n"
      "  (let [^affine k (generate-key)]\n"
      "    (initialize k)\n"
      "    (initialize k))  ; ERROR: k used twice\n"
      "\n"
      "Fix: use the affine value at most once:\n"
      "  (let [^affine k (generate-key)]\n"
      "    (initialize k))  ; OK\n"
      "\n"
      "If you need to use the value multiple times, consider ^relevant instead\n"
      "(must-use, but duplication is allowed).\n"
      "\n"
      "Enable with: tur -Xsubstructural myfile.tur\n",
    },
    /* CONV-S6: product-shape (struct / record-variant) construction explanations.
     * Struct and single-variant record ADT share one construction path; each
     * example block shows both the `defstruct` and the `defdata` surface. */
    { TUR_E0292_MISSING_FIELD,
      "TUR-E0292: Missing field in construction\n"
      "\n"
      "Keyword construction of a struct or record variant left a declared\n"
      "field unset.  Every field must be supplied.\n"
      "\n"
      "Example (defstruct surface):\n"
      "  (defstruct Person :copy [name : cstr age : int])\n"
      "  (Person :name \"Bob\")   ; error: missing field 'age' in struct 'Person'\n"
      "\n"
      "Example (defdata surface):\n"
      "  (defdata Shape (Circle [radius : float]))\n"
      "  (Circle)                ; error: missing field 'radius' in\n"
      "                          ;        variant 'Circle' of type 'Shape'\n"
      "\n"
      "Supply the missing field, or construct positionally in field order.\n",
    },
    { TUR_E0293_DUPLICATE_FIELD,
      "TUR-E0293: Duplicate field in construction\n"
      "\n"
      "Keyword construction named the same field twice.  Each field may be\n"
      "given at most once.\n"
      "\n"
      "Example (defstruct surface):\n"
      "  (defstruct Person :copy [name : cstr age : int])\n"
      "  (Person :name \"Bob\" :age 40 :age 41)\n"
      "  ; error: duplicate field 'age' in struct 'Person' construction\n"
      "\n"
      "Example (defdata surface):\n"
      "  (defdata Shape (Circle [radius : float]))\n"
      "  (Circle :radius 1.0 :radius 2.0)\n"
      "  ; error: duplicate field 'radius' in\n"
      "  ;        variant 'Circle' of type 'Shape' construction\n"
      "\n"
      "Remove the redundant keyword.\n",
    },
    { TUR_E0294_UNKNOWN_FIELD,
      "TUR-E0294: Unknown field in construction\n"
      "\n"
      "Keyword construction named a field the struct or record variant does\n"
      "not declare.\n"
      "\n"
      "Example (defstruct surface):\n"
      "  (defstruct Person :copy [name : cstr age : int])\n"
      "  (Person :name \"Bob\" :years 40)\n"
      "  ; error: unknown field 'years' on struct 'Person'\n"
      "\n"
      "Example (defdata surface):\n"
      "  (defdata Shape (Circle [radius : float]))\n"
      "  (Circle :diameter 2.0)\n"
      "  ; error: unknown field 'diameter' on variant 'Circle' of type 'Shape'\n"
      "\n"
      "Check the field name against the declaration.\n",
    },
    { TUR_E0295_BYVALUE_CARRIER_CAST,
      "TUR-E0295: Cannot reinterpret a by-value aggregate as a carrier\n"
      "\n"
      "`::` between a non-recursive by-value ADT/struct product and a one-word\n"
      "carrier (:int or :ptr<void>), in either direction, has no sound lowering:\n"
      "a by-value aggregate is a C struct with no int64 handle to reinterpret.\n"
      "Reinterpreting one as an integer (or an integer back as the struct) would\n"
      "miscompile -- a stray cc error one way, a segfault the other.\n"
      "\n"
      "Example:\n"
      "  (defdata Pair :copy (Pair :int :int))\n"
      "  (:: (Pair 3 4) :int)   ; error: cannot reinterpret by-value 'Pair' as int\n"
      "  (:: h :Pair)           ; error: cannot reinterpret int as by-value 'Pair'\n"
      "\n"
      "A *recursive* ADT rides the int64 carrier and does cast cleanly.  To carry\n"
      "a by-value aggregate through an erased handle, box it into `any`:\n"
      "\n"
      "  (defn thread [h : any] : any h)         ; the erased carrier is `any`\n"
      "  (let [h    (:: (Pair 3 4) :any)         ; heap-box the by-value value\n"
      "        back (cast (thread h) Pair)]      ; ... and read it back by value\n"
      "    ...)\n"
      "\n"
      "`(:: v :any)` (or just passing v where an `any` is expected) heap-boxes it\n"
      "as a one-word handle; `(cast h T)` reads it back as T.  See\n"
      "docs/archive/byvalue-adt-int-cast-plan.md.\n",
    },
    { TUR_E0303_NON_NULL_OPAQUE_ZERO,
      "TUR-E0303: Cannot ascribe the literal 0 into a :non-null opaque\n"
      "\n"
      "`(defopaque String :ptr<void> :non-null)` is the type author's claim\n"
      "that the handle's valid values exclude the null pointer -- no producer\n"
      "returns 0.  Ascribing the literal 0 into such a type is a provable\n"
      "violation of that claim, so it is rejected at compile time:\n"
      "\n"
      "  (:: 0 :String)                 ; error\n"
      "  (:: (:: 0 :ptr<void>) String)  ; error -- peeled through the relabel\n"
      "\n"
      "Why this matters: by default (the Option niche), an `(Option T)` over a\n"
      ":non-null T is carried AS the payload pointer with `(none)` as NULL.  A\n"
      "null smuggled into T makes `(some x)` and `(none)` the same value.  The\n"
      "niche `Some` constructor also checks at runtime (a null there aborts\n"
      "with a message naming the type) for the paths elaboration cannot see --\n"
      "inline-C bodies and computed values -- but a violation visible in the\n"
      "source should not wait for runtime.\n"
      "\n"
      "Fix: express absence as `(none)` / `option<T>`, or construct the value\n"
      "through the type's real constructors.  If the type genuinely has a null\n"
      "state, its declaration should not say :non-null.\n",
    },
    { TUR_E0302_SEALED_OPAQUE_CAST,
      "TUR-E0302: Cannot cast across a sealed opaque's representation boundary\n"
      "\n"
      "`(defopaque H :int :sealed)` declares that H's representation is private\n"
      "to the module that defines it.  Outside that module, `::` refuses BOTH\n"
      "directions: you can neither unwrap an H to its representation nor build\n"
      "an H from one.\n"
      "\n"
      "Example:\n"
      "  ;; in module ecs/refined-world\n"
      "  (defopaque RGWorld :int :sealed)\n"
      "\n"
      "  ;; in some other module\n"
      "  (:: w :int)         ; error: cannot unwrap sealed 'RGWorld'\n"
      "  (:: n RGWorld)      ; error: cannot fabricate sealed 'RGWorld'\n"
      "\n"
      "Why this exists: `::` is a COERCING cast, so an ordinary defopaque can\n"
      "always be unwrapped and re-wrapped -- which mints an ALIAS of a handle\n"
      "the type system believes is uniquely held.  That bounds every guarantee\n"
      "built on the handle.  The motivating case is a `frozen` region: mutating\n"
      "the borrowed world is correctly TUR-E0200, but mutating an alias rebuilt\n"
      "through `::` was not.\n"
      "\n"
      "Fix: go through the declaring module's API.  If you genuinely need the\n"
      "representation outside, that module should export a function for it --\n"
      "which makes the escape explicit and reviewable instead of implicit.\n"
      "\n"
      "This check is unconditional.  It was the `sealed-opaque` experiment\n"
      "until 0.34.0, where `:sealed` parsed but imposed nothing unless you\n"
      "passed --enable=sealed-opaque; it now enforces in every build.  Only\n"
      "code that deliberately wrote `:sealed` is affected.  See\n"
      "docs/archive/sealed-opaque-plan.md.\n",
    },
    { TUR_E0296_WITH_NOT_COPY,
      "TUR-E0296: `with` requires a :copy type\n"
      "\n"
      "The functional-update form `with` copies the source's unchanged fields\n"
      "into the new value.  On a move-only (non-:copy) type that copy would\n"
      "consume the source, so `with` is rejected.\n"
      "\n"
      "Example (defstruct surface):\n"
      "  (defstruct Acct [balance : int])\n"
      "  (with a [balance 50])\n"
      "  ; error: with requires a :copy struct -- 'Acct' is move-only;\n"
      "  ;        declare it `(defstruct Acct :copy ...)` to use with.\n"
      "\n"
      "Example (defdata surface):\n"
      "  (defdata Acct (Acct [balance : int]))\n"
      "  (with a [balance 50])\n"
      "  ; error: with requires a :copy variant -- 'Acct' is move-only;\n"
      "  ;        declare it `(defdata Acct :copy ...)` to use with.\n"
      "\n"
      "Add the :copy annotation to the declaration.\n",
    },
    { TUR_E0297_WITH_UNKNOWN_FIELD,
      "TUR-E0297: `with` override names an unknown field\n"
      "\n"
      "A `with` override named a field the struct or record variant does not\n"
      "declare.\n"
      "\n"
      "Example (defstruct surface):\n"
      "  (defstruct Point :copy [x : int y : int])\n"
      "  (with p [z 3])\n"
      "  ; error: with: unknown field 'z' on struct 'Point'\n"
      "\n"
      "Example (defdata surface):\n"
      "  (defdata Point :copy (Point [x : int y : int]))\n"
      "  (with p [z 3])\n"
      "  ; error: with: unknown field 'z' on variant 'Point' of type 'Point'\n"
      "\n"
      "Check the override field name against the declaration.\n",
    },
    { TUR_E0298_WITH_DUPLICATE_FIELD,
      "TUR-E0298: `with` overrides the same field twice\n"
      "\n"
      "A `with` clause listed the same override field more than once.  Each\n"
      "overridden field may appear at most once.\n"
      "\n"
      "Example:\n"
      "  (defstruct Point :copy [x : int y : int])\n"
      "  (with p [x 1 x 2])   ; error: with duplicate override field 'x'\n"
      "\n"
      "Remove the redundant override.\n",
    },
    { TUR_E0299_MIXED_POS_KW,
      "TUR-E0299: Cannot mix positional and keyword arguments\n"
      "\n"
      "A struct or record-variant construction mixed positional arguments with\n"
      "keyword (:field value) arguments.  Choose one style for the whole call.\n"
      "\n"
      "Example (defstruct surface):\n"
      "  (defstruct Person :copy [name : cstr age : int])\n"
      "  (Person \"Bob\" :age 40)\n"
      "  ; error: struct 'Person' construction: cannot mix positional and\n"
      "  ;        keyword arguments\n"
      "\n"
      "Example (defdata surface):\n"
      "  (defdata Shape (Circle [radius : float]) (Square [side : float]))\n"
      "  (Circle 2.0 :radius 3.0)\n"
      "  ; error: variant 'Circle' of type 'Shape' construction: cannot mix\n"
      "  ;        positional and keyword arguments\n"
      "\n"
      "Use all positional (in field order) or all keyword arguments.\n",
    },
    /* IT1: Union type explanations */
    { TUR_E0300_UNION_TYPE_MISMATCH,
      "TUR-E0300: Union type mismatch\n"
      "\n"
      "A value's type is not a member of the expected union type.\n"
      "\n"
      "Example:\n"
      "  (defn print-either [x : (int | cstr)] : nil (println x))\n"
      "  (print-either true)  ; error: bool is not a member of (int | cstr)\n"
      "\n"
      "Pass a value whose type is one of the union members, or widen the union\n"
      "type to include the actual type.\n"
      "\n"
      "Enable with: turc -Xunion-types myfile.tur\n",
    },
    { TUR_E0301_NON_EXHAUSTIVE_UNION_MATCH,
      "TUR-E0301: Non-exhaustive pattern match on union type\n"
      "\n"
      "A match expression on a union type does not cover all member types.\n"
      "Every arm must be a type-narrowing pattern (varname : Type) covering\n"
      "each union member, or a wildcard '_' or variable capturing the rest.\n"
      "\n"
      "Example:\n"
      "  (defn f [x : (int | cstr | bool)] : nil\n"
      "    (match x\n"
      "      (n : int)  (println n)\n"
      "      (s : cstr) (println s)))\n"
      "  ; error: match on (int | cstr | bool) is missing arm for bool\n"
      "\n"
      "Fix: add arms for all missing member types, or add a wildcard arm:\n"
      "  (match x\n"
      "    (n : int)  (println n)\n"
      "    (s : cstr) (println s)\n"
      "    (b : bool) (println b))\n"
      "\n"
      "Enable with: turc -Xunion-types myfile.tur\n",
    },
    /* IT3: Intersection type explanations */
    { TUR_E0350_INTERSECTION_UNSATISFIABLE,
      "TUR-E0350: Intersection type is unsatisfiable\n"
      "\n"
      "No value can simultaneously satisfy all members of this intersection type\n"
      "because two or more members are known-disjoint concrete types.\n"
      "\n"
      "Example:\n"
      "  (defn f [x : (int & cstr)] : int 0)\n"
      "  ; error: no value can be both int and cstr\n"
      "\n"
      "Intersection types are useful when at least one member is a typeclass\n"
      "constraint or type variable:\n"
      "  (defn serialize-int [x : (int & Serializable)] : cstr\n"
      "    (serialize x))\n"
      "\n"
      "Enable with: turc -Xintersection-types myfile.tur\n",
    },
    { TUR_E0351_INTERSECTION_MEMBER_MISMATCH,
      "TUR-E0351: Value does not satisfy all intersection members\n"
      "\n"
      "A function parameter expects a value that satisfies all members of an\n"
      "intersection type, but the argument's type does not match one or more\n"
      "of those members.\n"
      "\n"
      "Example:\n"
      "  (defclass Printable [a] (print-it [x : a] : unit))\n"
      "  (defn show [x : (int & Printable)] : unit (print-it x))\n"
      "  (show \"hello\")  ; error: cstr does not satisfy member int\n"
      "\n"
      "Pass a value whose type satisfies every member of the intersection, or\n"
      "widen the intersection to include the actual type.\n"
      "\n"
      "Enable with: turc -Xintersection-types myfile.tur\n",
    },
    /* RT3: refinement-type discharge explanations */
    { TUR_E0371_REFINE_NOT_PROVED,
      "TUR-E0371: Refinement predicate cannot be proved statically\n"
      "\n"
      "Under the `refined` experiment the compiler tries to PROVE each\n"
      "#refine{...} predicate instead of only checking it at runtime.  This\n"
      "obligation was not just undecided -- a backend found a counterexample,\n"
      "so the predicate genuinely does not hold for every input.\n"
      "\n"
      "Example:\n"
      "  (defn wrong [x : int] : #refine{ r : int | (> r 0) }\n"
      "    x)          ; x may be 0 or negative\n"
      "\n"
      "Fix by constraining the input so the result follows:\n"
      "  (defn ok [x : #refine{ v : int | (> v 0) }] : #refine{ r : int | (> r 0) }\n"
      "    x)\n"
      "\n"
      "The runtime contract check is still emitted, so the program remains\n"
      "safe; --strict-refine turns this into a hard failure instead.\n"
      "\n"
      "Refinement checking is on in every build -- `refined` graduated in\n"
      "v0.33.0, so there is no flag to enable.\n"
      "\n"
      "Loop invariants (--enable=loop-invariants):\n"
      "  (while c :invariant p body...) owes two proofs.  \"does not hold on\n"
      "  entry\" means the facts that reach the loop -- branch conditions, let\n"
      "  bindings, parameter refinements, an earlier loop's post-fact -- admit\n"
      "  a state where p is false.  \"does not preserve `q`\" means that from a\n"
      "  state satisfying p and c, one pass of the body can make the conjunct q\n"
      "  false; the note shows q as it reads after the body, and for a\n"
      "  branching body the path that breaks it.  The usual fix is a STRONGER\n"
      "  invariant: the counterexample names the variable the invariant says\n"
      "  too little about.\n"
      "\n"
      "    (while (< i n) :invariant (>= acc 0)       ; not inductive:\n"
      "      (set! acc (+ acc i)) ...)                 ;   i may be negative\n"
      "    (while (< i n) :invariant (and (>= acc 0) (>= i 0))   ; inductive\n"
      "      (set! acc (+ acc i)) ...)\n"
      "\n"
      "  Both runtime checks (entry, and end of every iteration) stay until\n"
      "  the matching proof succeeds.\n",
    },
    { TUR_W0372_REFINE_UNKNOWN,
      "TUR-W0372: Solver returned unknown for a refinement predicate\n"
      "\n"
      "No stage of the in-house decision procedure could decide this\n"
      "obligation, so the runtime contract check is kept -- exactly the\n"
      "behavior you would get with contract types alone.  This is a sound\n"
      "outcome, not a miscompile.\n"
      "\n"
      "Common causes:\n"
      "  - the predicate or the expression it constrains falls outside the\n"
      "    supported fragment (quantifier-free linear integer/real arithmetic\n"
      "    with equality and uninterpreted functions);\n"
      "  - a nonlinear subterm was abstracted away (see TUR-W0373);\n"
      "  - the propositional structure exceeded the small-DNF cap.\n"
      "\n"
      "Adding an explicit refinement to a parameter usually supplies the\n"
      "missing hypothesis.  --strict-refine turns this into a hard error for\n"
      "builds that want every obligation discharged statically.\n"
      "\n"
      "Loop invariants (--enable=loop-invariants):\n"
      "  An undecided `:invariant` obligation reports here as \"could not be\n"
      "  proved on entry\" or \"...to be preserved by the body\"; its runtime\n"
      "  check stays.  \"is not analysed statically: <reason>\" means the loop\n"
      "  was DECLINED before any solving -- both checks stay.  The analysis\n"
      "  declines rather than approximates when the body:\n"
      "    - assigns through a place (`(set! (.f s) v)`) or an atom;\n"
      "    - can leave early (`return`, `?`, a continuation) or nests a loop\n"
      "      that assigns;\n"
      "    - borrows a variable the loop depends on (`(& x)`, `&mut x`), or\n"
      "      the function assigns it inside a lambda or handler clause -- a\n"
      "      call could then change it with no `set!` in sight;\n"
      "    - reads a field, a deref, or a mutable global in the invariant or\n"
      "      the condition;\n"
      "    - rebinds, in a body `let`, a name that is already in scope.\n"
      "  The post-loop fact `p AND (not c)` is only offered once BOTH\n"
      "  obligations are proved.\n",
    },
    { TUR_E0378_REFINE_IN_FN_TYPE,
      "TUR-E0378: Refinement written inside a function type\n"
      "\n"
      "A `(fn ...)` type cannot carry refinements on its parameters or its\n"
      "result. Writing one there is rejected rather than ignored, because a\n"
      "silently dropped refinement reads like a guarantee that is being\n"
      "checked and is not.\n"
      "\n"
      "This is the known limit on HIGHER-ORDER checking. A function value with\n"
      "refined parameters may be passed and called freely -- its own entry\n"
      "checks still run, so nothing unsound follows -- but the refinement\n"
      "cannot be seen through the function type, so neither the body that\n"
      "calls it nor the caller that supplies it is checked statically:\n"
      "\n"
      "    (defn safe-div [a : int b : #refine{ v : int | (not= v 0) }] : int ...)\n"
      "    (defn apply1 [f : (fn [int int] int) x : int] : int (f 10 x))\n"
      "    (apply1 safe-div 0)   ; allowed; caught at run time, not compile time\n"
      "\n"
      "Closing that gap needs refinements to be part of function types, with\n"
      "the contravariant subtyping check that implies. Until then, options are\n"
      "to take the value at a named type with a `defn` wrapper that carries the\n"
      "refinement, or to accept the runtime check.\n" },
    { TUR_W0377_REFINE_INSTANCE_LENIENCY,
      "TUR-W0377: Call relies on instance-specific leniency\n"
      "\n"
      "The argument violates the CLASS signature's refinement, but the instance\n"
      "this call resolved to explicitly demands less, so the call is allowed.\n"
      "\n"
      "A typeclass instance may accept more than its class promises (see\n"
      "TUR-E0374 for the other direction), and a call whose instance is known\n"
      "statically is checked against that instance -- the more precise contract\n"
      "of the two. This warning marks where the two disagree.\n"
      "\n"
      "It matters because the leniency is not part of the interface. Adding a\n"
      "stricter instance later, or lifting this call into a generic function\n"
      "where dispatch stays dynamic, checks the argument against the CLASS\n"
      "predicate instead -- and this call would then fail.\n"
      "\n"
      "Fix by passing an argument the class signature admits, or, if the\n"
      "leniency is intended, by widening the class signature so it is part of\n"
      "the published contract rather than one instance's private extension.\n"
      "\n"
      "Only a DEFINITE violation warns: the argument has to be one the class\n"
      "predicate rejects outright, not merely one it cannot prove.\n" },
    { TUR_W0380_REFINE_TYPE_ARG_UNENFORCED,
      "TUR-W0380: Refinement in type-argument position is not enforced\n"
      "\n"
      "A refinement written as a TYPE ARGUMENT -- the payload slot of a\n"
      "container -- is peeled to its base type and the predicate is dropped:\n"
      "\n"
      "    (defn f [b : (Box #refine{ v : int | (> v 0) })] : int ...)\n"
      "    ;; behaves exactly as (Box int); nothing checks the payload\n"
      "\n"
      "The refinement is not silently honored and it is not an error either.\n"
      "It is peeled because leaving it in place is worse: a live contract type\n"
      "inside a type application makes every ordinary use of the payload fail\n"
      "(operator lookup, overload resolution, and return-type checking all\n"
      "compare kinds without peeling), so the annotation would break the\n"
      "program rather than merely fail to help it.\n"
      "\n"
      "Enforcing it needs a refinement to survive as a type argument all the\n"
      "way to the binder that unpacks the container, plus a checked crossing\n"
      "where a constructor call's result is matched against a declared type.\n"
      "That is a real feature, not an oversight, and it is not built.\n"
      "\n"
      "To actually check the value, refine at a position that IS enforced --\n"
      "a parameter, a return type, or a `let` annotation:\n"
      "\n"
      "    (defn unwrap [b : Box] : #refine{ v : int | (> v 0) } ...)\n"
      "\n"
      "or check the payload after unpacking it.\n" },
    { TUR_E0381_WRITES_FRAME_INVALID,
      "TUR-E0381: Malformed `#writes` frame\n"
      "\n"
      "A `#writes` annotation names the parameters whose mutable state the\n"
      "body may write.  It is spelled either as one parameter or as a vector\n"
      "of them, and every name must be a parameter of THIS function:\n"
      "\n"
      "    (defn move! [^mut w : World dt : float] #writes w : void ...)\n"
      "    (defn swap2! [^mut a : Buf ^mut b : Buf] #writes [a b] : void ...)\n"
      "    (defn peek [^borrow w : World] #writes [] : int ...)\n"
      "\n"
      "`#writes []` is the empty frame -- a positive claim that the body\n"
      "writes nothing.  It is NOT the same as omitting the annotation, which\n"
      "means \"unknown, assume anything\".\n"
      "\n"
      "This is an error rather than an ignored decoration because a frame that\n"
      "does not resolve cannot be checked against the body, and downstream\n"
      "code is entitled to believe a declaration that compiled.\n" },
    { TUR_E0382_WRITES_FRAME_EXCEEDED,
      "TUR-E0382: Body writes outside its declared `#writes` frame\n"
      "\n"
      "The function declared a write frame, and its body writes something the\n"
      "frame does not cover:\n"
      "\n"
      "    (defn bump! [^mut a : Ctr ^mut b : Ctr] #writes [a] : void\n"
      "      (set! (.n a) 1)\n"
      "      (set! (.n b) 2))   ;; TUR-E0382: `b` is not in the frame\n"
      "\n"
      "A declared frame the body exceeds is an error, not a silent widening --\n"
      "the same rule `#reads` follows.  Widening it silently would make the\n"
      "annotation unfalsifiable, and the whole point of the checked tier is\n"
      "that an optimization may act on the claim.\n"
      "\n"
      "Three write channels are checked: a direct `set!`/`swap!`/`reset!`, an\n"
      "argument passed `^mut` to a callee, and a callee's own declared frame.\n"
      "Fix it by widening the declaration to what the body actually writes, or\n"
      "by narrowing the body.\n"
      "\n"
      "Only a body with no inline C is checked.  An inline-C body cannot be\n"
      "walked, so its frame stays trusted-with-declaration and never reports\n"
      "this code -- checked-when-checkable, never checked-by-pretending.\n" },
    { TUR_W0383_READS_FRAME_OMITS_MUTABLE,
      "TUR-W0383: `#reads` frame omits mutable state the body reads\n"
      "\n"
      "A measure declared `#reads <param>` (or `#reads [a b]`) promises that\n"
      "the named parameters are the only mutable state it depends on.  The\n"
      "promise is TRUSTED, not\n"
      "checked, and it pays out in proofs: inside a `frozen` region the\n"
      "refinement solver treats two calls of the measure as one value and\n"
      "elides the caller-side crossing check.\n"
      "\n"
      "This body also reads mutable state the frame omits -- a mutable\n"
      "global (which no frame can name), or state rooted in a PARAMETER the\n"
      "frame does not list -- so the promise is broken as written:\n"
      "\n"
      "    (def ^mut fudge 1)\n"
      "    (defn alive? [^borrow w : World e : int] #reads w : bool\n"
      "      (> fudge 0))   ;; TUR-W0383: reads `fudge`, frame says only `w`\n"
      "\n"
      "    (defn linked? [^borrow w : World ^borrow g : Grid e : int]\n"
      "                  #reads w : bool\n"
      "      (> (.m g) 0))  ;; TUR-W0383: reads g's state, frame omits g\n"
      "\n"
      "Another function can mutate that state between two calls the solver\n"
      "proved identical, and the elided check will not catch it -- the program\n"
      "silently crosses on a predicate that is false.  The measure's own\n"
      "internal safety check (if it has one) still runs; only the caller-side\n"
      "proof is unearned.\n"
      "\n"
      "Fix a global read by threading the state through a parameter the frame\n"
      "can name, or by making the global immutable; fix an omitted-parameter\n"
      "read by naming the parameter (`#reads [w g]` -- the grant then\n"
      "requires BOTH frozen at the site).\n"
      "\n"
      "The frame also promises the body does not CHANGE the named state, and\n"
      "the same code reports a body seen to break that -- a store through a\n"
      "framed parameter, or a call handing it to a callee whose `#writes`\n"
      "frame names it (stdlib's container mutators carry one):\n"
      "\n"
      "    (defn liar [v : (Vec int)] #reads v : int\n"
      "      (do (vec-push! v 7) 0))   ;; TUR-W0383: writes v\n"
      "\n"
      "A measure that changes what it measures is not a function of it.  Drop\n"
      "`#reads`, or stop writing.  A use of a framed parameter that is neither\n"
      "a read nor a demonstrable write (aliased into a local, returned, passed\n"
      "to an unannotated callee) is not reported, but such a measure does not\n"
      "back the loop-invariant frozen grant, which elides a check with no\n"
      "callee-side backstop.\n"
      "\n"
      "The same evidence also REFUSES the congruence override, so the\n"
      "crossing must be proven some other way; an undecided one is TUR-W0372\n"
      "(an error under --strict-refine).  Only a demonstrable read does this:\n"
      "an inline-C body is unwalkable, yields no evidence, stays silent, and\n"
      "keeps the trusted grant.  See\n"
      "docs/archive/trusted-refinement-claims-plan.md.\n" },
    { TUR_E0384_REFLECT_NOT_TOTAL,
      "TUR-E0384: `^reflect` function is not proven total\n"
      "\n"
      "A `^reflect` defn asks the refinement solver to USE its definition: at a\n"
      "call whose argument is a constructor term or a literal, the body is\n"
      "unfolded and the equation `f(t) = <body at t>` is asserted.  Such an\n"
      "equation is only a fact when `f` is TOTAL.  Unfolding `f(x) = 1 + f(x)`\n"
      "asserts `0 = 1`, and an inconsistent hypothesis set discharges every\n"
      "obligation in the unit -- silently, including the false ones.  So the\n"
      "gate is a hard error on the definition, never a quiet downgrade.\n"
      "\n"
      "Three things are checked, and the message names which one failed:\n"
      "\n"
      "  purity      -- the body clears the same default-deny purity walk that\n"
      "                 grants a measure congruence (literals, if/let/do/match,\n"
      "                 arithmetic, calls to functions already known pure).\n"
      "  termination -- every self-call passes, in some fixed position, a\n"
      "                 STRICT STRUCTURAL SUBTERM of the corresponding\n"
      "                 parameter: a variable bound by a constructor pattern in\n"
      "                 a `match` on that parameter (the `t` in\n"
      "                 `(Cons _ t) (+ 1 (len t))`), or a subterm of one.\n"
      "                 Arithmetic on the argument (`(f (- n 1))`), calls\n"
      "                 through variables, and mutual recursion all reject.\n"
      "  coverage    -- every `match` is proven exhaustive: a `^non-exhaustive`\n"
      "                 opt-out, or a literal-scrutinee match with no `_` /\n"
      "                 variable arm, rejects.  So does any form the reflection\n"
      "                 walk does not positively recognise (macros, lambdas,\n"
      "                 `panic`, inline C, loops).\n"
      "\n"
      "    (defdata Lst [] (Cons [hd : int tl : Lst]) (Nil))\n"
      "    (defn ^reflect len [xs : Lst] : int          ;; passes\n"
      "      (match xs (Nil) 0 (Cons _ t) (+ 1 (len t))))\n"
      "    (defn ^reflect bad [n : int] : int           ;; TUR-E0384: termination\n"
      "      (if (= n 0) 0 (+ 1 (bad (- n 1)))))\n"
      "\n"
      "This is NOT a termination checker for programs.  A function that is\n"
      "never `^reflect`ed is untouched; only a function whose equation you\n"
      "asked to admit into the logic must be shown total.  Behind\n"
      "`--enable=reflected-measures`; see docs/upcoming/reflected-measures-plan.md.\n" },
    { TUR_W0385_REFLECT_FUEL_EXHAUSTED,
      "TUR-W0385: `^reflect` unfolding fuel exhausted; obligation left unknown\n"
      "\n"
      "The encoder unfolds a reflected measure at constructor-headed and\n"
      "literal arguments, one definitional equation per step, up to a fixed\n"
      "budget per obligation (default 8; `TUR_REFLECT_FUEL=<n>` overrides it\n"
      "for experiments).  This obligation ran the budget out and was then not\n"
      "decided, so the runtime check is kept -- the TUR-W0372 principle: running\n"
      "out of budget costs completeness, never soundness.\n"
      "\n"
      "Typical cause: a literal deeper than the budget, e.g. a 10-element cons\n"
      "literal against `(> (len v) 9)` with fuel 8.  Raise the fuel, or restate\n"
      "the argument so fewer steps are needed.  Under --strict-refine this is an\n"
      "error.  Behind `--enable=reflected-measures`.\n" },
    /* class-superclasses (docs/archive/typeclass-superclasses-plan.md) */
    { TUR_E0390_CLASS_SUPERCLASS_PREAMBLE,
      "TUR-E0390: Malformed `defclass` superclass preamble\n"
      "\n"
      "A `defclass` may list the classes a constraint on it entails, in a\n"
      "constraint vector right after the type-parameter vector -- the same\n"
      "`[(Class var)]` spelling `definstance` and `defn` already use:\n"
      "\n"
      "    (defclass Monoid [a]\n"
      "      [(Semigroup a)]\n"
      "      (mempty [] : a))\n"
      "\n"
      "This code means the vector was written but cannot be accepted:\n"
      "  - the vector is empty (list at least one `(Class var)`, or drop it);\n"
      "  - an element is not of the form `(Class var...)`;\n"
      "  - a variable named is not one of this class's own type parameters;\n"
      "  - the vector was placed AFTER the `|` functional-dependency clause.\n"
      "    The canonical order is\n"
      "    `(defclass Name [params] [(Super var)...] | (from -> to) methods...)`.\n" },
    { TUR_E0391_CLASS_SUPERCLASS_UNRESOLVED,
      "TUR-E0391: Superclass does not resolve\n"
      "\n"
      "A `defclass` constraint preamble names a class that is not defined\n"
      "anywhere in the program, or one whose shape does not fit the arguments\n"
      "given -- a different parameter count, or a parameter of a different\n"
      "kind (`[(Functor a)]` over a kind-`*` variable `a`).\n"
      "\n"
      "Superclasses are resolved after every form in the unit is registered,\n"
      "so a superclass declared BELOW its subclass is fine; the name must\n"
      "simply exist somewhere.\n" },
    { TUR_E0392_CLASS_SUPERCLASS_CYCLE,
      "TUR-E0392: Superclass cycle\n"
      "\n"
      "The superclass graph must be acyclic.  A class may not list itself,\n"
      "and two classes may not each list the other:\n"
      "\n"
      "    (defclass A [x] [(B x)] (fa [v : x] : int))\n"
      "    (defclass B [x] [(A x)] (fb [v : x] : int))   ;; TUR-E0392: A -> B -> A\n"
      "\n"
      "The diagnostic names the full cycle path.\n" },
    { TUR_E0393_CLASS_SUPERCLASS_INSTANCE_MISSING,
      "TUR-E0393: Instance is missing a superclass instance\n"
      "\n"
      "A constraint on a subclass entails its superclasses -- a `[^Monoid A]`\n"
      "body may call `combine` -- and that is only sound if every `Monoid`\n"
      "instance is backed by a `Semigroup` instance for the same type.  So:\n"
      "\n"
      "    (defclass Monoid [a] [(Semigroup a)] (mempty [] : a))\n"
      "    (definstance Monoid [int] (mempty [] 0))   ;; TUR-E0393: no Semigroup [int]\n"
      "\n"
      "Add the missing instance; it may appear anywhere in the program, above\n"
      "or below the subclass instance.  A parametric instance discharges the\n"
      "obligation against its own declared constraints: `Monoid [(Vec A)]\n"
      "[(Monoid A)]` needs a `Semigroup [(Vec A)]`, which may itself be\n"
      "parametric.  A superclass instance is REQUIRED, never inherited -- the\n"
      "`Monoid` instance does not supply `combine`, the `Semigroup` one does.\n" },
    { TUR_W0373_REFINE_NONLINEAR,
      "TUR-W0373: Nonlinear predicate subterm treated as uninterpreted\n"
      "\n"
      "Multiplication or division of two variables (`(* x y)`, `(/ x y)`) is\n"
      "outside the linear fragment the refinement solver decides.  Such a term\n"
      "is abstracted to an opaque function symbol: congruence closure still\n"
      "relates two occurrences of the same product, but no arithmetic facts\n"
      "about it are available, so proofs that depend on them will come back\n"
      "unknown (TUR-W0372) and fall back to the runtime check.\n"
      "\n"
      "This is deliberate.  Turmeric does not climb the nonlinear wall; a\n"
      "genuinely nonlinear obligation gets a runtime check instead.\n"
      "\n"
      "Multiplication by a LITERAL stays linear and is fully decided:\n"
      "  (* x 2)   ; linear -- decided\n"
      "  (* x y)   ; nonlinear -- uninterpreted\n",
    },
    { TUR_E0374_REFINE_INSTANCE_STRONGER,
      "TUR-E0374: Instance method demands more than its class signature\n"
      "\n"
      "A typeclass method's parameter refinement in the CLASS signature is the\n"
      "promise callers program against.  An instance may accept MORE than the\n"
      "class promises, but it may not accept less: a caller that honours the\n"
      "class contract would then be handed to an instance that rejects its\n"
      "argument, and the method's entry check would panic on a value the\n"
      "caller was entitled to pass.\n"
      "\n"
      "Example:\n"
      "  (defclass Scaler [a]\n"
      "    (scale-by [self : a, k : #refine{ v : int | (>= v 0) }] : int))\n"
      "\n"
      "  (definstance Scaler [int]\n"
      "    (scale-by [self : int, k : #refine{ v : int | (> v 0) }] : int\n"
      "      (* self k)))    ; error: rejects 0, which the class admits\n"
      "\n"
      "Either widen the instance to match the class, or narrow the class\n"
      "signature so every caller knows the stronger requirement.\n"
      "\n"
      "Only reported when the compiler can PROVE the instance is stronger; an\n"
      "undecidable pair is left to the runtime check.\n",
    },
    { TUR_E0151_RELEVANT_DROPPED,
      "TUR-E0151: Relevant value dropped without being used\n"
      "\n"
      "A value annotated with ^relevant went out of scope without being used at\n"
      "least once. Relevant values implement no-weakening: they must be observed\n"
      "or consumed before going out of scope, but may be duplicated freely.\n"
      "\n"
      "This is part of the unified substructural type system (-Xsubstructural).\n"
      "The three disciplines and their drop rules:\n"
      "  ^linear   -- must use exactly once; cannot drop unused\n"
      "  ^affine   -- may drop unused; cannot duplicate\n"
      "  ^relevant -- must use at least once; may duplicate (cannot drop unused)\n"
      "\n"
      "Example of the error:\n"
      "  (let [^relevant r (acquire-resource)]\n"
      "    0)            ; ERROR: r dropped without being used\n"
      "\n"
      "Fix: use the value at least once before the scope ends:\n"
      "  (let [^relevant r (acquire-resource)]\n"
      "    (log r)       ; first use\n"
      "    (store r))    ; second use -- OK, duplication allowed\n"
      "\n"
      "If you only need to ensure the value is dropped (not necessarily used),\n"
      "consider ^affine or ^linear instead.\n"
      "\n"
      "Enable with: tur -Xsubstructural myfile.tur\n",
    },
    /* ET4: effect scope errors */
    { TUR_E0250_ROW_VAR_ESCAPES_SCOPE,
      "TUR-E0250: Effect row variable escapes its quantifier scope\n"
      "\n"
      "A row variable introduced by forall [e] is being used outside the\n"
      "quantifier scope that defined it.  Row variables are only valid within\n"
      "the type expression that quantifies over them.\n"
      "\n"
      "Example (bad):\n"
      "  ;; forall [e] row variable 'e' referenced outside its scope\n"
      "\n"
      "Fix: ensure the row variable is only referenced within the forall body,\n"
      "or introduce a new forall quantifier at the appropriate scope level.\n",
    },
    { TUR_E0253_EFFECT_NOT_IN_SCOPE,
      "TUR-E0253: Effect not in scope at perform site\n"
      "\n"
      "A (perform (EffectName ...)) call uses an effect that is not declared\n"
      "in the current scope.  This can happen if the effect has not been\n"
      "defined (missing defeffect), is defined in another module without being\n"
      "imported, or is declared with ^private and is not accessible here.\n"
      "\n"
      "Example (bad):\n"
      "  (perform (Foo 1))  ; error if Foo is not in scope\n"
      "\n"
      "Fix: define the effect with defeffect or import it from the module\n"
      "that owns it before using perform.\n",
    },
    /* ET3: handler typing errors */
    { TUR_E0251_HANDLER_OVERLAP,
      "TUR-E0251: Overlapping handler effects\n"
      "\n"
      "Two handlers composed via compose-handlers both handle the same algebraic\n"
      "effect.  Each effect may only be handled once in a composed handler.\n"
      "\n"
      "Example (bad):\n"
      "  (defeffect Write [s :cstr] :nil)\n"
      "  (compose-handlers h1 h2)  ; error if both h1 and h2 handle Write\n"
      "\n"
      "Fix: ensure that each composed handler handles a distinct set of effects.\n",
    },
    { TUR_E0252_HANDLER_RESULT_MISMATCH,
      "TUR-E0252: Handler clause result type mismatch\n"
      "\n"
      "The result type of a handler clause body does not match the result type of\n"
      "the enclosing handle expression.  All handler clause bodies and the handled\n"
      "expression body must produce the same type.\n"
      "\n"
      "Example (bad):\n"
      "  (defeffect Write [s :cstr] :nil)\n"
      "  (handle\n"
      "    (do (perform (Write \"x\")) 0)  ; handle expression returns :int\n"
      "    (Write [s] k) \"wrong type\")    ; error: clause returns :cstr\n"
      "\n"
      "Fix: ensure the handler clause body produces the same type as the handled\n"
      "expression body.  Use (resume k value) to continue with a value of the\n"
      "correct type.\n",
    },
    /* MS2: Multi-shot continuation capture analysis */
    { TUR_E0500_MULTISHOT_UNIQUE_CAPTURE,
      "TUR-E0500: Multi-shot handler captures a non-copyable value\n"
      "\n"
      "A handler clause annotated with ^multishot captures a binding whose\n"
      "CopyKind is CK_UNIQUE (move-only) or CK_LINEAR (exactly-once).  Because\n"
      "^multishot continuations may be resumed any number of times via snapshots,\n"
      "each resume would re-enter the handler body, accessing the same non-copyable\n"
      "binding multiple times -- violating its ownership semantics.\n"
      "\n"
      "Example of the error:\n"
      "  (defstruct MyData [x :int])\n"
      "  (defeffect Ask [] :int)\n"
      "  (let [d (MyData 42)]\n"
      "    (handle (perform (Ask))\n"
      "      (Ask [] ^multishot k)\n"
      "        (resume k (.-x d))))  ; ERROR: d is move-only (TUR-E0500)\n"
      "\n"
      "Fix options:\n"
      "  1. Change the captured type to be Copy (e.g. use primitive int, not a struct):\n"
      "     (let [x 42]\n"
      "       (handle (perform (Ask))\n"
      "         (Ask [] ^multishot k) (resume k x)))  ; OK: int is CK_COPY\n"
      "\n"
      "  2. If the struct is `:copy`, the capture is allowed:\n"
      "     (defstruct CopyData :copy [x :int])\n",
    },
    { TUR_E0501_MULTISHOT_ANN_OUTSIDE_HANDLER,
      "TUR-E0501: ^multishot annotation outside a handler continuation\n"
      "\n"
      "The ^multishot annotation is only valid as the continuation-kind\n"
      "annotation in a (handle ...) clause:\n"
      "\n"
      "  (handle body\n"
      "    (Effect [params] ^multishot k) handler-body)\n"
      "\n"
      "Using ^multishot as a let-binding annotation, a function parameter\n"
      "annotation, or in any other position is not supported.\n"
      "\n"
      "Example of the error:\n"
      "  (let [^multishot x 5] ...)   ; ERROR: ^multishot not valid here\n"
      "\n"
      "Fix: remove the ^multishot annotation.  If you need multi-shot\n"
      "continuation semantics, annotate the k parameter in a handler clause:\n"
      "  (handle body\n"
      "    (Effect [] ^multishot k) (resume k v))\n",
    },
    { TUR_E0502_MULTISHOT_RESUME_IN_ATOMIC,
      "TUR-E0502: Cannot resume a ^multishot continuation inside atomically\n"
      "\n"
      "Resuming a multi-shot continuation inside an (atomically ...) block is\n"
      "forbidden.  Effect-handler fiber resumption runs arbitrary code outside\n"
      "STM semantics, which can observe or modify non-transactional state and\n"
      "violate the atomicity guarantee.\n"
      "\n"
      "Example of the error:\n"
      "  (atomically\n"
      "    (handle (perform (Ask))\n"
      "      (Ask [] ^multishot k)\n"
      "        (resume k 10)))  ; ERROR: ^multishot resume inside atomically\n"
      "\n"
      "Fix: move the handle expression outside the atomically block, or use\n"
      "a plain STM operation instead of a ^multishot effect handler.\n",
    },
    /* SS4: Session type explanations */
    { TUR_E0210_SESSION_NOT_DUAL,
      "TUR-E0210: Session endpoints are not dual\n"
      "\n"
      "The two protocols supplied to make-session must be exact duals of each\n"
      "other.  Duality is defined structurally:\n"
      "  Send T P   <->  Recv T P'\n"
      "  Recv T P   <->  Send T P'\n"
      "  Choose P Q <->  Branch P' Q'\n"
      "  Branch P Q <->  Choose P' Q'\n"
      "  Close      <->  Close\n"
      "  Rec self P <->  Rec self P'  (where P and P' are dual)\n"
      "\n"
      "make-session takes one protocol argument and derives the dual\n"
      "automatically; this error fires when you supply two explicit protocols\n"
      "that are not dual, or when a type synonym expands to a non-dual pair.\n"
      "\n"
      "Example of the error:\n"
      "  ;; Both ends claim to Send -- not dual.\n"
      "  (let [[s r] (make-session (Send int Close))]\n"
      "    ...)   ; ERROR: (Send int Close) and (Send int Close) are not dual\n"
      "\n"
      "Fix: ensure one end sends where the other receives:\n"
      "  ;; make-session derives the dual for you:\n"
      "  (let [[s r] (make-session (Send int Close))]\n"
      "    ;; s : Session[Send int Close]\n"
      "    ;; r : Session[Recv int Close]  (derived dual)\n"
      "    ...)\n"
      "\n"
      "Enable with: tur -Xsessions myfile.tur\n",
    },
    { TUR_E0211_SESSION_DROPPED,
      "TUR-E0211: Linear session channel dropped without being closed\n"
      "\n"
      "Session channels are linear resources: they must be consumed exactly once.\n"
      "A channel is consumed by calling (close ch) when the protocol reaches\n"
      "Close, or by passing it to a function that takes ownership and closes it.\n"
      "Letting a channel go out of scope without closing it leaks the underlying\n"
      "OS resources and leaves the peer waiting forever.\n"
      "\n"
      "Example of the error:\n"
      "  (defn bad [] :int\n"
      "    (let [[s r] (make-session (Send int Close))]\n"
      "      42))  ; ERROR: s and r go out of scope unclosed\n"
      "\n"
      "Fix: close both endpoints before the scope ends:\n"
      "  (defn good [] :int\n"
      "    (let [[s r] (make-session (Send int Close))]\n"
      "      (let [s (send s 0)]\n"
      "        (close s)\n"
      "        (let [[_ r] (recv r)]\n"
      "          (close r)\n"
      "          42))))\n"
      "\n"
      "If a channel endpoint is passed to another function, that function\n"
      "becomes responsible for closing it (linear ownership transfer).\n"
      "\n"
      "Enable with: tur -Xsessions myfile.tur\n",
    },
    { TUR_E0212_SESSION_PROTO_MISMATCH,
      "TUR-E0212: Session operation does not match the current protocol state\n"
      "\n"
      "Each session operation advances the channel to the next protocol state.\n"
      "Using the wrong operation for the current state is a compile-time error:\n"
      "  (send ch v)        -- requires Session[Send T ...]\n"
      "  (recv ch)          -- requires Session[Recv T ...]\n"
      "  (choose-left ch)   -- requires Session[Choose P Q]\n"
      "  (choose-right ch)  -- requires Session[Choose P Q]\n"
      "  (offer ch)         -- requires Session[Branch P Q]\n"
      "  (close ch)         -- requires Session[Close]\n"
      "\n"
      "Example of the error:\n"
      "  (let [[s _r] (make-session (Recv int Close))]\n"
      "    (send s 42))   ; ERROR: s is Session[Recv int Close], not Send\n"
      "\n"
      "Fix: use the operation that matches the protocol state:\n"
      "  (let [[s _r] (make-session (Send int Close))]\n"
      "    (let [s (send s 42)]   ; OK: s is Session[Send int Close]\n"
      "      (close s)))\n"
      "\n"
      "If the protocol requires receiving before sending, follow the protocol\n"
      "order: recv first, then send.\n"
      "\n"
      "Enable with: tur -Xsessions myfile.tur\n",
    },
    /* SS5: Global protocol type errors */
    { TUR_E0220_GLOBAL_NOT_PROJECTABLE,
      "TUR-E0220: Global protocol is not projectable onto role\n"
      "\n"
      "A global protocol step cannot be projected onto the given role because the\n"
      "role is not involved at that point in the protocol, or the choice structure\n"
      "is ambiguous from that role's perspective.\n"
      "\n"
      "This error is checked during SS6 (projection). In SS5, the global protocol\n"
      "is only parsed and well-formedness is checked.\n"
      "\n"
      "A protocol with a timed receive is projected onto every role when it is\n"
      "declared.  Only the receiver of a (timeout (-> From To T) [ok ...]\n"
      "[expired ...]) learns whether the deadline passed, so every other role --\n"
      "the sender included -- must continue the same way in both branches:\n"
      "  (defprotocol Bad [A B C]\n"
      "    (timeout (-> A B int)\n"
      "      [ok      (-> B C int)]\n"
      "      [expired]))          ; error: C cannot tell whether to receive\n"
      "Fix: give the other roles the same steps in both branches, e.g. have the\n"
      "receiver send C a message in each.\n",
    },
    { TUR_E0221_ROLE_NOT_DECLARED,
      "TUR-E0221: Role is not declared in global protocol\n"
      "\n"
      "A role name used in a protocol interaction was not listed in the protocol's\n"
      "role declaration.\n"
      "\n"
      "Example:\n"
      "  (defprotocol Ping [A B]\n"
      "    (-> A C int))   ; error: C is not declared\n"
      "\n"
      "Fix: add C to the role list or correct the role name.\n",
    },
    { TUR_E0222_ROLE_IMPL_MISMATCH,
      "TUR-E0222: Role implementation does not match projected local type\n"
      "\n"
      "The implementation code for a role does not match the local session type\n"
      "projected from the global protocol for that role.\n"
      "\n"
      "This error is checked during SS6 (projection and role implementation checking).\n",
    },
    { TUR_E0223_GLOBAL_NOT_WELLFORMED,
      "TUR-E0223: Global protocol is not well-formed\n"
      "\n"
      "The global protocol declaration contains a structural error:\n"
      "\n"
      "  - A role sends a message to itself (self-send)\n"
      "  - A role name in an interaction is not declared in the protocol header\n"
      "  - A (loop ...) body contains no interactions before (continue ...) (unguarded)\n"
      "  - A (choice ...) has fewer than 2 branches\n"
      "\n"
      "Examples:\n"
      "  (defprotocol Bad [A B]\n"
      "    (-> A A int))   ; error: self-send\n"
      "\n"
      "  (defprotocol Bad [A B]\n"
      "    (loop X (continue X)))  ; error: unguarded recursion\n"
      "\n"
      "Fix: correct the interaction structure so the protocol is well-formed.\n",
    },
    /* DV0-DV1: Dynamic var errors (-Xdynamic-vars) */
    { TUR_E0600_DYNVAR_SET_NOT_DYNAMIC,
      "TUR-E0600: set! or binding target is not a dynamic var\n"
      "\n"
      "The name given to set! or used as a binding key in a (binding [...] ...)\n"
      "form is not declared as a dynamic var with defdynamic.\n"
      "\n"
      "Example of the error (set!):\n"
      "  (let [x 0]\n"
      "    (set! x 1))   ; ERROR: x is a plain let binding, not a dynamic var\n"
      "\n"
      "Example of the error (binding with non-dynamic name):\n"
      "  (let [x 0]\n"
      "    (binding [x 1]   ; ERROR: x is not a dynamic var\n"
      "      x))\n"
      "\n"
      "Fix: declare the var with defdynamic at module toplevel:\n"
      "  (defdynamic *x* :int 0)\n"
      "  (binding [*x* 1]\n"
      "    *x*)   ; => 1\n"
      "\n"
      "For lexical shadowing of a plain local, use let instead:\n"
      "  (let [x 0]\n"
      "    (let [x 1]\n"
      "      x))   ; => 1\n",
    },
    { TUR_E0601_DYNVAR_SET_NO_BINDING,
      "TUR-E0601: set! on a dynamic var with no active binding frame\n"
      "\n"
      "set! mutates the current thread's top binding frame for the dynamic var.\n"
      "If no binding form is active on the current thread for that var, there is\n"
      "no frame to mutate and the operation is rejected.\n"
      "\n"
      "Example of the error:\n"
      "  (defdynamic *log-level* :int 0)\n"
      "  (set! *log-level* 2)   ; ERROR: no binding frame active\n"
      "\n"
      "Fix: wrap the set! inside a binding form:\n"
      "  (defdynamic *log-level* :int 0)\n"
      "  (binding [*log-level* 0]\n"
      "    (set! *log-level* 2)   ; OK: mutates the binding-frame value\n"
      "    *log-level*)           ; => 2\n"
      "\n"
      "To update the global root value (rarely needed), use alter-root! with\n"
      "the -Xunsafe-alter-root flag instead.\n",
    },
    { TUR_E0602_DYNVAR_TYPE_MISMATCH,
      "TUR-E0602: Override value type does not match the defdynamic declared type\n"
      "\n"
      "Every binding override and every set! mutation must produce a value of\n"
      "exactly the type declared in defdynamic.  The declared type is fixed and\n"
      "does not change when overridden.\n"
      "\n"
      "Example of the error:\n"
      "  (defdynamic *log-level* :int 0)\n"
      "  (binding [*log-level* \"verbose\"]   ; ERROR: expected :int, got :str\n"
      "    *log-level*)\n"
      "\n"
      "Fix: use a value of the correct type:\n"
      "  (binding [*log-level* 2]   ; OK\n"
      "    *log-level*)             ; => 2\n",
    },
    { TUR_E0603_DYNVAR_SUBSTRUCTURAL_TYPE,
      "TUR-E0603: Dynamic var declared with a substructural type\n"
      "\n"
      "Dynamic vars may be read by any number of callers on any thread.  A\n"
      "substructural type (linear, affine, relevant, or unique) carries an\n"
      "ownership discipline that limits how many times a value may be used or\n"
      "whether it may be copied.  Storing such a type in a dynamic var would\n"
      "violate that discipline on every read.\n"
      "\n"
      "This includes:\n"
      "  - ^linear  (CK_LINEAR)  -- exactly one consumer required\n"
      "  - ^affine  (CK_AFFINE)  -- at most one consumer\n"
      "  - ^relevant (CK_RELEVANT) -- at least one consumer required\n"
      "  - ^unique  (CK_UNIQUE)  -- move-only (single owner)\n"
      "  - session channels (^linear internally)\n"
      "\n"
      "Example of the error:\n"
      "  (defdynamic *resource* ^linear :int 0)   ; ERROR: linear type\n"
      "\n"
      "Fix: use a plain copyable (CK_COPY) type:\n"
      "  (defdynamic *resource* :int 0)   ; OK\n"
      "\n"
      "If you need to pass a unique resource through call chains, consider\n"
      "algebraic effects (defeffect / perform / handle) instead, which\n"
      "correctly track ownership at each resume point.\n",
    },
    { TUR_E0604_DYNVAR_NOT_TOPLEVEL,
      "TUR-E0604: defdynamic used outside module toplevel\n"
      "\n"
      "defdynamic declarations must appear at module toplevel, alongside defn\n"
      "and defstruct.  Defining a dynamic var inside a function body, a let\n"
      "block, or any other nested position is not supported.\n"
      "\n"
      "Example of the error:\n"
      "  (defn setup [] :unit\n"
      "    (defdynamic *x* :int 0))   ; ERROR: defdynamic inside defn\n"
      "\n"
      "Fix: move the defdynamic to module toplevel:\n"
      "  (defdynamic *x* :int 0)   ; OK: module toplevel\n"
      "  (defn setup [] :unit\n"
      "    (binding [*x* 1] ...))\n",
    },
    { TUR_E0605_DYNVAR_SET_IN_ATOMIC,
      "TUR-E0605: set! on a dynamic var inside an atomically block\n"
      "\n"
      "STM transactions (atomically) may be retried an arbitrary number of\n"
      "times.  Mutating the per-thread binding stack inside a transaction would\n"
      "not be rolled back on retry, leaving the binding stack in an inconsistent\n"
      "state.  To prevent this, set! on a dynamic var is rejected inside\n"
      "atomically.\n"
      "\n"
      "Example of the error:\n"
      "  (defdynamic *log-level* :int 0)\n"
      "  (binding [*log-level* 1]\n"
      "    (atomically\n"
      "      (set! *log-level* 2)))   ; ERROR: set! inside atomically\n"
      "\n"
      "Fix options:\n"
      "  1. Move the set! outside the atomically block.\n"
      "  2. Use an STM ref (ref / alter / deref) for values that must be\n"
      "     mutated atomically -- those are correctly rolled back on retry.\n",
    },
    /* DV0: Dynamic var naming warning */
    { TUR_W0600_DYNVAR_NO_EARMUFFS,
      "TUR-W0600: defdynamic name does not use *earmuffs* convention\n"
      "\n"
      "By convention, dynamic vars use *earmuffs* (leading and trailing\n"
      "asterisks) to signal that they are dynamically scoped and may vary\n"
      "across threads or binding frames.  Omitting the earmuffs makes dynamic\n"
      "vars harder to distinguish from ordinary module-level values.\n"
      "\n"
      "Example triggering this warning:\n"
      "  (defdynamic log-level :int 0)   ; TUR-W0600: no earmuffs\n"
      "\n"
      "Fix: rename to use earmuffs:\n"
      "  (defdynamic *log-level* :int 0)   ; OK\n"
      "\n"
      "Suppress with -Wno-dynvar-earmuffs if your project intentionally omits\n"
      "this convention.\n",
    },
    /* fn-type-bare-identifier-plan Phase 3: redundant colon inside (fn ...) */
    { TUR_D0001_FN_TYPE_COLON,
      "TUR-D0001: leading colon inside a (fn ...) type is deprecated\n"
      "\n"
      "Inside a (fn [params...] result) *type* expression, position alone\n"
      "tells the elaborator which forms are parameter types and which is the\n"
      "result type.  The leading ':' on each inner type is therefore\n"
      "redundant and is being phased out.\n"
      "\n"
      "Example triggering this warning:\n"
      "  (defn compose [^fat f : (fn [:float] #{} :float)] : ptr<void> ...)\n"
      "                            ^^^^^^         ^^^^^^  redundant colons\n"
      "\n"
      "Fix: drop the colons on the inner types:\n"
      "  (defn compose [^fat f : (fn [float] #{} float)] : ptr<void> ...)\n"
      "\n"
      "The structural colon between the binder (^fat f) and its type stays;\n"
      "only the *inner* types inside (fn ...) lose theirs.  Run\n"
      "tools/rewrite_fn_type_colons.py to migrate a tree automatically.\n"
      "Promoted to an error under --Werror=deprecated.\n",
    },
    /* fx-row-syntax-rename-plan Phase 2: bare #{...} effect row */
    { TUR_D0002_FX_ROW_LEGACY_HASH,
      "TUR-D0002: bare `#{...}` effect row is deprecated; prefer `#fx{...}`\n"
      "\n"
      "The effect-row annotation used in function/handler signatures has\n"
      "moved from the bare `#{...}` form to the self-describing `#fx{...}`\n"
      "form, in line with the other reader literals (`#map{...}`,\n"
      "`#set{...}`, `#row{...}`, `#refine{...}`, `#r{...}`).  The bare\n"
      "`#{...}` slot is being reclaimed for a future reader literal.\n"
      "\n"
      "Example triggering this warning:\n"
      "  (defn read-line [] #{Unsafe IO} : cstr ...)\n"
      "                     ^^^^^^^^^^^^  legacy form\n"
      "\n"
      "Fix: prefix with `fx`:\n"
      "  (defn read-line [] #fx{Unsafe IO} : cstr ...)\n"
      "\n"
      "Migration is fully mechanical -- run tools/migrate-fx-rows.py to\n"
      "rewrite a tree.  The legacy form is removed in a future release.\n",
    },
    /* fx-row-syntax-rename-plan Phase 2: @{...} effect row */
    { TUR_D0003_FX_ROW_LEGACY_AT,
      "TUR-D0003: `@{...}` effect row is deprecated; prefer `#fx{...}`\n"
      "\n"
      "`@{...}` was an alternate sugar for the same effect-row form spelled\n"
      "`#{...}`.  Having two spellings was the original wart this rename\n"
      "fixes, so `@{...}` is being retired alongside the bare `#{...}` form;\n"
      "both migrate to `#fx{...}`.\n"
      "\n"
      "Bare `@x` deref sugar is unaffected -- only the `@{...}` effect-row\n"
      "form is deprecated.\n"
      "\n"
      "Example triggering this warning:\n"
      "  (defn read-line [] @{Unsafe IO} : cstr ...)\n"
      "                     ^^^^^^^^^^^^  legacy form\n"
      "\n"
      "Fix: rewrite as `#fx{...}`:\n"
      "  (defn read-line [] #fx{Unsafe IO} : cstr ...)\n"
      "\n"
      "Run tools/migrate-fx-rows.py to rewrite a tree.  Removed in a future\n"
      "release.\n",
    },
    /* effect-row-honesty-plan W0 */
    { TUR_D0004_NONEXHAUSTIVE_FX_MARKER,
      "TUR-D0004: `#fx{NonExhaustive}` is deprecated; write `^non-exhaustive`\n"
      "\n"
      "The opt-out from match exhaustiveness checking is an attribute of the\n"
      "match, not an effect, so it no longer borrows the effect-row brackets.\n"
      "`#fx{...}` now holds effects and row variables only.\n"
      "\n"
      "Example triggering this warning:\n"
      "  (match #fx{NonExhaustive} e (Left l) l)\n"
      "\n"
      "Fix:\n"
      "  (match ^non-exhaustive e (Left l) l)\n"
      "\n"
      "The old spelling still opts out, so a program keeps compiling; under\n"
      "--Werror=deprecated it is an error.  It is removed in a future release.\n",
    },
    /* XF (experimental-flag-mechanism-plan): unknown --enable= name */
    { TUR_E0310_UNKNOWN_EXPERIMENT,
      "TUR-E0310: unknown experiment\n"
      "\n"
      "`--enable=<name>` (or a `:experiments [...]` entry in build.tur) named an\n"
      "experiment that is not in the compiler's registry.  Unknown names are a\n"
      "hard error -- not a warning -- so typos surface immediately rather than\n"
      "silently doing nothing.\n"
      "\n"
      "Fix: run `tur experiments` to see the exact set of recognized names, then\n"
      "correct the spelling (or drop the flag if the feature has graduated and no\n"
      "longer needs a gate).\n",
    },
    /* engine-selection-plan: unknown :engine value */
    { TUR_E0311_UNKNOWN_ENGINE,
      "TUR-E0311: unknown :engine value\n"
      "\n"
      "build.tur's `:engine` key (or the --engine flag / TUR_ENGINE env var)\n"
      "named an execution engine outside the recognized set: \"cc\" (compile\n"
      "via the C emitter and run the binary -- the default and the reference),\n"
      "\"jit\" (the in-process MIR engine; needs a -DTUR_JIT=ON build and the\n"
      "`jit` experiment), or \"interp\" (the tree-walking interpreter).\n"
      "\n"
      "Unknown values are a hard error rather than a fallback: the engines\n"
      "differ in SEMANTICS (see `#?(:tur ... :turi ...)`, inline-C carve-outs,\n"
      "c2mir divergences), so silently substituting one is the worst outcome.\n"
      "\n"
      "Fix: correct the spelling.  The precedence ladder is\n"
      "  --engine > TUR_ENGINE env > build.tur :engine > \"cc\".\n",
    },
    /* saffron-lang-plan D7: static-only feature in a Saffron file */
    { TUR_E0312_SAFFRON_STATIC_ONLY,
      "TUR-E0312: this feature's proof reads a type, and Saffron has `any`\n"
      "\n"
      "Today this is exactly one feature: `with-region` (and any other region\n"
      "bracket) in a `#lang saffron` file.\n"
      "\n"
      "A region rewinds a generation only when the compiler can prove nothing\n"
      "outside it still points in, and that proof is a static walk over the\n"
      "bracket's RESULT TYPE.  An `any` result clears nothing, so the walk\n"
      "refuses and the emitter RETIRES the generation instead of reclaiming\n"
      "it -- `tur_region_pop` where Turmeric emits `tur_region_pop_checked`.\n"
      "\n"
      "So the bracket is safe, and saves nothing.  You paid a push and a pop\n"
      "and every allocation inside stayed mapped.  That is why this is an\n"
      "error rather than a downgrade: a feature that silently does nothing\n"
      "leaves a file that LOOKS like it carries a guarantee it does not.\n"
      "\n"
      "The restriction is per FILE, not per program.  Move the allocation-\n"
      "heavy code into a Turmeric module, keep the bracket there, and call it\n"
      "across the boundary -- the two dialects link, they just do not each\n"
      "get the other's guarantees.\n"
      "\n"
      "WHAT IS NOT RESTRICTED.  An earlier draft of the plan listed GADTs,\n"
      "sessions, linearity and borrows here too.  Measured, all four check\n"
      "IDENTICALLY in a Saffron file, because their proofs do not read an\n"
      "inferred type:\n"
      "\n"
      "  defgadt      constructor return types are ANNOTATIONS, which stay\n"
      "               legal in Saffron -- skolem escape still fires.\n"
      "  Session[P]   the protocol is in the annotation -- TUR-E0211 still\n"
      "               fires, with the same protocol state.\n"
      "  ^linear      a use-count walk over the body -- TUR-E0101 still fires.\n"
      "  &T  &mut T   a scope walk over uses -- the aliasing conflict still\n"
      "               fires.\n"
      "\n"
      "`with-region` is the odd one out precisely because its proof reads the\n"
      "one thing Saffron makes `any`: the inferred result type of the body.\n",
    },
    /* XF: prototype experimental feature in use */
    { TUR_W0060_EXPERIMENTAL_PROTOTYPE,
      "TUR-W0060: experimental feature (prototype) in use\n"
      "\n"
      "You enabled an experiment whose lifecycle is `prototype`: its algorithm or\n"
      "surface still changes between releases, so breaking changes are likely.\n"
      "The warning fires once per compile at the first use site.\n"
      "\n"
      "This is expected when you opt in with `--enable=<name>`.  To proceed\n"
      "anyway, keep the flag; the feature works, it is just not stable.  Do NOT\n"
      "depend on the surface from a published spice yet.  The warning fires\n"
      "regardless of how the experiment was enabled -- there is no gate to\n"
      "silence it.\n",
    },
    /* XF: beta experimental feature in use */
    { TUR_W0061_EXPERIMENTAL_BETA,
      "TUR-W0061: experimental feature (beta) in use\n"
      "\n"
      "You enabled an experiment whose lifecycle is `beta`: the surface is frozen\n"
      "and soaking for one release cycle before it graduates to always-on.  The\n"
      "warning names the version it graduates in and fires once per compile.\n"
      "\n"
      "When the feature graduates the flag becomes an accept-and-warn no-op, so\n"
      "code written against the beta surface keeps compiling.  The warning fires\n"
      "regardless of how the experiment was enabled -- there is no gate to\n"
      "silence it.\n",
    },
    /* multiple-reads-params */
    { TUR_E0024_READS_FRAME_INVALID,
      "TUR-E0024: malformed or duplicated `#reads` frame\n"
      "\n"
      "`#reads` names the `^borrow` parameters whose mutable state a measure\n"
      "reads.  It takes one name or a vector, exactly like `#writes`:\n"
      "\n"
      "  (defn alive? [^borrow w : W e : int] #reads w : bool ...)        ; ok\n"
      "  (defn f [^borrow w : W ^borrow g : G] #reads [w g] : bool ...)   ; ok\n"
      "\n"
      "Rejected shapes:\n"
      "\n"
      "  #reads w #reads g   two frames -- name every parameter in ONE frame\n"
      "  #reads [w w]        a name repeated\n"
      "  #reads [w zz]       a name that is not a parameter\n"
      "  #reads []           an empty frame\n"
      "\n"
      "An empty frame is rejected where `#writes []` is allowed, because the two\n"
      "annotations claim different things.  `#writes []` usefully asserts *this\n"
      "body writes nothing*; an empty read frame says exactly what omitting the\n"
      "annotation says, and one claim with two spellings gives the solver two\n"
      "ways to ask the same question.\n"
      "\n"
      "A multi-parameter frame is CONJUNCTIVE where it matters: the congruence\n"
      "grant applies only when EVERY named parameter is frozen at the call site.\n"
      "One unfrozen parameter is enough for two occurrences of the measure to\n"
      "denote different values, which is the crossing check the grant elides.\n",
    },
    /* let-binding-void-call-emits-invalid-c */
    { TUR_E0023_BIND_VOID_EXPRESSION,
      "TUR-E0023: cannot bind an expression of type :void\n"
      "\n"
      "A `let` binding names a value, and a `:void` expression does not produce\n"
      "one -- there is nothing for the name to refer to, and the binding could\n"
      "never be legally read.\n"
      "\n"
      "This usually comes up when sequencing a side effect inside a binding\n"
      "list, which is natural to reach for when the `let` body has to end in a\n"
      "particular value:\n"
      "\n"
      "  (let [buf (alloc-buf 32)\n"
      "        _   (fill-buf! buf 32 255)]   ; :void -- rejected\n"
      "    (check buf))\n"
      "\n"
      "Sequence it with `do` instead:\n"
      "\n"
      "  (let [buf (alloc-buf 32)]\n"
      "    (do\n"
      "      (fill-buf! buf 32 255)\n"
      "      (check buf)))\n"
      "\n"
      "Or give the helper a return value it is useful to bind (a count, a\n"
      "status, the buffer itself), which also lets it be threaded through a\n"
      "binding list.\n",
    },
    /* exports-map-syntax-tighten-plan */
    { TUR_E0620_EXPORTS_FX_ROW,
      "TUR-E0620: `:exports` got an effect-row literal instead of a map\n"
      "\n"
      "`:exports` in build.tur expects either a map literal (`#map{ \"mod/name\"\n"
      "[sym ...] ... }`) or a legacy path vector.  Effect-row literals\n"
      "(`#fx{...}` and the older `@{...}`) are the spelling used in function\n"
      "type annotations to declare an effect set, not exported-module maps.\n"
      "\n"
      "Older manifests used the bare `#{...}` map spelling; that continues to\n"
      "work.  The bug is specifically the `#fx{...}` / `@{...}` reader tag.\n"
      "\n"
      "Fix: replace `#fx{...}` with `#map{...}`:\n"
      "\n"
      "  :exports #map{\n"
      "    \"app/main\" [\"main\"]\n"
      "    \"app/util\" [\"double-it\"]\n"
      "  }\n",
    },
    /* serial-shift-unsupported-context-miscompile */
    { TUR_E0706_SERIAL_CONTEXT_NOT_CAPTURABLE,
      "TUR-E0706: serial-shift context is not capturable\n"
      "\n"
      "A serial-shift can only be lowered when its delimited context (the part\n"
      "of the enclosing serial-reset between the reset and the shift) fits the\n"
      "DK-lowering grammar: a single-scalar-hole chain of scalar `let` preludes,\n"
      "`+ - * /` binops, 2-arg top-level calls, one `if`, and the supported\n"
      "statement-position `do` tail shape.  Outside that subset the continuation\n"
      "cannot be reified into a marshalable DK chain.\n"
      "\n"
      "Previously such contexts silently miscompiled -- the shift lowered to a\n"
      "`0` placeholder (wrong result, no error) or a `__builtin_trap()` (runtime\n"
      "crash).  They are now rejected at codegen instead.\n"
      "\n"
      "Example triggering this error (1-arg call context):\n"
      "  (serial-reset (dbl (serial-shift rt 0)))   ; dbl is 1-arg: not capturable\n"
      "\n"
      "Fix: restructure the context into a supported shape -- e.g. pack loop\n"
      "state into a single Serializable struct passed as a tail call's argument\n"
      "  (do (init) (serial-shift k v) (run-loop state))\n"
      "-- or move the non-capturable work outside the serial-reset boundary.\n"
      "\n"
      "The same code covers effects in two remaining places.  A RECEIVER (the\n"
      "function handed the continuation) may perform an effect it does not\n"
      "handle itself -- the reset then calls it on its own continuation, so the\n"
      "effect reaches the handlers around the serial-reset:\n"
      "  (defn recv [k : serial-cont] : int (k (perform (Ask))))  ; accepted\n"
      "That holds for a named receiver and for a closure literal capturing\n"
      "plain values (ints, floats, strings), with or without an `if` in the\n"
      "context.  A closure receiver capturing anything else, and a CONTEXT\n"
      "callee an effect escapes, are refused:\n"
      "  (defn page2 [env : cstr hole : int] : int (+ hole (perform (Ask))))\n"
      "  (serial-reset (page2 \"\" (serial-shift recv 0)))  ; Ask escapes page2\n"
      "A context callee runs when the continuation is RESUMED -- possibly in\n"
      "another process, from bytes -- so which handlers its effect should reach\n"
      "is not settled.  Handle the effect inside the receiver or the callee (or\n"
      "a function it calls), or perform it outside the reset.\n",
    },
    /* cloneable-shift-unsupported-context-miscompile (D6a) */
    { TUR_E0710_CLONEABLE_CONTEXT_NOT_CAPTURABLE,
      "TUR-E0710: cloneable-shift context is not capturable\n"
      "\n"
      "A cloneable-shift can only be lowered when its delimited context (the\n"
      "part of the enclosing cloneable-reset between the reset and the shift)\n"
      "fits the native build_cloneable grammar: a single-scalar-hole chain of\n"
      "scalar `let` preludes, `+ - * /` binops (with an atomic or pure operand),\n"
      "1- and 2-arg top-level uncolored calls, and one `if` branch point, up to\n"
      "a bounded depth.  Outside that subset the captured continuation cannot be\n"
      "reified into a multi-shot cloneable continuation.\n"
      "\n"
      "Previously such contexts silently miscompiled -- the legacy setjmp\n"
      "fallback lowered the continuation as the identity, dropping the context,\n"
      "so e.g. `(+ (compute) (cloneable-shift ...))` printed a wrong number with\n"
      "no error.  They are now rejected at codegen instead.\n"
      "\n"
      "Fix: restructure the context into a supported shape, or move the\n"
      "non-capturable work outside the cloneable-reset boundary.\n",
    },
    /* float-register-class-returns */
    { TUR_E0707_RETURN_REGISTER_CLASS_MISMATCH,
      "TUR-E0707: return type / body register-class mismatch\n"
      "\n"
      "A function's declared return type and the type of its body must occupy\n"
      "the same machine register class.  `int`, `cstr`, `bool`, opaque handles,\n"
      "and struct/ADT handles all ride the int64 general-purpose register, so\n"
      "the ABI tolerates swapping them in the result position -- a no-op bitwise\n"
      "reinterpret.  A `float`/`float32`/`float64`, by contrast, lives in a\n"
      "floating-point (xmm) register.  Returning a float where the declared\n"
      "return is a non-float (or vice versa) is a genuine register-class\n"
      "miscompile -- the caller reads xmm0 while the callee left the value in\n"
      "rax (or the reverse).\n"
      "\n"
      "Examples triggering this error:\n"
      "  (defn g [] : int   7.1)   ; float body, int return\n"
      "  (defn h [] : Pt    7.1)   ; float body, struct return\n"
      "\n"
      "Fix: make the body's register class match the declared return -- convert\n"
      "explicitly with (as int expr) / (as float expr), or correct whichever of\n"
      "the two annotations is wrong.\n"
      "\n"
      "Note: an *integer literal* returned where a float is declared (e.g.\n"
      "  (defn f [] : float 42)\n"
      ") is NOT an error -- it is widened to the float in place, exactly as a\n"
      "numeric literal coerces in argument and binding positions.\n",
    },
    /* pointer-vs-scalar-returns */
    { TUR_E0708_RETURN_POINTER_SCALAR_MISMATCH,
      "TUR-E0708: cstr return type / integer body mismatch\n"
      "\n"
      "A function (or instance method) declares its return type as `cstr` -- a\n"
      "`const char*` string pointer -- but its body yields a concrete\n"
      "integer-family scalar (int, bool, int8/16/32/64, uint8/16/32/64).\n"
      "\n"
      "`cstr` and the integer family all ride the same int64 general-purpose\n"
      "register, so the carrier ABI cannot see the swap -- it is a silent bitwise\n"
      "reinterpret.  But a bare integer is never a valid string pointer:\n"
      "committing to `cstr` and handing back an integer is a type-erasure bug\n"
      "that surfaces downstream as a bogus pointer dereference.\n"
      "\n"
      "Examples triggering this error:\n"
      "  (defn f [x : int] : cstr 42)        ; int body, cstr return\n"
      "  (defn g [x : int] : cstr (+ x 1))   ; int body, cstr return\n"
      "\n"
      "Fix: return an actual string (a string literal or a cstr-typed value), or\n"
      "correct the declared return type to match what the body produces.\n"
      "\n"
      "Note: only this *commit* direction is rejected.  The reverse -- a function\n"
      "declared to return an integer carrier whose body yields a `cstr` handle --\n"
      "is the deliberate int64 carrier-handle bridge (generic and typeclass code\n"
      "routinely passes pointer handles through an int64 result slot) and stays\n"
      "accepted.\n",
    },
    { TUR_E0709_RETURN_TYPE_MISMATCH,
      "TUR-E0709: committed return type / body type mismatch\n"
      "\n"
      "A genuinely *committed* function -- a monomorphic `defn` that is not\n"
      "`#{Unsafe}` and takes no type parameters, so it does not participate in the\n"
      "int64 carrier ABI -- declares a concrete return type but its body yields a\n"
      "concrete value of a different, carrier-sharing type.  Two cases are caught:\n"
      "\n"
      "  1. cstr-vs-integer (the REVERSE of TUR-E0708): a concrete integer-family\n"
      "     return (int, bool, int8/16/32/64, uint8/16/32/64) with a `cstr`\n"
      "     (`const char*` string pointer) body.  A string pointer is never a\n"
      "     valid integer.\n"
      "  2. bool-vs-integer: a `bool` return with a non-bool integer body, or the\n"
      "     reverse.  bool and the integer family share the int64 0/1\n"
      "     representation, but the language treats them as distinct (a\n"
      "     `(let [b : bool 1] ...)` binding is rejected; boolean constants are\n"
      "     `true`/`false`, not `0`/`1`).\n"
      "\n"
      "The first two are carrier-sharing reinterprets the carrier ABI cannot see.\n"
      "Generic / typeclass / `#{Unsafe}` code routinely relies on that bridge, so\n"
      "the swaps stay accepted there.  But a committed monomorphic function has no\n"
      "carrier to bridge -- the int64 reinterpret is absent -- so the mismatch is a\n"
      "real type-erasure bug.\n"
      "\n"
      "  3. aggregate-vs-scalar: a by-value record ADT (a real `tur_adt_S` C\n"
      "     struct) or a `:heap` ADT application (a typed pointer to one) on one\n"
      "     side, and a concrete scalar on the other.  This case is NOT limited to\n"
      "     a committed position: the tolerances above exist because both sides are\n"
      "     `int64_t` in the emitted C, and an aggregate is not, so no amount of\n"
      "     carrier participation makes the two interchangeable.  A generic defn,\n"
      "     an `#{Unsafe}` one, and a typeclass instance method all fail the same\n"
      "     way in the C compiler.  A transparent int newtype (`defopaque H :int`)\n"
      "     and a non-heap parametric ADT are NOT aggregates for this purpose --\n"
      "     they lower to the carrier or have a crossing that grounds them.\n"
      "\n"
      "Examples triggering this error:\n"
      "  (defn f [] : int  \"hello\")          ; cstr body, int return\n"
      "  (defn g [] : bool 42)                ; integer body, bool return\n"
      "  (defn h [x : int] : int (< x 3))     ; bool body, int return\n"
      "  (defn k [x : S] : int x)             ; record-ADT body, int return\n"
      "\n"
      "Fix: correct the declared return type to match the body, or return a value\n"
      "of the declared type.  For cases 1 and 2, if the function is genuinely\n"
      "carrier-participating, mark it `#{Unsafe}` or give it a type parameter --\n"
      "the bridge is then intentional and accepted.  Case 3 has no such escape:\n"
      "extract a scalar from the aggregate (a field, a tag) or declare the\n"
      "aggregate type.\n",
    },
    { TUR_E0332_SWEET_DOLLAR_IN_BRACKETS,
      "TUR-E0332: `$` inside brackets\n"
      "\n"
      "In a sweet-expression file, `$` is the rest-of-line marker: `f $ g x`\n"
      "reads as `(f (g x))`.  It belongs to the indentation layer, and a\n"
      "`(...)`, `[...]` or `{...}` form switches that whole layer off for\n"
      "everything inside it -- indentation and `$` alike.  Inside brackets,\n"
      "whitespace is insignificant and there is no \"rest of the line\" to\n"
      "delimit, so the marker has no meaning.\n"
      "\n"
      "Example:\n"
      "  (fn [msg : cstr] : unit\n"
      "    log/error $ str(\"a\" msg))     ; error: `$` inside brackets\n"
      "\n"
      "Write the call with a delimiter instead -- neoteric or s-expression:\n"
      "  (fn [msg : cstr] : unit\n"
      "    log/error(str(\"a\" msg)))      ; ok\n"
      "  (fn [msg : cstr] : unit\n"
      "    (log/error (str \"a\" msg)))    ; ok\n"
      "\n"
      "The same `$` is correct once it is outside every bracket, where the\n"
      "indentation layer is live again:\n"
      "  defn f [msg : cstr] : unit\n"
      "    log/error $ str(\"a\" msg)      ; ok: (log/error (str \"a\" msg))\n"
      "\n"
      "This used to be silent: the marker was left alone and reached the\n"
      "reader as an ordinary symbol named `$`, so the form gained an extra\n"
      "element and meant something the author never wrote.\n",
    },
    { TUR_E0716_TAILCALL_NOT_TAIL,
      "TUR-E0716: `^tailcall` call is not in tail position\n"
      "\n"
      "`^tailcall` is a CHECKED annotation: it says \"this call must become a\n"
      "real tail call, and if it cannot, tell me now.\"  Without it, whether a\n"
      "call is lowered to a backedge is invisible in the source and the failure\n"
      "mode is a stack overflow at an unpredictable depth, in production.\n"
      "\n"
      "  (defn loop-down [n : int] : int\n"
      "    (if (= n 0) 0 ^tailcall (loop-down (- n 1))))   ; ok: becomes a goto\n"
      "\n"
      "The message names the specific reason.  What Turmeric guarantees today is\n"
      "the SELF tail call, and the MUTUAL tail call within a group of functions\n"
      "that tail-call each other in a cycle (fused into one C function with a\n"
      "dispatch loop): a direct, arity-matching call, reached through `if` (two-armed), `do`, `match` arms, and\n"
      "`let`/`letrec` with plain scalar bindings -- including a `let` that owns a\n"
      "`ref<T>`, `rc<T>` or other value with drop glue, as long as that value is\n"
      "dead at the call.  That guarantee holds at -O0; it is a backedge in the\n"
      "emitted C, not an optimizer favor.\n"
      "\n"
      "The reasons you will see, and what to do about each:\n"
      "\n"
      "  work follows the call -- the call's value is used, or it is not the\n"
      "    last form of its block.  Restructure so the call is the whole answer.\n"
      "\n"
      "  a `defer` written in the block runs after it -- a `defer` you wrote has\n"
      "    an observable order (after the call, innermost first), so it cannot\n"
      "    move ahead of a backedge.  Move the `defer` into a block that ends\n"
      "    before the call.\n"
      "\n"
      "  an owned local is still live at the call -- a `ref<T>`, `rc<T>` or other\n"
      "    value with drop glue whose drop would have to wait for the call.  Its\n"
      "    drop moves ahead of the backedge only when every use of it copies a\n"
      "    plain number out (`@b`, `(.count o)`).  Passing it, capturing it in a\n"
      "    closure, or borrowing it keeps it live.  Read what you need into a\n"
      "    scalar first, or narrow the owned value's scope so it ends before the\n"
      "    call.\n"
      "\n"
      "  the enclosing `match` cannot serve a tail position -- it yields no\n"
      "    value, has no arms, or offers over a session channel.\n"
      "\n"
      "  the callee is a different function outside this one's group -- a\n"
      "    mutual tail call is guaranteed only when the functions form a group:\n"
      "    they call each other in a cycle of TAIL calls, each is a plain\n"
      "    top-level `defn` (no closure, dictionary, inline-C, effectful or\n"
      "    `catch-unwind` body; no variadic, `fn`-typed or pass-by-pointer\n"
      "    parameter), they share a C return type, and the group has at most 8\n"
      "    members and 16 parameters in all.  A call to a function that is not in\n"
      "    a cycle with this one is an ordinary call, and fine as one.\n"
      "\n"
      "  the callee is not statically known -- an indirect call, through a `fn`\n"
      "    value, a struct field, or a rank-2 polymorphic parameter.  Typed\n"
      "    Turmeric does not trampoline these (T-D6); a direct call does not have\n"
      "    the limit.  A DYNAMIC call in Saffron -- through an `any` -- is\n"
      "    different: in tail position it bounces to a trampoline, so\n"
      "    `^tailcall` accepts it and only its position can refuse it.\n"
      "\n"
      "  a one-armed `if` -- an `if` with no else branch is not on the tail path.\n"
      "    Give it an else branch.\n"
      "\n"
      "  the enclosing function is not eligible -- it is variadic, returns `void`,\n"
      "    is `main`, has an inline-C body, or has a parameter the backedge cannot\n"
      "    reassign (pass-by-pointer struct, `fn`-typed, or carrier-ABI).\n"
      "\n"
      "Under `--interpret` every tail call is already proper -- the tree-walking\n"
      "evaluator trampolines direct, mutual and indirect alike -- so this check is\n"
      "a property of the COMPILED path and runs during C emission.\n"
      "\n"
      "Removing the annotation always silences the error.  It does not make the\n"
      "call a tail call; it only stops asking.\n",
    },
};

#define N_DIAG_EXPLANATIONS \
    ((int)(sizeof(diag_explanations_) / sizeof(diag_explanations_[0])))

/* Phase HKT-P5: Print the long-form explanation for `code` to `out`.
 * Returns true if an explanation was found and printed, false otherwise. */
bool diag_explain(DiagCode code, FILE *out) {
    for (int i = 0; i < N_DIAG_EXPLANATIONS; i++) {
        if (diag_explanations_[i].code == code) {
            fputs(diag_explanations_[i].text, out);
            return true;
        }
    }
    return false;
}


Span diag_translate_span(Span span) {
    const SourceFile *f = diag_source_file(span.file_id);
    if (!f) return span;
    if (!f->xform_map || !f->orig_src) {
        /* No syntax transform, but a stripped `#lang` line still shifts every
         * offset relative to the file on disk. */
        span.off_start += (uint32_t)f->head_offset;
        span.off_end   += (uint32_t)f->head_offset;
        return span;
    }

    size_t orig_start = sweet_map_translate_offset(f->xform_map, span.off_start);
    size_t orig_end   = sweet_map_translate_offset(f->xform_map, span.off_end);
    if (orig_end < orig_start) orig_end = orig_start;

    uint32_t line = 1, col = 1;
    for (size_t i = 0; i < orig_start && i < f->orig_len; i++) {
        if (f->orig_src[i] == '\n') { line++; col = 1; }
        else col++;
    }
    uint32_t col_e = col;
    for (size_t i = orig_start; i < orig_end && i < f->orig_len; i++) {
        if (f->orig_src[i] == '\n') col_e = 1;
        else col_e++;
    }
    span.line      = line;
    span.col_start = col;
    span.col_end   = col_e;
    span.off_start = (uint32_t)(orig_start + f->head_offset);
    span.off_end   = (uint32_t)(orig_end + f->head_offset);
    return span;
}

/* Render a multi-line snippet with context, underlines, and colors.
   Phase 8: Enhanced with configurable options and multi-span support. */
static void render_snippet_ex(const SourceFile *f, Span span, const SnippetOpts *opts) {
    if (!f || span.off_start > f->len) return;

    /* If the file was produced by a syntax transformer (sweet-exp), render
     * the snippet from the user's original source and translate the span
     * via the xform map so column positions match what the user wrote.
     * Line numbers are preserved by the transformer, so span.line is the
     * same on both sides. */
    SourceFile mapped;
    if (f->xform_map != NULL && f->orig_src != NULL) {
        mapped = *f;
        mapped.src = f->orig_src;
        mapped.len = f->orig_len;
        size_t orig_start = sweet_map_translate_offset(f->xform_map, span.off_start);
        size_t orig_end   = sweet_map_translate_offset(f->xform_map, span.off_end);
        /* Recompute line/col from the original offsets. */
        uint32_t line = 1, col = 1;
        for (size_t i = 0; i < orig_start && i < mapped.len; i++) {
            if (mapped.src[i] == '\n') { line++; col = 1; }
            else col++;
        }
        span.line = line;
        span.col_start = col;
        uint32_t line_e = line, col_e = col;
        for (size_t i = orig_start; i < orig_end && i < mapped.len; i++) {
            if (mapped.src[i] == '\n') { line_e++; col_e = 1; }
            else col_e++;
        }
        (void)line_e;
        span.col_end = col_e;
        span.off_start = (uint32_t)orig_start;
        span.off_end = (uint32_t)orig_end;
        f = &mapped;
    }

    const SnippetOpts default_opts = SNIPPET_OPTS_DEFAULT;
    const SnippetOpts *o = opts ? opts : &default_opts;
    
    const char *color = use_color_ ? COLOR_BOLD : "";
    const char *reset = use_color_ ? COLOR_RESET : "";
    
    /* Calculate the range of lines to show */
    uint32_t error_line = span.line;
    uint32_t context = o->context_lines;
    uint32_t start_line = (error_line > context + 1) ? error_line - context - 1 : 1;
    uint32_t end_line = error_line + context;
    
    /* Get the width for line numbers (padding) */
    int line_num_width = 1;
    uint32_t temp = end_line;
    while (temp >= 10) {
        line_num_width++;
        temp /= 10;
    }
    
    /* Print each line with line number and content */
    uint32_t current_line = 1;
    uint32_t line_start = 0;
    
    for (uint32_t i = 0; i <= f->len; i++) {
        if (i == f->len || f->src[i] == '\n') {
            /* Check if we should stop before processing this line */
            if (current_line > end_line) break;
            
            if (current_line >= start_line && current_line <= end_line) {
                /* Skip empty lines at the end of the file */
                uint32_t line_len = i - line_start;
                if (line_len == 0 && i == f->len) {
                    /* Empty line at end of file - skip it */
                } else {
                    /* Print line number and gutter */
                    if (o->show_line_numbers) {
                        if (current_line == error_line) {
                            fprintf(stderr, "%s%*u %s|%s ", color, line_num_width, current_line, reset, color);
                        } else {
                            fprintf(stderr, "%*u | ", line_num_width, current_line);
                        }
                    } else {
                        if (current_line == error_line) {
                            fprintf(stderr, "%s|%s ", color, reset);
                        } else {
                            fprintf(stderr, " | ");
                        }
                    }
                    
                    /* Print the line content */
                    if (f->src[line_start] != '\n') {
                        fwrite(f->src + line_start, 1, line_len, stderr);
                    }
                    fprintf(stderr, "%s\n", reset);
                    
                    /* Print underline for the error line */
                    if (current_line == error_line) {
                        /* Calculate column position (0-based in the line) */
                        uint32_t col_start_0 = span.col_start - 1;
                        uint32_t col_end_0 = span.col_end - 1;
                        
                        /* Print gutter space */
                        if (o->show_line_numbers) {
                            fprintf(stderr, "%*s | ", line_num_width, "");
                        } else {
                            fprintf(stderr, " | ");
                        }
                        
                        /* Print spaces up to the start column */
                        for (uint32_t c = 0; c < col_start_0; c++) {
                            char ch = (c < line_len) ? f->src[line_start + c] : ' ';
                            if (ch == '\t') {
                                fputc('\t', stderr);
                            } else {
                                fputc(' ', stderr);
                            }
                        }
                        
                        /* Print underline with the appropriate style */
                        uint32_t underline_len = (col_end_0 > col_start_0) ? col_end_0 - col_start_0 : 1;
                        fprintf(stderr, "%s", color);
                        char underline_ch = underline_char(o->primary_style);
                        for (uint32_t c = 0; c < underline_len; c++) {
                            fputc(underline_ch, stderr);
                        }
                        fprintf(stderr, "%s\n", reset);
                    }
                }
            }
            
            if (f->src[i] == '\n') {
                line_start = i + 1;
                current_line++;
            }
            
            /* If we just processed the last line, check if we should stop */
            if (i == f->len) break;
        }
    }
}

/* Forward declaration (defined later, after diag_set_json_output) */
static void json_escape_string(Buf *b, const char *s);

/* -------------------------------------------------------------------------
 * LSP collection mode -- must be declared before diag_emitv / diag_emit_*
 * --------------------------------------------------------------------- */

typedef struct DiagLspEntry {
    DiagLevel level;
    DiagCode  code;
    uint32_t  line0;        /* 0-based */
    uint32_t  col_start0;   /* 0-based */
    uint32_t  col_end0;     /* 0-based */
    char      file[256];    /* path copied at emit time (avoids dangling ptr) */
    char      message[512];
    /* The outermost file on the origin chain (diag_set_file_origin) -- the
     * one the entry file named -- and the span of the form that named it, in
     * `via_parent`.  Empty when the file has no recorded origin. */
    char      via[256];
    char      via_parent[256];
    uint32_t  via_line0, via_col_start0, via_col_end0;
    /* The outermost macro call this diagnostic was raised under
     * (diag_set_expansion_site), when that call is in another file than the
     * diagnostic.  Empty otherwise. */
    char      exp_file[256];
    char      exp_macro[64];
    uint32_t  exp_line0, exp_col_start0, exp_col_end0;
} DiagLspEntry;

static bool          lsp_collect_ = false;
static DiagLspEntry *lsp_entries_ = NULL;
static size_t        lsp_entry_count_ = 0;
static size_t        lsp_entry_cap_ = 0;

/* Diagnostic sink (embed API -- see diag.h). */
static DiagSinkFn diag_sink_fn_ = NULL;
static void      *diag_sink_ud_ = NULL;

void diag_set_sink(DiagSinkFn fn, void *ud) {
    diag_sink_fn_ = fn;
    diag_sink_ud_ = ud;
}

DiagSinkFn diag_get_sink(void **out_ud) {
    if (out_ud) *out_ud = diag_sink_ud_;
    return diag_sink_fn_;
}

/* Deliver one diagnostic record to the installed sink.  Returns true when a
 * sink consumed it (so the caller suppresses the stderr render). */
static bool diag_sink_dispatch(DiagLevel level, DiagCode code, Span span,
                               const char *msg) {
    if (!diag_sink_fn_) return false;
    const SourceFile *f = (span.file_id < MAX_FILES) ? files_[span.file_id] : NULL;
    const char *path = (f && f->path) ? f->path : "";
    const char *code_str = diag_code_to_string(code);
    diag_sink_fn_(level, code_str ? code_str : "", path,
                  span.line, span.col_start, span.col_end,
                  msg ? msg : "", diag_sink_ud_);
    return true;
}

static void lsp_append(DiagLevel level, DiagCode code, Span span, const char *msg) {
    if (lsp_entry_count_ >= lsp_entry_cap_) {
        lsp_entry_cap_ = lsp_entry_cap_ ? lsp_entry_cap_ * 2 : 32;
        lsp_entries_ = realloc(lsp_entries_, lsp_entry_cap_ * sizeof(DiagLspEntry));
    }
    DiagLspEntry *e = &lsp_entries_[lsp_entry_count_++];
    e->level      = level;
    e->code       = code;
    e->line0      = span.line > 0 ? span.line - 1 : 0;
    e->col_start0 = span.col_start > 0 ? span.col_start - 1 : 0;
    e->col_end0   = span.col_end > 0 ? span.col_end - 1 : 0;
    /* Copy path now while SourceFile* is still valid (it may be stack-allocated) */
    const SourceFile *f = (span.file_id < MAX_FILES) ? files_[span.file_id] : NULL;
    snprintf(e->file, sizeof(e->file), "%s", f && f->path ? f->path : "");
    snprintf(e->message, sizeof(e->message), "%s", msg ? msg : "");

    e->exp_file[0] = e->exp_macro[0] = '\0';
    if (expansion_site_.line > 0 && expansion_site_.file_id < MAX_FILES &&
        expansion_site_.file_id != span.file_id &&
        files_[expansion_site_.file_id] && files_[expansion_site_.file_id]->path) {
        snprintf(e->exp_file, sizeof(e->exp_file), "%s",
                 files_[expansion_site_.file_id]->path);
        snprintf(e->exp_macro, sizeof(e->exp_macro), "%s",
                 expansion_macro_ ? expansion_macro_ : "");
        e->exp_line0      = expansion_site_.line - 1;
        e->exp_col_start0 = expansion_site_.col_start > 0 ? expansion_site_.col_start - 1 : 0;
        e->exp_col_end0   = expansion_site_.col_end > 0 ? expansion_site_.col_end - 1 : 0;
        /* A call spanning lines has an end column on another line. */
        if (expansion_site_.off_end > expansion_site_.off_start &&
            e->exp_col_end0 <= e->exp_col_start0)
            e->exp_col_end0 = e->exp_col_start0 + 1;
    }

    /* Walk the origin chain while the SourceFiles are still alive.  The
     * depth cap only guards a cycle (a file loading itself back). */
    e->via[0] = e->via_parent[0] = '\0';
    uint16_t cur = span.file_id, top = MAX_FILES;
    Span top_origin = SPAN_UNKNOWN;
    for (int d = 0; d < 64 && cur < MAX_FILES && origins_[cur].line > 0; d++) {
        top = cur;
        top_origin = origins_[cur];
        cur = top_origin.file_id;
    }
    if (top < MAX_FILES && files_[top] && files_[top]->path &&
        files_[top_origin.file_id] && files_[top_origin.file_id]->path) {
        snprintf(e->via, sizeof(e->via), "%s", files_[top]->path);
        snprintf(e->via_parent, sizeof(e->via_parent), "%s",
                 files_[top_origin.file_id]->path);
        e->via_line0      = top_origin.line - 1;
        e->via_col_start0 = top_origin.col_start > 0 ? top_origin.col_start - 1 : 0;
        e->via_col_end0   = top_origin.col_end > 0 ? top_origin.col_end - 1 : 0;
    }
}

/* Build the LSP diagnostics array.  With `doc_path` set, entries from another
 * file are relocated onto the document (see diag_lsp_flush_array_for); with
 * it NULL, every entry keeps its own coordinates -- the shape the MCP server
 * and diag_lsp_flush want. */
static void lsp_build_array_ex(Buf *b, const char *doc_path,
                               DiagLspRelocateFn relocate, void *ctx) {
    static const int lsp_severity[] = { 1, 2, 3, 4 };
    buf_putc(b, '[');
    for (size_t i = 0; i < lsp_entry_count_; i++) {
        const DiagLspEntry *e = &lsp_entries_[i];
        int sev = (e->level <= DIAG_HELP) ? lsp_severity[e->level] : 3;
        /* A zero-width range paints nothing: the squiggle has no characters to
         * sit under, so the diagnostic is invisible in the editor even though
         * it is present in the response. Every client had to widen these by
         * hand; widen once here instead. */
        unsigned col_end = e->col_end0 > e->col_start0 ? e->col_end0
                                                       : e->col_start0 + 1;

        /* An entry from another file -- a `load`ed or imported module -- has
         * coordinates that mean nothing in this document.  Published as-is it
         * underlined whatever sat at that line and column of the open buffer. */
        bool foreign = doc_path && relocate && e->file[0] &&
                       strcmp(e->file, doc_path) != 0;
        uint32_t line0 = e->line0, cs0 = e->col_start0, ce0 = col_end;
        char rel_uri[1024];
        bool have_uri = false;
        /* The file the document names, when it is not e->file itself: the
         * outermost file on the origin chain, if that chain reaches here. */
        const char *via = NULL;
        if (foreign && e->via[0] && strcmp(e->via, e->file) != 0 &&
            strcmp(e->via, doc_path) != 0)
            via = e->via;
        /* Raised inside an expansion of a macro the document calls: the call
         * is the code the user wrote, and beats any load/import anchor --
         * the macro's file may not be named in the document at all (the
         * auto-loaded stdlib). */
        bool at_call = foreign && e->exp_file[0] &&
                       strcmp(e->exp_file, doc_path) == 0;
        bool placed = at_call;
        if (at_call) via = NULL;
        if (at_call) {
            line0 = e->exp_line0; cs0 = e->exp_col_start0; ce0 = e->exp_col_end0;
            if (ce0 <= cs0) ce0 = cs0 + 1;
            rel_uri[0] = '\0';
            bool ignored = false;
            uint32_t l = 0, c0 = 0, c1 = 1;
            have_uri = relocate(ctx, e->file, &l, &c0, &c1, &ignored,
                                rel_uri, sizeof(rel_uri));
        } else if (foreign) {
            line0 = 0; cs0 = 0; ce0 = 1;
            rel_uri[0] = '\0';
            bool anchored = false;
            have_uri = relocate(ctx, e->file, &line0, &cs0, &ce0, &anchored,
                                rel_uri, sizeof(rel_uri));
            /* lsp-transitive-load-anchors-on-line-one: a file loaded by a
             * loaded file is not named in the document; anchor on the form
             * that names the outermost file on its chain instead. */
            if (!anchored && via) {
                char via_uri[1024];
                uint32_t l = 0, c0 = 0, c1 = 1;
                (void)relocate(ctx, via, &l, &c0, &c1, &anchored,
                               via_uri, sizeof(via_uri));
                if (anchored) { line0 = l; cs0 = c0; ce0 = c1; }
            }
            /* The text scan missed (a form split across lines, a path spelled
             * differently): fall back to the span the compiler recorded. */
            if (!anchored && e->via[0] && strcmp(e->via_parent, doc_path) == 0) {
                line0 = e->via_line0;
                cs0   = e->via_col_start0;
                ce0   = e->via_col_end0;
                anchored = true;
            }
            if (ce0 <= cs0) ce0 = cs0 + 1;
            placed = anchored;
        }

        /* A stdlib file is named `stdlib/<file>` wherever it is installed.
         * When nothing in the document leads to it, it is in practice the
         * auto-loaded stdlib with an error of its own -- a stdlib bug, or a
         * stdlib that does not match this compiler.  The user can do nothing
         * about it in this file, so say so instead of letting it read as
         * theirs.  `stdlib_dir` is set by resolve_stdlib_root at startup. */
        char stdlib_shown[300];
        const char *stdlib_dir = NULL;
        bool in_stdlib = false;
        if (foreign) {
            stdlib_dir = getenv("TUR_STDLIB_DIR");
            size_t sl = stdlib_dir ? strlen(stdlib_dir) : 0;
            while (sl > 1 && stdlib_dir[sl - 1] == '/') sl--;
            if (sl && strncmp(e->file, stdlib_dir, sl) == 0 && e->file[sl] == '/') {
                snprintf(stdlib_shown, sizeof(stdlib_shown), "stdlib%s",
                         e->file + sl);
                in_stdlib = true;
            }
        }

        if (i > 0) buf_putc(b, ',');
        buf_printf(b,
            "{\"severity\":%d"
            ",\"range\":{\"start\":{\"line\":%u,\"character\":%u}"
                       ",\"end\":{\"line\":%u,\"character\":%u}}",
            sev, line0, cs0, line0, ce0);
        buf_puts(b, ",\"message\":");
        if (foreign) {
            Buf m;
            buf_init(&m);
            /* Name the file relative to the document's directory when it
             * is under it -- `greeter.tur`, not a 90-character temp path. */
            const char *shown = e->file;
            const char *slash = strrchr(doc_path, '/');
            if (slash) {
                size_t dl = (size_t)(slash - doc_path) + 1;
                if (strncmp(e->file, doc_path, dl) == 0 && e->file[dl])
                    shown = e->file + dl;
            }
            if (in_stdlib) shown = stdlib_shown;
            buf_printf(&m, "in %s:%u:%u", shown, e->line0 + 1,
                       e->col_start0 + 1);
            if (via) {
                const char *vshown = via;
                if (slash) {
                    size_t dl = (size_t)(slash - doc_path) + 1;
                    if (strncmp(via, doc_path, dl) == 0 && via[dl])
                        vshown = via + dl;
                }
                buf_printf(&m, " (via %s)", vshown);
            }
            if (at_call && e->exp_macro[0])
                buf_printf(&m, " (expanding %s)", e->exp_macro);
            buf_printf(&m, ": %s", e->message);
            if (in_stdlib && !placed)
                buf_printf(&m, "\n  this error is in the standard library, not "
                           "in this file: check that the stdlib at %s matches "
                           "this compiler (tur --version), and report it as a "
                           "stdlib bug if it does", stdlib_dir);
            buf_putc(&m, '\0');
            json_escape_string(b, m.data);
            buf_free(&m);
        } else {
            json_escape_string(b, e->message);
        }
        const char *code_str = diag_code_to_string(e->code);
        if (code_str && code_str[0]) {
            buf_puts(b, ",\"code\":");
            json_escape_string(b, code_str);
        }
        buf_puts(b, ",\"source\":\"turmeric\"");
        if (e->file[0]) {
            buf_puts(b, ",\"file\":");
            json_escape_string(b, e->file);
        }
        if (foreign && have_uri) {
            buf_puts(b, ",\"relatedInformation\":[{\"location\":{\"uri\":");
            json_escape_string(b, rel_uri);
            buf_printf(b,
                ",\"range\":{\"start\":{\"line\":%u,\"character\":%u}"
                           ",\"end\":{\"line\":%u,\"character\":%u}}}",
                e->line0, e->col_start0, e->line0, col_end);
            buf_puts(b, ",\"message\":");
            json_escape_string(b, e->message);
            buf_puts(b, "}]");
        }
        buf_putc(b, '}');
    }
    buf_putc(b, ']');
}

static void lsp_build_array(Buf *b) {
    lsp_build_array_ex(b, NULL, NULL, NULL);
}

/* Original snippet rendering (backward compatible) */
static void render_snippet(const SourceFile *f, Span span) {
    render_snippet_ex(f, span, NULL);
}

/* Public snippet rendering function (Phase 8) */
void diag_render_snippet(const SourceFile *f, Span span, const SnippetOpts *opts) {
    render_snippet_ex(f, span, opts);
}

/* r7rs-lang-plan R10: the Scheme lowering gives every local binder a unique
 * name -- `n` becomes `n__v12` (and a template's `__h`/`__p`/`__l`/`__g`
 * renames) -- so a diagnostic about a Scheme file would show that spelling.
 * Strip the suffix back to the name the user wrote, for `#lang r7rs` files
 * only.  In place; the message only gets shorter. */
static void diag_scheme_names(Span span, char *msg) {
    const SourceFile *f = span.file_id < MAX_FILES ? files_[span.file_id] : NULL;
    if (!f || f->lang != LANG_R7RS) return;
    char *w = msg;
    for (const char *r = msg; *r; ) {
        if (r[0] == '_' && r[1] == '_' && r[2] && strchr("vhgpl", r[2]) &&
            r[3] >= '0' && r[3] <= '9' && r > msg) {
            const char *e = r + 3;
            while (*e >= '0' && *e <= '9') e++;
            char c = *e;
            /* Only at the END of an identifier. */
            if (!(c == '_' || c == '-' || c == '?' || c == '!' ||
                  (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
                r = e;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

void diag_emitv(DiagLevel level, Span span, const char *fmt, va_list ap) {
    if (diag_intercept(level)) return;
    if (level == DIAG_ERROR) had_error_ = true;

    if (lsp_collect_) {
        char msg[512];
        vsnprintf(msg, sizeof(msg), fmt, ap);
        diag_scheme_names(span, msg);
        lsp_append(level, DIAG_CODE_NONE, span, msg);
        return;
    }

    if (diag_sink_fn_) {
        char msg[512];
        vsnprintf(msg, sizeof(msg), fmt, ap);
        diag_scheme_names(span, msg);
        diag_sink_dispatch(level, DIAG_CODE_NONE, span, msg);
        return;
    }

    /* Phase 8: If JSON output is enabled, emit in JSON format */
    if (json_output_) {
        char msg[1024];
        vsnprintf(msg, sizeof(msg), fmt, ap);
        diag_scheme_names(span, msg);
        diag_emit_json(level, span, DIAG_CODE_NONE, msg);
        return;
    }

    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *path = f ? f->path : "<unknown>";

    /* Color the level name */
    const char *color = color_for_level(level);
    const char *reset = use_color_ ? COLOR_RESET : "";
    
    /* Phase 8: Rust-style diagnostics with --> pointing to file */
    fprintf(stderr, "%s%s:%u:%u: %s%s: ", color, path, span.line, span.col_start, level_name(level), reset);
    {
        char msg[4096];
        vsnprintf(msg, sizeof(msg), fmt, ap);
        diag_scheme_names(span, msg);
        fputs(msg, stderr);
    }
    fputc('\n', stderr);

    /* Multi-line source snippet with context */
    if (f && span.off_start <= f->len) {
        render_snippet(f, span);
    }
}

void diag_emit(DiagLevel level, Span span, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    diag_emitv(level, span, fmt, ap);
    va_end(ap);
}

/* Emit diagnostic with error code (Phase 8) */
void diag_emit_with_code(DiagLevel level, Span span, DiagCode code, const char *fmt, ...) {
    if (diag_intercept(level)) return;
    if (level == DIAG_ERROR) had_error_ = true;

    if (lsp_collect_) {
        va_list ap;
        va_start(ap, fmt);
        char msg[512];
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        diag_scheme_names(span, msg);
        lsp_append(level, code, span, msg);
        return;
    }

    if (diag_sink_fn_) {
        va_list ap;
        va_start(ap, fmt);
        char msg[512];
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        diag_scheme_names(span, msg);
        diag_sink_dispatch(level, code, span, msg);
        return;
    }

    /* Phase 8: If JSON output is enabled, emit in JSON format */
    if (json_output_) {
        va_list ap;
        va_start(ap, fmt);
        char msg[1024];
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        diag_scheme_names(span, msg);
        diag_emit_json(level, span, code, msg);
        return;
    }

    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *path = f ? f->path : "<unknown>";

    const char *color = color_for_level(level);
    const char *reset = use_color_ ? COLOR_RESET : "";
    const char *code_str = diag_code_to_string(code);
    
    va_list ap;
    va_start(ap, fmt);
    
    if (code != DIAG_CODE_NONE) {
        fprintf(stderr, "%s%s:%u:%u: %s [%s]%s: ", color, path, span.line, span.col_start, level_name(level), code_str, reset);
    } else {
        fprintf(stderr, "%s%s:%u:%u: %s%s: ", color, path, span.line, span.col_start, level_name(level), reset);
    }
    {
        char msg[4096];
        vsnprintf(msg, sizeof(msg), fmt, ap);
        diag_scheme_names(span, msg);
        fputs(msg, stderr);
    }
    fputc('\n', stderr);
    
    va_end(ap);

    if (f && span.off_start <= f->len) {
        render_snippet(f, span);
    }
}

/* Emit diagnostic with related notes (Phase 8) */
void diag_emit_with_notes(DiagLevel level, Span span, const char *message,
                          DiagNote *notes, size_t note_count) {
    if (diag_intercept(level)) return;
    if (level == DIAG_ERROR) had_error_ = true;

    if (lsp_collect_) {
        lsp_append(level, DIAG_CODE_NONE, span, message);
        for (size_t i = 0; i < note_count; i++)
            lsp_append(notes[i].level, DIAG_CODE_NONE, notes[i].span, notes[i].message);
        return;
    }

    if (diag_sink_fn_) {
        diag_sink_dispatch(level, DIAG_CODE_NONE, span, message);
        for (size_t i = 0; i < note_count; i++)
            diag_sink_dispatch(notes[i].level, DIAG_CODE_NONE, notes[i].span,
                               notes[i].message);
        return;
    }

    /* Phase 8: If JSON output is enabled, emit primary message in JSON format */
    if (json_output_) {
        diag_emit_json(level, span, DIAG_CODE_NONE, message);
        /* Also emit notes in JSON format */
        for (size_t i = 0; i < note_count; i++) {
            diag_emit_json(notes[i].level, notes[i].span, DIAG_CODE_NONE, notes[i].message);
        }
        return;
    }

    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *path = f ? f->path : "<unknown>";

    const char *color = color_for_level(level);
    const char *reset = use_color_ ? COLOR_RESET : "";
    
    /* Print primary error */
    fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", color, path, span.line, span.col_start, level_name(level), reset, message);

    /* Print primary snippet */
    if (f && span.off_start <= f->len) {
        render_snippet(f, span);
    }
    
    /* Print notes */
    for (size_t i = 0; i < note_count; i++) {
        const SourceFile *note_f = NULL;
        if (notes[i].span.file_id < MAX_FILES) note_f = files_[notes[i].span.file_id];
        const char *note_path = note_f ? note_f->path : "<unknown>";
        const char *note_color = color_for_level(notes[i].level);
        
        fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", note_color, note_path, 
                notes[i].span.line, notes[i].span.col_start,
                level_name(notes[i].level), reset, notes[i].message);
        
        if (note_f && notes[i].span.off_start <= note_f->len) {
            SnippetOpts note_opts = SNIPPET_OPTS_DEFAULT;
            note_opts.primary_style = UNDERLINE_SECONDARY;
            render_snippet_ex(note_f, notes[i].span, &note_opts);
        }
    }
}

/* Emit diagnostic with suggestion (Phase 8) */
void diag_emit_with_suggestion(DiagLevel level, Span span, const char *message,
                               const DiagSuggestion *suggestion) {
    if (diag_intercept(level)) return;
    if (level == DIAG_ERROR) had_error_ = true;

    if (lsp_collect_) {
        lsp_append(level, DIAG_CODE_NONE, span, message);
        if (suggestion && suggestion->text)
            lsp_append(DIAG_HELP, DIAG_CODE_NONE, span, suggestion->text);
        return;
    }

    if (diag_sink_fn_) {
        diag_sink_dispatch(level, DIAG_CODE_NONE, span, message);
        if (suggestion && suggestion->text)
            diag_sink_dispatch(DIAG_HELP, DIAG_CODE_NONE, span, suggestion->text);
        return;
    }

    /* Phase 8: If JSON output is enabled, emit in JSON format */
    if (json_output_) {
        diag_emit_json(level, span, DIAG_CODE_NONE, message);
        if (suggestion && suggestion->text) {
            diag_emit_json(DIAG_HELP, span, DIAG_CODE_NONE, suggestion->text);
        }
        return;
    }

    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *path = f ? f->path : "<unknown>";

    const char *color = color_for_level(level);
    const char *reset = use_color_ ? COLOR_RESET : "";
    const char *help_color = use_color_ ? COLOR_GREEN : "";
    
    /* Print primary error */
    fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", color, path, span.line, span.col_start, level_name(level), reset, message);

    /* Print snippet */
    if (f && span.off_start <= f->len) {
        render_snippet(f, span);
    }
    
    /* Print suggestion */
    if (suggestion && suggestion->text) {
        fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", help_color, path, span.line, span.col_start,
                level_name(DIAG_HELP), reset, suggestion->text);
        
        if (suggestion->replacement) {
            fprintf(stderr, "%s%s:%u:%u: %s%s: try: %s\n", help_color, path, span.line, span.col_start,
                    level_name(DIAG_HELP), reset, suggestion->replacement);
        }
        
        if (suggestion->doc_url) {
            fprintf(stderr, "%s%s:%u:%u: %s%s: see: %s\n", help_color, path, span.line, span.col_start,
                    level_name(DIAG_HELP), reset, suggestion->doc_url);
        }
    }
}

/* Emit multi-span diagnostic (Phase 8) */
void diag_emit_multi_span(DiagLevel level, const char *message,
                         Span primary_span, const char *primary_label,
                         Span *secondary_spans, const char **secondary_labels,
                         size_t secondary_count) {
    if (diag_intercept(level)) return;
    if (level == DIAG_ERROR) had_error_ = true;

    if (lsp_collect_) {
        lsp_append(level, DIAG_CODE_NONE, primary_span, message);
        for (size_t i = 0; i < secondary_count; i++)
            lsp_append(DIAG_NOTE, DIAG_CODE_NONE, secondary_spans[i],
                       secondary_labels ? secondary_labels[i] : "");
        return;
    }

    if (diag_sink_fn_) {
        diag_sink_dispatch(level, DIAG_CODE_NONE, primary_span, message);
        for (size_t i = 0; i < secondary_count; i++)
            diag_sink_dispatch(DIAG_NOTE, DIAG_CODE_NONE, secondary_spans[i],
                               secondary_labels ? secondary_labels[i] : "");
        return;
    }

    /* Phase 8: If JSON output is enabled, emit in JSON format */
    if (json_output_) {
        diag_emit_json(level, primary_span, DIAG_CODE_NONE, message);
        for (size_t i = 0; i < secondary_count; i++) {
            diag_emit_json(DIAG_NOTE, secondary_spans[i], DIAG_CODE_NONE,
                           secondary_labels ? secondary_labels[i] : "");
        }
        return;
    }

    const SourceFile *f = NULL;
    if (primary_span.file_id < MAX_FILES) f = files_[primary_span.file_id];
    const char *path = f ? f->path : "<unknown>";

    const char *color = color_for_level(level);
    const char *reset = use_color_ ? COLOR_RESET : "";
    
    /* Print primary error with label */
    fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", color, path, primary_span.line, primary_span.col_start,
            level_name(level), reset, message);
    
    /* Print primary snippet with label */
    if (f && primary_span.off_start <= f->len) {
        /* Render snippet */
        SnippetOpts opts = SNIPPET_OPTS_DEFAULT;
        render_snippet_ex(f, primary_span, &opts);
        
        /* Print label on its own line if we have one */
        if (primary_label) {
            int line_num_width = 1;
            uint32_t temp = primary_span.line + opts.context_lines;
            while (temp >= 10) { line_num_width++; temp /= 10; }
            
            fprintf(stderr, "%*s | %*s%s%s\n", line_num_width, "", 
                    (int)(primary_span.col_start - 1), "", color, primary_label);
            fprintf(stderr, "%s", reset);
        }
    }
    
    /* Print secondary spans */
    for (size_t i = 0; i < secondary_count; i++) {
        const SourceFile *sec_f = NULL;
        if (secondary_spans[i].file_id < MAX_FILES) sec_f = files_[secondary_spans[i].file_id];
        const char *sec_path = sec_f ? sec_f->path : "<unknown>";
        const char *note_color = color_for_level(DIAG_NOTE);
        
        fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", note_color, sec_path,
                secondary_spans[i].line, secondary_spans[i].col_start,
                level_name(DIAG_NOTE), reset, secondary_labels ? secondary_labels[i] : "");
        
        if (sec_f && secondary_spans[i].off_start <= sec_f->len) {
            SnippetOpts sec_opts = SNIPPET_OPTS_DEFAULT;
            sec_opts.primary_style = UNDERLINE_SECONDARY;
            render_snippet_ex(sec_f, secondary_spans[i], &sec_opts);
        }
    }
}

/* JSON diagnostics support (Phase 8) */

void diag_set_json_output(bool enabled) {
    json_output_ = enabled;
}

void diag_lsp_begin(void) {
    lsp_collect_ = true;
    lsp_entry_count_ = 0;
}

void diag_lsp_flush(FILE *out) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"diagnostics\":");
    lsp_build_array(&b);
    buf_puts(&b, "}");
    fwrite(b.data, 1, b.len, out);
    fputc('\n', out);
    fflush(out);
    buf_free(&b);
}

void diag_lsp_flush_array(struct Buf *buf) {
    lsp_build_array(buf);
}

void diag_lsp_flush_array_for(struct Buf *buf, const char *doc_path,
                              DiagLspRelocateFn relocate, void *ctx) {
    lsp_build_array_ex(buf, doc_path, relocate, ctx);
}

void diag_lsp_end(void) {
    lsp_collect_ = false;
    lsp_entry_count_ = 0;
    free(lsp_entries_);
    lsp_entries_ = NULL;
    lsp_entry_cap_ = 0;
}

void diag_lsp_remap_path(const char *from_path, const char *to_path) {
    if (!from_path || !to_path) return;
    for (size_t i = 0; i < lsp_entry_count_; i++) {
        DiagLspEntry *e = &lsp_entries_[i];
        if (strcmp(e->file, from_path) == 0)
            snprintf(e->file, sizeof(e->file), "%s", to_path);
        if (strcmp(e->via, from_path) == 0)
            snprintf(e->via, sizeof(e->via), "%s", to_path);
        if (strcmp(e->via_parent, from_path) == 0)
            snprintf(e->via_parent, sizeof(e->via_parent), "%s", to_path);
        if (strcmp(e->exp_file, from_path) == 0)
            snprintf(e->exp_file, sizeof(e->exp_file), "%s", to_path);
    }
}

/* Escape a string for JSON output */
static void json_escape_string(Buf *b, const char *s) {
    if (!s) {
        buf_puts(b, "null");
        return;
    }
    buf_putc(b, '"');
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"':  buf_puts(b, "\\\""); break;
            case '\\': buf_puts(b, "\\\\"); break;
            case '\b': buf_puts(b, "\\b");  break;
            case '\f': buf_puts(b, "\\f");  break;
            case '\n': buf_puts(b, "\\n");  break;
            case '\r': buf_puts(b, "\\r");  break;
            case '\t': buf_puts(b, "\\t");  break;
            default:
                if ((unsigned char)*p < 0x20) {
                    buf_printf(b, "\\u%04x", (unsigned char)*p);
                } else {
                    buf_putc(b, *p);
                }
        }
    }
    buf_putc(b, '"');
}

/* Emit a diagnostic in JSON format */
void diag_emit_json(DiagLevel level, Span span, DiagCode code, const char *message) {
    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *file = f ? f->path : "<unknown>";
    const char *severity = level_name(level);
    const char *code_str = diag_code_to_string(code);
    
    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\n");
    buf_printf(&b, "  \"severity\": ");
    json_escape_string(&b, severity);
    buf_printf(&b, ",\n");
    buf_printf(&b, "  \"code\": ");
    json_escape_string(&b, code != DIAG_CODE_NONE ? code_str : "");
    buf_printf(&b, ",\n");
    buf_printf(&b, "  \"message\": ");
    json_escape_string(&b, message);
    buf_printf(&b, ",\n");
    buf_printf(&b, "  \"file\": ");
    json_escape_string(&b, file);
    buf_printf(&b, ",\n");
    buf_printf(&b, "  \"line\": %u,\n", span.line);
    buf_printf(&b, "  \"col\": %u,\n", span.col_start);
    buf_printf(&b, "  \"endLine\": %u,\n", span.line);
    buf_printf(&b, "  \"endCol\": %u\n", span.col_end);
    buf_puts(&b, "}\n");
    
    fwrite(b.data, 1, b.len, stderr);
    buf_free(&b);
}


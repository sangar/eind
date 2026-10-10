#ifndef EIND_QUERY_H
#define EIND_QUERY_H

#include <time.h>

#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/text/str.h"

/*
 * The query language:
 *
 *   report ext:pdf;docx !draft size:>1mb dm:thisweek <foo|bar> path:"my dir"
 *
 * Whitespace is AND, "|" is OR, "!" negates, "<...>" groups, quotes protect
 * spaces. A word can carry modifiers (case:, regex:, ww:, wfn:, path:,
 * file:, folder:) and functions (ext:, size:, dm:, dc:, len:, depth:,
 * parent:, infolder:).
 */
typedef enum {
    Q_AND, /* no kids matches everything */
    Q_OR,
    Q_NOT,
    Q_TEXT,
    Q_EXT,
    Q_SIZE,
    Q_MODIFIED,
    Q_CREATED,
    Q_NAMELEN,
    Q_DEPTH,
    Q_ISDIR,
    Q_PARENT,
    Q_INFOLDER,
} QueryKind;

typedef enum { TEXT_SUBSTRING, TEXT_WILDCARD, TEXT_REGEX, TEXT_WHOLEWORD, TEXT_WHOLENAME } TextMode;

/* Range is inclusive on both ends. */
typedef struct {
    int64_t lo, hi;
} Range;

typedef struct QueryNode {
    QueryKind kind;
    struct QueryNode **kids; /* AND, OR */
    uint32_t kid_count;
    struct QueryNode *kid; /* NOT */
    /* TEXT: matched against the name, or the full path when match_path is set */
    String text;
    TextMode mode;
    bool match_path;
    bool case_sensitive;
    String *exts; /* EXT: lowercase, no dot */
    uint32_t ext_count;
    Range range; /* SIZE, MODIFIED, CREATED, NAMELEN, DEPTH */
    bool dir;    /* ISDIR */
    String path; /* PARENT, INFOLDER */
} QueryNode;

/* QueryDefaults come from command line switches and apply to plain words. */
typedef struct {
    bool regex, case_sensitive, whole_word, match_path;
    time_t now; /* anchors relative dates such as dm:today; 0 means the current time */
} QueryDefaults;

/* query_parse builds the tree in arena; a malformed function argument is ERR_PARSE. */
[[nodiscard]] Error query_parse(Arena *arena, String text, QueryDefaults defaults, QueryNode **query, Err *err);
/* query_restrict narrows a query to a folder and to files or folders, like --path, --files, --dirs. */
QueryNode *query_restrict(Arena *arena, QueryNode *node, String path, bool files_only, bool dirs_only);

/* A ValueParser reads one function argument such as 10mb or today; what it cannot read is ERR_PARSE. */
typedef Error (*ValueParser)(String s, time_t now, Range *out, Err *err);
/* parse_range reads a value, a comparison such as >=10mb, or a range such as 2023..2024. */
[[nodiscard]] Error parse_range(String s, ValueParser parse, time_t now, Range *out, Err *err);
[[nodiscard]] Error parse_size_value(String s, time_t now, Range *out, Err *err);
[[nodiscard]] Error parse_date_value(String s, time_t now, Range *out, Err *err);

#endif

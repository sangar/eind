#ifndef EIND_QUERY_H
#define EIND_QUERY_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "../core/arena.h"
#include "../core/util.h"

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
    const char *text;
    TextMode mode;
    bool match_path;
    bool case_sensitive;
    const char **exts; /* EXT: lowercase, no dot */
    uint32_t ext_count;
    Range range; /* SIZE, MODIFIED, CREATED, NAMELEN, DEPTH */
    bool dir;    /* ISDIR */
    const char *path; /* PARENT, INFOLDER */
} QueryNode;

/* QueryDefaults come from command line switches and apply to plain words. */
typedef struct {
    bool regex, case_sensitive, whole_word, match_path;
    time_t now; /* anchors relative dates such as dm:today; 0 means the current time */
} QueryDefaults;

QueryNode *query_parse(Arena *arena, const char *s, QueryDefaults defaults, Err *err);
/* query_restrict narrows a query to a folder and to files or folders, like --path, --files, --dirs. */
QueryNode *query_restrict(Arena *arena, QueryNode *node, const char *path, bool files_only, bool dirs_only);

typedef bool (*ValueParser)(const char *s, time_t now, Range *out, Err *err);
bool parse_range(const char *s, ValueParser parse, time_t now, Range *out, Err *err);
bool parse_size_value(const char *s, time_t now, Range *out, Err *err);
bool parse_date_value(const char *s, time_t now, Range *out, Err *err);

#endif

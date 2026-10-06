# Socket protocol

`eind serve` answers launchers and GUIs over a Unix socket from the index it
keeps in memory. Code: `src/app/server.c`.

```sh
eind serve                          # socket at $XDG_RUNTIME_DIR/eind.sock
eind serve --socket /tmp/eind.sock
eind status                         # shows whether a daemon is running
```

The socket path is `--socket`, else `$EIND_SOCKET`, else
`$XDG_RUNTIME_DIR/eind.sock`, else `eind-<uid>.sock` in `$TMPDIR` (or `/tmp`).
It is created with mode 0600 and must be shorter than 104 bytes. A stale
socket file from a dead daemon is replaced; a live one makes `serve` fail.

## Framing

JSON lines: send one JSON object per line, receive one JSON object per
request. Lines are limited to 1 MiB. Any `id` you send is echoed back
unchanged; without one, the response has none.

A new request on a connection cancels the one still running on it, which
then answers `{"id": ..., "cancelled": true}`. Send a request per keystroke
and render whichever response carries the latest id. Connections are
independent; open as many as you like.

## Search

Request (all fields except `query` optional):

```json
{"id": 1, "query": "report ext:pdf", "limit": 50, "offset": 0,
 "sort": "relevance", "descending": false,
 "regex": false, "case": false, "whole_word": false, "match_path": false,
 "path": "/home/me/Documents", "files": false, "dirs": false}
```

| Field | Meaning |
|---|---|
| `query` | the query, in the syntax of the command line |
| `limit` | results to return; default 100, `-1` for all, `0` for only `total` |
| `offset` | results to skip |
| `sort` | `relevance` (default), `path`, `name`, `size`, `dm`, `dc` or `ext` |
| `descending` | reverse the sort (not for relevance) |
| `regex`, `case`, `whole_word`, `match_path` | defaults for plain words, like `-r -i -w -p` |
| `path` | only results inside this folder (absolute) |
| `files`, `dirs` | only files, only folders |

Response:

```json
{"id": 1, "total": 132, "elapsed_ms": 2.1, "results": [
  {"path": "/home/me/x/report.pdf", "name": "report.pdf", "type": "file",
   "size": 1024, "modified": "2024-03-13T10:00:00+01:00",
   "created": "2024-03-01T08:00:00+01:00"}
]}
```

`total` counts all matches before `offset` and `limit`. `type` is `file` or
`dir`. Times are RFC 3339 in local time; `created` is omitted when unknown.

Relevance puts names equal to a search term first, then names starting with
it, then names containing it at a word boundary, then other matches, with
shallower paths winning ties.

## Status

```json
{"op": "status"}
{"files": 1847170, "folders": 330914, "roots": ["/home/me"],
 "built": "2024-03-13T09:00:00+01:00", "index": "/home/me/.local/share/eind/index.bin"}
```

## Errors

`{"id": 1, "error": "size: expected a size such as 10mb, got \"huge!\""}` for
a bad query, sort key, unknown `op` or a line that is not JSON.

Try it from a shell:

```sh
printf '{"query":"readme","limit":3}\n' | nc -U "$XDG_RUNTIME_DIR/eind.sock"
```

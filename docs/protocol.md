# Socket protocol

`eind serve` is what a launcher or GUI talks to. It loads the index once,
keeps it fresh from filesystem events exactly like `eind watch`, and answers
queries from memory over a Unix socket in a few milliseconds. On a 2.2 million
entry index a typical query round-trips in about 10 ms.

```sh
eind serve                          # socket at $XDG_RUNTIME_DIR/eind.sock
eind serve --socket /run/user/1000/eind.sock
eind status                         # shows whether a daemon is running
```

The default socket is `$XDG_RUNTIME_DIR/eind.sock`, falling back to
`eind-<uid>.sock` in the temp directory; `EIND_SOCKET` or `--socket` override
it. The socket is created with mode 0600. Unix socket paths are limited to
about 100 bytes.

The protocol is JSON lines: send one JSON object per line, receive one JSON
object per request. Any `id` you send is echoed back unchanged.

Search request, all fields except `query` optional:

```json
{"id": 1, "query": "report ext:pdf", "limit": 50, "offset": 0,
 "sort": "relevance", "descending": false,
 "regex": false, "case": false, "whole_word": false, "match_path": false,
 "path": "/home/me/Documents", "files": false, "dirs": false}
```

`limit` defaults to 100; `-1` returns everything and `0` returns only the
`total`, without sorting. `path` restricts results to one folder and its
descendants; `files` and `dirs` keep only files or only folders. `sort` is `relevance`
(default), `path`, `name`, `size`, `dm`, `dc` or `ext`. Relevance puts names
equal to a search term first, then names starting with it, then names
containing it at a word boundary, then plain substring matches, with shallower
paths winning ties; it is also available on the command line as
`-s relevance`.

Search response:

```json
{"id": 1, "total": 132, "elapsed_ms": 2.1, "results": [
  {"path": "/home/me/x/report.pdf", "name": "report.pdf", "type": "file",
   "size": 1024, "modified": "2024-03-13T10:00:00+01:00",
   "created": "2024-03-01T08:00:00+01:00"}
]}
```

`total` is the number of matches before `offset` and `limit`; `type` is
`file` or `dir`; `created` is omitted where the platform does not report it.

Sending a new request on a connection cancels the one still running, which
answers `{"id": 1, "cancelled": true}`. Send a request per keystroke and
render whichever response arrives with the latest id.

Status request and response:

```json
{"op": "status"}
{"files": 1847170, "folders": 330914, "roots": ["/home/me"],
 "built": "2024-03-13T09:00:00+01:00", "index": "/home/me/.local/share/eind/index.bin"}
```

Errors come back as `{"id": 1, "error": "size: expected a size such as 10mb, got \"huge!\""}`.
Connections are independent; open as many as you like.

Try it from a shell:

```sh
printf '{"query":"readme","limit":3}\n' | nc -U "$XDG_RUNTIME_DIR/eind.sock"
```

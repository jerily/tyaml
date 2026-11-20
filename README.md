# tyaml

A Tcl 8.6 and 9 extension for YAML and JSON, built on libfyaml's C generic API.
It follows the [python-libfyaml API](https://github.com/pantoniou/libfyaml/blob/v1.0.0-rc2/python-libfyaml/docs/API.md)
using Tcl commands and interpreter-local handles.

```tcl
package require tyaml

proc handlerName {yaml} {
    tyaml::parse $yaml doc
    set handler [tyaml::get_at_unix_path $doc /Properties/Handler]
    try {tyaml::to_simple $handler} finally {tyaml::destroy $handler}
}

handlerName {Properties: {Handler: bootstrap}}
tyaml::yaml_to_simple {Environment: {ENABLED: on, NUMBER: 001}}
# Environment {ENABLED on NUMBER 001}
tyaml::yaml_to_typed {Role: !Ref MyRole}
# M {Role {TAG {!Ref {S MyRole}}}}
```

## Loading and saving

Commands below belong to `::tyaml`.

| Command | Result |
| --- | --- |
| `loads text ?options?`, `load source ?options?` | Root handle for exactly one document. A source can be a path or an open Tcl channel. |
| `loads_all text ?options?`, `load_all source ?options?` | List of root handles, one per document. |
| `dumps value ?options?`, `dump destination value ?options?` | Serialize a handle or native Tcl value to text, a path, or an open channel. |
| `dumps_all documents ?options?`, `dump_all destination documents ?options?` | Serialize a Tcl list of documents as a stream. |
| `node_dump handle ?destination? ?options?` | Serialize an individual node, optionally to a path or channel. |
| `from_tcl value ?options?` | Construct a handle from a Tcl value. Accepts `-tag`, `-style`, `-mutable`, and `-dedup`. |
| `parse text ?varname? ?options?`, `create typedValue ?varname? ?options?` | Parse YAML or construct from a typed value. A named variable owns the handle until unset and is read-only. Pass an empty varname when supplying only options. |

Options are pairs such as `-mutable 1`. All four load commands support
`-mode yaml|yaml1.2|1.2|yaml1.1|1.1|yaml1.1-pyyaml|pyyaml|json`.
The default is YAML 1.2. `-dedup`, `-trim`, and `-keep_anchors` default to true.
`-mutable`, `-collect_diag`, `-create_markers`, `-keep_comments`, `-keep_style`,
and `-strings` default to false. Use `-strings 1` for the failsafe schema
when literal scalar spelling matters. With `-collect_diag 1`, inspect an
invalid document with `has_diag` and `get_diag`.

Dump options include `-json 1` or `-mode json`, `-compact 1`, `-indent n`,
`-strip_newline 1`, and `-style default|original|block|flow|pretty|compact|oneline`.
To preserve source style or comments, request them at parse time and use
`-style original` when dumping. For `from_tcl`, scalar style accepts
`plain`, `|`, `>`, a single quote, or a double quote. Owned file channels use
UTF-8. Caller-supplied channels retain their existing encoding and ownership.

## Tcl values and types

`to_simple handle` (also `to_tcl`) returns a native Tcl dict, list, number,
boolean, byte array, or string. YAML null becomes the empty Tcl value.
`to_typed handle` preserves distinctions such as an empty sequence versus
an empty mapping. `yaml_to_typed text ?options?` performs the parse and
conversion in one call. `yaml_to_simple text ?options?` also does both and
defaults to the failsafe schema so `001`, `on`, and `true` retain their
spelling. Conversion of an existing handle uses the schema chosen at load time.

| Typed pair | Meaning |
| --- | --- |
| `M dict` | Mapping with typed values |
| `L list` | Sequence of typed values |
| `S string`, `N number`, `BOOL boolean` | Scalar |
| `NULL {}` | YAML null |
| `TAG {tag typedValue}` | Explicit YAML tag on any node, including a collection |

`create` accepts the same notation. Tcl dictionary keys use their string
representations. Conversion to a Tcl dict reports an error for collection
keys or distinct YAML keys that coincide as Tcl strings. Explicit
`!!binary` values convert to Tcl byte arrays in simple mode and retain
their tag and base64 text in typed mode.

`from_tcl` and the dump/mutation commands use existing Tcl object types.
A literal such as `123` may have a string representation only. Use
`create {N 123}`, `create {BOOL 1}`, or `create {L {}}` when the type must
be explicit. A live handle passed as a value copies its YAML value.

## Handle operations

Each operation takes its handle first, for example `tyaml::get $doc name`.

| Operation | Arguments after the handle | Result |
| --- | --- | --- |
| `get`, `get_at_path`, `get_at_unix_path` | key/index, Tcl path list, or slash-delimited path | Child handle |
| `get_path`, `get_unix_path` | | Current location relative to the root |
| `set_item`, `set_at_path`, `set_at_unix_path` | key or path, replacement value | Updates a mutable document |
| `keys`, `values`, `items` | | Tcl keys, child handles, or key/handle pairs. `values` also iterates sequence elements. |
| `size`, `contains` | no argument, or key/value | Length or membership |
| `is_null`, `is_bool`, `is_int`, `is_float`, `is_string`, `is_sequence`, `is_mapping`, `is_indirect` | | Boolean |
| `has_tag`, `has_anchor`, `has_comment`, `has_marker`, `has_diag` | | Metadata presence |
| `get_tag`, `get_anchor`, `get_comment`, `get_marker`, `get_diag` | | Tag, anchor, comment text, six-element start/end byte-line-column list, or diagnostic handle. Absent metadata returns the empty value. |
| `document_state` | | Root directive dict with version, tags, explicitness flags, and JSON mode. Empty for child nodes. |
| `clone`, `trim`, `destroy` | | Independent copy, release unused arena space, or release the handle |

`path_list_to_unix_path path` and `unix_path_to_path_list path` convert path
forms. Use list paths for keys containing a slash. Access and arithmetic
that Python expresses through operators use Tcl commands, for example
`expr {[tyaml::to_simple $count] + 1}`.

Handles from loads, lookups, clones, diagnostic lookups, and iterators must
be destroyed when finished. Each keeps its document alive independently.
A child refers to its path and sees updates at that path. Destroying the
interpreter releases all remaining handles.

## Build

Requires Tcl development headers/stubs, a C compiler supporting libfyaml's
GNU C extensions, CMake 3.22+, and libfyaml with generics enabled. The ttrek
recipe pins libfyaml 1.0.0-rc2.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/libfyaml/prefix
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/tcl/prefix
```

Select a particular Tcl ABI with `TCL_INCLUDE_PATH` and `TCL_LIBRARY_DIR`.
Shared builds link Tcl stubs and install a package index for
`package require tyaml`. With `-DBUILD_SHARED_LIBS=OFF`, link the application
with tyaml, libfyaml and Tcl, and register `Tyaml_Init` as a static package.

#include "tyaml.h"
#include "script.h"
#include <stdio.h>
#include <stdatomic.h>

static Arena *newArena(int dedup, int strings) {
    struct fy_auto_allocator_cfg acfg = {
        .scenario = dedup ? FYAST_PER_TAG_FREE_DEDUP : FYAST_PER_TAG_FREE
    };
    struct fy_allocator *allocator = fy_allocator_create("auto", &acfg);
    if (!allocator) return NULL;
    struct fy_generic_builder_cfg cfg = {
        .allocator = allocator, .flags = FYGBCF_OWNS_ALLOCATOR |
            (strings ? FYGBCF_SCHEMA_YAML1_2_FAILSAFE : 0)
    };
    Builder *gb = fy_generic_builder_create(&cfg);
    if (!gb) {
        fy_allocator_destroy(allocator);
        return NULL;
    }
    Arena *arena = ckalloc(sizeof *arena);
    *arena = (Arena){gb, 1};
    return arena;
}

static void release(Arena *a) {
    if (--a->refs) return;
    fy_generic_builder_destroy(a->gb);
    ckfree(a);
}

static void freeNode(Node *n) {
    if (!--n->doc->refs) {
        release(n->doc->arena);
        ckfree(n->doc);
    }
    ckfree(n);
}

static void cleanup(ClientData data, Tcl_Interp *ip) {
    Context *ctx = data;
    Tcl_HashSearch search;
    for (Tcl_HashEntry *e = Tcl_FirstHashEntry(&ctx->handles, &search); e; e = Tcl_NextHashEntry(&search))
        freeNode(Tcl_GetHashValue(e));
    Tcl_DeleteHashTable(&ctx->handles);
    Tcl_FreeEncoding(ctx->utf8);
    ckfree(ctx);
}

static Tcl_Obj *handle(Tcl_Interp *ip, Document *doc, fy_generic path) {
    Context *ctx = context(ip);
    char name[80];
    static atomic_ullong serial;
    snprintf(name, sizeof name, "tyaml:%llu", atomic_fetch_add(&serial, 1) + 1);
    int fresh;
    Tcl_HashEntry *entry = Tcl_CreateHashEntry(&ctx->handles, name, &fresh);
    Node *n = ckalloc(sizeof *n);
    *n = (Node){doc, path};
    doc->refs++;
    Tcl_SetHashValue(entry, n);
    return Tcl_NewStringObj(name, -1);
}

static Tcl_Obj *root(Tcl_Interp *ip, Arena *arena, fy_generic v, fy_generic vds, int mutable) {
    Document *doc = ckalloc(sizeof *doc);
    *doc = (Document){arena, 0, mutable, v, vds};
    arena->refs++;
    return handle(ip, doc, fy_invalid);
}

Node *lookup(Tcl_Interp *ip, Tcl_Obj *obj) {
    const char *name = Tcl_GetString(obj);
    if (strncmp(name, "tyaml:", 6)) return NULL;
    Tcl_HashEntry *e = Tcl_FindHashEntry(&context(ip)->handles, name);
    return e ? Tcl_GetHashValue(e) : NULL;
}

fy_generic current(Node *n) {
    return fy_generic_is_invalid(n->path) ? n->doc->root :
        fy_gb_get_at_pathseq(n->doc->arena->gb, n->doc->root, n->path);
}

static int result(Tcl_Interp *ip, Tcl_Obj *obj) {
    if (!obj) return TCL_ERROR;
    Tcl_SetObjResult(ip, obj);
    return TCL_OK;
}

static int parse(ClientData cd, Tcl_Interp *ip, int argc, Tcl_Obj *const argv[]) {
    if (argc != 13) {
        Tcl_WrongNumArgs(ip, 1, argv, "text mode all mutable dedup trim collect_diag markers comments style anchors strings");
        return TCL_ERROR;
    }
    static const char *const modes[] = {"yaml", "yaml1.2", "1.2", "yaml1.1", "1.1", "yaml1.1-pyyaml", "pyyaml", "json", NULL};
    const unsigned modesFlags[] = {FYOPPF_MODE_YAML_1_2, FYOPPF_MODE_YAML_1_2, FYOPPF_MODE_YAML_1_2,
        FYOPPF_MODE_YAML_1_1, FYOPPF_MODE_YAML_1_1, FYOPPF_MODE_YAML_1_1_PYYAML, FYOPPF_MODE_YAML_1_1_PYYAML, FYOPPF_MODE_JSON};
    int mode, b[10];
    if (Tcl_GetIndexFromObj(ip, argv[2], modes, "mode", TCL_EXACT, &mode) != TCL_OK) return TCL_ERROR;
    for (int i = 0; i < 10; i++)
        if (Tcl_GetBooleanFromObj(ip, argv[i+3], &b[i]) != TCL_OK) return TCL_ERROR;
    unsigned flags = FYOPPF_INPUT_TYPE_STRING | FYOPPF_MULTI_DOCUMENT | FYOPPF_NO_YAML_1_2_BINARY | modesFlags[mode] | FYOPPF_COLLECT_DIAG;
    if (b[5]) flags |= FYOPPF_CREATE_MARKERS;
    if (b[6]) flags |= FYOPPF_KEEP_COMMENTS;
    if (b[7]) flags |= FYOPPF_KEEP_STYLE;
    if (b[8]) flags |= FYOPPF_KEEP_ANCHORS;
    Arena *a = newArena(b[2], b[9]);
    if (!a) return error(ip, "could not allocate YAML builder");
    fy_generic text = stringValue(ip, a->gb, argv[1]);
    fy_generic dir = fy_gb_parse(a->gb, text, flags, NULL);
    fy_generic diag = fy_generic_get_diag(dir);
    int count = fy_generic_is_null(dir) ? 0 : fy_generic_dir_get_document_count(dir);
    int rc = TCL_OK;
    if (fy_generic_is_valid(diag) && !fy_generic_is_null(diag)) {
        if (!b[4]) {
            Tcl_Obj *detail = asTcl(ip, diag, 0);
            if (detail) Tcl_SetObjResult(ip, detail);
            rc = TCL_ERROR;
        } else {
            Tcl_Obj *h = root(ip, a, dir, fy_invalid, b[1]);
            rc = result(ip, b[0] ? Tcl_NewListObj(1, &h) : h);
        }
    } else if (fy_generic_is_invalid(dir) || count < 0) rc = error(ip, "invalid YAML document");
    else if (!b[0] && count != 1) rc = error(ip, "expected exactly one document; use loads_all for streams");
    else {
        Tcl_Obj *out = b[0] ? Tcl_NewListObj(0, NULL) : NULL;
        for (int i = 0; i < count; i++) {
            fy_generic vds = fy_generic_dir_get_document_vds(dir, i);
            Tcl_Obj *h = root(ip, a, fy_generic_vds_get_root(vds), vds, b[1]);
            if (b[0]) Tcl_ListObjAppendElement(ip, out, h);
            else out = h;
        }
        if (b[3]) fy_gb_trim(a->gb);
        rc = result(ip, out);
    }
    release(a);
    return rc;
}

static int create(ClientData cd, Tcl_Interp *ip, int argc, Tcl_Obj *const argv[]) {
    if (argc != 7) {
        Tcl_WrongNumArgs(ip, 1, argv, "value typed mutable dedup tag style");
        return TCL_ERROR;
    }
    static const char *const styles[] = {"", "plain", "|", ">", "'", "\"", NULL};
    const enum fy_scalar_style flags[] = {FYSS_ANY, FYSS_PLAIN, FYSS_LITERAL, FYSS_FOLDED, FYSS_SINGLE_QUOTED, FYSS_DOUBLE_QUOTED};
    int typed, mutable, dedup, style;
    if (Tcl_GetBooleanFromObj(ip, argv[2], &typed) || Tcl_GetBooleanFromObj(ip, argv[3], &mutable) ||
        Tcl_GetBooleanFromObj(ip, argv[4], &dedup) ||
        Tcl_GetIndexFromObj(ip, argv[6], styles, "style", TCL_EXACT, &style)) return TCL_ERROR;
    Arena *a = newArena(dedup, 0);
    if (!a) return error(ip, "could not allocate YAML builder");
    fy_generic v = fromTcl(ip, a->gb, argv[1], typed, 0);
    if (fy_generic_is_valid(v) && (style || *Tcl_GetString(argv[5]))) {
        fy_generic tag = *Tcl_GetString(argv[5]) ? stringValue(ip, a->gb, argv[5]) : fy_invalid;
        v = decorate(a->gb, v, tag, flags[style]);
    }
    int rc = fy_generic_is_valid(v) ? result(ip, root(ip, a, v, fy_invalid, mutable)) : TCL_ERROR;
    release(a);
    return rc;
}

static int emit(ClientData cd, Tcl_Interp *ip, int argc, Tcl_Obj *const argv[]) {
    if (argc != 8) {
        Tcl_WrongNumArgs(ip, 1, argv, "value multi json compact style indent strip_newline");
        return TCL_ERROR;
    }
    static const char *const styles[] = {"", "default", "original", "block", "flow", "pretty", "compact", "oneline", NULL};
    unsigned stylesFlags[] = {0, FYOPEF_STYLE_DEFAULT, FYOPEF_STYLE_DEFAULT, FYOPEF_STYLE_BLOCK,
        FYOPEF_STYLE_FLOW, FYOPEF_STYLE_PRETTY, FYOPEF_STYLE_COMPACT, FYOPEF_STYLE_ONELINE};
    int all, json, compact, style, indent, strip;
    if (Tcl_GetBooleanFromObj(ip, argv[2], &all) || Tcl_GetBooleanFromObj(ip, argv[3], &json) ||
        Tcl_GetBooleanFromObj(ip, argv[4], &compact) || Tcl_GetIndexFromObj(ip, argv[5], styles, "style", TCL_EXACT, &style) ||
        Tcl_GetIntFromObj(ip, argv[6], &indent) || Tcl_GetBooleanFromObj(ip, argv[7], &strip)) return TCL_ERROR;
    if (indent < 0 || indent > 8) return error(ip, "indent must be between 0 and 8");
    unsigned indents[] = {FYOPEF_INDENT_2,FYOPEF_INDENT_1,FYOPEF_INDENT_2,FYOPEF_INDENT_3,FYOPEF_INDENT_4,
        FYOPEF_INDENT_6,FYOPEF_INDENT_6,FYOPEF_INDENT_8,FYOPEF_INDENT_8};
    unsigned flags = FYOPEF_DISABLE_DIRECTORY | FYOPEF_OUTPUT_TYPE_STRING | indents[indent];
    flags |= json ? FYOPEF_MODE_JSON : (FYOPEF_MODE_YAML_1_2 | FYOPEF_OUTPUT_COMMENTS);
    flags |= style ? stylesFlags[style] : compact ? FYOPEF_STYLE_FLOW : FYOPEF_STYLE_BLOCK;
    if (all) flags |= FYOPEF_MULTI_DOCUMENT;
    if (strip) flags |= FYOPEF_NO_ENDING_NEWLINE;
    Arena *a = newArena(0, 0);
    if (!a) return error(ip, "could not allocate YAML builder");
    fy_generic v;
    if (all) {
        Tcl_Obj **docs;
        Tcl_Size count;
        if (Tcl_ListObjGetElements(ip, argv[1], &count, &docs)) {release(a); return TCL_ERROR;}
        v = fy_gb_sequence_create(a->gb, 0, NULL);
        for (Tcl_Size i = 0; i < count; i++) {
            fy_generic doc = fromTcl(ip, a->gb, docs[i], 0, 0);
            if (fy_generic_is_invalid(doc)) {v = fy_invalid; break;}
            v = fy_gb_append(a->gb, v, doc);
        }
    } else v = fromTcl(ip, a->gb, argv[1], 0, 0);
    int rc = TCL_ERROR;
    if (fy_generic_is_valid(v)) {
        fy_generic out = fy_gb_emit(a->gb, v, flags, NULL);
        rc = fy_generic_is_string(out) ? result(ip, asTcl(ip, out, 0)) : error(ip, "YAML emission failed");
    }
    release(a);
    return rc;
}

static void field(Tcl_Obj *dict, const char *name, Tcl_Obj *value) {
    Tcl_DictObjPut(NULL, dict, Tcl_NewStringObj(name,-1), value);
}

static Tcl_Obj *documentState(Node *node) {
    if (fy_generic_is_valid(node->path) || fy_generic_is_invalid(node->doc->vds)) return Tcl_NewObj();
    struct fy_document_state *state = fy_generic_vds_get_document_state(node->doc->vds);
    if (!state) return Tcl_NewObj();
    Tcl_Obj *dict = Tcl_NewDictObj(), *tags = Tcl_NewListObj(0, NULL);
    const struct fy_version *v = fy_document_state_version(state);
    if (v) {
        Tcl_Obj *parts[] = {Tcl_NewIntObj(v->major), Tcl_NewIntObj(v->minor)};
        field(dict, "version", Tcl_NewListObj(2, parts));
    }
    field(dict, "version_explicit", Tcl_NewBooleanObj(fy_document_state_version_explicit(state)));
    field(dict, "tags_explicit", Tcl_NewBooleanObj(fy_document_state_tags_explicit(state)));
    field(dict, "json_mode", Tcl_NewBooleanObj(fy_document_state_json_mode(state)));
    void *iter = NULL;
    const struct fy_tag *tag;
    while ((tag = fy_document_state_tag_directive_iterate(state, &iter))) {
        Tcl_Obj *item = Tcl_NewDictObj();
        field(item, "handle", Tcl_NewStringObj(tag->handle, -1));
        field(item, "prefix", Tcl_NewStringObj(tag->prefix, -1));
        Tcl_ListObjAppendElement(NULL, tags, item);
    }
    field(dict, "tags", tags);
    return dict;
}

static fy_generic extendedPath(Builder *gb, fy_generic path, fy_generic key) {
    return fy_generic_is_invalid(path) ? fy_gb_sequence_create(gb, 1, &key) : fy_gb_append(gb, path, key);
}

static fy_generic keyFor(Tcl_Interp *ip, Builder *gb, fy_generic parent, Tcl_Obj *obj) {
    if (fy_generic_is_sequence(parent)) {
        Tcl_WideInt index;
        if (Tcl_GetWideIntFromObj(ip, obj, &index)) return fy_invalid;
        if (index < 0) index += (Tcl_WideInt)fy_generic_sequence_get_item_count(parent);
        return fy_gb_long_long_create(gb, index);
    }
    fy_generic key = fromTcl(ip, gb, obj, 0, 0);
    if (fy_generic_is_mapping(parent) && fy_generic_is_valid(key) && fy_generic_is_invalid(fy_get(parent, key))) {
        fy_generic text = stringValue(ip, gb, obj);
        if (fy_generic_is_valid(fy_get(parent, text))) return text;
        Tcl_WideInt index;
        if (Tcl_GetWideIntFromObj(NULL, obj, &index) == TCL_OK) {
            fy_generic integer = fy_gb_long_long_create(gb, index);
            if (fy_generic_is_valid(fy_get(parent, integer))) return integer;
        }
    }
    return key;
}

static int navigate(Tcl_Interp *ip, Node *node, Tcl_Obj *list, Tcl_Obj *replacement) {
    Tcl_Obj **parts;
    Tcl_Size count;
    if (Tcl_ListObjGetElements(ip, list, &count, &parts)) return TCL_ERROR;
    Document *doc = node->doc;
    Builder *gb = doc->arena->gb;
    fy_generic v = current(node), path = node->path;
    if (replacement && !doc->mutable) return error(ip, "YAML document is read-only");
    if (replacement && !count) return error(ip, "cannot set an empty path");
    for (Tcl_Size i = 0; i < count; i++) {
        fy_generic key = keyFor(ip, gb, v, parts[i]);
        if (fy_generic_is_invalid(key)) return TCL_ERROR;
        path = extendedPath(gb, path, key);
        if (fy_generic_is_invalid(path)) return error(ip, "could not construct path");
        v = fy_get(v, key);
        if (!replacement && fy_generic_is_invalid(v)) return error(ip, "YAML path does not exist");
    }
    if (replacement) {
        fy_generic item = fromTcl(ip, gb, replacement, 0, 0);
        if (fy_generic_is_invalid(item)) return TCL_ERROR;
        fy_generic updated = fy_gb_set_at_pathseq(gb, doc->root, path, item);
        if (fy_generic_is_invalid(updated)) return error(ip, "could not set YAML path");
        doc->root = updated;
        return TCL_OK;
    }
    return result(ip, handle(ip, doc, path));
}

typedef enum {
    SIMPLE, TYPED, DESTROY, VALID, CLONE, TRIM, STATE, PATH, GETPATH, SETPATH,
    SIZE, KEYS, VALUES, ITEMS, CONTAINS, IS_NULL, IS_BOOL, IS_INT, IS_FLOAT,
    IS_STRING, IS_SEQUENCE, IS_MAPPING, IS_INDIRECT, HAS_TAG, GET_TAG,
    HAS_ANCHOR, GET_ANCHOR, HAS_MARKER, GET_MARKER, HAS_DIAG, GET_DIAG, HAS_COMMENT, GET_COMMENT
} Operation;
typedef struct { const char *name; Operation op; int nargs; } Command;
static const Command commands[] = {
    {"to_simple", SIMPLE}, {"to_typed", TYPED}, {"destroy", DESTROY}, {"_valid", VALID},
    {"clone", CLONE}, {"trim", TRIM}, {"document_state", STATE}, {"get_path", PATH},
    {"get_at_path", GETPATH,1}, {"set_at_path", SETPATH,2}, {"size", SIZE},
    {"keys", KEYS}, {"values", VALUES}, {"items", ITEMS}, {"contains", CONTAINS,1},
    {"is_null",IS_NULL}, {"is_bool",IS_BOOL}, {"is_int",IS_INT}, {"is_float",IS_FLOAT},
    {"is_string",IS_STRING}, {"is_sequence",IS_SEQUENCE}, {"is_mapping",IS_MAPPING}, {"is_indirect",IS_INDIRECT},
    {"has_tag",HAS_TAG}, {"get_tag",GET_TAG}, {"has_anchor",HAS_ANCHOR}, {"get_anchor",GET_ANCHOR},
    {"has_marker",HAS_MARKER}, {"get_marker",GET_MARKER}, {"has_diag",HAS_DIAG}, {"get_diag",GET_DIAG},
    {"has_comment",HAS_COMMENT}, {"get_comment",GET_COMMENT}, {NULL}
};

static int dispatch(ClientData cd, Tcl_Interp *ip, int argc, Tcl_Obj *const argv[]) {
    const Command *cmd = cd;
    if (argc != 2 + cmd->nargs) {
        Tcl_WrongNumArgs(ip, 1, argv, cmd->nargs == 0 ? "handle" : cmd->nargs == 1 ? "handle argument" : "handle path value");
        return TCL_ERROR;
    }
    Node *n = lookup(ip, argv[1]);
    Operation op = cmd->op;
    if (op == VALID) return result(ip, Tcl_NewBooleanObj(n != NULL));
    if (!n) return error(ip, "unknown or destroyed tyaml handle");
    if (op == DESTROY) {
        Tcl_DeleteHashEntry(Tcl_FindHashEntry(&context(ip)->handles, Tcl_GetString(argv[1])));
        freeNode(n);
        return TCL_OK;
    }
    Document *doc = n->doc;
    Builder *gb = doc->arena->gb;
    fy_generic v = current(n), meta;
    if (fy_generic_is_invalid(v) && op != GET_DIAG && op != HAS_DIAG)
        return error(ip, "YAML node is no longer available");
    enum fy_generic_type type = fy_get_type(v);
    size_t count = type == FYGT_MAPPING ? fy_generic_mapping_get_pair_count(v) :
        type == FYGT_SEQUENCE ? fy_generic_sequence_get_item_count(v) : 0;
    int flag = 0;
    switch (op) {
    case SIMPLE: case TYPED: return result(ip, asTcl(ip, v, op == TYPED));
    case PATH: return result(ip, fy_generic_is_invalid(n->path) ? Tcl_NewListObj(0,NULL) : asTcl(ip, n->path, 0));
    case GETPATH: case SETPATH: return navigate(ip,n,argv[2], op == SETPATH ? argv[3] : NULL);
    case TRIM: fy_gb_trim(gb); return TCL_OK;
    case STATE: return result(ip, documentState(n));
    case CLONE: {
        Arena *a = newArena(1,0);
        if (!a) return error(ip, "could not allocate YAML builder");
        fy_generic copy = fy_gb_internalize(a->gb,v);
        int rc = fy_generic_is_invalid(copy) ? error(ip,"could not clone value") :
            result(ip, root(ip,a,copy,fy_invalid,doc->mutable));
        release(a);
        return rc;
    }
    case SIZE:
        if (type == FYGT_STRING) {
            Tcl_Obj *s = asTcl(ip, v, 0);
            if (!s) return TCL_ERROR;
            count = Tcl_GetCharLength(s);
            Tcl_DecrRefCount(s);
        } else if (type != FYGT_MAPPING && type != FYGT_SEQUENCE) return error(ip, "expected a container or string");
        return result(ip, Tcl_NewWideIntObj(count));
    case KEYS: case VALUES: case ITEMS: {
        if (type != FYGT_MAPPING && !(op == VALUES && type == FYGT_SEQUENCE)) return error(ip, "expected a mapping");
        Tcl_Obj *list = Tcl_NewListObj(0,NULL);
        for (size_t i = 0; i < count; i++) {
            fy_generic key = type == FYGT_MAPPING ? fy_get_key_at(v,i) : fy_value(gb,i);
            Tcl_Obj *k = NULL, *child = NULL;
            if (op != VALUES) {
                k = asTcl(ip,key,0);
                if (!k) {Tcl_DecrRefCount(list); return TCL_ERROR;}
            }
            if (op != KEYS) child = handle(ip,doc,extendedPath(gb,n->path,key));
            Tcl_Obj *parts[] = {k,child};
            Tcl_ListObjAppendElement(ip,list,op == KEYS ? k : op == VALUES ? child : Tcl_NewListObj(2,parts));
        }
        return result(ip,list);
    }
    case CONTAINS: {
        fy_generic target = fromTcl(ip,gb,argv[2],0,0);
        if (fy_generic_is_invalid(target)) return TCL_ERROR;
        if (type == FYGT_MAPPING) flag = fy_generic_is_valid(fy_get(v,keyFor(ip,gb,v,argv[2])));
        else if (type == FYGT_SEQUENCE) {
            for (size_t i=0; i<count && !flag; i++) flag = !fy_generic_compare(target,fy_get_at(v,i));
        } else return error(ip,"expected a mapping or sequence");
        break;
    }
    case IS_NULL: flag = type == FYGT_NULL; break;
    case IS_BOOL: flag = type == FYGT_BOOL; break;
    case IS_INT: flag = type == FYGT_INT; break;
    case IS_FLOAT: flag = type == FYGT_FLOAT; break;
    case IS_STRING: flag = type == FYGT_STRING; break;
    case IS_SEQUENCE: flag = type == FYGT_SEQUENCE; break;
    case IS_MAPPING: flag = type == FYGT_MAPPING; break;
    case IS_INDIRECT: flag = fy_generic_is_indirect(v); break;
    case HAS_COMMENT: flag = fy_generic_has_comments(v); break;
    case GET_COMMENT: {
        char *s = fy_generic_get_comments(v);
        Tcl_Obj *obj = Tcl_NewStringObj(s ? s : "",-1);
        free(s);
        return result(ip,obj);
    }
    case HAS_TAG: case GET_TAG: meta = fy_generic_get_tag(v); goto metadata;
    case HAS_ANCHOR: case GET_ANCHOR: meta = fy_generic_get_anchor(v); goto metadata;
    case HAS_MARKER: case GET_MARKER: meta = fy_generic_get_marker(v); goto metadata;
    case HAS_DIAG: case GET_DIAG: meta = fy_generic_get_diag(v);
metadata:
        flag = fy_generic_is_valid(meta) && !fy_generic_is_null(meta);
        if (op == GET_TAG || op == GET_ANCHOR || op == GET_MARKER || op == GET_DIAG) {
            if (!flag) return TCL_OK;
            return result(ip, op == GET_DIAG ? root(ip,doc->arena,meta,fy_invalid,0) : asTcl(ip,meta,0));
        }
        break;
    default: return error(ip,"unknown node operation");
    }
    return result(ip,Tcl_NewBooleanObj(flag));
}

DLLEXPORT int Tyaml_Init(Tcl_Interp *ip) {
#if TCL_MAJOR_VERSION >= 9
    if (!Tcl_InitStubs(ip,"9.0",0)) return TCL_ERROR;
#else
    if (!Tcl_InitStubs(ip,"8.6",0)) return TCL_ERROR;
#endif
    if (Tcl_GetAssocData(ip,"tyaml",NULL)) return TCL_OK;
    if (!Tcl_CreateNamespace(ip,"::tyaml",NULL,NULL)) return TCL_ERROR;
    Context *ctx = ckalloc(sizeof *ctx);
    Tcl_InitHashTable(&ctx->handles,TCL_STRING_KEYS);
    ctx->utf8 = Tcl_GetEncoding(ip,"utf-8");
    Tcl_SetAssocData(ip,"tyaml",cleanup,ctx);
    Tcl_CreateObjCommand(ip,"::tyaml::_parse",parse,NULL,NULL);
    Tcl_CreateObjCommand(ip,"::tyaml::_create",create,NULL,NULL);
    Tcl_CreateObjCommand(ip,"::tyaml::_emit",emit,NULL,NULL);
    for (const Command *cmd = commands; cmd->name; cmd++) {
        char name[64];
        snprintf(name,sizeof name,"::tyaml::%s",cmd->name);
        Tcl_CreateObjCommand(ip,name,dispatch,(ClientData)cmd,NULL);
    }
    if (Tcl_EvalEx(ip,tyaml_script,-1,TCL_EVAL_GLOBAL) != TCL_OK) return TCL_ERROR;
    return Tcl_PkgProvide(ip,"tyaml",PACKAGE_VERSION);
}

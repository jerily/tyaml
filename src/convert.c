#include "tyaml.h"
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

fy_generic stringValue(Tcl_Interp *ip, Builder *gb, Tcl_Obj *obj) {
    Tcl_DString ds;
    Tcl_Size size;
    const char *text = Tcl_GetStringFromObj(obj, &size);
    Tcl_UtfToExternalDString(context(ip)->utf8, text, size, &ds);
    fy_generic value = fy_gb_string_size_create(gb, Tcl_DStringValue(&ds), Tcl_DStringLength(&ds));
    Tcl_DStringFree(&ds);
    return value;
}

static Tcl_Obj *stringObject(Tcl_Interp *ip, fy_generic value) {
    fy_generic_sized_string str = fy_castp(&value, fy_szstr_empty);
    if (str.size > TCL_SIZE_MAX) {
        error(ip, "string exceeds Tcl size limit");
        return NULL;
    }
    Tcl_DString ds;
    Tcl_ExternalToUtfDString(context(ip)->utf8, str.data, str.size, &ds);
    Tcl_Obj *obj = Tcl_NewStringObj(Tcl_DStringValue(&ds), Tcl_DStringLength(&ds));
    Tcl_DStringFree(&ds);
    return obj;
}

fy_generic decorate(Builder *gb, fy_generic value, fy_generic tag, enum fy_scalar_style style) {
    fy_generic_indirect info;
    fy_generic_indirect_reset(&info);
    info.value = value;
    info.flags = FYGIF_VALUE;
    if (fy_generic_is_valid(tag)) {
        info.flags |= FYGIF_TAG;
        info.tag = tag;
    }
    if (style != FYSS_ANY) {
        info.flags |= FYGIF_STYLE;
        info.style = fy_value((unsigned int)style);
    }
    return fy_gb_indirect_create(gb, &info);
}

/* Keep !!binary distinguishable from text, including when every byte is ASCII. */
static const char base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char binaryTag[] = "tag:yaml.org,2002:binary";

static Tcl_Obj *decodeBytes(Tcl_Interp *ip, fy_generic v) {
    fy_generic_sized_string str = fy_castp(&v, fy_szstr_empty);
    if (str.size > TCL_SIZE_MAX) {
        error(ip, "binary value exceeds Tcl size limit");
        return NULL;
    }
    Tcl_Obj *obj = Tcl_NewByteArrayObj(NULL, str.size);
    unsigned char *out = Tcl_GetByteArrayFromObj(obj, NULL);
    size_t count = 0;
    unsigned bits = 0, buffer = 0, padding = 0;
    for (size_t i = 0; i < str.size; i++) {
        unsigned char c = str.data[i];
        if (isspace(c)) continue;
        if (c == '=') {
            if (++padding > 2) goto invalid;
            continue;
        }
        const char *digit = c ? strchr(base64, c) : NULL;
        if (!digit || padding) goto invalid;
        buffer = (buffer << 6) | (unsigned)(digit - base64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[count++] = (unsigned char)(buffer >> bits);
        }
    }
    if (bits == 6 || (padding && bits != 2 * padding)) goto invalid;
    Tcl_SetByteArrayLength(obj, count);
    return obj;
invalid:
    Tcl_DecrRefCount(obj);
    error(ip, "invalid base64 in !!binary scalar");
    return NULL;
}

static fy_generic encodeBytes(Builder *gb, Tcl_Obj *obj) {
    Tcl_Size count;
    const unsigned char *bytes = Tcl_GetByteArrayFromObj(obj, &count);
    if ((size_t)count > (SIZE_MAX / 4) * 3 - 2) return fy_invalid;
    size_t len = (((size_t)count + 2) / 3) * 4;
    char *out = malloc(len ? len : 1);
    if (!out) return fy_invalid;
    size_t j = 0;
    for (size_t i = 0; i < (size_t)count; i += 3) {
        unsigned n = (unsigned)bytes[i] << 16;
        size_t left = (size_t)count - i;
        if (left > 1) n |= (unsigned)bytes[i+1] << 8;
        if (left > 2) n |= bytes[i+2];
        out[j++] = base64[n >> 18];
        out[j++] = base64[(n >> 12) & 63];
        out[j++] = left > 1 ? base64[(n >> 6) & 63] : '=';
        out[j++] = left > 2 ? base64[n & 63] : '=';
    }
    fy_generic text = fy_gb_string_size_create(gb, out, len);
    free(out);
    return decorate(gb, text, fy_gb_string_create(gb, binaryTag), FYSS_ANY);
}

static Tcl_Obj *pair(const char *label, Tcl_Obj *value) {
    Tcl_Obj *parts[] = {Tcl_NewStringObj(label, -1), value};
    return Tcl_NewListObj(2, parts);
}

/* Memoize collections so aliases share Tcl values. Never put a cycle into
 * a Tcl_Obj, since Tcl containers use reference counting. */
static Tcl_Obj *convert(Tcl_Interp *ip, fy_generic v, int typed, int depth, Tcl_HashTable *memo) {
    if (depth > 256) {
        error(ip, "YAML conversion exceeds maximum nesting depth");
        return NULL;
    }
    const char *label;
    Tcl_Obj *obj = NULL;
    Tcl_HashEntry *entry = NULL;
    enum fy_generic_type type = fy_get_type(v);
    if (type == FYGT_SEQUENCE || type == FYGT_MAPPING) {
        int fresh;
        entry = Tcl_CreateHashEntry(memo, (char *)(uintptr_t)v.v, &fresh);
        if (!fresh) {
            obj = Tcl_GetHashValue(entry);
            if (!obj) error(ip, "cyclic YAML collection");
            return obj;
        }
        Tcl_SetHashValue(entry, NULL);
    }
    switch (type) {
    case FYGT_NULL:
        label = "NULL";
        obj = Tcl_NewObj();
        break;
    case FYGT_BOOL:
        label = "BOOL";
        obj = Tcl_NewBooleanObj(fy_cast(v, (_Bool)0));
        break;
    case FYGT_INT: {
        fy_generic_decorated_int n = fy_cast(v, fy_dint_empty);
        label = "N";
        if (n.flags & FYGDIF_UNSIGNED_RANGE_EXTEND) {
            char buf[32];
            snprintf(buf, sizeof buf, "%llu", (unsigned long long)n.uv);
            obj = Tcl_NewStringObj(buf, -1);
        } else obj = Tcl_NewWideIntObj(n.sv);
        break;
    }
    case FYGT_FLOAT:
        label = "N";
        obj = Tcl_NewDoubleObj(fy_cast(v, 0.0));
        break;
    case FYGT_STRING:
        if (!typed) {
            fy_generic tag = fy_generic_get_tag(v);
            if (fy_generic_is_string(tag) && !fy_generic_compare(tag, fy_value(binaryTag)))
                return decodeBytes(ip, v);
        }
        label = "S";
        obj = stringObject(ip, v);
        break;
    case FYGT_MAPPING:
    case FYGT_SEQUENCE: {
        int mapping = type == FYGT_MAPPING;
        size_t count = mapping ? fy_generic_mapping_get_pair_count(v) : fy_generic_sequence_get_item_count(v);
        label = mapping ? "M" : "L";
        obj = mapping ? Tcl_NewDictObj() : Tcl_NewListObj(0, NULL);
        for (size_t i = 0; i < count; i++) {
            Tcl_Obj *key = NULL;
            if (mapping) {
                fy_generic k = fy_get_key_at(v, i);
                if (fy_generic_is_mapping(k) || fy_generic_is_sequence(k)) {
                    error(ip, "collection keys cannot be represented in a Tcl dict");
                    goto fail;
                }
                key = convert(ip, k, 0, depth+1, memo);
                if (!key) goto fail;
                Tcl_IncrRefCount(key);
                Tcl_Obj *existing;
                Tcl_DictObjGet(ip, obj, key, &existing);
                if (existing) {
                    Tcl_DecrRefCount(key);
                    error(ip, "distinct YAML keys have the same Tcl string representation");
                    goto fail;
                }
            }
            Tcl_Obj *child = convert(ip, fy_get_at(v, i), typed, depth+1, memo);
            if (child) {
                if (mapping) Tcl_DictObjPut(ip, obj, key, child);
                else Tcl_ListObjAppendElement(ip, obj, child);
            }
            if (key) Tcl_DecrRefCount(key);
            if (!child) goto fail;
        }
        break;
    }
    default:
        error(ip, "unresolved YAML alias or invalid value");
        return NULL;
    }
    if (!obj) return NULL;
    if (typed) {
        obj = pair(label, obj);
        fy_generic tag = fy_generic_get_tag(v);
        if (fy_generic_is_string(tag)) {
            Tcl_Obj *parts[] = {stringObject(ip, tag), obj};
            obj = pair("TAG", Tcl_NewListObj(2, parts));
        }
    }
    if (entry) {
        Tcl_IncrRefCount(obj);
        Tcl_SetHashValue(entry, obj);
    }
    return obj;
fail:
    Tcl_DecrRefCount(obj);
    return NULL;
}

Tcl_Obj *asTcl(Tcl_Interp *ip, fy_generic value, int typed) {
    Tcl_HashTable memo;
    Tcl_InitHashTable(&memo, TCL_ONE_WORD_KEYS);
    Tcl_Obj *obj = convert(ip, value, typed, 0, &memo);
    /* Give the caller an ordinary refcount-zero result even on a cache hit. */
    if (obj && (fy_generic_is_mapping(value) || fy_generic_is_sequence(value)))
        obj = Tcl_DuplicateObj(obj);
    Tcl_HashSearch search;
    for (Tcl_HashEntry *e = Tcl_FirstHashEntry(&memo, &search); e; e = Tcl_NextHashEntry(&search)) {
        Tcl_Obj *cached = Tcl_GetHashValue(e);
        if (cached) Tcl_DecrRefCount(cached);
    }
    Tcl_DeleteHashTable(&memo);
    return obj;
}

static fy_generic invalid(Tcl_Interp *ip, const char *msg) {
    error(ip, msg);
    return fy_invalid;
}

fy_generic fromTcl(Tcl_Interp *ip, Builder *gb, Tcl_Obj *obj, int typed, int depth) {
    if (depth > 256) return invalid(ip, "Tcl value exceeds maximum nesting depth");
    const char *kind = "S";
    Tcl_Size count;
    Tcl_Obj **parts;
    if (typed) {
        if (Tcl_ListObjGetElements(ip, obj, &count, &parts) != TCL_OK || count != 2)
            return invalid(ip, "expected a {type value} pair");
        kind = Tcl_GetString(parts[0]);
        obj = parts[1];
    } else {
        Node *node = obj->bytes ? lookup(ip, obj) : NULL;
        if (node) return fy_gb_internalize(gb, current(node));
        const char *type = obj->typePtr ? obj->typePtr->name : "";
        if (!strcmp(type, "bytearray")) return encodeBytes(gb, obj);
        if (!strcmp(type, "dict")) kind = "M";
        else if (!strcmp(type, "list")) kind = "L";
        else if (!strcmp(type, "booleanString") || !strcmp(type, "boolean")) kind = "BOOL";
        else if (!strcmp(type, "int") || !strcmp(type, "wideInt") || !strcmp(type, "double") || !strcmp(type, "bignum")) kind = "N";
    }
    if (!strcmp(kind, "S")) return stringValue(ip, gb, obj);
    if (!strcmp(kind, "NULL")) return fy_null;
    if (!strcmp(kind, "N")) {
        Tcl_WideInt n;
        double d;
        if (Tcl_GetWideIntFromObj(NULL, obj, &n) == TCL_OK) {
            /* Tcl accepts unsigned 64-bit integers in this API and wraps them. */
            const char *s = Tcl_GetString(obj);
            if (n < 0 && s[strspn(s, " \t\r\n")] != '-')
                return fy_gb_unsigned_long_long_create(gb, (unsigned long long)n);
            return fy_gb_long_long_create(gb, n);
        }
        const char *text = Tcl_GetString(obj);
        const char *digits = text + strspn(text, " \t\n\r");
        if (*digits == '+') digits++;
        char *end;
        errno = 0;
        unsigned long long u = strtoull(digits, &end, 10);
        if (end != digits && !end[strspn(end, " \t\n\r")]) {
            if (*digits == '-' || errno == ERANGE)
                return invalid(ip, "integer exceeds libfyaml's 64-bit range");
            return fy_gb_unsigned_long_long_create(gb, u);
        }
        if (!strcmp(text, "NaN") || !strcmp(text, ".nan")) return fy_gb_double_create(gb, NAN);
        if (Tcl_GetDoubleFromObj(ip, obj, &d) != TCL_OK) return fy_invalid;
        return fy_gb_double_create(gb, d);
    }
    if (!strcmp(kind, "BOOL")) {
        int b;
        if (Tcl_GetBooleanFromObj(ip, obj, &b) != TCL_OK) return fy_invalid;
        return fy_gb_bool_create(gb, b);
    }
    if (!strcmp(kind, "TAG")) {
        if (Tcl_ListObjGetElements(ip, obj, &count, &parts) != TCL_OK || count != 2)
            return invalid(ip, "expected {TAG {tag typedValue}}");
        fy_generic v = fromTcl(ip, gb, parts[1], 1, depth+1);
        if (fy_generic_is_invalid(v)) return v;
        return decorate(gb, v, stringValue(ip, gb, parts[0]), FYSS_ANY);
    }
    int map = !strcmp(kind, "M");
    if (!map && strcmp(kind, "L")) return invalid(ip, "unknown typed value label");
    if (map) {
        if (Tcl_DictObjSize(ip, obj, &count) != TCL_OK) return fy_invalid;
        fy_generic *pairs = calloc(count ? count : 1, 2 * sizeof *pairs);
        if (!pairs) return invalid(ip, "out of memory");
        Tcl_DictSearch search;
        Tcl_Obj *key, *value;
        int done;
        Tcl_DictObjFirst(ip, obj, &search, &key, &value, &done);
        size_t used = 0;
        while (!done) {
            pairs[used++] = stringValue(ip, gb, key);
            pairs[used++] = fromTcl(ip, gb, value, typed, depth+1);
            if (fy_generic_is_invalid(pairs[used-1])) break;
            Tcl_DictObjNext(&search, &key, &value, &done);
        }
        Tcl_DictObjDone(&search);
        fy_generic v = done ? fy_gb_mapping_create(gb, count, pairs) : fy_invalid;
        free(pairs);
        return v;
    }
    if (Tcl_ListObjGetElements(ip, obj, &count, &parts) != TCL_OK) return fy_invalid;
    fy_generic *values = calloc(count ? count : 1, sizeof *values);
    if (!values) return invalid(ip, "out of memory");
    fy_generic result = fy_invalid;
    for (Tcl_Size i = 0; i < count; i++) {
        values[i] = fromTcl(ip, gb, parts[i], typed, depth+1);
        if (fy_generic_is_invalid(values[i])) goto done;
    }
    result = fy_gb_sequence_create(gb, count, values);
done:
    free(values);
    return result;
}

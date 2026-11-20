#ifndef TYAML_H
#define TYAML_H
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tcl.h>
#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>
#ifndef TCL_SIZE_MAX
typedef int Tcl_Size;
#define TCL_SIZE_MAX INT_MAX
#endif
typedef struct fy_generic_builder Builder;
typedef struct {
    Tcl_HashTable handles;
    Tcl_Encoding utf8;
} Context;
typedef struct {
    Builder *gb;
    size_t refs;
} Arena;
typedef struct {
    Arena *arena;
    size_t refs;
    int mutable;
    fy_generic root, vds;
} Document;
typedef struct {
    Document *doc;
    fy_generic path;
} Node;

static inline int error(Tcl_Interp *ip, const char *msg) {
    Tcl_SetObjResult(ip, Tcl_NewStringObj(msg, -1));
    return TCL_ERROR;
}
static inline Context *context(Tcl_Interp *ip) {
    return Tcl_GetAssocData(ip, "tyaml", NULL);
}
Node *lookup(Tcl_Interp *, Tcl_Obj *);
fy_generic current(Node *);
Tcl_Obj *asTcl(Tcl_Interp *, fy_generic, int typed);
fy_generic fromTcl(Tcl_Interp *, Builder *, Tcl_Obj *, int typed, int depth);
fy_generic stringValue(Tcl_Interp *, Builder *, Tcl_Obj *);
fy_generic decorate(Builder *, fy_generic, fy_generic tag, enum fy_scalar_style);
#endif

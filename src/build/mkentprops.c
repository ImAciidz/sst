/*
 * Copyright © 2022 Michael Smith <mikesmiffy128@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED “AS IS” AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../intdefs.h"
#include "../kv.h"
#include "../noreturn.h"
#include "../os.h"
#include "skiplist.h"
#include "vec.h"

#ifdef _WIN32
#define fS "S"
#else
#define fS "s"
#endif

static noreturn die(const char *s) {
	fprintf(stderr, "mkentprops: %s\n", s);
	exit(100);
}

struct prop {
	const char *varname; /* the C global name */
	const char *propname; /* the entity property name */
	struct prop *next;
};
struct vec_prop VEC(struct prop *);

DECL_SKIPLIST(static, class, struct class, const char *, 4)
struct class {
	const char *name; /* the entity class name */
	struct vec_prop props;
	struct skiplist_hdr_class hdr;
};
static inline int cmp_class(struct class *c, const char *s) {
	return strcmp(c->name, s);
}
static inline struct skiplist_hdr_class *hdr_class(struct class *c) {
	return &c->hdr;
}
DEF_SKIPLIST(static, class, cmp_class, hdr_class)
static struct skiplist_hdr_class classes = {0};
static int nclasses = 0;

struct parsestate {
	const os_char *filename;
	struct kv_parser *parser;
	char *lastvar;
};

static noreturn badparse(struct parsestate *state, const char *e) {
	fprintf(stderr, "mkentprops: %" fS ":%d:%d: parse error: %s",
			state->filename, state->parser->line, state->parser->col, e);
	exit(1);
}

static void kv_cb(enum kv_token type, const char *p, uint len, void *ctxt) {
	struct parsestate *state = ctxt;
	switch (type) {
		case KV_IDENT: case KV_IDENT_QUOTED:
			state->lastvar = malloc(len + 1);
			if (!state->lastvar) die("couldn't allocate memory");
			memcpy(state->lastvar, p, len); state->lastvar[len] = '\0';
			break;
		case KV_NEST_START: badparse(state, "unexpected nested block");
		case KV_NEST_END: badparse(state, "unexpected closing brace");
		case KV_VAL: case KV_VAL_QUOTED:;
			struct prop *prop = malloc(sizeof(*prop));
			if (!p) die("couldn't allocate memory");
			prop->varname = state->lastvar;
			char *classname = malloc(len + 1);
			if (!classname) die("couldn't allocate memory");
			memcpy(classname, p, len); classname[len] = '\0';
			char *propname = strchr(classname, '/');
			if (!propname) {
				badparse(state, "network name not in class/prop format");
			}
			*propname = '\0'; ++propname; // split!
			prop->propname = propname;
			struct class *class = skiplist_get_class(&classes, classname);
			if (!class) {
				class = malloc(sizeof(*class));
				if (!class) die("couldn't allocate memory");
				*class = (struct class){.name = classname};
				skiplist_insert_class(&classes, classname, class);
				++nclasses;
			}
			// (if class is already there just leak classname, no point freeing)
			if (!vec_push(&class->props, prop)) die("couldn't append to array");
			break;
		case KV_COND_PREFIX: case KV_COND_SUFFIX:
			badparse(state, "unexpected conditional");
	}
}

static inline noreturn diewrite(void) { die("couldn't write to file"); }

#define _(x) \
	if (fprintf(out, "%s\n", x) < 0) diewrite();
#define F(f, ...) \
	if (fprintf(out, f "\n", __VA_ARGS__) < 0) diewrite();
#define H() \
_( "/* This file is autogenerated by src/build/mkentprops.c. DO NOT EDIT! */") \
_( "")

static void decls(FILE *out) {
	for (struct class *c = classes.x[0]; c; c = c->hdr.x[0]) {
		for (struct prop **pp = c->props.data;
				pp - c->props.data < c->props.sz; ++pp) {
F( "extern bool has_%s;", (*pp)->varname)
F( "extern int %s;", (*pp)->varname)
		}
	}
}

static void defs(FILE *out) {
	for (struct class *c = classes.x[0]; c; c = c->hdr.x[0]) {
		for (struct prop **pp = c->props.data;
				pp - c->props.data < c->props.sz; ++pp) {
F( "bool has_%s = false;", (*pp)->varname)
F( "int %s;", (*pp)->varname)
		}
	}
_( "")
_( "static void initentprops(struct ServerClass *class) {")
F( "	for (int needclasses = %d; class; class = class->next) {", nclasses)
	char *else1 = "";
	for (struct class *c = classes.x[0]; c; c = c->hdr.x[0]) {
		// TODO(opt): some sort of PHF instead of chained strcmp, if we ever
		// have more than a few classes/properties?
F( "		%sif (!strcmp(class->name, \"%s\")) {", else1, c->name)
_( "			struct SendTable *st = class->table;")
				// christ this is awful :(
F( "			int needprops = %d;", c->props.sz)
_( "			for (struct SendProp *p = st->props; (char *)p -")
_( "					(char *)st->props < st->nprops * sz_SendProp;")
_( "					p = mem_offset(p, sz_SendProp)) {")
		char *else2 = "";
		for (struct prop **pp = c->props.data;
				pp - c->props.data < c->props.sz; ++pp) {
F( "				%sif (!strcmp(*(const char **)mem_offset(p, off_SP_varname), \"%s\")) {",
		else2, (*pp)->propname) // ugh
F( "					has_%s = true;", (*pp)->varname)
F( "					%s = *(int *)mem_offset(p, off_SP_offset);", (*pp)->varname)
_( "					if (!--needprops) break;")
_( "				}")
			else2 = "else ";
		}
_( "			}")
_( "			if (!--needclasses) break;")
_( "		}")
		else1 = "else ";
	}
_( "	}")
_( "}")
}

int OS_MAIN(int argc, os_char *argv[]) {
	for (++argv; *argv; ++argv) {
		int fd = os_open(*argv, O_RDONLY);
		if (fd == -1) die("couldn't open file");
		struct kv_parser kv = {0};
		struct parsestate state = {*argv, &kv};
		char buf[1024];
		int nread;
		while (nread = read(fd, buf, sizeof(buf))) {
			if (nread == -1) die("couldn't read file");
			if (!kv_parser_feed(&kv, buf, nread, &kv_cb, &state)) goto ep;
		}
		if (!kv_parser_done(&kv)) {
ep:			fprintf(stderr, "mkentprops: %" fS ":%d:%d: bad syntax: %s\n",
					*argv, kv.line, kv.col, kv.errmsg);
			exit(1);
		}
		close(fd);
	}

	FILE *out = fopen(".build/include/entprops.gen.h", "wb");
	if (!out) die("couldn't open entprops.gen.h");
	H();
	decls(out);

	out = fopen(".build/include/entpropsinit.gen.h", "wb");
	if (!out) die("couldn't open entpropsinit.gen.h");
	H();
	defs(out);
	return 0;
}

// vi: sw=4 ts=4 noet tw=80 cc=80

/* fast.c
 * Copyright (c) 2012, Peter Ohler
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  - Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 *  - Neither the name of Peter Ohler nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "ruby.h"
#include "oj.h"

#define MAX_STACK	64

// TBD combine key and index
// TBD combine elements and str
// TBD add leaf_type and shorten size of type to 1 or two bytes
typedef struct _Leaf {
    struct _Leaf	*next;
    const char		*key;      // hash key
    size_t		index;     // array index, 0 is not set
    char		*str;      // pointer to location in json string
    struct _Leaf	*elements; // array and hash elements
    int			type;
    VALUE		value;
} *Leaf;

//#define BATCH_SIZE	(4096 / sizeof(struct _Leaf) - 1)
#define BATCH_SIZE	80

typedef struct _Batch {
    struct _Batch	*next;
    int			next_avail;
    struct _Leaf	leaves[BATCH_SIZE];
} *Batch;

typedef struct _Doc {
    Leaf		data;
    Leaf		*where;      // points to current location
    Leaf		where_path[MAX_STACK]; // points to head of path
#ifdef HAVE_RUBY_ENCODING_H
    rb_encoding		*encoding;
#else
    void		*encoding;
#endif
    VALUE		self;
    Batch		batches;
    //Leaf		where_array[MAX_STACK];
    //size_t		where_len;   // length of allocated if longer than where_array
    struct _Batch	batch0;
} *Doc;

typedef struct _ParseInfo {
    char	*str;		/* buffer being read from */
    char	*s;		/* current position in buffer */
    Doc		doc;
} *ParseInfo;

static void	leaf_init(Leaf leaf, char *str, int type);
static Leaf	leaf_new(Doc doc, char *str, int type);
static void	leaf_append_element(Leaf parent, Leaf element);
static VALUE	leaf_value(Doc doc, Leaf leaf);
static void	leaf_fixnum_value(Leaf leaf);
static void	leaf_float_value(Leaf leaf);
static void	leaf_array_value(Doc doc, Leaf leaf);
static void	leaf_hash_value(Doc doc, Leaf leaf);

static Leaf	read_next(ParseInfo pi);
static Leaf	read_obj(ParseInfo pi);
static Leaf	read_array(ParseInfo pi);
static Leaf	read_str(ParseInfo pi);
static Leaf	read_num(ParseInfo pi);
static Leaf	read_true(ParseInfo pi);
static Leaf	read_false(ParseInfo pi);
static Leaf	read_nil(ParseInfo pi);
static void	next_non_white(ParseInfo pi);
static char*	read_quoted_value(ParseInfo pi);

static void	doc_init(Doc doc);
static void	doc_free(Doc doc);
static VALUE	protect_open_proc(VALUE x);
static VALUE	doc_open(VALUE clas, VALUE str);
static VALUE	doc_where(VALUE self);
static VALUE	doc_local_key(VALUE self);
static VALUE	doc_home(VALUE self);
static Leaf	get_doc_leaf(Doc doc, const char *path);
static Leaf	get_leaf(Leaf *stack, Leaf *lp, const char *path);
static VALUE	doc_type(int argc, VALUE *argv, VALUE self);
static VALUE	doc_fetch(int argc, VALUE *argv, VALUE self);
static VALUE	doc_each_leaf(int argc, VALUE *argv, VALUE self);
static VALUE	doc_move(VALUE self, VALUE str);
static VALUE	doc_each_branch(int argc, VALUE *argv, VALUE self);
static void	each_value(Doc doc, Leaf leaf);
static VALUE	doc_each_value(int argc, VALUE *argv, VALUE self);
static VALUE	doc_dump(VALUE self);

VALUE	oj_doc_class = 0;

inline static void
next_non_white(ParseInfo pi) {
    for (; 1; pi->s++) {
	switch(*pi->s) {
	case ' ':
	case '\t':
	case '\f':
	case '\n':
	case '\r':
	    break;
	default:
	    return;
	}
    }
}

inline static void
next_white(ParseInfo pi) {
    for (; 1; pi->s++) {
	switch(*pi->s) {
	case ' ':
	case '\t':
	case '\f':
	case '\n':
	case '\r':
	case '\0':
	    return;
	default:
	    break;
	}
    }
}


inline static char*
ulong_fill(char *s, size_t num) {
    char	buf[32];
    char        *b = buf + sizeof(buf) - 1;

    *b-- = '\0';
    for (; 0 < num; num /= 10, b--) {
        *b = (num % 10) + '0';
    }
    b++;
    if ('\0' == *b) {
	b--;
	*b = '0';
    }
    for (; '\0' != *b; b++, s++) {
	*s = *b;
    }
    return s;
}

inline static void
leaf_init(Leaf leaf, char *str, int type) {
    leaf->next = 0;
    leaf->key = 0;
    leaf->index = 0;
    leaf->str = str;
    leaf->elements = 0;
    leaf->type = type;
    leaf->value = Qundef;
}

static Leaf
leaf_new(Doc doc, char *str, int type) {
    Leaf	leaf;

    if (0 == doc->batches || BATCH_SIZE == doc->batches->next_avail) {
	Batch	b = ALLOC(struct _Batch);

	b->next = doc->batches;
	doc->batches = b;
	b->next_avail = 0;
    }
    leaf = &doc->batches->leaves[doc->batches->next_avail];
    doc->batches->next_avail++;
    leaf_init(leaf, str, type);

    return leaf;
}

static void
leaf_append_element(Leaf parent, Leaf element) {
    if (0 == parent->elements) {
	parent->elements = element;
	element->next = element;
    } else {
	element->next = parent->elements->next;
	parent->elements->next = element;
	parent->elements = element;
    }
}

static VALUE
leaf_value(Doc doc, Leaf leaf) {
    if (Qundef == leaf->value) {
	switch (leaf->type) {
	case T_NIL:
	    leaf->value = Qnil;
	    break;
	case T_TRUE:
	    leaf->value = Qtrue;
	    break;
	case T_FALSE:
	    leaf->value = Qfalse;
	    break;
	case T_FIXNUM:
	    leaf_fixnum_value(leaf);
	    break;
	case T_FLOAT:
	    leaf_float_value(leaf);
	    break;
	case T_STRING:
	    leaf->value = rb_str_new2(leaf->str);
#ifdef HAVE_RUBY_ENCODING_H
	    if (0 != doc->encoding) {
		rb_enc_associate(leaf->value, doc->encoding);
	    }
#endif
	    break;
	case T_ARRAY:
	    leaf_array_value(doc, leaf);
	    break;
	case T_HASH:
	    leaf_hash_value(doc, leaf);
	    break;
	default:
	    rb_raise(rb_eTypeError, "Unexpected type %02x.", leaf->type);
	    break;
	}
    }
    return leaf->value;
}

#ifdef RUBINIUS
#define NUM_MAX 0x07FFFFFF
#else
#define NUM_MAX (FIXNUM_MAX >> 8)
#endif


static void
leaf_fixnum_value(Leaf leaf) {
    char	*s = leaf->str;
    int64_t	n = 0;
    int		neg = 0;
    int		big = 0;

    if ('-' == *s) {
	s++;
	neg = 1;
    } else if ('+' == *s) {
	s++;
    }
    for (; '0' <= *s && *s <= '9'; s++) {
	n = n * 10 + (*s - '0');
	if (NUM_MAX <= n) {
	    big = 1;
	}
    }
    if (big) {
	char	c = *s;
	
	*s = '\0';
	leaf->value = rb_cstr_to_inum(leaf->str, 10, 0);
	*s = c;
    } else {
	if (neg) {
	    n = -n;
	}
	leaf->value = LONG2NUM(n);
    }
}

#if 1
static void
leaf_float_value(Leaf leaf) {
    leaf->value = DBL2NUM(rb_cstr_to_dbl(leaf->str, 1));
}
#else
static void
leaf_float_value(Leaf leaf) {
    char	*s = leaf->str;
    int64_t	n = 0;
    long	a = 0;
    long	div = 1;
    long	e = 0;
    int		neg = 0;
    int		eneg = 0;
    int		big = 0;

    if ('-' == *s) {
	s++;
	neg = 1;
    } else if ('+' == *s) {
	s++;
    }
    for (; '0' <= *s && *s <= '9'; s++) {
	n = n * 10 + (*s - '0');
	if (NUM_MAX <= n) {
	    big = 1;
	}
    }
    if (big) {
	char	c = *s;
	
	*s = '\0';
	leaf->value = rb_cstr_to_inum(leaf->str, 10, 0);
	*s = c;
    } else {
	double	d;

	if ('.' == *s) {
	    s++;
	    for (; '0' <= *s && *s <= '9'; s++) {
		a = a * 10 + (*s - '0');
		div *= 10;
	    }
	}
	if ('e' == *s || 'E' == *s) {
	    s++;
	    if ('-' == *s) {
		s++;
		eneg = 1;
	    } else if ('+' == *s) {
		s++;
	    }
	    for (; '0' <= *s && *s <= '9'; s++) {
		e = e * 10 + (*s - '0');
	    }
	}
	d = (double)n + (double)a / (double)div;
	if (neg) {
	    d = -d;
	}
	if (0 != e) {
	    if (eneg) {
		e = -e;
	    }
	    d *= pow(10.0, e);
	}
	leaf->value = DBL2NUM(d);
    }
}
#endif

static void
leaf_array_value(Doc doc, Leaf leaf) {
    VALUE	a = rb_ary_new();

    if (0 != leaf->elements) {
	Leaf	first = leaf->elements->next;
	Leaf	e = first;

	do {
	    rb_ary_push(a, leaf_value(doc, e));
	    e = e->next;
	} while (e != first);
    }
    leaf->value = a;
}

static void
leaf_hash_value(Doc doc, Leaf leaf) {
    VALUE	h = rb_hash_new();

    if (0 != leaf->elements) {
	Leaf	first = leaf->elements->next;
	Leaf	e = first;
	VALUE	key;

	do {
	    key = rb_str_new2(e->key);
#ifdef HAVE_RUBY_ENCODING_H
	    if (0 != doc->encoding) {
		rb_enc_associate(key, doc->encoding);
	    }
#endif
	    rb_hash_aset(h, key, leaf_value(doc, e));
	    e = e->next;
	} while (e != first);
    }
    leaf->value = h;
}

static Leaf
read_next(ParseInfo pi) {
    Leaf	leaf = 0;

    next_non_white(pi);	// skip white space
    switch (*pi->s) {
    case '{':
	leaf = read_obj(pi);
	break;
    case '[':
	leaf = read_array(pi);
	break;
    case '"':
	leaf = read_str(pi);
	break;
    case '+':
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
	leaf = read_num(pi);
	break;
    case 't':
	leaf = read_true(pi);
	break;
    case 'f':
	leaf = read_false(pi);
	break;
    case 'n':
	leaf = read_nil(pi);
	break;
    case '\0':
    default:
	break; // returns 0
    }
    return leaf;
}

static Leaf
read_obj(ParseInfo pi) {
    Leaf	h = leaf_new(pi->doc, pi->s, T_HASH);
    char	*end;
    const char	*key = 0;
    Leaf	val = 0;

    pi->s++;
    next_non_white(pi);
    if ('}' == *pi->s) {
	pi->s++;
	return h;
    }
    while (1) {
	next_non_white(pi);
	key = 0;
	val = 0;
	if ('"' != *pi->s || 0 == (key = read_quoted_value(pi))) {
	    raise_error("unexpected character", pi->str, pi->s);
	}
	next_non_white(pi);
	if (':' == *pi->s) {
	    pi->s++;
	} else {
	    raise_error("invalid format, expected :", pi->str, pi->s);
	}
	if (0 == (val = read_next(pi))) {
	    //printf("*** '%s'\n", pi->s);
	    raise_error("unexpected character", pi->str, pi->s);
	}
	end = pi->s;
	val->key = key;
	leaf_append_element(h, val);
	next_non_white(pi);
	if ('}' == *pi->s) {
	    pi->s++;
	    *end = '\0';
	    break;
	} else if (',' == *pi->s) {
	    pi->s++;
	} else {
	    printf("*** '%s'\n", pi->s);
	    raise_error("invalid format, expected , or } while in an object", pi->str, pi->s);
	}
	*end = '\0';
    }
    return h;
}

static Leaf
read_array(ParseInfo pi) {
    Leaf	a = leaf_new(pi->doc, pi->s, T_ARRAY);
    Leaf	e;
    char	*end;
    int		cnt = 0;

    pi->s++;
    next_non_white(pi);
    if (']' == *pi->s) {
	pi->s++;
	return a;
    }
    while (1) {
	next_non_white(pi);
	if (0 == (e = read_next(pi))) {
	    raise_error("unexpected character", pi->str, pi->s);
	}
	cnt++;
	e->index = cnt;
	leaf_append_element(a, e);
	end = pi->s;
	next_non_white(pi);
	if (',' == *pi->s) {
	    pi->s++;
	} else if (']' == *pi->s) {
	    pi->s++;
	    *end = '\0';
	    break;
	} else {
	    raise_error("invalid format, expected , or ] while in an array", pi->str, pi->s);
	}
	*end = '\0';
    }
    return a;
}

static Leaf
read_str(ParseInfo pi) {
    return leaf_new(pi->doc, read_quoted_value(pi), T_STRING);
}

static Leaf
read_num(ParseInfo pi) {
    char	*start = pi->s;
    int		type = T_FIXNUM;

    if ('-' == *pi->s) {
	pi->s++;
    }
    // digits
    for (; '0' <= *pi->s && *pi->s <= '9'; pi->s++) {
    }
    if ('.' == *pi->s) {
	type = T_FLOAT;
	pi->s++;
	for (; '0' <= *pi->s && *pi->s <= '9'; pi->s++) {
	}
    }
    if ('e' == *pi->s || 'E' == *pi->s) {
	pi->s++;
	if ('-' == *pi->s || '+' == *pi->s) {
	    pi->s++;
	}
	for (; '0' <= *pi->s && *pi->s <= '9'; pi->s++) {
	}
    }
    return leaf_new(pi->doc, start, type);
}

static Leaf
read_true(ParseInfo pi) {
    Leaf	leaf = leaf_new(pi->doc, pi->s, T_TRUE);

    pi->s++;
    if ('r' != *pi->s || 'u' != *(pi->s + 1) || 'e' != *(pi->s + 2)) {
	raise_error("invalid format, expected 'true'", pi->str, pi->s);
    }
    pi->s += 3;

    return leaf;
}

static Leaf
read_false(ParseInfo pi) {
    Leaf	leaf = leaf_new(pi->doc, pi->s, T_FALSE);

    pi->s++;
    if ('a' != *pi->s || 'l' != *(pi->s + 1) || 's' != *(pi->s + 2) || 'e' != *(pi->s + 3)) {
	raise_error("invalid format, expected 'false'", pi->str, pi->s);
    }
    pi->s += 4;

    return leaf;
}

static Leaf
read_nil(ParseInfo pi) {
    Leaf	leaf = leaf_new(pi->doc, pi->s, T_NIL);

    pi->s++;
    if ('u' != *pi->s || 'l' != *(pi->s + 1) || 'l' != *(pi->s + 2)) {
	raise_error("invalid format, expected 'nil'", pi->str, pi->s);
    }
    pi->s += 3;

    return leaf;
}

static char
read_hex(ParseInfo pi, char *h) {
    uint8_t	b = 0;

    if ('0' <= *h && *h <= '9') {
	b = *h - '0';
    } else if ('A' <= *h && *h <= 'F') {
	b = *h - 'A' + 10;
    } else if ('a' <= *h && *h <= 'f') {
	b = *h - 'a' + 10;
    } else {
	pi->s = h;
	raise_error("invalid hex character", pi->str, pi->s);
    }
    h++;
    b = b << 4;
    if ('0' <= *h && *h <= '9') {
	b += *h - '0';
    } else if ('A' <= *h && *h <= 'F') {
	b += *h - 'A' + 10;
    } else if ('a' <= *h && *h <= 'f') {
	b += *h - 'a' + 10;
    } else {
	pi->s = h;
	raise_error("invalid hex character", pi->str, pi->s);
    }
    return (char)b;
}

/* Assume the value starts immediately and goes until the quote character is
 * reached again. Do not read the character after the terminating quote.
 */
static char*
read_quoted_value(ParseInfo pi) {
    char	*value = 0;
    char	*h = pi->s; // head
    char	*t = h;     // tail
    
    h++;	// skip quote character
    t++;
    value = h;
    for (; '"' != *h; h++, t++) {
	if ('\0' == *h) {
	    pi->s = h;
	    raise_error("quoted string not terminated", pi->str, pi->s);
	} else if ('\\' == *h) {
	    h++;
	    switch (*h) {
	    case 'n':	*t = '\n';	break;
	    case 'r':	*t = '\r';	break;
	    case 't':	*t = '\t';	break;
	    case 'f':	*t = '\f';	break;
	    case 'b':	*t = '\b';	break;
	    case '"':	*t = '"';	break;
	    case '/':	*t = '/';	break;
	    case '\\':	*t = '\\';	break;
	    case 'u':
		h++;
		*t = read_hex(pi, h);
		h += 2;
		if ('\0' != *t) {
		    t++;
		}
		*t = read_hex(pi, h);
		h++;
		break;
	    default:
		pi->s = h;
		raise_error("invalid escaped character", pi->str, pi->s);
		break;
	    }
	} else if (t != h) {
	    *t = *h;
	}
    }
    *t = '\0'; // terminate value
    pi->s = h + 1;

    return value;
}

inline static void
doc_init(Doc doc) {
    //doc->where_path = doc->where_array;
    //doc->where_len = 0;
    doc->where = doc->where_path;
    *doc->where = 0;
    doc->data = 0;
    doc->self = Qundef;
#ifdef HAVE_RUBY_ENCODING_H
    doc->encoding = ('\0' == *oj_default_options.encoding) ? 0 : rb_enc_find(oj_default_options.encoding);
#else
    doc->encoding = 0;
#endif
    doc->batches = &doc->batch0;
    doc->batch0.next = 0;
    doc->batch0.next_avail = 0;
}

static void
doc_free(Doc doc) {
    if (0 != doc) {
	Batch	b;

	while (0 != (b = doc->batches)) {
	    doc->batches = doc->batches->next;
	    if (&doc->batch0 != b) {
		xfree(b);
	    }
	}
	/*
	if (doc->where_array != doc->where_path) {
	    free(doc->where_path);
	}
	*/
	//xfree(f);
    }
}

static VALUE
protect_open_proc(VALUE x) {
    ParseInfo	pi = (ParseInfo)x;

    pi->doc->data = read_next(pi); // parse
    *pi->doc->where = pi->doc->data;
    pi->doc->where = pi->doc->where_path;
    return rb_yield(pi->doc->self); // caller processing
}

static VALUE
doc_open(VALUE clas, VALUE str) {
    struct _ParseInfo	pi;
    VALUE		result = Qnil;
    size_t		len;
    struct _Doc	doc;
    int			ex = 0;

    Check_Type(str, T_STRING);
    if (!rb_block_given_p()) {
	rb_raise(rb_eArgError, "Block or Proc is required.");
    }
    len = RSTRING_LEN(str);
    pi.str = ALLOCA_N(char, len + 1);
    //pi.str = ALLOC_N(char, len + 1);
    memcpy(pi.str, StringValuePtr(str), len + 1);
    pi.s = pi.str;
    doc_init(&doc);
    pi.doc = &doc;
    doc.self = rb_obj_alloc(clas);
    DATA_PTR(doc.self) = pi.doc;
    result = rb_protect(protect_open_proc, (VALUE)&pi, &ex);
    DATA_PTR(doc.self) = 0;
    doc_free(pi.doc);
    //xfree(pi.str);
    if (0 != ex) {
	rb_jump_tag(ex);
    }
    return result;
}

static VALUE
doc_where(VALUE self) {
    Doc	doc = DATA_PTR(self);

    if (0 == *doc->where_path || doc->where == doc->where_path) {
	return oj_slash_string;
    } else {
	Leaf	*lp;
	Leaf	leaf;
	size_t	size = 3; // leading / and terminating \0
	char	*path;
	char	*p;

	for (lp = doc->where_path; lp <= doc->where; lp++) {
	    leaf = *lp;
	    if (0 != leaf->key) {
		size += strlen((*lp)->key) + 1;
	    } else if (0 < leaf->index) {
		size += ((*lp)->index < 100) ? 3 : 11;
	    }
	}
	path = ALLOCA_N(char, size);
	p = path;
	for (lp = doc->where_path; lp <= doc->where; lp++) {
	    leaf = *lp;
	    if (0 != leaf->key) {
		p = stpcpy(p, (*lp)->key);
	    } else if (0 < leaf->index) {
		p = ulong_fill(p, (*lp)->index);
	    }
	    *p++ = '/';
	}
	*--p = '\0';
	return rb_str_new2(path);
    }
}

static VALUE
doc_local_key(VALUE self) {
    Doc		doc = DATA_PTR(self);
    Leaf	leaf = *doc->where;
    VALUE	key = Qnil;

    if (0 != leaf->key) {
	key = rb_str_new2(leaf->key);
#ifdef HAVE_RUBY_ENCODING_H
	if (0 != doc->encoding) {
	    rb_enc_associate(key, doc->encoding);
	}
#endif
    } else if (0 < leaf->index) {
	key = LONG2NUM(leaf->index);
    }
    return key;
}

static VALUE
doc_home(VALUE self) {
    Doc	doc = DATA_PTR(self);

    *doc->where_path = doc->data;
    doc->where = doc->where_path;

    return oj_slash_string;
}

static Leaf
get_doc_leaf(Doc doc, const char *path) {
    Leaf	leaf = *doc->where;

    if (0 != doc->data && 0 != path) {
	Leaf	stack[MAX_STACK];
	Leaf	*lp;

	if ('/' == *path) {
	    path++;
	    *stack = doc->data;
	    lp = stack;
	} else {
	    size_t	cnt = doc->where - doc->where_path;

	    memcpy(stack, doc->where_path, sizeof(Leaf) * cnt);
	    lp = stack + cnt;
	}
	return get_leaf(stack, lp, path);
    }
    return leaf;
}

static Leaf
get_leaf(Leaf *stack, Leaf *lp, const char *path) {
    Leaf	leaf = *lp;

    if ('\0' != *path) {
	if ('.' == *path && '.' == *(path + 1)) {
	    path += 2;
	    if ('/' == *path) {
		path++;
	    }
	    if (stack < lp) {
		leaf = get_leaf(stack, lp - 1, path);
	    } else {
		return 0;
	    }
	} else if (0 != leaf->elements) {
	    Leaf	first = leaf->elements->next;
	    Leaf	e = first;
	    int		type = leaf->type;

	    // TBD fail if stack too deep
	    leaf = 0;
	    if (T_ARRAY == type) {
		int	cnt = 0;

		for (; '0' <= *path && *path <= '9'; path++) {
		    cnt = cnt * 10 + (*path - '0');
		}
		if ('/' == *path) {
		    path++;
		}
		do {
		    if (1 >= cnt) {
			lp++;
			*lp = e;
			leaf = get_leaf(stack, lp, path);
			break;
		    }
		    cnt--;
		    e = e->next;
		} while (e != first);
	    } else if (T_HASH == type) {
		const char	*key = path;
		const char	*slash = strchr(path, '/');
		int		klen;

		if (0 == slash) {
		    klen = (int)strlen(key);
		    path += klen;
		} else {
		    klen = (int)(slash - key);
		    path += klen + 1;
		}
		do {
		    if (0 == strncmp(key, e->key, klen) && '\0' == e->key[klen]) {
			lp++;
			*lp = e;
			leaf = get_leaf(stack, lp, path);
			break;
		    }
		    e = e->next;
		} while (e != first);
	    }
	}
    }
    return leaf;
}

static VALUE
doc_type(int argc, VALUE *argv, VALUE self) {
    Doc		doc = DATA_PTR(self);
    Leaf	leaf;
    const char	*path = 0;
    VALUE	type = Qnil;

    if (1 <= argc) {
	Check_Type(*argv, T_STRING);
	path = StringValuePtr(*argv);
    }
    if (0 != (leaf = get_doc_leaf(doc, path))) {
	switch (leaf->type) {
	case T_NIL:	type = rb_cNilClass;	break;
	case T_TRUE:	type = rb_cTrueClass;	break;
	case T_FALSE:	type = rb_cFalseClass;	break;
	case T_STRING:	type = rb_cString;	break;
	case T_FIXNUM:	type = rb_cFixnum;	break;
	case T_FLOAT:	type = rb_cFloat;	break;
	case T_ARRAY:	type = rb_cArray;	break;
	case T_HASH:	type = rb_cHash;	break;
	default:				break;
	}
    }
    return type;
}

static VALUE
doc_fetch(int argc, VALUE *argv, VALUE self) {
    Doc		doc = DATA_PTR(self);
    Leaf	leaf;
    VALUE	val = Qnil;
    const char	*path = 0;

    if (1 <= argc) {
	Check_Type(*argv, T_STRING);
	path = StringValuePtr(*argv);
	if (2 == argc) {
	    val = argv[1];
	}
    }
    if (0 != (leaf = get_doc_leaf(doc, path))) {
	val = leaf_value(doc, leaf);
    }
    return val;
}

static VALUE
doc_each_leaf(int argc, VALUE *argv, VALUE self) {
    //Doc	f = DATA_PTR(self);

    return Qnil;
}

static int
move_step(Doc doc, const char *path, int loc) {
    // TBD raise if too deep
    if ('\0' == *path) {
	loc = 0;
    } else {
	Leaf	leaf;

	if (0 == doc->where || 0 == (leaf = *doc->where)) {
	    printf("*** Internal error at %s\n", path);
	    return loc;
	}
	if ('.' == *path && '.' == *(path + 1)) {
	    Leaf	init = *doc->where;

	    path += 2;
	    if (doc->where == doc->where_path) {
		return loc;
	    }
	    if ('/' == *path) {
		path++;
	    }
	    *doc->where = 0;
	    doc->where--;
	    loc = move_step(doc, path, loc + 1);
	    if (0 != loc) {
		*doc->where = init;
		doc->where++;
	    }
	} else if (0 != leaf->elements) {
	    Leaf	first = leaf->elements->next;
	    Leaf	e = first;

	    if (T_ARRAY == leaf->type) {
		int	cnt = 0;

		for (; '0' <= *path && *path <= '9'; path++) {
		    cnt = cnt * 10 + (*path - '0');
		}
		if ('/' == *path) {
		    path++;
		} else if ('\0' != *path) {
		    return loc;
		}
		do {
		    if (1 >= cnt) {
			doc->where++;
			*doc->where = e;
			loc = move_step(doc, path, loc + 1);
			if (0 != loc) {
			    *doc->where = 0;
			    doc->where--;
			}
			break;
		    }
		    cnt--;
		    e = e->next;
		} while (e != first);
	    } else if (T_HASH == leaf->type) {
		const char	*key = path;
		const char	*slash = strchr(path, '/');
		int		klen;

		if (0 == slash) {
		    klen = (int)strlen(key);
		    path += klen;
		} else {
		    klen = (int)(slash - key);
		    path += klen + 1;
		}
		do {
		    if (0 == strncmp(key, e->key, klen) && '\0' == e->key[klen]) {
			doc->where++;
			*doc->where = e;
			loc = move_step(doc, path, loc + 1);
			if (0 != loc) {
			    *doc->where = 0;
			    doc->where--;
			}
			break;
		    }
		    e = e->next;
		} while (e != first);
	    }
	}
    }
    return loc;
}

static VALUE
doc_move(VALUE self, VALUE str) {
    Doc		doc = DATA_PTR(self);
    const char	*path;
    int		loc;

    Check_Type(str, T_STRING);
    path = StringValuePtr(str);
    if ('/' == *path) {
	doc->where = doc->where_path;
	path++;
    }
    if (0 != (loc = move_step(doc, path, 1))) {
	rb_raise(rb_eArgError, "Failed to locate element %d of the path %s.", loc, path);
    }
    return Qnil;
}

static VALUE
doc_each_branch(int argc, VALUE *argv, VALUE self) {
    if (rb_block_given_p()) {
	Leaf		save_path[MAX_STACK];
	Doc		doc = DATA_PTR(self);
	const char	*path = 0;
	size_t		wlen;

	wlen = doc->where - doc->where_path;
	memcpy(save_path, doc->where_path, sizeof(Leaf) * wlen);
	if (1 <= argc) {
	    Check_Type(*argv, T_STRING);
	    path = StringValuePtr(*argv);
	    if ('/' == *path) {
		doc->where = doc->where_path;
		path++;
	    }
	    if (0 != move_step(doc, path, 1)) {
		memcpy(doc->where_path, save_path, sizeof(Leaf) * wlen);
		return Qnil;
	    }
	}
	if (0 != (*doc->where)->elements) {
	    Leaf	first = (*doc->where)->elements->next;
	    Leaf	e = first;
	    VALUE	args[1];

	    *args = self;
	    doc->where++;
	    do {
		*doc->where = e;
		rb_yield_values2(1, args);
		e = e->next;
	    } while (e != first);
	}
	memcpy(doc->where_path, save_path, sizeof(Leaf) * wlen);
    }
    return Qnil;
}

static void
each_value(Doc doc, Leaf leaf) {
    if (T_ARRAY == leaf->type || T_HASH == leaf->type) {
	if (0 != leaf->elements) {
	    Leaf	first = leaf->elements->next;
	    Leaf	e = first;

	    do {
		each_value(doc, e);
		e = e->next;
	    } while (e != first);
	}
    } else {
	VALUE	args[1];

	*args = leaf_value(doc, leaf);
	rb_yield_values2(1, args);
    }
}

static VALUE
doc_each_value(int argc, VALUE *argv, VALUE self) {
    if (rb_block_given_p()) {
	Doc		doc = DATA_PTR(self);
	const char	*path = 0;
	Leaf		leaf;

	if (1 <= argc) {
	    Check_Type(*argv, T_STRING);
	    path = StringValuePtr(*argv);
	}
	if (0 != (leaf = get_doc_leaf(doc, path))) {
	    each_value(doc, leaf);
	}
    }
    return Qnil;
}

// TBD improve later to be more direct for higher performance, also make relative
static VALUE
doc_dump(VALUE self) {
    Doc		doc = DATA_PTR(self);
    VALUE	obj = leaf_value(doc, *doc->where);
    const char	*json;

    json = oj_write_obj_to_str(obj, &oj_default_options);
    
    return rb_str_new2(json);
}

void
oj_init_doc() {
    oj_doc_class = rb_define_class_under(Oj, "Doc", rb_cObject);
    rb_define_singleton_method(oj_doc_class, "open", doc_open, 1);
    rb_define_singleton_method(oj_doc_class, "parse", doc_open, 1);
    rb_define_method(oj_doc_class, "where?", doc_where, 0);
    rb_define_method(oj_doc_class, "local_key", doc_local_key, 0);
    rb_define_method(oj_doc_class, "home", doc_home, 0);
    rb_define_method(oj_doc_class, "type", doc_type, -1);
    rb_define_method(oj_doc_class, "fetch", doc_fetch, -1);
    rb_define_method(oj_doc_class, "each_leaf", doc_each_leaf, -1);
    rb_define_method(oj_doc_class, "move", doc_move, 1);
    rb_define_method(oj_doc_class, "each_branch", doc_each_branch, -1);
    rb_define_method(oj_doc_class, "each_value", doc_each_value, -1);
    rb_define_method(oj_doc_class, "dump", doc_dump, 0);
}

/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2011 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "dmlib.h"

#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>

#define SECTION_B_CHAR '{'
#define SECTION_E_CHAR '}'

enum {
	TOK_INT,
	TOK_FLOAT,
	TOK_STRING,		/* Single quotes */
	TOK_STRING_ESCAPED,	/* Double quotes */
	TOK_EQ,
	TOK_SECTION_B,
	TOK_SECTION_E,
	TOK_ARRAY_B,
	TOK_ARRAY_E,
	TOK_IDENTIFIER,
	TOK_COMMA,
	TOK_EOF
};

struct parser {
	const char *fb, *fe;		/* file limits */

	int t;			/* token limits and type */
	const char *tb, *te;

	int line;		/* line number we are on */

	struct dm_pool *mem;
};

struct output_line {
	struct dm_pool *mem;
	dm_putline_fn putline;
	void *putline_baton;
};

static void _get_token(struct parser *p, int tok_prev);
static void _eat_space(struct parser *p);
static struct dm_config_node *_file(struct parser *p);
static struct dm_config_node *_section(struct parser *p);
static struct dm_config_value *_value(struct parser *p);
static struct dm_config_value *_type(struct parser *p);
static int _match_aux(struct parser *p, int t);
static struct dm_config_value *_create_value(struct dm_pool *mem);
static struct dm_config_node *_create_node(struct dm_pool *mem);
static char *_dup_tok(struct parser *p);

static const int sep = '/';

#define MAX_INDENT 32

#define match(t) do {\
   if (!_match_aux(p, (t))) {\
	log_error("Parse error at byte %" PRIptrdiff_t " (line %d): unexpected token", \
		  p->tb - p->fb + 1, p->line); \
      return 0;\
   } \
} while(0);

static int _tok_match(const char *str, const char *b, const char *e)
{
	while (*str && (b != e)) {
		if (*str++ != *b++)
			return 0;
	}

	return !(*str || (b != e));
}

struct dm_config_tree *dm_config_create(void)
{
	struct dm_config_tree *cft;
	struct dm_pool *mem = dm_pool_create("config", 10 * 1024);

	if (!mem) {
		log_error("Failed to allocate config pool.");
		return 0;
	}

	if (!(cft = dm_pool_zalloc(mem, sizeof(*cft)))) {
		log_error("Failed to allocate config tree.");
		dm_pool_destroy(mem);
		return 0;
	}
	cft->root = NULL;
	cft->cascade = NULL;
	cft->custom = NULL;
	cft->mem = mem;
	return cft;
}

void dm_config_set_custom(struct dm_config_tree *cft, void *custom)
{
	cft->custom = custom;
}

void *dm_config_get_custom(struct dm_config_tree *cft)
{
	return cft->custom;
}

void dm_config_destroy(struct dm_config_tree *cft)
{
	dm_pool_destroy(cft->mem);
}

/*
 * If there's a cascaded dm_config_tree, remove and return it, otherwise
 * return NULL.
 */
struct dm_config_tree *dm_config_remove_cascaded_tree(struct dm_config_tree *cft)
{
	struct dm_config_tree *second_cft;

	if (!cft)
		return NULL;

	second_cft = cft->cascade;
	cft->cascade = NULL;

	return second_cft;
}

/*
 * When searching, first_cft is checked before second_cft.
 */
struct dm_config_tree *dm_config_insert_cascaded_tree(struct dm_config_tree *first_cft, struct dm_config_tree *second_cft)
{
	first_cft->cascade = second_cft;

	return first_cft;
}

int dm_config_parse(struct dm_config_tree *cft, const char *start, const char *end)
{
	/* TODO? if (start == end) return 1; */

	struct parser *p;
	if (!(p = dm_pool_alloc(cft->mem, sizeof(*p))))
		return_0;

	p->mem = cft->mem;
	p->fb = start;
	p->fe = end;
	p->tb = p->te = p->fb;
	p->line = 1;

	_get_token(p, TOK_SECTION_E);
	if (!(cft->root = _file(p)))
		return_0;

	return 1;
}

struct dm_config_tree *dm_config_from_string(const char *config_settings)
{
	struct dm_config_tree *cft;

	if (!(cft = dm_config_create()))
		return_NULL;

	if (!dm_config_parse(cft, config_settings, config_settings + strlen(config_settings))) {
		dm_config_destroy(cft);
		return_NULL;
	}

	return cft;
}

static int _line_start(struct output_line *outline)
{
	if (!dm_pool_begin_object(outline->mem, 128)) {
		log_error("dm_pool_begin_object failed for config line");
		return 0;
	}

	return 1;
}

__attribute__ ((format(printf, 2, 3)))
static int _line_append(struct output_line *outline, const char *fmt, ...)
{
	char buf[4096];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(&buf[0], sizeof buf - 1, fmt, ap);
	va_end(ap);

	if (n < 0 || n > (int) sizeof buf - 1) {
		log_error("vsnprintf failed for config line");
		return 0;
	}

	if (!dm_pool_grow_object(outline->mem, &buf[0], strlen(buf))) {
		log_error("dm_pool_grow_object failed for config line");
		return 0;
	}

	return 1;
}

#define line_append(args...) do {if (!_line_append(outline, args)) {return_0;}} while (0)

static int _line_end(struct output_line *outline)
{
	const char *line;

	if (!dm_pool_grow_object(outline->mem, "\0", 1)) {
		log_error("dm_pool_grow_object failed for config line");
		return 0;
	}

	line = dm_pool_end_object(outline->mem);

	if (!outline->putline)
		return 0;

	outline->putline(line, outline->putline_baton);

	return 1;
}

static int _write_value(struct output_line *outline, const struct dm_config_value *v)
{
	char *buf;

	switch (v->type) {
	case DM_CFG_STRING:
		if (!(buf = alloca(dm_escaped_len(v->v.str)))) {
			log_error("temporary stack allocation for a config "
				  "string failed");
			return 0;
		}
		line_append("\"%s\"", dm_escape_double_quotes(buf, v->v.str));
		break;

	case DM_CFG_FLOAT:
		line_append("%f", v->v.f);
		break;

	case DM_CFG_INT:
		line_append("%" PRId64, v->v.i);
		break;

	case DM_CFG_EMPTY_ARRAY:
		line_append("[]");
		break;

	default:
		log_error("_write_value: Unknown value type: %d", v->type);

	}

	return 1;
}

static int _write_config(const struct dm_config_node *n, int only_one,
			 struct output_line *outline, int level)
{
	char space[MAX_INDENT + 1];
	int l = (level < MAX_INDENT) ? level : MAX_INDENT;
	int i;

	if (!n)
		return 1;

	for (i = 0; i < l; i++)
		space[i] = '\t';
	space[i] = '\0';

	do {
		if (!_line_start(outline))
			return_0;
		line_append("%s%s", space, n->key);
		if (!n->v) {
			/* it's a sub section */
			line_append(" {");
			if (!_line_end(outline))
				return_0;
			_write_config(n->child, 0, outline, level + 1);
			if (!_line_start(outline))
				return_0;
			line_append("%s}", space);
		} else {
			/* it's a value */
			const struct dm_config_value *v = n->v;
			line_append("=");
			if (v->next) {
				line_append("[");
				while (v && v->type != DM_CFG_EMPTY_ARRAY) {
					if (!_write_value(outline, v))
						return_0;
					v = v->next;
					if (v && v->type != DM_CFG_EMPTY_ARRAY)
						line_append(", ");
				}
				line_append("]");
			} else
				if (!_write_value(outline, v))
					return_0;
		}
		if (!_line_end(outline))
			return_0;
		n = n->sib;
	} while (n && !only_one);
	/* FIXME: add error checking */
	return 1;
}

int dm_config_write_node(const struct dm_config_node *cn, dm_putline_fn putline, void *baton)
{
	struct output_line outline;
	if (!(outline.mem = dm_pool_create("config_line", 1024)))
		return_0;
	outline.putline = putline;
	outline.putline_baton = baton;
	if (!_write_config(cn, 0, &outline, 0)) {
		dm_pool_destroy(outline.mem);
		return_0;
	}
	dm_pool_destroy(outline.mem);
	return 1;
}


/*
 * parser
 */
static struct dm_config_node *_file(struct parser *p)
{
	struct dm_config_node *root = NULL, *n, *l = NULL;
	while (p->t != TOK_EOF) {
		if (!(n = _section(p)))
			return_NULL;

		if (!root)
			root = n;
		else
			l->sib = n;
		n->parent = root;
		l = n;
	}
	return root;
}

static struct dm_config_node *_section(struct parser *p)
{
	/* IDENTIFIER SECTION_B_CHAR VALUE* SECTION_E_CHAR */
	struct dm_config_node *root, *n, *l = NULL;
	if (!(root = _create_node(p->mem))) {
		log_error("Failed to allocate section node");
		return NULL;
	}

	if (!(root->key = _dup_tok(p)))
		return_NULL;

	match(TOK_IDENTIFIER);

	if (p->t == TOK_SECTION_B) {
		match(TOK_SECTION_B);
		while (p->t != TOK_SECTION_E) {
			if (!(n = _section(p)))
				return_NULL;

			if (!l)
				root->child = n;
			else
				l->sib = n;
			n->parent = root;
			l = n;
		}
		match(TOK_SECTION_E);
	} else {
		match(TOK_EQ);
		if (!(root->v = _value(p)))
			return_NULL;
	}

	return root;
}

static struct dm_config_value *_value(struct parser *p)
{
	/* '[' TYPE* ']' | TYPE */
	struct dm_config_value *h = NULL, *l, *ll = NULL;
	if (p->t == TOK_ARRAY_B) {
		match(TOK_ARRAY_B);
		while (p->t != TOK_ARRAY_E) {
			if (!(l = _type(p)))
				return_NULL;

			if (!h)
				h = l;
			else
				ll->next = l;
			ll = l;

			if (p->t == TOK_COMMA)
				match(TOK_COMMA);
		}
		match(TOK_ARRAY_E);
		/*
		 * Special case for an empty array.
		 */
		if (!h) {
			if (!(h = _create_value(p->mem))) {
				log_error("Failed to allocate value");
				return NULL;
			}

			h->type = DM_CFG_EMPTY_ARRAY;
		}

	} else
		if (!(h = _type(p)))
			return_NULL;

	return h;
}

static struct dm_config_value *_type(struct parser *p)
{
	/* [+-]{0,1}[0-9]+ | [0-9]*\.[0-9]* | ".*" */
	struct dm_config_value *v = _create_value(p->mem);
	char *str;

	if (!v) {
		log_error("Failed to allocate type value");
		return NULL;
	}

	switch (p->t) {
	case TOK_INT:
		v->type = DM_CFG_INT;
		v->v.i = strtoll(p->tb, NULL, 0);	/* FIXME: check error */
		match(TOK_INT);
		break;

	case TOK_FLOAT:
		v->type = DM_CFG_FLOAT;
		v->v.f = strtod(p->tb, NULL);	/* FIXME: check error */
		match(TOK_FLOAT);
		break;

	case TOK_STRING:
		v->type = DM_CFG_STRING;

		p->tb++, p->te--;	/* strip "'s */
		if (!(v->v.str = _dup_tok(p)))
			return_NULL;
		p->te++;
		match(TOK_STRING);
		break;

	case TOK_STRING_ESCAPED:
		v->type = DM_CFG_STRING;

		p->tb++, p->te--;	/* strip "'s */
		if (!(str = _dup_tok(p)))
			return_NULL;
		dm_unescape_double_quotes(str);
		v->v.str = str;
		p->te++;
		match(TOK_STRING_ESCAPED);
		break;

	default:
		log_error("Parse error at byte %" PRIptrdiff_t " (line %d): expected a value",
			  p->tb - p->fb + 1, p->line);
		return NULL;
	}
	return v;
}

static int _match_aux(struct parser *p, int t)
{
	if (p->t != t)
		return 0;

	_get_token(p, t);
	return 1;
}

/*
 * tokeniser
 */
static void _get_token(struct parser *p, int tok_prev)
{
	int values_allowed = 0;

	const char *te;

	p->tb = p->te;
	_eat_space(p);
	if (p->tb == p->fe || !*p->tb) {
		p->t = TOK_EOF;
		return;
	}

	/* Should next token be interpreted as value instead of identifier? */
	if (tok_prev == TOK_EQ || tok_prev == TOK_ARRAY_B ||
	    tok_prev == TOK_COMMA)
		values_allowed = 1;

	p->t = TOK_INT;		/* fudge so the fall through for
				   floats works */

	te = p->te;
	switch (*te) {
	case SECTION_B_CHAR:
		p->t = TOK_SECTION_B;
		te++;
		break;

	case SECTION_E_CHAR:
		p->t = TOK_SECTION_E;
		te++;
		break;

	case '[':
		p->t = TOK_ARRAY_B;
		te++;
		break;

	case ']':
		p->t = TOK_ARRAY_E;
		te++;
		break;

	case ',':
		p->t = TOK_COMMA;
		te++;
		break;

	case '=':
		p->t = TOK_EQ;
		te++;
		break;

	case '"':
		p->t = TOK_STRING_ESCAPED;
		te++;
		while ((te != p->fe) && (*te) && (*te != '"')) {
			if ((*te == '\\') && (te + 1 != p->fe) &&
			    *(te + 1))
				te++;
			te++;
		}

		if ((te != p->fe) && (*te))
			te++;
		break;

	case '\'':
		p->t = TOK_STRING;
		te++;
		while ((te != p->fe) && (*te) && (*te != '\''))
			te++;

		if ((te != p->fe) && (*te))
			te++;
		break;

	case '.':
		p->t = TOK_FLOAT;
		/* Fall through */
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
	case '+':
	case '-':
		if (values_allowed) {
			while (++te != p->fe) {
				if (!isdigit((int) *te)) {
					if (*te == '.') {
						if (p->t != TOK_FLOAT) {
							p->t = TOK_FLOAT;
							continue;
						}
					}
					break;
				}
			}
			break;
		}
		/* fall through */

	default:
		p->t = TOK_IDENTIFIER;
		while ((te != p->fe) && (*te) && !isspace(*te) &&
		       (*te != '#') && (*te != '=') &&
		       (*te != SECTION_B_CHAR) &&
		       (*te != SECTION_E_CHAR))
			te++;
		break;
	}

	p->te = te;
}

static void _eat_space(struct parser *p)
{
	while (p->tb != p->fe) {
		if (*p->te == '#')
			while ((p->te != p->fe) && (*p->te != '\n') && (*p->te))
				++p->te;

		else if (!isspace(*p->te))
			break;

		while ((p->te != p->fe) && isspace(*p->te)) {
			if (*p->te == '\n')
				++p->line;
			++p->te;
		}

		p->tb = p->te;
	}
}

/*
 * memory management
 */
static struct dm_config_value *_create_value(struct dm_pool *mem)
{
	return dm_pool_zalloc(mem, sizeof(struct dm_config_value));
}

static struct dm_config_node *_create_node(struct dm_pool *mem)
{
	return dm_pool_zalloc(mem, sizeof(struct dm_config_node));
}

static char *_dup_tok(struct parser *p)
{
	size_t len = p->te - p->tb;
	char *str = dm_pool_alloc(p->mem, len + 1);
	if (!str) {
		log_error("Failed to duplicate token.");
		return 0;
	}
	memcpy(str, p->tb, len);
	str[len] = '\0';
	return str;
}

/*
 * Utility functions
 */

/*
 * node_lookup_fn is either:
 *   _find_config_node to perform a lookup starting from a given config_node 
 *   in a config_tree;
 * or
 *   _find_first_config_node to find the first config_node in a set of 
 *   cascaded trees.
 */
typedef const struct dm_config_node *node_lookup_fn(const void *start, const char *path);

static const struct dm_config_node *_find_config_node(const void *start,
						      const char *path)
{
	const char *e;
	const struct dm_config_node *cn = start;
	const struct dm_config_node *cn_found = NULL;

	while (cn) {
		/* trim any leading slashes */
		while (*path && (*path == sep))
			path++;

		/* find the end of this segment */
		for (e = path; *e && (*e != sep); e++) ;

		/* hunt for the node */
		cn_found = NULL;
		while (cn) {
			if (_tok_match(cn->key, path, e)) {
				/* Inefficient */
				if (!cn_found)
					cn_found = cn;
				else
					log_warn("WARNING: Ignoring duplicate"
						 " config node: %s ("
						 "seeking %s)", cn->key, path);
			}

			cn = cn->sib;
		}

		if (cn_found && *e)
			cn = cn_found->child;
		else
			break;	/* don't move into the last node */

		path = e;
	}

	return cn_found;
}

static const struct dm_config_node *_find_first_config_node(const void *start, const char *path)
{
	const struct dm_config_tree *cft = start;
	const struct dm_config_node *cn = NULL;

	while (cft) {
		if ((cn = _find_config_node(cft->root, path)))
			return cn;
		cft = cft->cascade;
	}

	return NULL;
}

static const char *_find_config_str(const void *start, node_lookup_fn find_fn,
				    const char *path, const char *fail, int allow_empty)
{
	const struct dm_config_node *n = find_fn(start, path);

	/* Empty strings are ignored if allow_empty is set */
	if (n && n->v) {
		if ((n->v->type == DM_CFG_STRING) &&
		    (allow_empty || (*n->v->v.str))) {
			log_very_verbose("Setting %s to %s", path, n->v->v.str);
			return n->v->v.str;
		}
		if ((n->v->type != DM_CFG_STRING) || (!allow_empty && fail))
			log_warn("WARNING: Ignoring unsupported value for %s.", path);
	}

	if (fail)
		log_very_verbose("%s not found in config: defaulting to %s",
				 path, fail);
	return fail;
}

const char *dm_config_find_str(const struct dm_config_node *cn,
			       const char *path, const char *fail)
{
	return _find_config_str(cn, _find_config_node, path, fail, 0);
}

const char *dm_config_find_str_allow_empty(const struct dm_config_node *cn,
					   const char *path, const char *fail)
{
	return _find_config_str(cn, _find_config_node, path, fail, 1);
}

static int64_t _find_config_int64(const void *start, node_lookup_fn find,
				  const char *path, int64_t fail)
{
	const struct dm_config_node *n = find(start, path);

	if (n && n->v && n->v->type == DM_CFG_INT) {
		log_very_verbose("Setting %s to %" PRId64, path, n->v->v.i);
		return n->v->v.i;
	}

	log_very_verbose("%s not found in config: defaulting to %" PRId64,
			 path, fail);
	return fail;
}

static float _find_config_float(const void *start, node_lookup_fn find,
				const char *path, float fail)
{
	const struct dm_config_node *n = find(start, path);

	if (n && n->v && n->v->type == DM_CFG_FLOAT) {
		log_very_verbose("Setting %s to %f", path, n->v->v.f);
		return n->v->v.f;
	}

	log_very_verbose("%s not found in config: defaulting to %f",
			 path, fail);

	return fail;

}

static int _str_in_array(const char *str, const char * const values[])
{
	int i;

	for (i = 0; values[i]; i++)
		if (!strcasecmp(str, values[i]))
			return 1;

	return 0;
}

static int _str_to_bool(const char *str, int fail)
{
	const char * const _true_values[]  = { "y", "yes", "on", "true", NULL };
	const char * const _false_values[] = { "n", "no", "off", "false", NULL };

	if (_str_in_array(str, _true_values))
		return 1;

	if (_str_in_array(str, _false_values))
		return 0;

	return fail;
}

static int _find_config_bool(const void *start, node_lookup_fn find,
			     const char *path, int fail)
{
	const struct dm_config_node *n = find(start, path);
	const struct dm_config_value *v;

	if (!n)
		return fail;

	v = n->v;

	switch (v->type) {
	case DM_CFG_INT:
		return v->v.i ? 1 : 0;

	case DM_CFG_STRING:
		return _str_to_bool(v->v.str, fail);
	default:
		;
	}

	return fail;
}

/***********************************
 * node-based lookup
 **/

struct dm_config_node *dm_config_find_node(const struct dm_config_node *cn,
					   const char *path)
{
	return (struct dm_config_node *) _find_config_node(cn, path);
}

int dm_config_find_int(const struct dm_config_node *cn, const char *path, int fail)
{
	/* FIXME Add log_error message on overflow */
	return (int) _find_config_int64(cn, _find_config_node, path, (int64_t) fail);
}

int64_t dm_config_find_int64(const struct dm_config_node *cn, const char *path, int64_t fail)
{
	return _find_config_int64(cn, _find_config_node, path, fail);
}

float dm_config_find_float(const struct dm_config_node *cn, const char *path,
			   float fail)
{
	return _find_config_float(cn, _find_config_node, path, fail);
}

int dm_config_find_bool(const struct dm_config_node *cn, const char *path, int fail)
{
	return _find_config_bool(cn, _find_config_node, path, fail);
}

/***********************************
 * tree-based lookup
 **/

const struct dm_config_node *dm_config_tree_find_node(const struct dm_config_tree *cft,
						      const char *path)
{
	return _find_first_config_node(cft, path);
}

const char *dm_config_tree_find_str(const struct dm_config_tree *cft, const char *path,
				    const char *fail)
{
	return _find_config_str(cft, _find_first_config_node, path, fail, 0);
}

const char *dm_config_tree_find_str_allow_empty(const struct dm_config_tree *cft, const char *path,
						const char *fail)
{
	return _find_config_str(cft, _find_first_config_node, path, fail, 1);
}

int dm_config_tree_find_int(const struct dm_config_tree *cft, const char *path, int fail)
{
	/* FIXME Add log_error message on overflow */
	return (int) _find_config_int64(cft, _find_first_config_node, path, (int64_t) fail);
}

int64_t dm_config_tree_find_int64(const struct dm_config_tree *cft, const char *path, int64_t fail)
{
	return _find_config_int64(cft, _find_first_config_node, path, fail);
}

float dm_config_tree_find_float(const struct dm_config_tree *cft, const char *path,
				float fail)
{
	return _find_config_float(cft, _find_first_config_node, path, fail);
}

int dm_config_tree_find_bool(const struct dm_config_tree *cft, const char *path, int fail)
{
	return _find_config_bool(cft, _find_first_config_node, path, fail);
}

/************************************/


int dm_config_get_uint32(const struct dm_config_node *cn, const char *path,
			 uint32_t *result)
{
	const struct dm_config_node *n;

	n = _find_config_node(cn, path);

	if (!n || !n->v || n->v->type != DM_CFG_INT)
		return 0;

	if (result)
		*result = n->v->v.i;
	return 1;
}

int dm_config_get_uint64(const struct dm_config_node *cn, const char *path,
			 uint64_t *result)
{
	const struct dm_config_node *n;

	n = _find_config_node(cn, path);

	if (!n || !n->v || n->v->type != DM_CFG_INT)
		return 0;

	if (result)
		*result = (uint64_t) n->v->v.i;
	return 1;
}

int dm_config_get_str(const struct dm_config_node *cn, const char *path,
		      const char **result)
{
	const struct dm_config_node *n;

	n = _find_config_node(cn, path);

	if (!n || !n->v || n->v->type != DM_CFG_STRING)
		return 0;

	if (result)
		*result = n->v->v.str;
	return 1;
}

int dm_config_get_list(const struct dm_config_node *cn, const char *path,
		       const struct dm_config_value **result)
{
	const struct dm_config_node *n;

	n = _find_config_node(cn, path);
	/* TODO when we represent single-item lists consistently, add a check
	 * for n->v->next != NULL */
	if (!n || !n->v)
		return 0;

	if (result)
		*result = n->v;
	return 1;
}

int dm_config_get_section(const struct dm_config_node *cn, const char *path,
			  const struct dm_config_node **result)
{
	const struct dm_config_node *n;

	n = _find_config_node(cn, path);
	if (!n || n->v)
		return 0;

	if (result)
		*result = n;
	return 1;
}

int dm_config_has_node(const struct dm_config_node *cn, const char *path)
{
	return _find_config_node(cn, path) ? 1 : 0;
}

/*
 * Convert a token type to the char it represents.
 */
static char _token_type_to_char(int type)
{
	switch (type) {
		case TOK_SECTION_B:
			return SECTION_B_CHAR;
		case TOK_SECTION_E:
			return SECTION_E_CHAR;
		default:
			return 0;
	}
}

/*
 * Returns:
 *  # of 'type' tokens in 'str'.
 */
static unsigned _count_tokens(const char *str, unsigned len, int type)
{
	char c;

	c = _token_type_to_char(type);

	return dm_count_chars(str, len, c);
}

const char *dm_config_parent_name(const struct dm_config_node *n)
{
	return (n->parent ? n->parent->key : "(root)");
}
/*
 * Heuristic function to make a quick guess as to whether a text
 * region probably contains a valid config "section".  (Useful for
 * scanning areas of the disk for old metadata.)
 * Config sections contain various tokens, may contain other sections
 * and strings, and are delimited by begin (type 'TOK_SECTION_B') and
 * end (type 'TOK_SECTION_E') tokens.  As a quick heuristic, we just
 * count the number of begin and end tokens, and see if they are
 * non-zero and the counts match.
 * Full validation of the section should be done with another function
 * (for example, read_config_fd).
 *
 * Returns:
 *  0 - probably is not a valid config section
 *  1 - probably _is_ a valid config section
 */
unsigned dm_config_maybe_section(const char *str, unsigned len)
{
	int begin_count;
	int end_count;

	begin_count = _count_tokens(str, len, TOK_SECTION_B);
	end_count = _count_tokens(str, len, TOK_SECTION_E);

	if (begin_count && end_count && (begin_count == end_count))
		return 1;
	else
		return 0;
}

__attribute__((nonnull(1, 2)))
static struct dm_config_value *_clone_config_value(struct dm_pool *mem,
						   const struct dm_config_value *v)
{
	struct dm_config_value *new_cv;

	if (!(new_cv = _create_value(mem))) {
		log_error("Failed to clone config value.");
		return NULL;
	}

	new_cv->type = v->type;
	if (v->type == DM_CFG_STRING) {
		if (!(new_cv->v.str = dm_pool_strdup(mem, v->v.str))) {
			log_error("Failed to clone config string value.");
			return NULL;
		}
	} else
		new_cv->v = v->v;

	if (v->next && !(new_cv->next = _clone_config_value(mem, v->next)))
		return_NULL;

	return new_cv;
}

struct dm_config_node *dm_config_clone_node_with_mem(struct dm_pool *mem, const struct dm_config_node *cn, int siblings)
{
	struct dm_config_node *new_cn;

	if (!cn) {
		log_error("Cannot clone NULL config node.");
		return NULL;
	}

	if (!(new_cn = _create_node(mem))) {
		log_error("Failed to clone config node.");
		return NULL;
	}

	if ((cn->key && !(new_cn->key = dm_pool_strdup(mem, cn->key)))) {
		log_error("Failed to clone config node key.");
		return NULL;
	}

	if ((cn->v && !(new_cn->v = _clone_config_value(mem, cn->v))) ||
	    (cn->child && !(new_cn->child = dm_config_clone_node_with_mem(mem, cn->child, 1))) ||
	    (siblings && cn->sib && !(new_cn->sib = dm_config_clone_node_with_mem(mem, cn->sib, siblings))))
		return_NULL; /* 'new_cn' released with mem pool */

	return new_cn;
}

struct dm_config_node *dm_config_clone_node(struct dm_config_tree *cft, const struct dm_config_node *node, int sib)
{
	return dm_config_clone_node_with_mem(cft->mem, node, sib);
}

struct dm_config_node *dm_config_create_node(struct dm_config_tree *cft, const char *key)
{
	struct dm_config_node *cn;

	if (!(cn = _create_node(cft->mem))) {
		log_error("Failed to create config node.");
		return NULL;
	}
	if (!(cn->key = dm_pool_strdup(cft->mem, key))) {
		log_error("Failed to create config node's key.");
		return NULL;
	}
	if (!(cn->v = _create_value(cft->mem))) {
		log_error("Failed to create config node's value.");
		return NULL;
	}
	cn->parent = NULL;
	cn->v->type = DM_CFG_INT;
	cn->v->v.i = 0;
	cn->v->next = NULL;
	return cn;
}

struct dm_config_value *dm_config_create_value(struct dm_config_tree *cft)
{
	return _create_value(cft->mem);
}

struct dm_pool *dm_config_memory(struct dm_config_tree *cft)
{
	return cft->mem;
}

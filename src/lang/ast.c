#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"

const char *type_str(type_t type)
{
#define TYPE(_type, _typestr) [_type] = _typestr,
	static const char *strs[] = {
		NODE_TYPE_TABLE
	};
#undef TYPE

	return strs[type];
}

const char *loc_str(loc_t loc)
{
	switch (loc) {
	case LOC_NOWHERE:
		return "nowhere";
	case LOC_VIRTUAL:
		return "virtual";
	case LOC_REG:
		return "reg";
	case LOC_STACK:
		return "stack";
	}

	return "UNKNOWN";
}

static void _indent(int *indent)
{
	int left = *indent;

	while(left--)
		fputc(' ', stderr);

	*indent += 3;
}

static int _unindent(node_t *n, void *indent)
{
	*((int *)indent) -= 3;
	return 0;
}

static void _fputs_escape(FILE *fp, const char *s)
{
	fputc('\"', fp);
	for (; *s; s++) {
		if (isprint(*s)) {
			fputc(*s, fp);
			continue;
		}

		fputc('\\', fp);

		switch (*s) {
		case '\n':
			fputc('n', fp);
			break;
		case '\r':
			fputc('r', fp);
			break;
		case '\t':
			fputc('t', fp);
			break;
		default:
			fprintf(fp, "x%2.2x", *s);
			break;
		}
	}
	fputc('\"', fp);
}

static int _node_ast_dump(node_t *n, void *indent)
{
	_indent((int *)indent);

	switch (n->type) {
	case TYPE_NONE:
	case TYPE_SCRIPT:
	case TYPE_RETURN:
	case TYPE_NOT:
	case TYPE_REC:
		fprintf(stderr, "<%s> ", type_str(n->type));
		break;
		
	case TYPE_PROBE:		
	case TYPE_CALL:
	case TYPE_ASSIGN:
	case TYPE_BINOP:
	case TYPE_MAP:
		fprintf(stderr, "%s ", n->string);
		break;

	case TYPE_INT:
		fprintf(stderr, "%" PRIx64 " ", n->integer);
		break;
		
	case TYPE_STR:
		_fputs_escape(stderr, n->string);
		break;
	}

	fprintf(stderr, "(type:%s/%s size:0x%zx loc:%s",
		type_str(n->type), type_str(n->dyn.type),
		n->dyn.size, loc_str(n->dyn.loc));

	switch (n->dyn.loc) {
	case LOC_NOWHERE:
	case LOC_VIRTUAL:
		break;
	case LOC_REG:
		fprintf(stderr, "/%d", n->dyn.reg);
		break;
	case LOC_STACK:
		fprintf(stderr, "/-0x%zx", -n->dyn.addr);
		break;
	}

	fputs(")\n", stderr);
	return 0;
}

void node_ast_dump(node_t *n)
{
	int indent = 3;

	fprintf(stderr, "ast:\n");
	node_walk(n, _node_ast_dump, _unindent, &indent);
}


static node_t *node_get_parent_of_type(type_t type, node_t *n)
{
	for (; n && n->type != type; n = n->parent);
	return n;
}

node_t *node_get_stmt(node_t *n) {
	for (; n; n = n->parent) {
		if (n->parent->type == TYPE_PROBE)
			return n;
	}

	return NULL;
}

node_t *node_get_probe(node_t *n)
{
	return node_get_parent_of_type(TYPE_PROBE, n);
}

pvdr_t *node_get_pvdr(node_t *n)
{
	node_t *probe = node_get_probe(n);

	return probe ? probe->probe.pvdr : NULL;
}

node_t *node_get_script(node_t *n)
{
	return node_get_parent_of_type(TYPE_SCRIPT, n);
}


int node_map_get_fd(node_t *map)
{
	node_t *script = node_get_script(map);
	mdyn_t *mdyn;

	for (mdyn = script->script.mdyns; mdyn; mdyn = mdyn->next) {
		if (!strcmp(mdyn->map->string, map->string))
			return mdyn->mapfd;
	}

	return -ENOENT;
}

int node_stmt_reg_get(node_t *stmt)
{
	int reg;

	for (reg = BPF_REG_6; reg < BPF_REG_9; reg++) {
		if (stmt->dyn.free_regs & (1 << reg)) {
			stmt->dyn.free_regs &= ~(1 << reg);
			return reg;
		}
	}

	return -1;
}

ssize_t node_probe_stack_get(node_t *probe, size_t size)
{
	probe->probe.sp -= size;
	return probe->probe.sp;
}

static inline node_t *node_new(type_t type) {
	node_t *n = calloc(1, sizeof(*n));

	assert(n);
	n->type = type;
	return n;
}

node_t *node_str_new(char *val)
{
	node_t *n = node_new(TYPE_STR);

	n->string = val;
	return n;
}

node_t *node_int_new(int64_t val)
{
	node_t *n = node_new(TYPE_INT);

	n->integer = val;
	return n;
}

node_t *node_rec_new(node_t *vargs)
{
	node_t *c, *n = node_new(TYPE_REC);

	n->rec.vargs = vargs;

	node_foreach(c, vargs)
		c->parent = n;
	return n;
}

node_t *node_map_new(char *name, node_t *rec)
{
	node_t *n = node_new(TYPE_MAP);

	n->string = name;
	n->map.rec = rec;

	rec->parent = n;
	return n;
}

node_t *node_not_new(node_t *expr)
{
	node_t *n = node_new(TYPE_NOT);

	n->not = expr;

	expr->parent = n;
	return n;
}

node_t *node_return_new(node_t *expr)
{
	node_t *n = node_new(TYPE_RETURN);

	n->ret = expr;

	expr->parent = n;
	return n;
}

static alu_op_t alu_op_from_str(const char *opstr)
{
	switch (opstr[0]) {
	case '+':
		return ALU_OP_ADD;
	case '-':
		return ALU_OP_SUB;
	case '*':
		return ALU_OP_MUL;
	case '/':
		return ALU_OP_DIV;
	case '|':
		return ALU_OP_OR;
	case '&':
		return ALU_OP_AND;
	case '<':
		return ALU_OP_LSH;
	case '>':
		return ALU_OP_RSH;
	case '%':
		return ALU_OP_MOD;
	case '^':
		return ALU_OP_XOR;
	case '=':
		return ALU_OP_MOV;
	default:
		assert(0);
		return 0;
	}
}

node_t *node_binop_new(node_t *left, char *opstr, node_t *right)
{
	node_t *n = node_new(TYPE_BINOP);

	n->string = opstr;
	n->binop.op    = alu_op_from_str(opstr);
	n->binop.left  = left;
	n->binop.right = right;
	return n;
}

node_t *node_assign_new(node_t *lval, char *opstr, node_t *expr)
{
	node_t *n = node_new(TYPE_ASSIGN);

	n->string = opstr;
	n->assign.op   = alu_op_from_str(opstr);
	n->assign.lval = lval;
	n->assign.expr = expr;

	lval->parent = n;
	expr->parent = n;
	return n;
}

node_t *node_call_new(char *func, node_t *vargs)
{
	node_t *c, *n = node_new(TYPE_CALL);

	n->string = func;
	n->call.vargs = vargs;

	node_foreach(c, vargs)
		c->parent = n;
	return n;
}

node_t *node_probe_new(char *pspec, node_t *pred,
			     node_t *stmts)
{
	node_t *c, *n = node_new(TYPE_PROBE);

	n->string = pspec;
	n->probe.pred   = pred;
	n->probe.stmts  = stmts;

	if (pred)
		pred->parent = n;

	node_foreach(c, stmts)
		c->parent = n;
	return n;
}

node_t *node_script_new(node_t *probes)
{
	node_t *c, *n = node_new(TYPE_SCRIPT);

	n->script.probes = probes;

	node_foreach(c, probes)
		c->parent = n;
	return n;
}


static int _node_free(node_t *n, void *ctx)
{
	switch (n->type) {
	case TYPE_PROBE:
	case TYPE_CALL:
	case TYPE_ASSIGN:
	case TYPE_BINOP:
	case TYPE_MAP:
	case TYPE_STR:
		free(n->string);
		break;

	default:
		break;
	}

	free(n);
	return 0;
}

void node_free(node_t *n)
{
	node_walk(n, NULL, _node_free, NULL);
}

static int _node_walk_list(node_t *head,
			 int (*pre) (node_t *n, void *ctx),
			 int (*post)(node_t *n, void *ctx), void *ctx)
{
	node_t *elem, *next = head;
	int err = 0;
	
	for (elem = next; !err && elem;) {
		next = elem->next;
		err = node_walk(elem, pre, post, ctx);
		elem = next;
	}

	return err;
}


int node_walk(node_t *n,
	    int (*pre) (node_t *n, void *ctx),
	    int (*post)(node_t *n, void *ctx), void *ctx)
{
#define do_list(_head) err = _node_walk_list(_head, pre, post, ctx); if (err) return err
#define do_walk(_node) err =       node_walk(_node, pre, post, ctx); if (err) return err
	int err = 0;

	err = pre ? pre(n, ctx) : 0;
	if (err)
		return err;
	
	switch (n->type) {
	case TYPE_SCRIPT:
		do_list(n->script.probes);
		break;

	case TYPE_PROBE:
		if (n->probe.pred)
			do_walk(n->probe.pred);
		do_list(n->probe.stmts);
		break;

	case TYPE_CALL:
		do_list(n->call.vargs);
		break;

	case TYPE_ASSIGN:
		do_walk(n->assign.lval);
		do_walk(n->assign.expr);
		break;

	case TYPE_RETURN:
		do_walk(n->ret);
		break;

	case TYPE_BINOP:
		do_walk(n->binop.left);
		do_walk(n->binop.right);
		break;

	case TYPE_NOT:
		do_walk(n->not);
		break;

	case TYPE_MAP:
		do_walk(n->map.rec);
		break;

	case TYPE_REC:
		do_list(n->rec.vargs);
		break;

	case TYPE_NONE:
		return -1;

	default:
		break;
	}

	return post ? post(n, ctx) : 0;
}

		
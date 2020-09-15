#include <u.h>
#include <libc.h>

#include "list.h"
#include "util.h"

List*
listalloc(void)
{
	List *n;

	n = emalloc(sizeof(List));
	n->tag = Listlead;
	n->link = n;
	n->tail = n;
	return n;
}

List*
listlink(List *p, List *n)
{
	n->link = p->link;
	p->link = n;
	n->tail = p;
	n->link->tail = n;
	return n;
}

List*
listunlink(List *p)
{
	p->link->tail = p->tail;
	p->tail->link = p->link;
	return p;
}

int
listisempty(List *p)
{
	return p->link == p;
}

int
listislead(List *p)
{
	return p->tag == Listlead;
}

int
listisfirst(List *p)
{
	return p->tail->tag == Listlead;
}

int
listislast(List *p)
{
	return p->link->tag == Listlead;
}

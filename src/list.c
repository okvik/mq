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
listend(List *p)
{
	return p->link->tag == Listlead;
}

int
listempty(List *p)
{
	return p->link == p;
}

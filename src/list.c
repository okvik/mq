#include <u.h>
#include <libc.h>

#include "list.h"
#include "util.h"

Listelem*
listinit(Listelem *l)
{
	l->front = l->back = l;
	return l;
}

Listelem*
listlink(Listelem *list, Listelem *n)
{
	n->front = list->front;
	n->back = list;
	((Listelem*)list->front)->back = n;
	list->front = n;
	return n;
}

Listelem*
listunlink(Listelem *n)
{
	((Listelem*)n->front)->back = n->back;
	((Listelem*)n->back)->front = n->front;
	return n;
}

int
listisempty(Listelem *list)
{
	return list->front == list;
}

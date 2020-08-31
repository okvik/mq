#include <u.h>
#include <libc.h>

#include "util.h"

int DEBUG = 0;

void
dprint(int force, char *fmt, ...)
{
	va_list v;

	va_start(v, fmt);
	if(DEBUG == 0 && force == 0)
		return;
	fprint(2, "debug: ");
	vfprint(2, fmt, v);
	va_end(v);
}

void*
emalloc(ulong sz)
{
	void *v = malloc(sz);
	if(v == nil)
		sysfatal("malloc: %r");
	setmalloctag(v, getcallerpc(&sz));
	memset(v, 0, sz);
	return v;
}

char*
estrdup(char *s)
{
	if((s = strdup(s)) == nil)
		sysfatal("strdup: %r");
	setmalloctag(s, getcallerpc(&s));
	return s;
}

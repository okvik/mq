#include <u.h>
#include <libc.h>

#include "util.h"

typedef struct Stream Stream;

struct Stream {
	char *name;
	int fd;
};

char buf[8192];

void
usage(void)
{
	fprint(2, "usage: %s group stream ...\n", argv0);
	exits("usage");
}

long
rdwr(int fd0, int fd1)
{
	long n;
	
	if((n = read(fd0, buf, sizeof buf)) == -1)
		sysfatal("read: %r");
	if(n == 0)
		return 0;
	if(write(fd1, buf, n) != n)
		sysfatal("write: %r");
	return n;
}

void
main(int argc, char *argv[])
{
	int orderfd, n, ns, i;
	char name[512+1];
	Stream *streams, *s;

	ARGBEGIN{
	default: usage();
	}ARGEND;
	if(argc < 2) usage();

	if(chdir(argv[0]) == -1)
		sysfatal("chdir: %r");
	argv++, argc--;
	ns = argc;
	streams = s = emalloc(ns*sizeof(Stream));
	for(int i = 0; i < ns; i++, s++){
		s->name = argv[i];
		if((s->fd = open(argv[i], OREAD)) == -1)
			sysfatal("open: %r");
	}
	if((orderfd = open("order", OREAD)) == -1)
		sysfatal("open: %r");
	for(;;){
		if((n = read(orderfd, name, sizeof(name)-1)) == 0)
			break;
		if(n == -1)
			sysfatal("read: %r");
		name[n] = 0;
		for(i = 0, s = streams; i < ns; i++, s++){
			if(strcmp(s->name, name) != 0 || s->fd == -1)
				continue;
			if(rdwr(s->fd, 1) == 0){
				close(s->fd);
				s->fd = -1;
			}
			break;
		}
	}
	exits(nil);
}

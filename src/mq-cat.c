#include <u.h>
#include <libc.h>

#include "util.h"

typedef struct Stream Stream;

struct Stream {
	char *name;
	int fd;
};

int nstreams;
Stream *streams;

char buf[8192];

void
usage(void)
{
	fprint(2, "usage: %s mq\n", argv0);
	exits("usage");
}

int
eopen(char *s, int m)
{
	int fd;

	if((fd = open(s, m)) < 0)
		sysfatal("open: %r");
	return fd;
}

int
openmq(char *name)
{
	int mqfd, n, ismq;
	Dir *dirs, *d;
	Stream *s;

	mqfd = eopen(name, OREAD);
	if((n = dirreadall(mqfd, &dirs)) == -1)
		sysfatal("dirread: %r");
	if(n == 0)
		return -1;
	close(mqfd);

	ismq = 0;
	nstreams = n - 2;
	streams = s = emalloc(nstreams*sizeof(Stream));
	for(d = dirs; n--; d++){
		if(strncmp(d->name, "ctl", 3) == 0
		|| strncmp(d->name, "order", 5) == 0){
			ismq++;
			continue;
		}
		s->name = estrdup(d->name);
		s->fd = eopen(d->name, OREAD);
		s++;
	}
	free(dirs);
	if(ismq != 2)
		return -1;
	return eopen("order", OREAD);
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
	int orderfd, n, i;
	char name[512+1];
	Stream *s;

	ARGBEGIN{
	default: usage();
	}ARGEND;
	if(argc != 1) usage();

	if(chdir(argv[0]) == -1)
		sysfatal("chdir: %r");
	if((orderfd = openmq(".")) == -1)
		sysfatal("not mq");
	for(;;){
		if((n = read(orderfd, name, sizeof(name)-1)) == 0)
			break;
		name[n] = 0;
		for(i = 0, s = streams; i < nstreams; i++, s++){
			if(strcmp(s->name, name) != 0 || s->fd == -1)
				continue;
			if(rdwr(s->fd, 1) == 0)
					s->fd = -1;
			break;
		}
	}
	exits(nil);
}

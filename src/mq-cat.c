#include <u.h>
#include <libc.h>

#include "util.h"

typedef struct Pipe Pipe;

struct Pipe {
	char *name;
	int fd;
};

int npipes;
Pipe *pipes;

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
	Pipe *p;

	mqfd = eopen(name, OREAD);
	if((n = dirreadall(mqfd, &dirs)) == -1)
		sysfatal("dirread: %r");
	if(n == 0)
		return -1;
	close(mqfd);

	ismq = 0;
	npipes = n - 2;
	pipes = p = emalloc(npipes*sizeof(Pipe));
	for(d = dirs; n--; d++){
		if(strncmp(d->name, "ctl", 3) == 0
		|| strncmp(d->name, "order", 5) == 0){
			ismq++;
			continue;
		}
		p->name = estrdup(d->name);
		p->fd = eopen(d->name, OREAD);
		p++;
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
	Pipe *p;

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
		buf[n] = 0;
		for(i = 0, p = pipes; i < npipes; i++, p++)
			if(strcmp(p->name, name) == 0)
			if(p->fd != -1){
				if(rdwr(p->fd, 1) == 0)
					p->fd = -1;
				break;
			}
	}
	exits(nil);
}

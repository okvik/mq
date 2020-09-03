#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#include "list.h"
#include "util.h"

typedef struct Mq Mq;
typedef struct Stream Stream;
typedef struct Client Client;
typedef struct Read Read;
typedef struct Write Write;

struct Mq {
	Stream *group;
	Stream *order;

	/* configuration */
	int replay;
};

struct Stream {
	List;

	Mq *mq; /* parent */

	Write *queue; /* stored messages */
	Read *reads; /* readers queue */
};

struct Client {
	Write *cursor; /* reader position */
};

struct Read {
	List;

	Req *r;
};

struct Write {
	List;

	/* Twrite.ifcall */
	vlong offset; /* ignored */
	uint count;
	uchar *data;
};

enum {
	Qroot,
		Qmq,
			Qstream,
			Qorder,
			Qctl,
};
void
filesettype(File *f, ushort type)
{
	/*
	 * Use four most-significant bits to store the type.
	 * This depends on the 9pfile(2) library generating
	 * simple incremental qid paths.
	*/
	f->qid.path |= (uvlong)(type&0xF)<<60;
}

ushort
filetype(File *f)
{
	return (f->qid.path>>60)&0xF;
}

File*
mqcreate(File *parent, char *name, char *uid, ulong perm)
{
	Stream *streamalloc(Mq*);
	void *streamclose(Stream*);
	File *d, *ctl, *order;
	Mq *mq;

	mq = emalloc(sizeof(Mq));
	mq->group = (Stream*)listalloc();
	mq->order = (Stream*)streamalloc(mq);
	mq->replay = 0;

	ctl = order = nil;
	if((d = createfile(parent, name, uid, perm, mq)) == nil)
		goto err;
	filesettype(d, Qmq);

	if((ctl = createfile(d, "ctl", nil, 0220, mq)) == nil)
		goto err;
	filesettype(ctl, Qctl);
	closefile(ctl);

	if((order = createfile(d, "order", nil, 0444, mq->order)) == nil)
		goto err;
	filesettype(order, Qorder);
	closefile(order);

	return d;
err:
	free(mq->group);
	streamclose(mq->order);
	if(d) closefile(d);
	if(ctl) closefile(ctl);
	if(order) closefile(order);
	return nil;
}

void
mqclose(File *f)
{
	Mq *mq = f->aux;

	free(mq);
}

Stream*
streamalloc(Mq *mq)
{
	Stream *s;
	
	s = emalloc(sizeof(Stream));
	s->mq = mq;
	s->queue = (Write*)listalloc();
	s->reads = (Read*)listalloc();
	return s;
}

void
streamclose(Stream *s)
{
	Read *r;
	Write *w;

	listunlink(s);
	if(s->reads)
	foreach(Read*, s->reads){
		/* eof these? */
		r = ptr;
		ptr = (Read*)r->tail;
		listunlink(r);
		free(r);
	}
	free(s->reads);
	if(s->queue)
	foreach(Write*, s->queue){
		w = ptr;
		ptr = (Write*)w->tail;
		listunlink(w);
		free(w);
	}
	free(s->queue);
	free(s);
}

File*
streamcreate(File *parent, char *name, char *uid, ulong perm)
{
	File *f;
	Mq *mq;
	Stream *s;

	mq = parent->aux;
	s = streamalloc(mq);
	if((f = createfile(parent, name, uid, perm, s)) == nil){
		streamclose(s);
		return nil;
	}
	listlink(mq->group, s);
	filesettype(f, Qstream);
	return f;
}

void
respondread(Req *r, Write *w)
{
	r->ofcall.count = w->count;
	memmove(r->ofcall.data, w->data, w->count);
	respond(r, nil);
}

void
streamread(Req *r)
{
	File *f = r->fid->file;
	Stream *s = f->aux;
	Client *c = r->fid->aux;
	Read *rd;

	/* Delay the response if the queue is empty
	 * or if we've already caught up. */
	if(listempty(s->queue) || listend(c->cursor)){
		rd = emalloc(sizeof(Read));
		rd->r = r;
		listlink(s->reads, rd);
		return;
	}
	c->cursor = (Write*)c->cursor->link;
	respondread(r, c->cursor);
}

Write*
writealloc(long n)
{
	Write *w;
	
	w = emalloc(sizeof(Write)+n);
	w->data = (uchar*)&w[1];
	return w;
}

void
pipewrite(Req *r)
{
	File *f = r->fid->file;
	Stream *s = f->aux;
	Mq *mq = s->mq;
	Write *w, *o;
	long n;

	/* Commit to queue */
	w = writealloc(r->ifcall.count);
	w->count = r->ifcall.count;
	w->offset = r->ifcall.offset;
	memmove(w->data, r->ifcall.data, w->count);
	listlink(s->queue->tail, w);

	/* Commit to order */
	n = strlen(f->name)+1;
	o = writealloc(n);
	o->offset = 0;
	o->count = n;
	memmove(o->data, f->name, n);
	listlink(mq->order->queue->tail, o);

	/* Kick the blocked stream readers */
	foreach(Read*, s->reads){
		Client *c = ptr->r->fid->aux;

		respondread(ptr->r, w);
		c->cursor = w;
		listunlink(ptr);
	}

	/* Kick the blocked order readers */
	foreach(Read*, mq->order->reads){
		Client *c = ptr->r->fid->aux;

		respondread(ptr->r, o);
		c->cursor = o;
		listunlink(ptr);
	}

	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

enum {
	Cmdreplay,
	Cmddebug, Cmddebug9p,
};
Cmdtab mqcmd[] = {
	/* replay on|off*/
	{Cmdreplay, "replay", 2},

	/* debug on|off */
	{Cmddebug, "debug", 2},
	/* debug9p on|off */
	{Cmddebug9p, "debug9p", 2},
};

void
ctlwrite(Req *r)
{
	File *f = r->fid->file;
	Mq *mq = f->aux;
	char *e = nil;
	Cmdbuf *cmd;
	Cmdtab *t;

	cmd = parsecmd(r->ifcall.data, r->ifcall.count);
	t = lookupcmd(cmd, mqcmd, nelem(mqcmd));
	if(t == nil){
		free(cmd);
		respondcmderror(r, cmd, "%r");
		return;
	}
	switch(t->index){
	case Cmdreplay: {
		if(strncmp(cmd->f[1], "on", 2) == 0)
			mq->replay = 1;
		else
		if(strncmp(cmd->f[1], "off", 3) == 0)
			mq->replay = 0;
		else
			e = "usage: replay on|off";
		break;
	}
	case Cmddebug: {
		if(strncmp(cmd->f[1], "on", 2) == 0)
			DEBUG = 1;
		else
		if(strncmp(cmd->f[1], "off", 3) == 0)
			DEBUG = 0;
		else
			e = "usage: debug on|off";
		break;
	}
	case Cmddebug9p: {
		if(strncmp(cmd->f[1], "on", 2) == 0)
			chatty9p = 1;
		else
		if(strncmp(cmd->f[1], "off", 3) == 0)
			chatty9p = 0;
		else
			e = "usage: debug9p on|off";
		break;
	}}
	free(cmd);
	respond(r, e);
}

void
xcreate(Req *r)
{
	char *name = r->ifcall.name;
	char *uid = r->fid->uid;
	ulong perm = r->ifcall.perm;
	File *parent = r->fid->file;
	File *f = nil;

	switch(filetype(parent)){
	case Qroot:
		if(!(perm&DMDIR)){
			respond(r, "forbidden");
			return;
		}
		/* fallthrough */
	case Qmq:
		if(perm&DMDIR)
			f = mqcreate(parent, name, uid, perm);
		else
			f = streamcreate(parent, name, uid, perm);
		break;
	}
	if(f == nil)
		responderror(r);
	else
		respond(r, nil);
}

void
xopen(Req *r)
{
	File *f = r->fid->file;

	switch(filetype(f)){
	case Qstream:
	case Qorder: {
		Stream *s = f->aux;
		Client *c;

		c = r->fid->aux = emalloc(sizeof(Client));
		if(s->mq->replay)
			c->cursor = (Write*)s->queue;
		else
			c->cursor = (Write*)s->queue->tail;
		break;
	}}
	respond(r, nil);
}

void
xwrite(Req *r)
{
	File *f = r->fid->file;

	switch(filetype(f)){
	case Qstream:
		pipewrite(r);
		break;
	case Qctl:
		ctlwrite(r);
		break;
	default:
		respond(r, "forbidden");
		return;
	}
}

void
xread(Req *r)
{
	File *f = r->fid->file;

	switch(filetype(f)){
	case Qstream:
	case Qorder:
		streamread(r);
		break;
	default:
		respond(r, "forbidden");
	}
}

void
xflush(Req *r)
{
	Req *old = r->oldreq;
	File *f = old->fid->file;

	switch(filetype(f)){
	case Qstream:
	case Qorder: {
		Stream *s = f->aux;

		if(old->ifcall.type != Tread)
			break;
		foreach(Read*, s->reads){
			if(ptr->r == old){
				free(listunlink(ptr));
				break;
			}
		}
		respond(old, "interrupted");
	}}
	respond(r, nil);
}

void
xdestroyfid(Fid *fid)
{
	Client *f = fid->aux;

	free(f);
}

void
xdestroyfile(File *f)
{
	switch(filetype(f)){
	case Qmq:
		mqclose(f);
		break;
	case Qstream:
		streamclose(f->aux);
		break;
	}
	return;
}

Srv fs = {
	.create = xcreate,
	.open = xopen,
	.write = xwrite,
	.read = xread,
	.flush = xflush,
	.destroyfid = xdestroyfid,
};

void
usage(void)
{
	fprint(2, "usage: %s [-D] [-s name] [-m mtpt]\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	char *name = nil;
	char *mtpt = nil;

	ARGBEGIN{
	case 's': name = EARGF(usage()); break;
	case 'm': mtpt = EARGF(usage()); break;
	case 'D': chatty9p++; break;
	default: usage();
	}ARGEND;

	fs.tree = alloctree(nil, nil, DMDIR|0777, xdestroyfile);
	filesettype(fs.tree->root, Qroot);

	if(name || mtpt){
		postmountsrv(&fs, name, mtpt, MREPL|MCREATE);
		exits(nil);
	}
	fs.infd = fs.outfd = 0;
	dup(2, 1);
	srv(&fs);
	exits(nil);
}

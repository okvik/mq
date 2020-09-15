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
	enum {Replayoff, Replaylast, Replayall} replay;
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
	/* Dirty trick to help clients tell us from most others. */
	Qroot = 0xA,
		Qmq = 0x1,
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
	mq->replay = Replayoff;

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
streamwrite(Req *r)
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
	/* replay off|last|all */
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
		if(strncmp(cmd->f[1], "off", 3) == 0)
			mq->replay = Replayoff;
		else
		if(strncmp(cmd->f[1], "last", 4) == 0)
			mq->replay = Replaylast;
		else
		if(strncmp(cmd->f[1], "all", 3) == 0)
			mq->replay = Replayall;
		else
			e = "usage: replay off|last|all";
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
		switch(s->mq->replay){
		case Replayoff:
			c->cursor = (Write*)s->queue->tail; break;
		case Replaylast:
			c->cursor = (Write*)s->queue->tail->tail; break;
		case Replayall:
			c->cursor = (Write*)s->queue; break;
		}
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
		streamwrite(r);
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
xwstat(Req *r)
{
	File *w, *f = r->fid->file;
	char *uid = r->fid->uid;

	/* To change name, must have write permission in parent. */
	if(r->d.name[0] != '\0' && strcmp(r->d.name, f->name) != 0){
		if((w = f->parent) == nil)
			goto perm;
		incref(w);
	 	if(!hasperm(w, uid, AWRITE)){
			closefile(w);
			goto perm;
		}
		if((w = walkfile(w, r->d.name)) != nil){
			closefile(w);
			respond(r, "file already exists");
			return;
		}
	}

	/* To change group, must be owner and member of new group,
	 * or leader of current group and leader of new group.
	 * Second case cannot happen, but we check anyway. */
	while(r->d.gid[0] != '\0' && strcmp(f->gid, r->d.gid) != 0){
		if(strcmp(uid, f->uid) == 0)
			break;
		if(strcmp(uid, f->gid) == 0)
		if(strcmp(uid, r->d.gid) == 0)
			break;
		respond(r, "not owner");
		return;
	}

	/* To change mode, must be owner or group leader.
	 * Because of lack of users file, leader=>group itself. */
	if(r->d.mode != ~0 && f->mode != r->d.mode){
		if(strcmp(uid, f->uid) != 0)
		if(strcmp(uid, f->gid) != 0){
			respond(r, "not owner");
			return;
		}
	}

	if(r->d.name[0] != '\0'){
		free(f->name);
		f->name = estrdup(r->d.name);
	}
	if(r->d.uid[0] != '\0'){
		free(f->uid);
		f->uid = estrdup(r->d.uid);
	}
	if(r->d.gid[0] != '\0'){
		free(f->gid);
		f->gid = estrdup(r->d.gid);
	}
	if(r->d.mode != ~0){
		f->mode = r->d.mode;
		f->qid.type = f->mode >> 24;
	}

	respond(r, nil);
	return;
perm:
	respond(r, "permission denied");
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
	.wstat = xwstat,
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

#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#include "list.h"
#include "util.h"

typedef struct Group Group;
typedef struct Stream Stream;
typedef struct Client Client;
typedef struct Read Read;
typedef struct Write Write;

struct Group {
	Stream *streams;
	Stream *order;

	enum {Message, Coalesce} mode;
	enum {Replayoff, Replaylast, Replayall} replay;
};

struct Stream {
	List;

	Group *parent;
	Write *wqueue;
	Read *rqueue;
};

struct Client {
	Write *cursor;
	vlong offset;
};

struct Read {
	List;

	Req *r;
};

struct Write {
	List;

	/* Twrite.ifcall */
	vlong offset;
	uint count;
	uchar *data;
};

enum {
	/* Dirty trick to help clients tell our
	 * root from most others, see pin(1). */
	Qroot = 0xA,
	Qgroup = 0x1,
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
	f->qid.path &= ~(uvlong)0xF<<60;
	f->qid.path |= (uvlong)(type&0xF)<<60;
}

ushort
filetype(File *f)
{
	return (f->qid.path>>60)&0xF;
}

File*
groupcreate(File *parent, char *name, char *uid, ulong perm)
{
	Stream *streamalloc(Group*);
	void *streamclose(Stream*);
	File *d, *ctl, *order;
	Group *group;

	group = emalloc(sizeof(Group));
	group->streams = (Stream*)listalloc();
	group->order = (Stream*)streamalloc(group);
	group->mode = Message;
	group->replay = Replayoff;

	ctl = order = nil;
	if(strcmp(name, "/") == 0){
		d = parent;
		d->aux = group;
	}
	else
		d = createfile(parent, name, uid, perm, group);
	if(d == nil)
		goto err;
	filesettype(d, Qgroup);

	if((ctl = createfile(d, "ctl", nil, 0664, group)) == nil)
		goto err;
	filesettype(ctl, Qctl);
	closefile(ctl);

	if((order = createfile(d, "order", nil, 0444, group->order)) == nil)
		goto err;
	filesettype(order, Qorder);
	closefile(order);

	return d;
err:
	free(group->streams);
	streamclose(group->order);
	if(d) closefile(d);
	if(ctl) closefile(ctl);
	if(order) closefile(order);
	return nil;
}

void
groupclose(File *f)
{
	Group *group = f->aux;

	free(group);
}

Stream*
streamalloc(Group *group)
{
	Stream *s;
	
	s = emalloc(sizeof(Stream));
	s->parent = group;
	s->wqueue = (Write*)listalloc();
	s->rqueue = (Read*)listalloc();
	return s;
}

void
streamclose(Stream *s)
{
	Read *r;
	Write *w;

	listunlink(s);
	if(s->rqueue)
	foreach(Read*, s->rqueue){
		/* eof these? */
		r = ptr;
		ptr = (Read*)r->tail;
		listunlink(r);
		free(r);
	}
	free(s->rqueue);
	if(s->wqueue)
	foreach(Write*, s->wqueue){
		w = ptr;
		ptr = (Write*)w->tail;
		listunlink(w);
		free(w);
	}
	free(s->wqueue);
	free(s);
}

File*
streamcreate(File *parent, char *name, char *uid, ulong perm)
{
	File *f;
	Group *group;
	Stream *s;

	group = parent->aux;
	s = streamalloc(group);
	if((f = createfile(parent, name, uid, perm, s)) == nil){
		streamclose(s);
		return nil;
	}
	listlink(group->streams, s);
	filesettype(f, Qstream);
	return f;
}

void
streamopen(Stream *s, Req *r)
{
	Client *c;
	
	c = r->fid->aux = emalloc(sizeof(Client));
	switch(s->parent->replay){
	case Replayoff:
		c->cursor = (Write*)s->wqueue->tail; break;
	case Replaylast:
		c->cursor = (Write*)s->wqueue->tail->tail; break;
	case Replayall:
		c->cursor = (Write*)s->wqueue; break;
	}
}


void
respondmessage(Req *r)
{
	int n;
	Client *c = r->fid->aux;
	Write *w = c->cursor;
	
	n = w->count;
	if(n > r->ifcall.count)
		n = r->ifcall.count;
	r->ofcall.count = n;
	memmove(r->ofcall.data, w->data, n);
	respond(r, nil);
}

void
respondcoalesce(Req *r)
{
	Client *c = r->fid->aux;
	Write *w;
	/* request size and offset, chunk size and offset, total read */
	vlong rn, ro, n, o, t;

	ro = 0; o = 0; t = 0;
	rn = r->ifcall.count;
	w = c->cursor;
	foreach(Write*, w){
		w = ptr;
		for(o = c->offset; n = w->count - o, n > 0; o += n){
			if(t == rn)
				goto done;
			if(n > rn - ro)
				n = rn - ro;
			memmove(r->ofcall.data+ro, w->data+o, n);
			ro += n; t += n;
		}
		c->offset = 0;
	}
done:
	c->cursor = w;
	c->offset = o;
	r->ofcall.count = t;
	respond(r, nil);
}

void
streamread(Req *r)
{
	File *f = r->fid->file;
	Stream *s = f->aux;
	Client *c = r->fid->aux;
	Read *rd;

	/* Delay the response if the wqueue is empty
	 * or if we've already caught up, respond otherwise. */
	switch(s->parent->mode){
	case Message:
		if(listisempty(s->wqueue) || listislast(c->cursor))
			break;
		c->cursor = (Write*)c->cursor->link;
		respondmessage(r);
		return;
	case Coalesce:
		if(listisempty(s->wqueue)
		|| (listislast(c->cursor) && c->offset == c->cursor->count))
			break;
		respondcoalesce(r);
		return;
	}
	rd = emalloc(sizeof(Read));
	rd->r = r;
	listlink(s->rqueue, rd);
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
	Group *group = s->parent;
	Write *w, *o;
	long n;

	/* Commit to wqueue */
	w = writealloc(r->ifcall.count);
	w->count = r->ifcall.count;
	w->offset = r->ifcall.offset;
	memmove(w->data, r->ifcall.data, w->count);
	listlink(s->wqueue->tail, w);

	/* Commit to order */
	n = strlen(f->name)+1;
	o = writealloc(n);
	o->offset = 0;
	o->count = n;
	memmove(o->data, f->name, n);
	listlink(group->order->wqueue->tail, o);

	/* Kick the blocked stream readers */
	foreach(Read*, s->rqueue){
		Client *c = ptr->r->fid->aux;

		c->cursor = w;
		c->offset = 0;
		switch(group->mode){
		case Message:
			respondmessage(ptr->r); break;
		case Coalesce:
			respondcoalesce(ptr->r); break;
		}
		ptr = (Read*)ptr->tail;
		free(listunlink(ptr->link));
	}

	/* Kick the blocked order readers */
	foreach(Read*, group->order->rqueue){
		Client *c = ptr->r->fid->aux;

		c->cursor = o;
		respondmessage(ptr->r);
		ptr = (Read*)ptr->tail;
		free(listunlink(ptr->link));
	}

	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

void
ctlread(Req *r)
{
	File *f = r->fid->file;
	Group *group = f->aux;
	char buf[256];

	char *mode2str[] = {
		[Message] "message",
		[Coalesce] "coalesce",
	};
	char *replay2str[] = {
		[Replayoff] "off",
		[Replaylast] "last",
		[Replayall] "all",
	};
	snprint(buf, sizeof buf, "data %s\nreplay %s\n",
		mode2str[group->mode], replay2str[group->replay]);
	readstr(r, buf);
	respond(r, nil);
}

enum {
	Cmddata,
	Cmdreplay,
	Cmddebug, Cmddebug9p,
};
Cmdtab groupcmd[] = {
	/* data message|coalesce */
	{Cmddata, "data", 2},
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
	Group *group = f->aux;
	char *e = nil;
	Cmdbuf *cmd;
	Cmdtab *t;

	cmd = parsecmd(r->ifcall.data, r->ifcall.count);
	t = lookupcmd(cmd, groupcmd, nelem(groupcmd));
	if(t == nil){
		respondcmderror(r, cmd, "%r");
		free(cmd);
		return;
	}
	switch(t->index){
	case Cmddata: {
		if(strncmp(cmd->f[1], "message", 7) == 0)
			group->mode = Message;
		else
		if(strncmp(cmd->f[1], "coalesce", 8) == 0)
			group->mode = Coalesce;
		else
			e = "usage: data message|coalesce";
		break;
	}
	case Cmdreplay: {
		if(strncmp(cmd->f[1], "off", 3) == 0)
			group->replay = Replayoff;
		else
		if(strncmp(cmd->f[1], "last", 4) == 0)
			group->replay = Replaylast;
		else
		if(strncmp(cmd->f[1], "all", 3) == 0)
			group->replay = Replayall;
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
	case Qgroup:
		if(perm&DMDIR)
			f = groupcreate(parent, name, uid, perm);
		else{
			f = streamcreate(parent, name, uid, perm);
			r->fid->file = f;
			r->ofcall.qid = f->qid;
			streamopen(f->aux, r);
		}
		break;
	}
	if(f == nil)
		respond(r, "internal failure");
	else
		respond(r, nil);
}

void
xopen(Req *r)
{
	File *f = r->fid->file;

	switch(filetype(f)){
	case Qstream:
	case Qorder:
		streamopen(f->aux, r);
		break;
	}
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
	case Qctl:
		ctlread(r);
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
		foreach(Read*, s->rqueue){
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
	case Qgroup:
		groupclose(f);
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
	groupcreate(fs.tree->root, "/", nil, 0);
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

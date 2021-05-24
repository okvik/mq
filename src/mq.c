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

struct Read {
	Listelem;

	Req *req;
};

struct Write {
	Listelem;

	/* Twrite.ifcall */
	vlong offset;
	uint count;
	uchar *data;
};

struct Stream {
	Listelem;

	Group *group;
	Write *wqueue;
	Read *rqueue;
};

struct Group {
	Stream *streams;
	Stream *order;

	enum {Message, Coalesce} mode;
	enum {Replayoff, Replaylast, Replayall} replay;
};

struct Client {
	Write *cursor;
	vlong offset;
	int blocked;
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
	f->qid.path &= ~((uvlong)0xF<<60);
	f->qid.path |= (uvlong)(type&0xF)<<60;
}

ushort
filetype(File *f)
{
	return (f->qid.path>>60)&0xF;
}

File*
groupcreate(File *dir, char *name, char *uid, ulong perm)
{
	Stream *streamalloc(Group*);
	void *streamclose(Stream*);
	File *d, *ctl, *order;
	Group *group;

	group = emalloc(sizeof(Group));
	group->streams = (Stream*)listinit(emalloc(sizeof(Stream)));
	group->order = streamalloc(group);
	group->mode = Message;
	group->replay = Replayoff;

	ctl = order = nil;
	if(strcmp(name, "/") == 0){
		d = dir;
		d->aux = group;
	}
	else
		d = createfile(dir, name, uid, perm, group);
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
	streamclose(group->order);
	if(d) closefile(d);
	if(ctl) closefile(ctl);
	if(order) closefile(order);
	return nil;
}

void
groupclose(Group *g)
{
	free(g);
}

Stream*
streamalloc(Group *g)
{
	Stream *s;
	
	s = emalloc(sizeof(Stream));
	s->group = g;
	s->wqueue = (Write*)listinit(emalloc(sizeof(Write)));
	s->rqueue = (Read*)listinit(emalloc(sizeof(Read)));
	return s;
}

void
streamclose(Stream *s)
{
	Read *r;
	Write *w;

	listeach(Read*, s->rqueue, r){
		listunlink(r);
		free(r);
	}
	free(s->wqueue);
	listeach(Write*, s->wqueue, w){
		listunlink(w);
		free(w);
	}
	free(s->wqueue);
	listunlink(s);
	free(s);
}

File*
streamcreate(File *dir, char *name, char *uid, ulong perm)
{
	File *f;
	Group *group;
	Stream *s;

	group = dir->aux;
	s = streamalloc(group);
	if((f = createfile(dir, name, uid, perm, s)) == nil){
		streamclose(s);
		return nil;
	}
	filesettype(f, Qstream);
	listlink(group->streams, s);
	return f;
}

void
streamopen(Stream *s, Req *req)
{
	Client *c;
	
	c = req->fid->aux = emalloc(sizeof(Client));
	switch(s->group->replay){
	case Replayoff:
		c->offset = 0;
		c->blocked = 1;
		c->cursor = nil;
		break;

	case Replayall:
		c->offset = 0;
		if(listisempty(s->wqueue)){
			c->blocked = 1;
			c->cursor = nil;
		}else{
			c->blocked = 0;
			c->cursor = s->wqueue->front;
		}
		break;

	case Replaylast:
		c->offset = 0;
		if(listisempty(s->wqueue)){
			c->blocked = 1;
			c->cursor = nil;
		}else{
			c->blocked = 0;
			c->cursor = s->wqueue->back;
		}
		break;
	}
}

void
streamrespond(Req *req, int mode)
{
	Client *c = req->fid->aux;
	Stream *s = req->fid->file->aux;
	Write *w;
	/* request size, response buffer offset */
	vlong rn, ro;
	/* chunk size and offset, total read */
	vlong n, o, t;

	t = 0;
	rn = req->ifcall.count;
	ro = 0;
	w = c->cursor;
	o = c->offset;
	listrange(Write*, s->wqueue, w){
		if(mode == Message && w != c->cursor)
			break;
		for(; n = w->count - o, n > 0; o += n, ro += n, t += n){
			if(t == rn)
				goto done;
			if(n > rn - ro)
				n = rn - ro;
			memmove(req->ofcall.data+ro, w->data+o, n);
		}
		o = 0;
	}
done:
	req->ofcall.count = t;
	respond(req, nil);
	
	/* Determine the Client state */
	if(w == s->wqueue){
		c->offset = 0;
		c->blocked = 1;
		c->cursor = nil;
		return;
	}
	c->offset = o;
	c->blocked = 0;
	c->cursor = w;
}

void
streamread(Req *req)
{
	Client *c = req->fid->aux;
	Stream *s = req->fid->file->aux;
	Read *r;
	
	if(c->blocked){
		r = emalloc(sizeof(Read));
		r->req = req;
		listlink(s->rqueue, r);
		return;
	}
	streamrespond(req, s->group->mode);
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
streamwrite(Req *req)
{
	File *f = req->fid->file;
	Stream *s = req->fid->file->aux;
	Group *group = s->group;
	Write *w, *wq, *o, *oq;
	Read *r;
	Client *c;
	long n;
	
	wq = s->wqueue;
	oq = group->order->wqueue;

	/* Commit to queue */
	w = writealloc(req->ifcall.count);
	w->count = req->ifcall.count;
	w->offset = req->ifcall.offset;
	memmove(w->data, req->ifcall.data, w->count);
	listlink(wq->back, w);

	/* Commit to group order queue */
	n = strlen(f->name)+1;
	o = writealloc(n);
	o->offset = 0;
	o->count = n;
	memmove(o->data, f->name, n);
	listlink(oq->back, o);
 
	/* Kick the blocked stream readers */
	listeach(Read*, s->rqueue, r){
		c = r->req->fid->aux;
		
		c->cursor = w;
		c->offset = 0;
		c->blocked = 0;
		streamrespond(r->req, group->mode);
		listunlink(r);
		free(r);
	}

	/* Kick the blocked order readers */
	listeach(Read*, group->order->rqueue, r){
		c = r->req->fid->aux;
		
		c->cursor = o;
		c->offset = 0;
		c->blocked = 0;
		streamrespond(r->req, Message);
		listunlink(r);
		free(r);
	}

	req->ofcall.count = req->ifcall.count;
	respond(req, nil);
}

void
ctlread(Req *req)
{
	Group *group = req->fid->file->aux;
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
	readstr(req, buf);
	respond(req, nil);
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
ctlwrite(Req *req)
{
	Group *group = req->fid->file->aux;
	char *e = nil;
	Cmdbuf *cmd;
	Cmdtab *t;

	cmd = parsecmd(req->ifcall.data, req->ifcall.count);
	t = lookupcmd(cmd, groupcmd, nelem(groupcmd));
	if(t == nil){
		respondcmderror(req, cmd, "%r");
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
	respond(req, e);
}

void
xcreate(Req *req)
{
	char *name = req->ifcall.name;
	char *uid = req->fid->uid;
	ulong perm = req->ifcall.perm;
	File *group = req->fid->file;
	File *f = nil;

	switch(filetype(group)){
	case Qroot:
	case Qgroup:
		if(perm&DMDIR)
			f = groupcreate(group, name, uid, perm);
		else{
			f = streamcreate(group, name, uid, perm);
			req->fid->file = f;
			req->ofcall.qid = f->qid;
			streamopen(f->aux, req);
		}
		break;
	}
	if(f == nil)
		respond(req, "internal failure");
	else
		respond(req, nil);
}

void
xopen(Req *req)
{
	File *f = req->fid->file;

	switch(filetype(f)){
	case Qstream:
	case Qorder:
		streamopen(f->aux, req);
		break;
	}
	respond(req, nil);
}

void
xwrite(Req *req)
{
	File *f = req->fid->file;

	switch(filetype(f)){
	case Qstream:
		streamwrite(req);
		break;
	case Qctl:
		ctlwrite(req);
		break;
	default:
		respond(req, "forbidden");
		return;
	}
}

void
xread(Req *req)
{
	File *f = req->fid->file;

	switch(filetype(f)){
	case Qstream:
	case Qorder:
		streamread(req);
		break;
	case Qctl:
		ctlread(req);
		break;
	default:
		respond(req, "forbidden");
	}
}

void
xflush(Req *req)
{
	Req *old = req->oldreq;
	File *f = old->fid->file;
	Read *r;

	switch(filetype(f)){
	case Qstream:
	case Qorder: {
		Stream *s = f->aux;

		if(old->ifcall.type != Tread)
			break;
		listeach(Read*, s->rqueue, r){
			if(r->req == old){
				listunlink(r);
				free(r);
				break;
			}
		}
		respond(old, "interrupted");
	}}
	respond(req, nil);
}

void
xwstat(Req *req)
{
	File *w, *f = req->fid->file;
	char *uid = req->fid->uid;

	/* To change name, must have write permission in group. */
	if(req->d.name[0] != '\0' && strcmp(req->d.name, f->name) != 0){
		if((w = f->parent) == nil)
			goto perm;
		incref(w);
	 	if(!hasperm(w, uid, AWRITE)){
			closefile(w);
			goto perm;
		}
		if((w = walkfile(w, req->d.name)) != nil){
			closefile(w);
			respond(req, "file already exists");
			return;
		}
	}

	/* To change group, must be owner and member of new group,
	 * or leader of current group and leader of new group.
	 * Second case cannot happen, but we check anyway. */
	while(req->d.gid[0] != '\0' && strcmp(f->gid, req->d.gid) != 0){
		if(strcmp(uid, f->uid) == 0)
			break;
		if(strcmp(uid, f->gid) == 0)
		if(strcmp(uid, req->d.gid) == 0)
			break;
		respond(req, "not owner");
		return;
	}

	/* To change mode, must be owner or group leader.
	 * Because of lack of users file, leader=>group itself. */
	if(req->d.mode != ~0 && f->mode != req->d.mode){
		if(strcmp(uid, f->uid) != 0)
		if(strcmp(uid, f->gid) != 0){
			respond(req, "not owner");
			return;
		}
	}

	if(req->d.name[0] != '\0'){
		free(f->name);
		f->name = estrdup(req->d.name);
	}
	if(req->d.uid[0] != '\0'){
		free(f->uid);
		f->uid = estrdup(req->d.uid);
	}
	if(req->d.gid[0] != '\0'){
		free(f->gid);
		f->gid = estrdup(req->d.gid);
	}
	if(req->d.mode != ~0){
		f->mode = req->d.mode;
		f->qid.type = f->mode >> 24;
	}

	respond(req, nil);
	return;
perm:
	respond(req, "permission denied");
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
		groupclose(f->aux);
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

	fs.tree = alloctree(nil, nil, DMDIR|0775, xdestroyfile);
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

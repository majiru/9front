#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include <avl.h>

#include "dat.h"
#include "fns.h"
#include "atomic.h"

static void	respond(Fmsg*, Fcall*);
static void	rerror(Fmsg*, char*, ...);
static void	clunkfid(Conn*, Fid*, Amsg**);

static void	authfree(AuthRpc*);

int
walk1(Tree *t, vlong up, char *name, Qid *qid, vlong *len)
{
	char *p, kbuf[Keymax], rbuf[Kvmax];
	int err;
	Xdir d;
	Kvp kv;
	Key k;

	err = 0;
	p = packdkey(kbuf, sizeof(kbuf), up, name);
	k.k = kbuf;
	k.nk = p - kbuf;
	if(err)
		return -1;
	if(!btlookup(t, &k, &kv, rbuf, sizeof(rbuf)))
		return -1;
	kv2dir(&kv, &d);
	*qid = d.qid;
	*len = d.length;
	return 0;
}

static void
touch(Dent *de, Msg *msg)
{
	wlock(de);
	de->qid.vers++;
	msg->op = Owstat;
	msg->k = de->k;
	msg->nk = de->nk;
	msg->v = "\0";
	msg->nv = 1;
	wunlock(de);
}

static void
wrbarrier(void)
{
	tracev("barrier", fs->qgen);
	aincv(&fs->qgen, 1);
}

static void
wrwait(void)
{
	Qent qe;
	int i;

	tracev("wrwait", fs->qgen);
	aincv(&fs->qgen, 1);
	fs->syncing = fs->nsyncers;
	for(i = 0; i < fs->nsyncers; i++){
		qe.op = Qfence;
		qe.bp.addr = 0;
		qe.bp.hash = -1;
		qe.bp.gen = -1;
		qe.b = nil;
		qput(&fs->syncq[i], qe);
	}
	aincv(&fs->qgen, 1);
	while(fs->syncing != 0)
		rsleep(&fs->syncrz);
	tracev("flushed", fs->qgen);
}

static void
sync(void)
{
	Mount *mnt;
	Arena *a;
	Dlist dl;
	int i;

	qlock(&fs->synclk);
	if(waserror()){
		fprint(2, "failed to sync: %s\n", errmsg());
		qunlock(&fs->synclk);
		nexterror();
	}

	/* 
	 * Wait for data that we're syncing to hit disk
	 */
	tracem("flush1");
	wrbarrier();
	/*
	 * pass 0: Update all open snapshots, and
	 *  pack the blocks we want to sync. Snap
	 *  while holding the write lock, and then
	 *  wait until all the blocks they point at
	 *  have hit disk; once they're on disk, we
	 *  can take a consistent snapshot.
         */
	qlock(&fs->mutlk);
	tracem("packb");
	for(mnt = agetp(&fs->mounts); mnt != nil; mnt = mnt->next)
		updatesnap(&mnt->root, mnt->root, mnt->name, mnt->flag);
	/*
	 * Now that we've updated the snaps, we can sync the
	 * dlist; the snap tree will not change from here.
	 */
	dlsync();
	dl = fs->snapdl;
	fs->snapdl.hd = Zb;
	fs->snapdl.tl = Zb;
	fs->snapdl.ins = nil;
	traceb("syncdl.dl", dl.hd);
	traceb("syncdl.rb", fs->snap.bp);
	for(i = 0; i < fs->narena; i++){
		a = &fs->arenas[i];
		qlock(a);
		/*
		 * because the log uses preallocated
		 * blocks, we need to write the log
		 * block out synchronously, or it may
		 * get reused.
		 */
		logbarrier(a, agetv(&fs->qgen));
		flushlog(a);

		packarena(a->h0->data, Blksz, a);
		packarena(a->h1->data, Blksz, a);
		finalize(a->h0);
		finalize(a->h1);
		fs->arenabp[i] = a->h0->bp;
		qunlock(a);
	}
	assert(fs->snapdl.hd.addr == -1);
	traceb("packsb.rb", fs->snap.bp);
	packsb(fs->sb0->buf, Blksz, fs);
	packsb(fs->sb1->buf, Blksz, fs);
	finalize(fs->sb0);
	finalize(fs->sb1);
	fs->snap.dirty = 0;
	qunlock(&fs->mutlk);

	/*
	 * pass 1: sync block headers; if we crash here,
	 *  the block footers are consistent, and we can
	 *  use them.
	 */
	tracem("arenas0");
	for(i = 0; i < fs->narena; i++)
		enqueue(fs->arenas[i].h0);
	wrbarrier();

	/*
	 * pass 2: sync superblock; we have a consistent
	 * set of block headers, so if we crash, we can
	 * use the loaded block headers; the footers will
	 * get synced after so that we can use them next
	 * time around.
         */
	tracem("supers");
	enqueue(fs->sb0);
	enqueue(fs->sb1);
	wrbarrier();

	/*
	 * pass 3: sync block footers; if we crash here,
	 *  the block headers are consistent, and we can
	 *  use them.
         */
	tracem("arenas1");
	for(i = 0; i < fs->narena; i++)
		enqueue(fs->arenas[i].h1);

	/*
	 * Pass 4: clean up the old snap tree's deadlist.
	 * we need to wait for all the new data to hit disk
	 * before we can free anything, otherwise it gets
	 * clobbered.
	 */
	tracem("snapdl");
	wrwait();
	freedl(&dl, 1);
	qunlock(&fs->synclk);
	tracem("synced");
	poperror();
}

static void
snapfs(Amsg *a, Tree **tp)
{
	Tree *t, *s;
	Mount *mnt;

	if(waserror()){
		*tp = nil;
		nexterror();
	}
	t = nil;
	*tp = nil;
	for(mnt = agetp(&fs->mounts); mnt != nil; mnt = mnt->next){
		if(strcmp(a->old, mnt->name) == 0){
			updatesnap(&mnt->root, mnt->root, mnt->name, mnt->flag);
			t = agetp(&mnt->root);
			ainc(&t->memref);
			break;
		}
	}
	if(t == nil && (t = opensnap(a->old, nil)) == nil){
		if(a->fd != -1)
			fprint(a->fd, "snap: open '%s': does not exist\n", a->old);
		poperror();
		return;
	}
	if(a->delete){
		if(mnt != nil) {
			if(a->fd != -1)
				fprint(a->fd, "snap: snap is mounted: '%s'\n", a->old);
			poperror();
			return;
		}
		if(t->nlbl == 1 && t->nref <= 1 && t->succ == -1){
			ainc(&t->memref);
			*tp = t;
		}
		delsnap(t, t->succ, a->old);
	}else{
		if((s = opensnap(a->new, nil)) != nil){
			if(a->fd != -1)
				fprint(a->fd, "snap: already exists '%s'\n", a->new);
			closesnap(s);
			poperror();
			return;
		}
		tagsnap(t, a->new, a->flag);
	}
	closesnap(t);
	poperror();
	if(a->fd != -1){
		if(a->delete)
			fprint(a->fd, "deleted: %s\n", a->old);
		else if(a->flag & Lmut)
			fprint(a->fd, "forked: %s from %s\n", a->new, a->old);
		else
			fprint(a->fd, "labeled: %s from %s\n", a->new, a->old);
	}
}

static void
filldumpdir(Xdir *d)
{
	memset(d, 0, sizeof(Xdir));
	d->name = "/";
	d->qid.path = Qdump;
	d->qid.vers = fs->nextgen;
	d->qid.type = QTDIR;
	d->mode = DMDIR|0555;
	d->atime = 0;
	d->mtime = 0;
	d->length = 0;
	d->uid = -1;
	d->gid = -1;
	d->muid = -1;
}

static char*
okname(char *name)
{
	int i;

	if(name[0] == 0)
		return Ename;
	if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return Ename;
	for(i = 0; i < Maxname; i++){
		if(name[i] == 0)
			return nil;
		if((name[i]&0xff) < 0x20 || name[i] == '/')
			return Ename;
	}
	return Elength;
}

Chan*
mkchan(int size)
{
	Chan *c;

	if((c = mallocz(sizeof(Chan) + size*sizeof(void*), 1)) == nil)
		sysfatal("create channel");
	c->size = size;
	c->avail = size;
	c->count = 0;
	c->rp = c->args;
	c->wp = c->args;
	return c;

}

void*
chrecv(Chan *c)
{
	void *a;
	long v;

	v = agetl(&c->count);
	if(v == 0 || !acasl(&c->count, v, v-1))
		semacquire(&c->count, 1);
	lock(&c->rl);
	a = *c->rp;
	if(++c->rp >= &c->args[c->size])
		c->rp = c->args;
	unlock(&c->rl);
	semrelease(&c->avail, 1);
	return a;
}

void
chsend(Chan *c, void *m)
{
	long v;

	v = agetl(&c->avail);
	if(v == 0 || !acasl(&c->avail, v, v-1))
		semacquire(&c->avail, 1);
	lock(&c->wl);
	*c->wp = m;
	if(++c->wp >= &c->args[c->size])
		c->wp = c->args;
	unlock(&c->wl);
	semrelease(&c->count, 1);
}

static void
fshangup(Conn *c, char *fmt, ...)
{
	char buf[ERRMAX];
	va_list ap;

	c->hangup = 1;

	va_start(ap, fmt);
	vsnprint(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	fprint(2, "hangup: %s\n", buf);

	if(c->cfd >= 0)
		hangup(c->cfd);
}

static void
respond(Fmsg *m, Fcall *r)
{
	Conn *c;
	RWLock *lk;
	uchar buf[Max9p+IOHDRSZ];
	int w, n;

	r->tag = m->tag;
	dprint("→ %F\n", r);
	assert(m->type+1 == r->type || r->type == Rerror);
	if((n = convS2M(r, buf, sizeof(buf))) == 0)
		abort();
	c = m->conn;
	qlock(&c->wrlk);
	w = c->hangup? n: write(c->wfd, buf, n);
	qunlock(&c->wrlk);
	if(w != n)
		fshangup(c, Eio);
	if(m->type == Tflush){
		lk = &fs->flushq[ihash(m->oldtag) % Nflushtab];
		wunlock(lk);
	}else{
		lk = &fs->flushq[ihash(m->tag) % Nflushtab];
		runlock(lk);
	}
	free(m);
	putconn(c);
}

static void
rerror(Fmsg *m, char *fmt, ...)
{
	char buf[128];
	va_list ap;
	Fcall r;

	va_start(ap, fmt);
	vsnprint(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	r.type = Rerror;
	r.ename = buf;
	respond(m, &r);
}


static void
upsert(Mount *mnt, Msg *m, int nm)
{
	if(!(mnt->flag & Lmut))
		error(Erdonly);
	if(mnt->root->nlbl != 1 || mnt->root->nref != 0)
		updatesnap(&mnt->root, mnt->root, mnt->name, mnt->flag);
	btupsert(mnt->root, m, nm);
}

/*
 * When truncating a file, mutations need
 * to wait for the sweeper to finish; this
 * means the mutator needs to release the
 * mutation lock, exit the epoch, and
 * allow the sweeper to finish its job
 * before resuming.
 */
static void
truncwait(Dent *de, int id)
{
	epochend(id);
	qunlock(&fs->mutlk);
	qlock(&de->trunclk);
	while(de->trunc)
		rsleep(&de->truncrz);
	qunlock(&de->trunclk);
	qlock(&fs->mutlk);
	epochstart(id);
}

static int
readb(Tree *t, Fid *f, char *d, vlong o, vlong n, vlong sz)
{
	char buf[Offksz], kvbuf[Offksz+32];
	vlong fb, fo;
	Bptr bp;
	Blk *b;
	Key k;
	Kvp kv;

	if(o >= sz)
		return 0;

	fb = o & ~(Blksz-1);
	fo = o & (Blksz-1);
	if(fo+n > Blksz)
		n = Blksz-fo;

	k.k = buf;
	k.nk = sizeof(buf);
	k.k[0] = Kdat;
	PACK64(k.k+1, f->qpath);
	PACK64(k.k+9, fb);

	if(!btlookup(t, &k, &kv, kvbuf, sizeof(kvbuf))){
		memset(d, 0, n);
		return n;
	}

	bp = unpackbp(kv.v, kv.nv);
	b = getblk(bp, GBraw);
	memcpy(d, b->buf+fo, n);
	dropblk(b);
	return n;
}

static int
writeb(Fid *f, Msg *m, Bptr *ret, char *s, vlong o, vlong n, vlong sz)
{
	char buf[Kvmax];
	vlong fb, fo;
	Blk *b, *t;
	int seq;
	Tree *r;
	Bptr bp;
	Kvp kv;

	fb = o & ~(Blksz-1);
	fo = o & (Blksz-1);

	m->k[0] = Kdat;
	PACK64(m->k+1, f->qpath);
	PACK64(m->k+9, fb);

	if(fo+n >= Blksz)
		seq = 1;
	else
		seq = 0;
	b = newdblk(f->mnt->root, f->qpath, seq);
	t = nil;
	r = f->mnt->root;
	if(btlookup(r, m, &kv, buf, sizeof(buf))){
		bp = unpackbp(kv.v, kv.nv);
		if(fb < sz && (fo != 0 || n != Blksz)){
			t = getblk(bp, GBraw);
			memcpy(b->buf, t->buf, Blksz);
			dropblk(t);
		}
	}
	if(fo+n > Blksz)
		n = Blksz-fo;
	memcpy(b->buf+fo, s, n);
	if(t == nil){
		if(fo > 0)
			memset(b->buf, 0, fo);
		if(fo+n < Blksz)
			memset(b->buf+fo+n, 0, Blksz-fo-n);
	}
	enqueue(b);

	packbp(m->v, m->nv, &b->bp);
	*ret = b->bp;
	dropblk(b);
	return n;
}

static Dent*
getdent(Mount *mnt, vlong pqid, Xdir *d)
{
	Dent *de;
	char *e;
	u32int h;

	h = ihash(d->qid.path) % Ndtab;
	lock(&mnt->dtablk);
	for(de = mnt->dtab[h]; de != nil; de = de->next){
		if(de->qid.path == d->qid.path){
			ainc(&de->ref);
			goto Out;
		}
	}

	de = emalloc(sizeof(Dent), 1);
	de->Xdir = *d;
	de->ref = 1;
	de->up = pqid;
	de->qid = d->qid;
	de->length = d->length;
	de->truncrz.l = &de->trunclk;

	if((e = packdkey(de->buf, sizeof(de->buf), pqid, d->name)) == nil){
		free(de);
		de = nil;
		goto Out;
	}
	de->k = de->buf;
	de->nk = e - de->buf;
	de->name = de->buf + 11;
	de->next = mnt->dtab[h];
	mnt->dtab[h] = de;

Out:
	unlock(&mnt->dtablk);
	return de;
}

static void
loadautos(Mount *mnt)
{
	char pfx[128];
	int m, h, ns;
	uint flg;
	Scan s;

	m = 0;
	h = 0;
	pfx[0] = Klabel;
	ns = snprint(pfx+1, sizeof(pfx)-1, "%s@minute.", mnt->name);
	btnewscan(&s, pfx, ns+1);
	btenter(&fs->snap, &s);
	while(1){
		if(!btnext(&s, &s.kv))
			break;
		flg = UNPACK32(s.kv.v+1+8);
		if(flg & Lauto){
			memcpy(mnt->minutely[m], s.kv.k+1, s.kv.nk-1);
			mnt->minutely[m][s.kv.nk-1] = 0;
			m = (m+1)%60;
			continue;
		}
	}
	btexit(&s);

	pfx[0] = Klabel;
	ns = snprint(pfx+1, sizeof(pfx)-1, "%s@hour.", mnt->name);
	btnewscan(&s, pfx, ns+1);
	btenter(&fs->snap, &s);
	while(1){
		if(!btnext(&s, &s.kv))
			break;
		flg = UNPACK32(s.kv.v+1+8);
		if(flg & Lauto){
			memcpy(mnt->hourly[h], s.kv.k+1, s.kv.nk-1);
			mnt->hourly[h][s.kv.nk-1] = 0;
			h = (h+1)%24;
			continue;
		}
	}
	btexit(&s);
}

Mount *
getmount(char *name)
{
	Mount *mnt;
	Tree *t;
	int flg;

	if(strcmp(name, "dump") == 0){
		ainc(&fs->snapmnt->ref);
		return fs->snapmnt;
	}

	qlock(&fs->mountlk);
	for(mnt = agetp(&fs->mounts); mnt != nil; mnt = mnt->next){
		if(strcmp(name, mnt->name) == 0){
			ainc(&mnt->ref);
			goto Out;
		}
	}

	if((mnt = mallocz(sizeof(*mnt), 1)) == nil)
		error(Enomem);
	if(waserror()){
		qunlock(&fs->mountlk);
		free(mnt);
		nexterror();
	}
	mnt->ref = 1;
	snprint(mnt->name, sizeof(mnt->name), "%s", name);
	if((t = opensnap(name, &flg)) == nil)
		error(Enosnap);
	loadautos(mnt);
	mnt->flag = flg;
	mnt->root = t;
	mnt->next = fs->mounts;
	asetp(&fs->mounts, mnt);
	poperror();

Out:
	qunlock(&fs->mountlk);
	return mnt;
}

void
clunkmount(Mount *mnt)
{
	Mount *me, **p;

	if(mnt == nil)
		return;
	if(adec(&mnt->ref) == 0){
		qlock(&fs->mountlk);
		for(p = &fs->mounts; (me = *p) != nil; p = &me->next){
			if(me == mnt)
				break;
		}
		assert(me != nil);
		*p = me->next;
		limbo(DFmnt, me);
		qunlock(&fs->mountlk);
	}
}

static void
clunkdent(Mount *mnt, Dent *de)
{
	Dent *e, **pe;
	u32int h;

	if(de == nil)
		return;
	if(de->qid.type & QTAUTH){
		if(adec(&de->ref) == 0){
			authfree(de->auth);
			free(de);
		}
		return;
	}
	lock(&mnt->dtablk);
	if(adec(&de->ref) != 0)
		goto Out;
	h = ihash(de->qid.path) % Ndtab;
	pe = &mnt->dtab[h];
	for(e = mnt->dtab[h]; e != nil; e = e->next){
		if(e == de)
			break;
		pe = &e->next;
	}
	assert(e != nil);
	*pe = e->next;
	free(de);
Out:
	unlock(&mnt->dtablk);
}

static Fid*
getfid(Conn *c, u32int fid)
{
	u32int h;
	Fid *f;

	h = ihash(fid) % Nfidtab;
	lock(&c->fidtablk[h]);
	for(f = c->fidtab[h]; f != nil; f = f->next)
		if(f->fid == fid){
			ainc(&f->ref);
			break;
		}
	unlock(&c->fidtablk[h]);
	return f;
}

static void
putfid(Fid *f)
{
	if(adec(&f->ref) != 0)
		return;
	clunkdent(f->mnt, f->dent);
	clunkdent(f->mnt, f->dir);
	clunkmount(f->mnt);
	free(f);
}

static Fid*
dupfid(Conn *c, u32int new, Fid *f)
{
	Fid *n, *o;
	u32int h;

	h = ihash(new) % Nfidtab;
	if((n = malloc(sizeof(Fid))) == nil)
		return nil;

	*n = *f;
	n->fid = new;
	n->ref = 2; /* one for dup, one for clunk */
	n->mode = -1;
	n->next = nil;

	lock(&c->fidtablk[h]);
	for(o = c->fidtab[h]; o != nil; o = o->next)
		if(o->fid == new)
			break;
	if(o == nil){
		n->next = c->fidtab[h];
		c->fidtab[h] = n;
	}
	unlock(&c->fidtablk[h]);

	if(o != nil){
		fprint(2, "fid in use: %d == %d\n", o->fid, new);
		free(n);
		return nil;
	}
	if(n->mnt != nil)
		ainc(&n->mnt->ref);
	ainc(&n->dent->ref);
	ainc(&n->dir->ref);
	setmalloctag(n, getcallerpc(&c));
	return n;
}

static void
clunkfid(Conn *c, Fid *fid, Amsg **ao)
{
	Fid *f, **pf;
	u32int h;

	h = ihash(fid->fid) % Nfidtab;
	lock(&c->fidtablk[h]);
	pf = &c->fidtab[h];
	for(f = c->fidtab[h]; f != nil; f = f->next){
		if(f == fid){
			assert(adec(&f->ref) != 0);
			*pf = f->next;
			break;
		}
		pf = &f->next;
	}
	assert(f != nil);
	if(f->scan != nil){
		free(f->scan);
		f->scan = nil;
	}
	if(f->rclose != nil){
		*ao = f->rclose;

		qlock(&f->dent->trunclk);
		f->dent->trunc = 1;
		qunlock(&f->dent->trunclk);

		wlock(f->dent);
		f->dent->gone = 1;
		wunlock(f->dent);

		ainc(&f->dent->ref);
		ainc(&f->mnt->ref);
		(*ao)->op = AOrclose;
		(*ao)->mnt = f->mnt;
		(*ao)->qpath = f->qpath;
		(*ao)->off = 0;
		(*ao)->end = f->dent->length;
		(*ao)->dent = f->dent;
	}
	unlock(&c->fidtablk[h]);
}

static int
readmsg(Conn *c, Fmsg **pm)
{
	char szbuf[4];
	int sz, n;
	Fmsg *m;

	n = readn(c->rfd, szbuf, 4);
	if(n <= 0){
		*pm = nil;
		return n;
	}
	if(n != 4){
		werrstr("short read: %r");
		return -1;
	}
	sz = GBIT32(szbuf);
	if(sz > c->iounit){
		werrstr("message size too large");
		return -1;
	}
	if((m = malloc(sizeof(Fmsg)+sz)) == nil)
		return -1;
	if(readn(c->rfd, m->buf+4, sz-4) != sz-4){
		werrstr("short read: %r");
		free(m);
		return -1;
	}
	ainc(&c->ref);
	m->conn = c;
	m->sz = sz;
	PBIT32(m->buf, sz);
	*pm = m;
	return 0;
}

static void
fsversion(Fmsg *m)
{
	Fcall r;
	char *p;

	memset(&r, 0, sizeof(Fcall));
	p = strchr(m->version, '.');
	if(p != nil)
		*p = '\0';
	r.type = Rversion;
	r.msize = Max9p + IOHDRSZ;
	if(strcmp(m->version, "9P2000") == 0){
		if(m->msize < r.msize)
			r.msize = m->msize;
		r.version = "9P2000";
		m->conn->versioned = 1;
		m->conn->iounit = r.msize;
	}else{
		r.version = "unknown";
		m->conn->versioned = 0;
	}
	respond(m, &r);
}

static void
authfree(AuthRpc *auth)
{
	AuthRpc *rpc;

	if(rpc = auth){
		close(rpc->afd);
		auth_freerpc(rpc);
	}
}

AuthRpc*
authnew(void)
{
	static char *keyspec = "proto=p9any role=server";
	AuthRpc *rpc;
	int fd;

	if(access("/mnt/factotum", 0) < 0)
		if((fd = open("/srv/factotum", ORDWR)) >= 0)
			mount(fd, -1, "/mnt", MBEFORE, "");
	if((fd = open("/mnt/factotum/rpc", ORDWR)) < 0)
		return nil;
	if((rpc = auth_allocrpc(fd)) == nil){
		close(fd);
		return nil;
	}
	if(auth_rpc(rpc, "start", keyspec, strlen(keyspec)) != ARok){
		authfree(rpc);
		return nil;
	}
	return rpc;
}

static void
authread(Fid *f, Fcall *r, void *data, vlong count)
{
	AuthInfo *ai;
	AuthRpc *rpc;
	User *u;

	if((f->dir->qid.type & QTAUTH) == 0 || (rpc = f->dir->auth) == nil)
		error(Etype);

	switch(auth_rpc(rpc, "read", nil, 0)){
	default:
		error(Eauthp);
	case ARdone:
		if((ai = auth_getinfo(rpc)) == nil)
			goto Phase;
		rlock(&fs->userlk);
		u = name2user(ai->cuid);
		auth_freeAI(ai);
		if(u == nil){
			runlock(&fs->userlk);
			error(Enouser);
		}
		f->uid = u->id;
		runlock(&fs->userlk);
		return;
	case ARok:
		if(count < rpc->narg)
			error(Eauthd);
		memmove(data, rpc->arg, rpc->narg);
		r->count = rpc->narg;
		return;
	case ARphase:
	Phase:
		error(Eauthph);
	}
}

static void
authwrite(Fid *f, Fcall *r, void *data, vlong count)
{
	AuthRpc *rpc;

	if((f->dir->qid.type & QTAUTH) == 0 || (rpc = f->dir->auth) == nil)
		error(Etype);
	if(auth_rpc(rpc, "write", data, count) != ARok)
		error(Ebotch);
	r->type = Rwrite;
	r->count = count;

}

static void
fsauth(Fmsg *m)
{
	Dent *de;
	Fcall r;
	Fid f, *nf;

	if(fs->noauth){
		rerror(m, Eauth);
		return;
	}
	if(strcmp(m->uname, "none") == 0){
		rerror(m, Enone);
		return;
	}
	if((de = mallocz(sizeof(Dent), 1)) == nil){
		rerror(m, Enomem);
		return;
	}
	memset(de, 0, sizeof(Dent));
	de->auth = authnew();
	if(de->auth == nil){
		rerror(m, errmsg());
		return;
	}
	de->ref = 0;
	de->qid.type = QTAUTH;
	de->qid.path = aincv(&fs->nextqid, 1);
	de->qid.vers = 0;
	de->length = 0;
	de->k = nil;
	de->nk = 0;

	memset(&f, 0, sizeof(Fid));
	f.fid = NOFID;
	f.mnt = nil;
	f.qpath = de->qid.path;
	f.pqpath = de->qid.path;
	f.mode = -1;
	f.iounit = m->conn->iounit;
	f.dent = de;
	f.dir = de;
	f.uid = -1;
	f.duid = -1;
	f.dgid = -1;
	f.dmode = 0600;
	nf = dupfid(m->conn, m->afid, &f);
	if(nf == nil){
		rerror(m, Efid);
		authfree(de->auth);
		free(de);
		return;
	}
	r.type = Rauth;
	r.aqid = de->qid;
	respond(m, &r);
	putfid(nf);
}

static int
ingroup(int uid, int gid)
{
	User *u, *g;
	int i, in;

	rlock(&fs->userlk);
	in = 0;
	u = uid2user(uid);
	g = uid2user(gid);
	if(u != nil && g != nil)
		if(u->id == g->id)
			in = 1;
		else for(i = 0; i < g->nmemb; i++)
			if(u->id == g->memb[i])
				in = 1;
	runlock(&fs->userlk);
	return in;
}

static int
groupleader(int uid, int gid)
{
	User *g;
	int i, lead;

	lead = 0;
	rlock(&fs->userlk);
	g = uid2user(gid);
	if(g != nil){
		if(g->lead == 0){
			for(i = 0; i < g->nmemb; i++)
				if(g->memb[i] == uid){
					lead = 1;
					break;
				}
		}else if(uid == g->lead)
			lead = 1;
	}
	runlock(&fs->userlk);
	return lead;

}

static int
mode2bits(int req)
{
	int m;

	m = 0;
	switch(req&0xf){
	case OREAD:	m = DMREAD;		break;
	case OWRITE:	m = DMWRITE;		break;
	case ORDWR:	m = DMREAD|DMWRITE;	break;
	case OEXEC:	m = DMREAD|DMEXEC;	break;
	}
	if(req&OTRUNC)
		m |= DMWRITE;
	return m;
}

static int
fsaccess(Fid *f, ulong fmode, int fuid, int fgid, int m)
{
	/* uid none gets only other permissions */
	if(f->permit)
		return 0;
	if(f->uid != noneid) {
		if(f->uid == fuid)
			if((m & (fmode>>6)) == m)
				return 0;
		if(ingroup(f->uid, fgid))
			if((m & (fmode>>3)) == m)
				return 0;
	}
	if((m & fmode) == m) {
		if((fmode & DMDIR) && (m == DMEXEC))
			return 0;
		if(!ingroup(f->uid, nogroupid))
			return 0;
	}
	return -1;
}

static void
fsattach(Fmsg *m)
{
	char dbuf[Kvmax], kvbuf[Kvmax];
	char *p, *n, *aname;
	Mount *mnt;
	Dent *de;
	Tree *t;
	User *u;
	Fcall r;
	Xdir d;
	Kvp kv;
	Key dk;
	Fid f, *af, *nf;
	int uid;

	de = nil;
	mnt = nil;
	if(waserror()){
		rerror(m, errmsg());
		goto Err;
	}
	aname = m->aname;
	if(aname[0] == '%')
		aname++;
	if(aname[0] == '\0')
		aname = "main";
	if((mnt = getmount(aname)) == nil)
		error(Enosnap);

	rlock(&fs->userlk);
	n = m->uname;
	/*
	 * to allow people to add themselves to the user file,
	 * we need to force the user id to one that exists.
	 */
	if(permissive && strcmp(aname, "adm") == 0)
		n = "adm";
	if((u = name2user(n)) == nil){
		runlock(&fs->userlk);
		error(Enouser);
	}
	uid = u->id;
	runlock(&fs->userlk);

	if(m->afid != NOFID){
		r.data = nil;
		r.count = 0;
		if((af = getfid(m->conn, m->afid)) == nil)
			error(Enofid);
		authread(af, &r, nil, 0);
		putfid(af);
		if(af->uid != uid)
			error(Ebadu);
		m->conn->authok = 1;	/* none attach allowed now */
	}else if(!fs->noauth){
		if(uid != noneid || !m->conn->authok)
			error(Ebadu);
	}

	if(strcmp(m->aname, "dump") == 0){
		if(uid == noneid)
			error(Eperm);
		memset(&d, 0, sizeof(d));
		filldumpdir(&d);
	}else{
		if((p = packdkey(dbuf, sizeof(dbuf), -1ULL, "")) == nil)
			error(Elength);
		dk.k = dbuf;
		dk.nk = p - dbuf;
		t = agetp(&mnt->root);
		if(!btlookup(t, &dk, &kv, kvbuf, sizeof(kvbuf)))
			error(Enosnap);
		kv2dir(&kv, &d);
	}
	de = getdent(mnt, -1, &d);
	memset(&f, 0, sizeof(Fid));
	f.fid = NOFID;
	f.mnt = mnt;
	f.qpath = d.qid.path;
	f.pqpath = d.qid.path;
	f.mode = -1;
	f.iounit = m->conn->iounit;
	f.dent = de;
	f.dir = de;
	f.uid = uid;
	f.duid = d.uid;
	f.dgid = d.gid;
	f.dmode = d.mode;
	if(m->aname[0] == '%'){
		if(!permissive && !ingroup(uid, admid))
			error(Eperm);
		f.permit = 1;
	}
	if(strcmp(aname, "dump") == 0)
		f.fromdump = 1;
	nf = dupfid(m->conn, m->fid, &f);
	if(nf == nil)
		error(Efid);
	r.type = Rattach;
	r.qid = d.qid;
	respond(m, &r);
	putfid(nf);
	poperror();


Err:	clunkdent(mnt, de);
	clunkmount(mnt);
}

static int
findparent(Tree *t, vlong up, vlong *qpath, char **name, char *buf, int nbuf)
{
	char *p, kbuf[Keymax];
	Kvp kv;
	Key k;

	p = packsuper(kbuf, sizeof(kbuf), up);
	k.k = kbuf;
	k.nk = p - kbuf;
	if(!btlookup(t, &k, &kv, buf, nbuf))
		error(Esrch);
	*name = unpackdkey(kv.v, kv.nv, qpath);
	return 1;
}

static void
dkey(Key *k, vlong up, char *name, char *buf, int nbuf)
{
	char *p;

	p = packdkey(buf, nbuf, up, name);
	k->k = buf;
	k->nk = p - buf;
}

static void
fswalk(Fmsg *m)
{
	char *name, kbuf[Maxent], kvbuf[Kvmax];
	int duid, dgid, dmode;
	vlong up, upup, prev;
	Dent *dent, *dir;
	Fid *o, *f;
	Mount *mnt;
	Amsg *ao;
	Tree *t;
	Fcall r;
	Xdir d;
	Kvp kv;
	Key k;
	int i;

	if((o = getfid(m->conn, m->fid)) == nil){
		rerror(m, Enofid);
		return;
	}
	if(waserror()){
		rerror(m, errmsg());
		putfid(o);
		return;
	}
	if(o->mode != -1)
		error(Einuse);
	t = o->mnt->root;
	mnt = o->mnt;
	up = o->pqpath;
	prev = o->qpath;
	rlock(o->dent);
	d = *o->dent;
	runlock(o->dent);
	duid = d.uid;
	dgid = d.gid;
	dmode = d.mode;
	r.type = Rwalk;
	for(i = 0; i < m->nwname; i++){
		name = m->wname[i];
		if(strlen(name) > Maxname)
			error(Elength);
		if(fsaccess(o, d.mode, d.uid, d.gid, DMEXEC) != 0)
			break;
		if(strcmp(name, "..") == 0){
			if(up == -1 && o->fromdump){
				mnt = fs->snapmnt;
				filldumpdir(&d);
				prev = -1ULL;
				up = -1ULL;
				r.wqid[i] = d.qid;
				continue;
			}
			findparent(t, up, &prev, &name, kbuf, sizeof(kbuf));
		}else if(d.qid.path == Qdump){
			mnt = getmount(m->wname[i]);
			name = "";
			prev = -1ULL;
			t = mnt->root;
		}
		up = prev;
		duid = d.uid;
		dgid = d.gid;
		dmode = d.mode;
		dkey(&k, prev, name, kbuf, sizeof(kbuf));
		if(!btlookup(t, &k, &kv, kvbuf, sizeof(kvbuf)))
			break;
		kv2dir(&kv, &d);
		prev = d.qid.path;
		r.wqid[i] = d.qid;
	}
	r.nwqid = i;
	if(i == 0 && m->nwname != 0)
		error(Esrch);
	f = o;
	if(m->fid != m->newfid && i == m->nwname){
		if((f = dupfid(m->conn, m->newfid, o)) == nil)
			error(Efid);
		putfid(o);
		poperror();
		if(waserror()){
			rerror(m, errmsg());
			putfid(f);
			return;
		}
	}
	if(i > 0 && i == m->nwname){
		lock(f);
		ao = nil;
		if(waserror()){
			if(f != o)
				clunkfid(m->conn, f, &ao);
			assert(ao == nil);
			unlock(f);
			nexterror();
		}
		if(up == -1ULL){
			/* the root contains itself, I guess */
			dent = getdent(mnt, up, &d);
			dir = getdent(mnt, up, &d);
		}else{
			dent = getdent(mnt, up, &d);
			findparent(t, up, &upup, &name, kbuf, sizeof(kbuf));
			dkey(&k, upup, name, kbuf, sizeof(kbuf));
			if(!btlookup(t, &k, &kv, kvbuf, sizeof(kvbuf)))
				broke("missing parent");
			kv2dir(&kv, &d);
			dir = getdent(mnt, upup, &d);
		}
		clunkdent(f->mnt, f->dent);
		clunkdent(f->mnt, f->dir);
		if(mnt != f->mnt){
			clunkmount(f->mnt);
			ainc(&mnt->ref);
			f->mnt = mnt;
		}
		f->qpath = r.wqid[i-1].path;
		f->pqpath = up;
		f->dent = dent;
		f->dir = dir;
		f->duid = duid;
		f->dgid = dgid;
		f->dmode = dmode;
		poperror();
		unlock(f);
	}
	respond(m, &r);
	putfid(f);
	poperror();
}

static void
fsstat(Fmsg *m)
{
	char buf[STATMAX];
	Fcall r;
	Fid *f;
	int n;

	if((f = getfid(m->conn, m->fid)) == nil){
		rerror(m, Enofid);
		return;
	}
	if(waserror()){
		rerror(m, errmsg());
		putfid(f);
		return;
	}
	rlock(f->dent);
	if((n = dir2statbuf(f->dent, buf, sizeof(buf))) == -1)
		error(Efs);
	runlock(f->dent);
	r.type = Rstat;
	r.stat = (uchar*)buf;
	r.nstat = n;
	respond(m, &r);
	poperror();
	putfid(f);
}

static void
fswstat(Fmsg *m, int id, Amsg **ao)
{
	char rnbuf[Kvmax], opbuf[Kvmax], upbuf[Upksz];
	char *p, *e, strs[65535];
	int op, nm, rename;
	vlong oldlen;
	Qid old;
	Fcall r;
	Dent *de;
	Msg mb[4];
	Xdir n;
	Dir d;
	Tree *t;
	Fid *f;
	Key k;
	User *u;

	*ao = nil;
	rename = 0;
	if((f = getfid(m->conn, m->fid)) == nil){
		rerror(m, Enofid);
		return;
	}
	de = f->dent;
	truncwait(de, id);
	wlock(de);
	if(waserror()){
		rerror(m, errmsg());
		free(*ao);
		*ao = nil;
		goto Err;
	}
	if(de->gone)
		error(Ephase);
	if((de->qid.type & QTAUTH) || (de->qid.path & Qdump))
		error(Emode);
	if(convM2D(m->stat, m->nstat, &d, strs) <= BIT16SZ)
		error(Edir);

	t = agetp(&f->mnt->root);
	n = de->Xdir;
	n.qid.vers++;
	p = opbuf+1;
	op = 0;

	/* check validity of updated fields and construct Owstat message */
	if(d.qid.path != ~0 || d.qid.vers != ~0){
		if(d.qid.path != de->qid.path)
			error(Ewstatp);
		if(d.qid.vers != de->qid.vers)
			error(Ewstatv);
	}
	if(*d.name != '\0'){
		if(strlen(d.name) > Maxname)
			error(Elength);
		if(strcmp(d.name, de->name) != 0){
			rename = 1;
			if((e = okname(d.name)) != nil)
				error(e);
			if(walk1(t, f->dent->up, d.name, &old, &oldlen) == 0)
				error(Eexist);
			n.name = d.name;
		}
	}
	if(d.length != ~0){
		if(d.length < 0)
			error(Ewstatl);
		if(d.length != de->length){
			if(d.length < de->length){
				if((*ao = malloc(sizeof(Amsg))) == nil)
					error(Enomem);
				qlock(&de->trunclk);
				de->trunc = 1;
				qunlock(&de->trunclk);
				ainc(&de->ref);
				ainc(&f->mnt->ref);
				(*ao)->op = AOclear;
				(*ao)->mnt = f->mnt;
				(*ao)->qpath = f->qpath;
				(*ao)->off = d.length;
				(*ao)->end = f->dent->length;
				(*ao)->dent = de;
			}
			de->length = d.length;
			n.length = d.length;
			op |= Owsize;
			PACK64(p, n.length);
			p += 8;
		}
	}
	if(d.mode != ~0){
		if((d.mode^de->mode) & DMDIR)
			error(Ewstatd);
		if(d.mode & ~(DMDIR|DMAPPEND|DMEXCL|DMTMP|0777))
			error(Ewstatb);
		if(d.mode != de->mode){
			n.mode = d.mode;
			n.qid.type = d.mode>>24;
			op |= Owmode;
			PACK32(p, n.mode);
			p += 4;
		}
	}
	if(d.mtime != ~0){
		n.mtime = d.mtime*Nsec;
		if(n.mtime != de->mtime){
			op |= Owmtime;
			PACK64(p, n.mtime);
			p += 8;
		}
	}
	if(*d.uid != '\0'){
		if(strlen(d.uid) > Maxuname)
			error(Elength);
		rlock(&fs->userlk);
		u = name2user(d.uid);
		if(u == nil){
			runlock(&fs->userlk);
			error(Enouser);
		}
		n.uid = u->id;
		runlock(&fs->userlk);
		if(n.uid != de->uid){
			op |= Owuid;
			PACK32(p, n.uid);
			p += 4;
		}
	}
	if(*d.gid != '\0'){
		if(strlen(d.gid) > Maxuname)
			error(Elength);
		rlock(&fs->userlk);
		u = name2user(d.gid);
		if(u == nil){
			runlock(&fs->userlk);
			error(Enogrp);
		}
		n.gid = u->id;
		runlock(&fs->userlk);
		if(n.gid != de->gid){
			op |= Owgid;
			PACK32(p, n.gid);
			p += 4;
		}
	}
	op |= Owmuid;
	n.muid = f->uid;
	PACK32(p, n.muid);
	p += 4;

	/* check permissions */
	if(rename)
		if(fsaccess(f, f->dmode, f->duid, f->dgid, DMWRITE) == -1)
			error(Eperm);
	if(op & Owsize)
		if(fsaccess(f, de->mode, de->uid, de->gid, DMWRITE) == -1)
			error(Eperm);
	if(op & (Owmode|Owmtime))
		if(!f->permit && f->uid != de->uid && !groupleader(f->uid, de->gid))
			error(Ewstato);
	if(op & Owuid)
		if(!f->permit)
			error(Ewstatu);
	if(op & Owgid)
		if(!f->permit
		&& !(f->uid == de->uid && ingroup(f->uid, n.gid))
		&& !(groupleader(f->uid, de->gid) && groupleader(f->uid, n.gid)))
			error(Ewstatg);

	/* update directory entry */
	nm = 0;
	if(rename && !de->gone){
		mb[nm].op = Oclobber;
		mb[nm].Key = de->Key;
		mb[nm].v = nil;
		mb[nm].nv = 0;
		nm++;
	
		mb[nm].op = Oinsert;
		dir2kv(f->pqpath, &n, &mb[nm], rnbuf, sizeof(rnbuf));
		k = mb[nm].Key;
		nm++;

		if(de->qid.type & QTDIR){
			packsuper(upbuf, sizeof(upbuf), f->qpath);
			mb[nm].op = Oinsert;
			mb[nm].k = upbuf;
			mb[nm].nk = Upksz;
			mb[nm].v = mb[nm-1].k;
			mb[nm].nv = mb[nm-1].nk;
			nm++;
		}
		touch(f->dir, &mb[nm++]);
	}else{
		opbuf[0] = op;
		mb[nm].op = Owstat;
		mb[nm].Key = de->Key;
		mb[nm].v = opbuf;
		mb[nm].nv = p - opbuf;
		nm++;
	}
	assert(nm <= nelem(mb));
	upsert(f->mnt, mb, nm);

	de->Xdir = n;
	if(rename)
		cpkey(de, &k, de->buf, sizeof(de->buf));

	r.type = Rwstat;
	respond(m, &r);
	poperror();

Err:	wunlock(de);
	putfid(f);
}


static void
fsclunk(Fmsg *m, Amsg **ao)
{
	Fcall r;
	Fid *f;

	if((f = getfid(m->conn, m->fid)) == nil){
		rerror(m, Enofid);
		return;
	}
	lock(f);
	clunkfid(m->conn, f, ao);
	unlock(f);
	r.type = Rclunk;
	respond(m, &r);
	putfid(f);
}

static void
fscreate(Fmsg *m)
{
	char *p, *e, buf[Kvmax], upkbuf[Keymax], upvbuf[Inlmax];
	int nm, duid, dgid, dmode;
	Dent *de;
	vlong oldlen;
	Qid old;
	Fcall r;
	Msg mb[3];
	Fid *f;
	Xdir d;

	if((e = okname(m->name)) != nil){
		rerror(m, e);
		return;
	}
	if(m->perm & (DMMOUNT|DMAUTH)){
		rerror(m, Ebotch);
		return;
	}
	if((f = getfid(m->conn, m->fid)) == nil){
		rerror(m, Enofid);
		return;
	}
	lock(f);

	if(waserror()){
		rerror(m, errmsg());
		goto Err;
		
	}
	if(f->mode != -1){
		rerror(m, Einuse);
		goto Out;
	}
	de = f->dent;
	if(walk1(f->mnt->root, f->qpath, m->name, &old, &oldlen) == 0){
		rerror(m, Eexist);
		goto Out;
	}

	rlock(de);
	if(fsaccess(f, de->mode, de->uid, de->gid, DMWRITE) == -1){
		rerror(m, Eperm);
		runlock(de);
		goto Out;
	}
	duid = de->uid;
	dgid = de->gid;
	dmode = de->mode;
	runlock(de);

	nm = 0;
	d.qid.type = 0;
	if(m->perm & DMDIR)
		d.qid.type |= QTDIR;
	if(m->perm & DMAPPEND)
		d.qid.type |= QTAPPEND;
	if(m->perm & DMEXCL)
		d.qid.type |= QTEXCL;
	if(m->perm & DMTMP)
		d.qid.type |= QTTMP;
	d.qid.path = aincv(&fs->nextqid, 1);
	d.qid.vers = 0;
	d.mode = m->perm;
	if(m->perm & DMDIR)
		d.mode &= ~0777 | de->mode & 0777;
	else
		d.mode &= ~0666 | de->mode & 0666;
	d.name = m->name;
	d.atime = nsec();
	d.mtime = d.atime;
	d.length = 0;
	d.uid = f->uid;
	d.gid = dgid;
	d.muid = f->uid;

	mb[nm].op = Oinsert;
	dir2kv(f->qpath, &d, &mb[nm], buf, sizeof(buf));
	nm++;

	if(m->perm & DMDIR){
		mb[nm].op = Oinsert;
		if((p = packsuper(upkbuf, sizeof(upkbuf), d.qid.path)) == nil)
			sysfatal("ream: pack super");
		mb[nm].k = upkbuf;
		mb[nm].nk = p - upkbuf;
		if((p = packdkey(upvbuf, sizeof(upvbuf), f->qpath, d.name)) == nil)
			sysfatal("ream: pack super");
		mb[nm].v = upvbuf;
		mb[nm].nv = p - upvbuf;
		nm++;
	}
	touch(f->dent, &mb[nm++]);
	assert(nm <= nelem(mb));
	upsert(f->mnt, mb, nm);

	de = getdent(f->mnt, f->qpath, &d);
	clunkdent(f->mnt, f->dent);
	f->mode = mode2bits(m->mode);
	f->pqpath = f->qpath;
	f->qpath = d.qid.path;
	f->dent = de;
	f->duid = duid;
	f->dgid = dgid;
	f->dmode = dmode;
	if(m->mode & ORCLOSE)
		f->rclose = emalloc(sizeof(Amsg), 1);

	r.type = Rcreate;
	r.qid = d.qid;
	r.iounit = f->iounit;
	respond(m, &r);
Out:	poperror();
Err:	unlock(f);
	putfid(f);
	return;
}

static char*
candelete(Fid *f)
{
	char *e, pfx[Dpfxsz];
	Tree *t;
	Scan s;

	if(!(f->dent->qid.type & QTDIR))
		return nil;

	t = agetp(&f->mnt->root);
	packdkey(pfx, sizeof(pfx), f->qpath, nil);
	btnewscan(&s, pfx, sizeof(pfx));
	btenter(t, &s);
	if(btnext(&s, &s.kv))
		e = Enempty;
	else
		e = nil;
	btexit(&s);
	return e;
}

static void
fsremove(Fmsg *m, int id, Amsg **ao)
{
	char *e, buf[Kvmax];
	Fcall r;
	int nm;
	Msg mb[3];
	Tree *t;
	Kvp kv;
	Fid *f;

	if((f = getfid(m->conn, m->fid)) == nil){
		rerror(m, Enofid);
		return;
	}
	t = f->mnt->root;
	nm = 0;
	*ao = nil;
	lock(f);
	clunkfid(m->conn, f, ao);
	/* rclose files are getting removed here anyways */
	if(*ao != nil)
		f->rclose = nil;
	unlock(f);

	truncwait(f->dent, id);
	wlock(f->dent);
	if(waserror()){
		rerror(m, errmsg());
		free(*ao);
		*ao = nil;
		goto Err;
	}
	if(f->dent->gone)
		error(Ephase);
	/*
	 * we need a double check that the file is in the tree
	 * here, because the walk to the fid is done in a reader
	 * proc that can look it up in a stale version of the
	 * tree, while we clunk the dent in the mutator proc.
	 *
	 * this means we can theoretically get some deletions
	 * of files that are already gone.
	 */
	if(!btlookup(t, &f->dent->Key, &kv, buf, sizeof(buf)))
		error(Ephase);
	if((e = candelete(f)) != nil)
		error(e);
	if(fsaccess(f, f->dmode, f->duid, f->dgid, DMWRITE) == -1)
		error(Eperm);
	lock(f);
	mb[nm].op = Odelete;
	mb[nm].k = f->dent->k;
	mb[nm].nk = f->dent->nk;
	mb[nm].v = "\0";
	mb[nm].nv = 1;
	nm++;
	unlock(f);

	if(f->dent->qid.type & QTDIR){
		packsuper(buf, sizeof(buf), f->qpath);
		mb[nm].op = Oclobber;
		mb[nm].k = buf;
		mb[nm].nk = Upksz;
		mb[nm].nv = 0;
		nm++;
	}else{
		if(*ao == nil)
			*ao = emalloc(sizeof(Amsg), 1);
		ainc(&f->mnt->ref);
		(*ao)->op = AOclear;
		(*ao)->mnt = f->mnt;
		(*ao)->qpath = f->qpath;
		(*ao)->off = 0;
		(*ao)->end = f->dent->length;
		(*ao)->dent = nil;
	}
	touch(f->dir, &mb[nm++]);
	assert(nm <= nelem(mb));
	upsert(f->mnt, mb, nm);
	f->dent->gone = 1;
	r.type = Rremove;
	respond(m, &r);
	poperror();
Err:
	wunlock(f->dent);
	putfid(f);
	return;
}

static void
fsopen(Fmsg *m, int id, Amsg **ao)
{
	char *p, *e, buf[Kvmax];
	int mbits;
	Tree *t;
	Fcall r;
	Xdir d;
	Fid *f;
	Kvp kv;
	Msg mb;

	mbits = mode2bits(m->mode);
	if((f = getfid(m->conn, m->fid)) == nil){
		rerror(m, Enofid);
		return;
	}
	if(waserror()){
		rerror(m, errmsg());
		putfid(f);
		return;
	}
	if(m->mode & OTRUNC)
		truncwait(f->dent, id);
	t = agetp(&f->mnt->root);
	if((f->qpath & Qdump) != 0){
		filldumpdir(&d);
	}else{
		if(!btlookup(t, f->dent, &kv, buf, sizeof(buf)))
			error(Esrch);
		kv2dir(&kv, &d);
	}
	wlock(f->dent);
	if(waserror()){
		wunlock(f->dent);
		nexterror();
	}
	if(f->dent->gone)
		error(Ephase);
	if(f->dent->qid.type & QTEXCL)
	if(f->dent->ref != 1)
		error(Elocked);
	if(m->mode & ORCLOSE)
		if((e = candelete(f)) != nil)
			error(e);
	if(fsaccess(f, d.mode, d.uid, d.gid, mbits) == -1)
		error(Eperm);
	f->dent->length = d.length;
	poperror();
	wunlock(f->dent);
	r.type = Ropen;
	r.qid = d.qid;
	r.iounit = f->iounit;

	lock(f);
	if(f->mode != -1){
		unlock(f);
		error(Einuse);
	}
	if((m->mode & OTRUNC) && !(f->dent->mode & DMAPPEND)){
		wlock(f->dent);

		if(waserror()){
			wunlock(f->dent);
			free(*ao);
			*ao = nil;
			nexterror();
		}
		*ao = emalloc(sizeof(Amsg), 1);
		qlock(&f->dent->trunclk);
		f->dent->trunc = 1;
		qunlock(&f->dent->trunclk);
		ainc(&f->dent->ref);
		ainc(&f->mnt->ref);
		(*ao)->op = AOclear;
		(*ao)->mnt = f->mnt;
		(*ao)->qpath = f->qpath;
		(*ao)->off = 0;
		(*ao)->end = f->dent->length;
		(*ao)->dent = f->dent;

		f->dent->muid = f->uid;
		f->dent->qid.vers++;
		f->dent->length = 0;

		mb.op = Owstat;
		p = buf;
		p[0] = Owsize|Owmuid;	p += 1;
		PACK64(p, 0);		p += 8;
		PACK32(p, f->uid);	p += 4;
		mb.k = f->dent->k;
		mb.nk = f->dent->nk;
		mb.v = buf;
		mb.nv = p - buf;

		upsert(f->mnt, &mb, 1);
		wunlock(f->dent);
		poperror();
	}
	f->mode = mode2bits(m->mode);
	if(m->mode & ORCLOSE)
		f->rclose = emalloc(sizeof(Amsg), 1);
	unlock(f);
	poperror();
	respond(m, &r);
	putfid(f);
}

static void
readsnap(Fmsg *m, Fid *f, Fcall *r)
{
	char pfx[1], *p;
	int n, ns;
	Scan *s;
	Xdir d;

	s = f->scan;
	if(s != nil && s->offset != 0 && s->offset != m->offset)
		error(Edscan);
	if(s == nil || m->offset == 0){
		s = emalloc(sizeof(Scan), 1);
		pfx[0] = Klabel;
		btnewscan(s, pfx, 1);
		lock(f);
		if(f->scan != nil){
			free(f->scan);
		}
		f->scan = s;
		unlock(f);
	}
	if(s->donescan){
		r->count = 0;
		return;
	}
	p = r->data;
	n = m->count;
	filldumpdir(&d);
	if(s->overflow){
		memcpy(d.name, s->kv.k+1, s->kv.nk-1);
		d.name[s->kv.nk-1] = 0;
		d.qid.path = UNPACK64(s->kv.v + 1);
		if((ns = dir2statbuf(&d, p, n)) == -1){
			r->count = 0;
			return;
		}
		s->overflow = 0;
		p += ns;
		n -= ns;
	}
	btenter(&fs->snap, s);
	while(1){
		if(!btnext(s, &s->kv))
			break;
		memcpy(d.name, s->kv.k+1, s->kv.nk-1);
		d.name[s->kv.nk-1] = 0;
		d.qid.path = UNPACK64(s->kv.v + 1);
		if((ns = dir2statbuf(&d, p, n)) == -1){
			s->overflow = 1;
			break;
		}
		p += ns;
		n -= ns;
	}
	btexit(s);
	r->count = p - r->data;
	return;
}

static void
readdir(Fmsg *m, Fid *f, Fcall *r)
{
	char pfx[Dpfxsz], *p;
	int n, ns;
	Tree *t;
	Scan *s;

	s = f->scan;
	t = agetp(&f->mnt->root);
	if(s != nil && s->offset != 0 && s->offset != m->offset)
		error(Edscan);
	if(s == nil || m->offset == 0){
		s = emalloc(sizeof(Scan), 1);
		packdkey(pfx, sizeof(pfx), f->qpath, nil);
		btnewscan(s, pfx, sizeof(pfx));
		lock(f);
		if(f->scan != nil)
			free(f->scan);
		f->scan = s;
		unlock(f);
	}
	if(s->donescan){
		r->count = 0;
		return;
	}
	p = r->data;
	n = m->count;
	if(s->overflow){
		if((ns = kv2statbuf(&s->kv, p, n)) == -1){
			r->count = 0;
			return;
		}
		s->overflow = 0;
		p += ns;
		n -= ns;
	}
	btenter(t, s);
	while(1){
		if(!btnext(s, &s->kv))
			break;
		if((ns = kv2statbuf(&s->kv, p, n)) == -1){
			s->overflow = 1;
			break;
		}
		p += ns;
		n -= ns;
	}
	btexit(s);
	r->count = p - r->data;
}

static void
readfile(Fmsg *m, Fid *f, Fcall *r)
{
	vlong n, c, o;
	char *p;
	Dent *e;
	Tree *t;

	e = f->dent;
	rlock(e);
	if(m->offset > e->length){
		runlock(e);
		return;
	}
	p = r->data;
	c = m->count;
	o = m->offset;
	t = agetp(&f->mnt->root);
	if(m->offset + m->count > e->length)
		c = e->length - m->offset;
	while(c != 0){
		n = readb(t, f, p, o, c, e->length);
		r->count += n;
		if(n == 0)
			break;
		p += n;
		o += n;
		c -= n;
	}
	runlock(e);
}

static void
fsread(Fmsg *m)
{
	Fcall r;
	Fid *f;

	if((f = getfid(m->conn, m->fid)) == nil){
		rerror(m, Enofid);
		return;
	}
	r.type = Rread;
	r.count = 0;
	r.data = nil;
	if(waserror()){
		rerror(m, errmsg());
		free(r.data);
		putfid(f);
		return;
	}	
	r.data = emalloc(m->count, 0);
	if(f->dent->qid.type & QTAUTH)
		authread(f, &r, r.data, m->count);
	else if(f->dent->qid.path == Qdump)
		readsnap(m, f, &r);
	else if(f->dent->qid.type & QTDIR)
		readdir(m, f, &r);
	else
		readfile(m, f, &r);
	respond(m, &r);
	free(r.data);
	poperror();
	putfid(f);
}

static void
fswrite(Fmsg *m, int id)
{
	char sbuf[Wstatmax], kbuf[Max9p/Blksz+2][Offksz], vbuf[Max9p/Blksz+2][Ptrsz];
	Bptr bp[Max9p/Blksz + 2];
	Msg kv[Max9p/Blksz + 2];
	vlong n, o, c, w;
	int i, j;
	char *p;
	Fcall r;
	Tree *t;
	Fid *f;

	if((f = getfid(m->conn, m->fid)) == nil){
		rerror(m, Enofid);
		return;
	}
	if(!(f->mode & DMWRITE)){
		rerror(m, Einuse);
		putfid(f);
		return;
	}
	truncwait(f->dent, id);
	wlock(f->dent);
	if(waserror()){
		rerror(m, errmsg());
		wunlock(f->dent);
		putfid(f);
		return;
	}
	if(f->dent->gone)
		error(Ephase);
	if(f->dent->qid.type & QTAUTH){
		authwrite(f, &r, m->data, m->count);
		goto Out;
	}	

	w = 0;
	p = m->data;
	o = m->offset;
	c = m->count;
	if(f->dent->mode & DMAPPEND)
		o = f->dent->length;
	t = agetp(&f->mnt->root);
	for(i = 0; c != 0; i++){
		assert(i < nelem(kv));
		assert(i == 0 || o%Blksz == 0);
		kv[i].op = Oinsert;
		kv[i].k = kbuf[i];
		kv[i].nk = sizeof(kbuf[i]);
		kv[i].v = vbuf[i];
		kv[i].nv = sizeof(vbuf[i]);
		if(waserror()){
			if(!fs->rdonly)
				for(j = 0; j < i; j++)
					freebp(t, bp[j]);
			nexterror();
		}
		n = writeb(f, &kv[i], &bp[i], p, o, c, f->dent->length);
		poperror();
		w += n;
		p += n;
		o += n;
		c -= n;
	}

	p = sbuf;
	kv[i].op = Owstat;
	kv[i].k = f->dent->k;
	kv[i].nk = f->dent->nk;
	*p++ = 0;
	if(o > f->dent->length){ 
		sbuf[0] |= Owsize;
		PACK64(p, o);
		p += 8;
		f->dent->length = o;
	}
	sbuf[0] |= Owmtime;
	f->dent->mtime = nsec();
	PACK64(p, f->dent->mtime);
	p += 8;
	sbuf[0] |= Owmuid;
	PACK32(p, f->uid);
	p += 4;

	kv[i].v = sbuf;
	kv[i].nv = p - sbuf;
	upsert(f->mnt, kv, i+1);

	r.type = Rwrite;
	r.count = w;
Out:
	poperror();
 	respond(m, &r);
	wunlock(f->dent);
	putfid(f);	
}

void
fsflush(Fmsg *m)
{
	Fcall r;

	r.type = Rflush;
	respond(m, &r);
}

Conn *
newconn(int rfd, int wfd, int cfd)
{
	Conn *c;

	if((c = mallocz(sizeof(*c), 1)) == nil)
		return nil;

	c->rfd = rfd;
	c->wfd = wfd;
	c->cfd = cfd;

	c->iounit = Max9p;

	c->ref = 1;

	lock(&fs->connlk);
	c->next = fs->conns;
	fs->conns = c;
	unlock(&fs->connlk);

	return c;
}

void
putconn(Conn *c)
{
	Conn **pp;
	Amsg *a;
	Fid *f, *nf;
	int i;

	if(adec(&c->ref) != 0)
		return;

	lock(&fs->connlk);
	for(pp = &fs->conns; *pp != nil; pp = &((*pp)->next)){
		if(*pp == c){
			*pp = c->next;
			break;
		}
	}
	unlock(&fs->connlk);

	close(c->rfd);
	if(c->rfd != c->wfd)
		close(c->wfd);
	if(c->cfd >= 0)
		close(c->cfd);

	for(i = 0; i < Nfidtab; i++){
		lock(&c->fidtablk[i]);
		for(f = c->fidtab[i]; f != nil; f = nf){
			nf = f->next;
			ainc(&f->ref);
			lock(f);
			a = nil;
			clunkfid(c, f, &a);
			unlock(f);
			putfid(f);
			if(a != nil)
				chsend(fs->admchan, a);
		}
		unlock(&c->fidtablk[i]);
	}
	free(c);
}

void
runfs(int, void *pc)
{
	char err[128];
	RWLock *lk;
	Amsg *a;
	Conn *c;
	Fcall r;
	Fmsg *m;
	u32int h;

	c = pc;
	while(!c->hangup){
		if(readmsg(c, &m) < 0){
			fshangup(c, "read message: %r");
			break;
		}
		if(m == nil)
			break;
		if(convM2S(m->buf, m->sz, m) == 0){
			fshangup(c, "invalid message: %r");
			break;
		}
		if(m->type != Tversion && !c->versioned){
			fshangup(c, "version required");
			break;
		}
		dprint("← %F\n", &m->Fcall);

		if(m->type == Tflush){
			lk = &fs->flushq[ihash(m->oldtag) % Nflushtab];
			wlock(lk);
		}else{
			lk = &fs->flushq[ihash(m->tag) % Nflushtab];
			rlock(lk);
		}

		a = nil;
		h = ihash(m->fid) % fs->nreaders;
		switch(m->type){
		/* sync setup, must not access tree */
		case Tversion:	fsversion(m);	break;
		case Tauth:	fsauth(m);	break;
		case Tflush:	fsflush(m);	break;
		case Tclunk:	fsclunk(m, &a);	break;

		/* mutators */
		case Tcreate:	chsend(fs->wrchan, m);	break;
		case Twrite:	chsend(fs->wrchan, m);	break;
		case Twstat:	chsend(fs->wrchan, m);	break;
		case Tremove:	chsend(fs->wrchan, m);	break;

		/* reads */
		case Tattach:	chsend(fs->rdchan[h], m);	break;
		case Twalk:	chsend(fs->rdchan[h], m);	break;
		case Tread:	chsend(fs->rdchan[h], m);	break;
		case Tstat:	chsend(fs->rdchan[h], m);	break;

		/* both */
		case Topen:
			if((m->mode & OTRUNC) || (m->mode & ORCLOSE) != 0)
				chsend(fs->wrchan, m);
			else
				chsend(fs->rdchan[h], m);
			break;

		default:
			fprint(2, "unknown message %F\n", &m->Fcall);
			snprint(err, sizeof(err), "unknown message: %F", &m->Fcall);
			r.type = Rerror;
			r.ename = err;
			respond(m, &r);
			break;
		}
		assert(estacksz() == 0);
		if(a != nil)
			chsend(fs->admchan, a);
	}
	putconn(c);
}

void
runmutate(int id, void *)
{
	Fmsg *m;
	Amsg *a;
	Fid *f;

	while(1){
		a = nil;
		m = chrecv(fs->wrchan);
		if(fs->rdonly){
			/*
			 * special case: even if Tremove fails, we need
			 * to clunk the fid.
			 */
			if(m->type == Tremove){
				if((f = getfid(m->conn, m->fid)) == nil){
					rerror(m, Enofid);
					continue;
				}
				lock(f);
				clunkfid(m->conn, f, &a);
				/* read only: ignore rclose */
				f->rclose = nil;
				unlock(f);
				free(a);
				putfid(f);
			}
			rerror(m, Erdonly);
			continue;
 		}

		qlock(&fs->mutlk);
		epochstart(id);
		fs->snap.dirty = 1;
		switch(m->type){
		case Tcreate:	fscreate(m);		break;
		case Twrite:	fswrite(m, id);		break;
		case Twstat:	fswstat(m, id, &a);	break;
		case Tremove:	fsremove(m, id, &a);	break;
		case Topen:	fsopen(m, id, &a);	break;
		default:	abort();		break;
		}
		assert(estacksz() == 0);
		epochend(id);
		qunlock(&fs->mutlk);
		epochclean();

		if(a != nil)
			chsend(fs->admchan, a);
	}
}

void
runread(int id, void *ch)
{
	Fmsg *m;

	while(1){
		m = chrecv(ch);
		epochstart(id);
		switch(m->type){
		case Tattach:	fsattach(m);		break;
		case Twalk:	fswalk(m);		break;
		case Tread:	fsread(m);		break;
		case Tstat:	fsstat(m);		break;
		case Topen:	fsopen(m, id, nil);	break;
		}
		assert(estacksz() == 0);
		epochend(id);
	}
}

void
freetree(Bptr rb, vlong pred)
{
	Bptr bp;
	Blk *b;
	Kvp kv;
	int i;

	b = getblk(rb, 0);
	if(b->type == Tpivot){
		for(i = 0; i < b->nval; i++){
			getval(b, i, &kv);
			bp = unpackbp(kv.v, kv.nv);
			freetree(bp, pred);
			qlock(&fs->mutlk);
			qunlock(&fs->mutlk);
			epochclean();
		}
	}
	if(rb.gen > pred)
		freebp(nil, rb);
	dropblk(b);
}

/*
 * Here, we clean epochs frequently, but we run outside of
 * an epoch; this is because the caller of this function
 * has already waited for an epoch to tick over, there's
 * nobody that can be accessing the tree other than us,
 * and we just need to keep the limbo list short.
 *
 * Because this is the last reference to the tree, we don't
 * need to hold the mutlk, other than when we free or kill
 * blocks via epochclean.
 */
void
sweeptree(Tree *t)
{
	char pfx[1];
	Scan s;
	Bptr bp;
	pfx[0] = Kdat;
	btnewscan(&s, pfx, 1);
	btenter(t, &s);
	while(1){
		if(!btnext(&s, &s.kv))
			break;
		bp = unpackbp(s.kv.v, s.kv.nv);
		if(bp.gen > t->pred)
			freebp(nil, bp);
		qlock(&fs->mutlk);
		qunlock(&fs->mutlk);
		epochclean();
	}
	btexit(&s);
	freetree(t->bp, t->pred);
}

void
runsweep(int id, void*)
{
	char buf[Kvmax];
	Msg mb[Kvmax/Offksz];
	Bptr bp, nb, *oldhd;
	int i, nm;
	vlong off;
	Tree *t;
	Arena *a;
	Amsg *am;
	Blk *b;

	if((oldhd = calloc(fs->narena, sizeof(Bptr))) == nil)
		sysfatal("malloc log heads");
	while(1){
		am = chrecv(fs->admchan);
		switch(am->op){
		case AOsync:
			tracem("syncreq");
			if(!fs->snap.dirty && !am->halt)
				goto Next;
			if(agetl(&fs->rdonly))
				goto Justhalt;
			if(waserror()){
				fprint(2, "sync error: %s\n", errmsg());
				ainc(&fs->rdonly);
				break;
			}

			if(am->halt)
				ainc(&fs->rdonly);
			for(i = 0; i < fs->narena; i++){
				a = &fs->arenas[i];
				oldhd[i].addr = -1;
				oldhd[i].hash = -1;
				oldhd[i].gen = -1;
				qlock(a);
				/*
				 * arbitrary heuristic -- try compressing
				 * when the log doubles in size.
				 */
				if(a->nlog >= 2*a->lastlogsz){
					oldhd[i] = a->loghd;
					epochstart(id);
					if(waserror()){
						epochend(id);
						qunlock(a);
						nexterror();
					}
					compresslog(a);
					epochend(id);
					poperror();
				}
				qunlock(a);
				epochclean();
			}
			sync();

			for(i = 0; i < fs->narena; i++){
				for(bp = oldhd[i]; bp.addr != -1; bp = nb){
					qlock(&fs->mutlk);
					epochstart(id);
					b = getblk(bp, 0);
					nb = b->logp;
					freeblk(nil, b);
					dropblk(b);
					epochend(id);
					qunlock(&fs->mutlk);
					epochclean();
				}
			}

Justhalt:
			if(am->halt){
				assert(fs->snapdl.hd.addr == -1);
				assert(fs->snapdl.tl.addr == -1);
				postnote(PNGROUP, getpid(), "halted");
				exits(nil);
			}
			poperror();
			break;

		case AOsnap:
			tracem("snapreq");
			if(agetl(&fs->rdonly)){
				fprint(2, "snap on read only fs");
				goto Next;
			}
			if(waserror()){
				fprint(2, "taking snap: %s\n", errmsg());
				ainc(&fs->rdonly);
				break;
			}

			qlock(&fs->mutlk);
			if(waserror()){
				qunlock(&fs->mutlk);
				nexterror();
			}
			epochstart(id);
			snapfs(am, &t);
			epochend(id);
			poperror();
			qunlock(&fs->mutlk);

			sync();

			if(t != nil){
				epochwait();
				sweeptree(t);
				closesnap(t);
			}
			poperror();
			break;

		case AOrclose:
			if(agetl(&fs->rdonly)){
				fprint(2, "rclose on read only fs");
				goto Next;
			}
			nm = 0;
			mb[nm].op = Odelete;
			mb[nm].k = am->dent->k;
			mb[nm].nk = am->dent->nk;
			mb[nm].nv = 0;
			nm++;
			if(am->dent->qid.type & QTDIR){
				packsuper(buf, sizeof(buf), am->qpath);
				mb[nm].op = Oclobber;
				mb[nm].k = buf;
				mb[nm].nk = Upksz;
				mb[nm].nv = 0;
				nm++;
			}
			qlock(&fs->mutlk);
			upsert(am->mnt, mb, nm);
			qunlock(&fs->mutlk);
			/* fallthrough */
		case AOclear:
			if(agetl(&fs->rdonly)){
				fprint(2, "clear on read only fs");
				goto Next;
			}
			tracem("bgclear");
			if(waserror()){
				fprint(2, "clear file %llx: %s\n", am->qpath, errmsg());
				ainc(&fs->rdonly);
				break;
			}
			if(am->dent != nil)
				qlock(&am->dent->trunclk);
			fs->snap.dirty = 1;
			nm = 0;
			for(off = am->off; off < am->end; off += Blksz){
				mb[nm].op = Oclearb;
				mb[nm].k = buf + Offksz * nm;
				mb[nm].nk = Offksz;
				mb[nm].k[0] = Kdat;
				PACK64(mb[nm].k+1, am->qpath);
				PACK64(mb[nm].k+9, off);
				mb[nm].v = nil;
				mb[nm].nv = 0;
				if(++nm >= nelem(mb) || off + Blksz >= am->end){
					qlock(&fs->mutlk);
					if(waserror()){
						qunlock(&fs->mutlk);
						nexterror();
					}
					epochstart(id);
					upsert(am->mnt, mb, nm);
					epochend(id);
					qunlock(&fs->mutlk);
					epochclean();
					poperror();
					nm = 0;
				}
			}
			if(am->dent != nil){
				am->dent->trunc = 0;
				rwakeup(&am->dent->truncrz);
				qunlock(&am->dent->trunclk);
				clunkdent(am->mnt, am->dent);
			}
			clunkmount(am->mnt);
			poperror();
			break;
		}
Next:
		assert(estacksz() == 0);
		free(am);
	}
}

void
snapmsg(char *old, char *new, int flg)
{
	Amsg *a;

	a = emalloc(sizeof(Amsg), 1);
	a->op = AOsnap;
	a->fd = -1;
	a->flag = flg;
	strecpy(a->old, a->old+sizeof(a->old), old);
	if(new == nil)
		a->delete = 1;
	else
		strecpy(a->new, a->new+sizeof(a->new), new);
	chsend(fs->admchan, a);
}

void
runtasks(int, void *)
{
	char buf[128];
	Tm now, then;
	Mount *mnt;
	int m, h;
	Amsg *a;

	m = 0;
	h = 0;
	tmnow(&then, nil);
	tmnow(&now, nil);
	while(1){
		sleep(5000);
		if(fs->rdonly)
			continue;
		if(waserror()){
			fprint(2, "task error: %s\n", errmsg());
			continue;
		}
		a = emalloc(sizeof(Amsg), 1);
		a->op = AOsync;
		a->halt = 0;
		a->fd = -1;
		chsend(fs->admchan, a);

		tmnow(&now, nil);
		for(mnt = agetp(&fs->mounts); mnt != nil; mnt = mnt->next){
			if(!(mnt->flag & Ltsnap))
				continue;
			if(now.yday != then.yday){
				snprint(buf, sizeof(buf),
					"%s@day.%τ", mnt->name, tmfmt(&now, "YYYY.MM.DD[_]hh:mm:ss"));
				snapmsg(mnt->name, buf, Lauto);
			}
			if(now.hour != then.hour){
				if(mnt->hourly[h][0] != 0)
					snapmsg(mnt->hourly[h], nil, 0);
				snprint(mnt->hourly[h], sizeof(mnt->hourly[h]),
					"%s@hour.%τ", mnt->name, tmfmt(&now, "YYYY.MM.DD[_]hh:mm:ss"));
				snapmsg(mnt->name, mnt->hourly[h], Lauto);
			}
			if(now.min != then.min){
				if(mnt->minutely[m][0] != 0)
					snapmsg(mnt->minutely[m], nil, 0);
				snprint(mnt->minutely[m], sizeof(mnt->minutely[m]),
					"%s@minute.%τ", mnt->name, tmfmt(&now, "YYYY.MM.DD[_]hh:mm:ss"));
				snapmsg(mnt->name, mnt->minutely[m], Lauto);
			}
		}
		if(now.hour != then.hour)
			h = (h+1)%24;
		if(now.min != then.min)
			m = (m+1)%60;
		then = now;
		poperror();
	}
}

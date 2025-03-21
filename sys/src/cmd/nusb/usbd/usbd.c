#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>
#include "usb.h"
#include "dat.h"
#include "fns.h"

enum {
	Qroot,
	Qusbevent,
	Qusbhubctl,
	Qmax
};

char *names[] = {
	"",
	"usbevent",
	"usbhubctl",
};

static char Enonexist[] = "file does not exist";
static char Eperm[] = "permission denied";

typedef struct Event Event;

struct Event {
	Dev *dev;	/* the device producing the event,
			   dev->aux points to Fid processing the event */
	char *data;
	int len;
	Event *link;
	int ref;	/* number of readers which will read this one
			   the next time they'll read */
	int prev;	/* number of events pointing to this one with
			   their link pointers */
};

static Event *evlast;
static Req *reqfirst, *reqlast;
static QLock evlock;

static void
addreader(Req *req)
{
	req->aux = nil;
	if(reqfirst == nil)
		reqfirst = req;
	else
		reqlast->aux = req;
	reqlast = req;
}

static void
fulfill(Req *req, Event *e)
{
	int n;
	
	n = e->len;
	if(n > req->ifcall.count)
		n = req->ifcall.count;
	memmove(req->ofcall.data, e->data, n);
	req->ofcall.count = n;
}

static void
initevent(void)
{
	evlast = emallocz(sizeof(Event), 1);
}

static Event*
putevent(Event *e)
{
	Event *ee;

	ee = e->link;
	if(e->ref || e->prev)
		return ee;
	ee->prev--;
	closedev(e->dev);
	free(e->data);
	free(e);
	return ee;
}

static void
procreqs(void)
{
	Req *r, *p, *x;
	Event *e;
	Fid *f;

Loop:
	for(p = nil, r = reqfirst; r != nil; p = r, r = x){
		x = r->aux;
		f = r->fid;
		e = f->aux;
		if(e == evlast)
			continue;
		if(e->dev->aux == f){
			e->dev->aux = nil;	/* release device */
			e->ref--;
			e = putevent(e);
			e->ref++;
			f->aux = e;
			goto Loop;
		}
		if(e->dev->aux == nil){
			e->dev->aux = f;	/* claim device */
			if(x == nil)
				reqlast = p;
			if(p == nil)
				reqfirst = x;
			else
				p->aux = x;
			r->aux = nil;
			fulfill(r, e);
			respond(r, nil);
			goto Loop;
		}
	}
}

static void
pushevent(Dev *d, char *data)
{
	Event *e;
	
	qlock(&evlock);
	e = evlast;
	evlast = emallocz(sizeof(Event), 1);
	incref(d);
	e->dev = d;
	e->data = data;
	e->len = strlen(data);
	e->link = evlast;
	evlast->prev++;
	procreqs();
	putevent(e);
	qunlock(&evlock);
}

static int
dirgen(int n, Dir *d, void *)
{
	if(n >= Qmax - 1)
		return -1;
	d->qid.path = n + 1;
	d->qid.vers = 0;
	switch((ulong)d->qid.path){
	case Qroot:
		d->qid.type = QTDIR;
		d->mode = 0555 | DMDIR;
		break;
	case Qusbevent:
		d->qid.type = 0;
		d->mode = 0444;
		break;
	case Qusbhubctl:
		d->qid.type = 0;
		d->mode = 0222;
		break;
	}
	d->uid = estrdup9p(getuser());
	d->gid = estrdup9p(d->uid);
	d->muid = estrdup9p(d->uid);
	d->name = estrdup9p(names[n+1]);
	d->atime = d->mtime = time(0);
	d->length = 0;
	return 0;
}

static void
usbdattach(Req *req)
{
	req->fid->qid = (Qid) {Qroot, 0, QTDIR};
	req->ofcall.qid = req->fid->qid;
	respond(req, nil);
}

static char *
usbdwalk(Fid *fid, char *name, Qid *qid)
{
	int i;

	if(fid->qid.path != Qroot)
		return "not a directory";
	if(strcmp(name, "..") == 0){
		*qid = fid->qid;
		return nil;
	}
	for(i = Qroot+1; i < Qmax; i++)
		if(strcmp(name, names[i]) == 0){
			fid->qid = (Qid) {i, 0, 0};
			*qid = fid->qid;
			return nil;
		}
	return Enonexist;
}

static void
usbdread(Req *req)
{
	switch((long)req->fid->qid.path){
	case Qroot:
		dirread9p(req, dirgen, nil);
		respond(req, nil);
		break;
	case Qusbevent:
		qlock(&evlock);
		addreader(req);
		procreqs();
		qunlock(&evlock);
		break;
	default:
		respond(req, Enonexist);
		break;
	}
}

static void
usbdwrite(Req *req)
{
	extern QLock hublock;
	extern Hub *hubs;
	Hub *hub;
	Cmdbuf *cb;
	char hubid[16];
	int port, feature, on;

	if((long)req->fid->qid.path != Qusbhubctl){
		respond(req, Enonexist);
		return;
	}
	cb = parsecmd(req->ifcall.data, req->ifcall.count);
	if(cb->nf < 4){
		respond(req, "not enough arguments");
		goto out;
	}
	if(strcmp(cb->f[0], "portpower") == 0)
		feature = Fportpower;
	else if(strcmp(cb->f[0], "portindicator") == 0)
		feature = Fportindicator;
	else {
		respond(req, "unknown feature");
		goto out;
	}
	port = atoi(cb->f[2]);
	if(strcmp(cb->f[3], "on") == 0)
		on = 1;
	else if(strcmp(cb->f[3], "off") == 0)
		on = 0;
	else
		on = atoi(cb->f[3]) != 0;

	qlock(&hublock);
	for(hub = hubs; hub != nil; hub = hub->next){
		if(hub->dev->hname != nil && strcmp(hub->dev->hname, cb->f[1]) == 0)
			break;
		snprint(hubid, sizeof(hubid), "%d", hub->dev->id);
		if(strcmp(hubid, cb->f[1]) == 0)
			break;
	}
	if(hub == nil){
		qunlock(&hublock);
		respond(req, "unknown hub");
		goto out;
	}
	if(port < 1 || port > hub->nport){
		qunlock(&hublock);
		respond(req, "unknown port");
		goto out;
	}
	if(feature == Fportpower && hub->pwrmode != 1
	|| feature == Fportindicator && !hub->leds){
		qunlock(&hublock);
		respond(req, "not supported");
		goto out;
	}
	portfeature(hub, port, feature, on);
	qunlock(&hublock);
	req->ofcall.count = req->ifcall.count;
	respond(req, nil);
out:
	free(cb);
}

static void
usbdstat(Req *req)
{
	if(dirgen(req->fid->qid.path - 1, &req->d, nil) < 0)
		respond(req, Enonexist);
	else
		respond(req, nil);
}

static char *
formatdev(Dev *d, int type)
{
	Usbdev *u = d->usb;
	return smprint("%s %d %.4x %.4x %.6lx %s\n",
		type ? "detach" : "attach",
		d->id, u->vid, u->did, u->csp, 
		d->hname != nil ? d->hname : "");
}

static void
enumerate(Event **l)
{
	extern Hub *hubs;

	Event *e;
	Hub *h;
	Port *p;
	Dev *d;
	int i;
	
	for(h = hubs; h != nil; h = h->next){
		for(i = 1; i <= h->nport; i++){
			p = &h->port[i];
			d = p->dev;
			if(d == nil || d->usb == nil || p->hub != nil)
				continue;
			e = emallocz(sizeof(Event), 1);
			incref(d);
			e->dev = d;
			e->data = formatdev(d, 0);
			e->len = strlen(e->data);
			e->prev = 1;
			*l = e;
			l = &e->link;

		}
	}
	*l = evlast;
	evlast->prev++;
}

static void
usbdopen(Req *req)
{
	extern QLock hublock;
	Event *e;

	switch((ulong)req->fid->qid.path){
	case Qusbevent:
		if(req->ifcall.mode != OREAD){
			respond(req, Eperm);
			return;
		}
		qlock(&hublock);
		qlock(&evlock);

		enumerate(&e);
		e->prev--;
		e->ref++;
		req->fid->aux = e;

		qunlock(&evlock);
		qunlock(&hublock);
		break;
	case Qusbhubctl:
		if((req->ifcall.mode&~OTRUNC) != OWRITE){
			respond(req, Eperm);
			return;
		}
		break;
	}
	respond(req, nil);
}

static void
usbddestroyfid(Fid *fid)
{
	if(fid->qid.path == Qusbevent){
		Event *e;

		qlock(&evlock);
		e = fid->aux;
		if(e != nil){
			fid->aux = nil;
			if(e->dev != nil && e->dev->aux == fid){
				e->dev->aux = nil;	/* release device */
				procreqs();
			}
			e->ref--;
			while(e->ref == 0 && e->prev == 0 && e != evlast)
				e = putevent(e);
		}
		qunlock(&evlock);
	}
}

static void
usbdflush(Req *req)
{
	Req *r, *p, *x;

	qlock(&evlock);
	for(p = nil, r = reqfirst; r != nil; p = r, r = x){
		x = r->aux;
		if(r == req->oldreq){
			if(x == nil)
				reqlast = p;
			if(p == nil)
				reqfirst = x;
			else
				p->aux = x;
			r->aux = nil;
			respond(r, "interrupted");
			break;
		}
	}
	qunlock(&evlock);
	respond(req, nil);
}

static void
usbdstart(Srv*)
{
	switch(rfork(RFPROC|RFMEM|RFNOWAIT)){
	case -1: sysfatal("rfork: %r");
	case 0: work(); exits(nil);
	}
}

static void
usbdend(Srv*)
{
	postnote(PNGROUP, getpid(), "shutdown");
}

Srv usbdsrv = {
	.start = usbdstart,
	.end = usbdend,
	.attach = usbdattach,
	.walk1 = usbdwalk,
	.read = usbdread,
	.write = usbdwrite,
	.stat = usbdstat,
	.open = usbdopen,
	.flush = usbdflush,
	.destroyfid = usbddestroyfid,
};

void
assignhname(Dev *dev)
{
	extern Hub *hubs;
	char buf[64];
	Usbdev *ud;
	Hub *h;
	int col, nr;
	int i, n;

	ud = dev->usb;

	/* build string of device unique stuff */
	snprint(buf, sizeof(buf), "%.4x%.4x%.4x%.6lx%s",
		ud->vid, ud->did, ud->dno, ud->csp, ud->serial);

	n = hname(buf);

	/* check for collisions */
	col = 0;
	for(h = hubs; h != nil; h = h->next){
		if(ud->class == Clhub){
			if(h->dev->hname == nil)
				continue;
			if(strncmp(h->dev->hname, buf, n) == 0){
				nr = atoi(h->dev->hname+n)+1;
				if(nr > col)
					col = nr;
			}
			continue;
		}
		for(i = 1; i <= h->nport; i++){
			if(h->port[i].dev == nil)
				continue;
			if(h->port[i].dev->hname == nil || h->port[i].dev == dev)
				continue;
			if(strncmp(h->port[i].dev->hname, buf, n) == 0){
				nr = atoi(h->port[i].dev->hname+n)+1;
				if(nr > col)
					col = nr;
			}
		}
	}

	if(col == 0)
		dev->hname = strdup(buf);
	else
		dev->hname = smprint("%s%d", buf, col);
}

void
attachdev(Dev *d)
{
	int id;

	/*
	 * create all endpoint files for default conf #1
	 * but do not create endpoints that have an altsetting.
	 */
	for(id=1; id<nelem(d->usb->ep); id++){
		Ep *ep = d->usb->ep[id];
		if(ep != nil
		&& ep->next == nil
		&& ep->conf != nil
		&& ep->conf->cval == 1
		&& ep->iface->alt == 0){
			Dev *epd = openep(d, ep);
			if(epd != nil)
				closedev(epd);
		}
	}
	pushevent(d, formatdev(d, 0));
}

void
detachdev(Dev *d)
{
	pushevent(d, formatdev(d, 1));
}

/*
 * we create /env/usbbusy on startup and once all devices have been
 * enumerated and readers have consumed all the events, we remove the
 * file so nusbrc can continue.
 */
static int busyfd = -1;

void
checkidle(void)
{
	if(busyfd < 0 || reqlast == nil || evlast == nil || evlast->prev > 0)
		return;

	close(busyfd);
	busyfd = -1;
}

void
main(int argc, char **argv)
{
	int fd, i, nd;
	char *fn;
	Dir *d;

	ARGBEGIN {
	case 'D':
		chatty9p++;
		break;
	case 'd':
		usbdebug++;
		break;
	} ARGEND;

	quotefmtinstall();

	/* for usbdebug */
	fmtinstall('U', Ufmt);
	fmtinstall('H', encodefmt);

	initevent();

	hubs = nil;
	if(argc == 0){
		if((fd = open("/dev/usb", OREAD)) < 0)
			sysfatal("/dev/usb: %r");
		nd = dirreadall(fd, &d);
		close(fd);
		for(i = 0; i < nd; i++){
			if(strcmp(d[i].name, "ctl") == 0)
				continue;
			fn = smprint("/dev/usb/%s", d[i].name);
			newhub(fn, nil);
			free(fn);
		}
		free(d);
	}else {
		for(i = 0; i < argc; i++)
			newhub(argv[i], nil);
	}

	if(hubs == nil)
		sysfatal("no hubs");

	busyfd = create("/env/usbbusy", ORCLOSE, 0600);
	postsharesrv(&usbdsrv, nil, "usb", "usbd");
	exits(nil);
}

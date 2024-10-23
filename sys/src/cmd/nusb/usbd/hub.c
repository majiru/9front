#include <u.h>
#include <libc.h>
#include <thread.h>
#include "usb.h"
#include "dat.h"
#include "fns.h"

Hub *hubs;
QLock hublock;
static int nhubs;
static int mustdump;
static int pollms = Pollms;
static ulong nowms;

int
portfeature(Hub *h, int port, int f, int on)
{
	int cmd;

	if(on)
		cmd = Rsetfeature;
	else
		cmd = Rclearfeature;
	return usbcmd(h->dev, Rh2d|Rclass|Rother, cmd, f, port, nil, 0);
}

static void*
getdesc(Usbdev *d, uchar typ)
{
	int i;

	for(i = 0; i < nelem(d->ddesc); i++){
		if(d->ddesc[i] == nil)
			break;
		if(d->ddesc[i]->data.bDescriptorType == typ)
			return &d->ddesc[i]->data;
	}
	return nil;
}

static int
configusb2hub(Hub *h, DHub *dd, int nr)
{
	uchar *PortPwrCtrlMask;
	int i, offset, mask, nmap;
	Port *pp;

	h->nport = dd->bNbrPorts;
	if(h->nport < 1 || h->nport > 127){
		fprint(2, "%s: %s: bad port count %d for a hub\n",
			argv0, h->dev->dir, h->nport);
		return -1;
	}
	nmap = 1 + h->nport/8;
	if(nr < 7 + 2*nmap){
		fprint(2, "%s: %s: descr. too small\n", argv0, h->dev->dir);
		return -1;
	}
	h->port = emallocz((h->nport+1)*sizeof(Port), 1);
	h->pwrms = dd->bPwrOn2PwrGood*2;
	if(h->pwrms < Powerdelay)
		h->pwrms = Powerdelay;
	h->maxcurrent = dd->bHubContrCurrent;
	h->pwrmode = dd->wHubCharacteristics[0] & 3;
	h->compound = (dd->wHubCharacteristics[0] & (1<<2))!=0;
	h->ttt = (dd->wHubCharacteristics[0] >> 5) & 3;
	h->leds = (dd->wHubCharacteristics[0] & (1<<7)) != 0;
	PortPwrCtrlMask = dd->DeviceRemovable + nmap;
	for(i = 1; i <= h->nport; i++){
		pp = &h->port[i];
		offset = i/8;
		mask = 1<<(i%8);
		pp->removable = (dd->DeviceRemovable[offset] & mask) != 0;
		pp->pwrctl = (PortPwrCtrlMask[offset] & mask) != 0;
	}
	h->mtt = h->dev->usb->ver == 0x0200 && Proto(h->dev->usb->csp) == 2;
	if(h->mtt){	/* try enable multi TT */
		if(usbcmd(h->dev, Rh2d|Rstd|Riface, Rsetiface, 1, 0, nil, 0) < 0){
			fprint(2, "%s: %s: setifcace (mtt): %r\n", argv0, h->dev->dir);
			h->mtt = 0;
		}
	}
	return 0;
}

static int
configusb3hub(Hub *h, DSSHub *dd, int)
{
	int i, offset, mask;
	Port *pp;

	h->nport = dd->bNbrPorts;
	if(h->nport < 1 || h->nport > 15){
		fprint(2, "%s: %s: bad port count %d for a usb3 hub\n",
			argv0, h->dev->dir, h->nport);
		return -1;
	}
	h->port = emallocz((h->nport+1)*sizeof(Port), 1);
	h->pwrms = dd->bPwrOn2PwrGood*2;
	if(h->pwrms < Powerdelay)
		h->pwrms = Powerdelay;
	h->maxcurrent = dd->bHubContrCurrent;
	h->pwrmode = dd->wHubCharacteristics[0] & 3;
	h->compound = (dd->wHubCharacteristics[0] & (1<<2))!=0;
	h->ttt = 0;
	h->leds = 0;
	for(i = 1; i <= h->nport; i++){
		pp = &h->port[i];
		offset = i/8;
		mask = 1<<(i%8);
		pp->removable = (dd->DeviceRemovable[offset] & mask) != 0;
	}
	h->mtt = 0;
	if(usbcmd(h->dev, Rh2d|Rclass|Rdev, Rsethubdepth, h->dev->depth, 0, nil, 0) < 0){
		fprint(2, "%s: %s: sethubdepth: %r\n", argv0, h->dev->dir);
		return -1;
	}
	return 0;
}

static int
confighub(Hub *h)
{
	int dt, dl, nr;
	uchar buf[128];
	void *dd;

	if(h->dev->isusb3){
		dt = Dsshub;
		dl = Dsshublen;
	} else {
		dt = Dhub;
		dl = Dhublen;
	}
	dd = getdesc(h->dev->usb, dt);
	if(dd == nil){
		nr = usbcmd(h->dev, Rd2h|Rclass|Rdev, Rgetdesc, dt<<8|0, 0, buf, sizeof buf);
		if(nr < 0){
			fprint(2, "%s: %s: getdesc hub: %r\n", argv0, h->dev->dir);
			return -1;
		}
		dd = buf;
	} else {
		nr = ((DDesc*)dd)->bLength;
	}
	if(nr < dl){
		fprint(2, "%s: %s: hub descriptor too small (%d < %d)\n", argv0, h->dev->dir, nr, dl);
		return -1;
	}
	if(h->dev->isusb3)
		return configusb3hub(h, dd, nr);
	else
		return configusb2hub(h, dd, nr);
}

static void
configroothub(Hub *h)
{
	char buf[1024];
	char *p;
	int nr;
	Dev *d;

	d = h->dev;
	h->nport = 2;
	h->maxpkt = 8;
	seek(d->cfd, 0, 0);
	nr = read(d->cfd, buf, sizeof(buf)-1);
	if(nr < 0)
		goto Done;
	buf[nr] = 0;
	d->isusb3 = strstr(buf, "speed super") != nil;
	p = strstr(buf, "ports ");
	if(p == nil)
		fprint(2, "%s: %s: no port information\n", argv0, d->dir);
	else
		h->nport = atoi(p+6);
	p = strstr(buf, "maxpkt ");
	if(p == nil)
		fprint(2, "%s: %s: no maxpkt information\n", argv0, d->dir);
	else
		h->maxpkt = atoi(p+7);
Done:
	h->port = emallocz((h->nport+1)*sizeof(Port), 1);
	dprint(2, "%s: %s: ports %d maxpkt %d\n", argv0, d->dir, h->nport, h->maxpkt);
}

Hub*
newhub(char *fn, Dev *d)
{
	Hub *h;
	int i;
	Usbdev *ud;

	h = emallocz(sizeof(Hub), 1);
	if(d == nil){
		h->dev = opendev(fn);
		if(h->dev == nil){
			fprint(2, "%s: %s: opendev: %r\n", argv0, fn);
			goto Fail;
		}
		h->dev->depth = -1;
		configroothub(h);	/* never fails */
		devctl(h->dev, "info roothub csp %#08ux ports %d", 0x000009, h->nport);
	}else{
		incref(d);
		h->dev = d;
		if(confighub(h) < 0)
			goto Fail;

		/* close control endpoint so we can re-configure as a hub */
		close(d->dfd);
		d->dfd = -1;

		if(devctl(d, "hub %d %d %d", h->nport, h->ttt, h->mtt) < 0){
			fprint(2, "%s: %s: devctl hub: %r\n", argv0, fn);
			if(devctl(d, "hub") < 0)	/* try old kernel */
				goto Fail;
		}

		ud = d->usb;
		devctl(d, "info hub csp %#08ulx ports %d vid %#.4ux did %#.4ux %q %q %s",
			ud->csp, h->nport, ud->vid, ud->did, ud->vendor, ud->product, d->hname);
	}
	if(opendevdata(h->dev, ORDWR) < 0){
		fprint(2, "%s: %s: opendevdata: %r\n", argv0, fn);
		goto Fail;
	}
	if(d != nil){
		for(i = 1; i <= h->nport; i++)
			if(portfeature(h, i, Fportpower, 1) < 0)
				fprint(2, "%s: %s: power: %r\n", argv0, fn);
		sleep(h->pwrms);
		if(h->leds){
			for(i = 1; i <= h->nport; i++)
				portfeature(h, i, Fportindicator, 1);
		}
	}
	h->next = hubs;
	hubs = h;
	nhubs++;
	dprint(2, "%s: %s: hub %#p allocated:", argv0, fn, h);
	dprint(2, " ports %d pwrms %d max curr %d pwrm %d cmp %d leds %d\n",
		h->nport, h->pwrms, h->maxcurrent,
		h->pwrmode, h->compound, h->leds);
	return h;
Fail:
	closedev(h->dev);
	free(h->port);
	free(h);
	return nil;
}

static void portdetach(Hub *h, int p);

/*
 * If during enumeration we get an I/O error the hub is gone or
 * in pretty bad shape. Because of retries of failed usb commands
 * (and the sleeps they include) it can take a while to detach all
 * ports for the hub. This detaches all ports and makes the hub void.
 * The parent hub will detect a detach (probably right now) and
 * close it later.
 */
static void
hubfail(Hub *h)
{
	int i;

	for(i = 1; i <= h->nport; i++)
		portdetach(h, i);
	h->failed = 1;
}

static void
closehub(Hub *h)
{
	Hub **hl;

	dprint(2, "%s: %s: closing hub %#p\n", argv0, h->dev->dir, h);
	for(hl = &hubs; *hl != nil; hl = &(*hl)->next)
		if(*hl == h)
			break;
	if(*hl == nil)
		sysfatal("closehub: no hub");
	*hl = h->next;
	nhubs--;
	hubfail(h);		/* detach all ports */
	closedev(h->dev);
	free(h->port);
	free(h);
}

static u32int
portstatus(Hub *h, int p)
{
	Dev *d;
	uchar buf[4];
	u32int sts;
	int t;
	int dbg;

	dbg = usbdebug;
	if(dbg != 0 && dbg < 4)
		usbdebug = 1;	/* do not be too chatty */
	d = h->dev;
	t = Rd2h|Rclass|Rother;
	if(usbcmd(d, t, Rgetstatus, 0, p, buf, sizeof(buf)) < 0)
		sts = -1;
	else
		sts = GET4(buf);
	usbdebug = dbg;
	return sts;
}

static char*
stsstr(int sts, int isusb3)
{
	static char s[80];
	char *e;

	e = s;
	if(sts&PSpresent)
		*e++ = 'p';
	if(sts&PSenable)
		*e++ = 'e';
	if(sts&PSovercurrent)
		*e++ = 'o';
	if(sts&PSreset)
		*e++ = 'r';
	if(!isusb3){
		if(sts&PSslow)
			*e++ = 'l';
		if(sts&PShigh)
			*e++ = 'h';
		if(sts&PSchange)
			*e++ = 'c';
		if(sts&PSstatuschg)
			*e++ = 's';
		if(sts&PSsuspend)
			*e++ = 'z';
	}
	if(e == s)
		*e++ = '-';
	*e = 0;
	return s;
}

static int
getmaxpkt(Dev *d)
{
	uchar buf[8];
	DDev *dd;

	if(d->isusb3)
		return 512;
	dd = (DDev*)buf;
	if(usbcmd(d, Rd2h|Rstd|Rdev, Rgetdesc, Ddev<<8|0, 0, buf, sizeof(buf)) != sizeof(buf))
		return -1;
	return dd->bMaxPacketSize0;
}

/*
 * BUG: does not consider max. power avail.
 */
static int
portattach(Hub *h, int p, u32int sts)
{
	Dev *d;
	Port *pp;
	Dev *nd;
	char fname[80];
	char buf[40];
	char *sp;
	int mp;
	int nr;
	int i;

	d = h->dev;
	pp = &h->port[p];
	if(pp->state != Pdisabled)
		return -1;

	/*
	 * prevent repeated attaches in short succession as it is a indication
	 * for a reset loop or a very flanky device.
	 */
	if(pp->acount && nowms - pp->atime >= Attachdelay)
		pp->acount = 0;
	pp->atime = nowms;
	if(++pp->acount > Attachcount){
		fprint(2, "%s: %s: port %d: too many attaches in short succession\n",
			argv0, d->dir, p);
		return -1;
	}

	dprint(2, "%s: %s: port %d attach sts %#ux\n", argv0, d->dir, p, sts);
	if(h->dev->isusb3){
		sleep(Enabledelay);
		sts = portstatus(h, p);
		if(sts == -1 || (sts & PSenable) == 0){
			dprint(2, "%s: %s: port %d: not enabled?\n", argv0, d->dir, p);
			return -1;
		}
		sp = "super";
	} else {
		sleep(Enabledelay);
		if(portfeature(h, p, Fportreset, 1) < 0){
			dprint(2, "%s: %s: port %d: reset: %r\n", argv0, d->dir, p);
			return -1;
		}
		sleep(Resetdelay);
		sts = portstatus(h, p);
		if(sts == -1 || (sts & PSenable) == 0){
			dprint(2, "%s: %s: port %d: not enabled?\n", argv0, d->dir, p);
			portfeature(h, p, Fportenable, 1);
			sts = portstatus(h, p);
			if(sts == -1 || (sts & PSenable) == 0)
				return -1;
		}
		sp = "full";
		if(sts & PSslow)
			sp = "low";
		if(sts & PShigh)
			sp = "high";
	}
	pp->sts = sts;
	pp->state = Pattached;
	dprint(2, "%s: %s: port %d: attached status %#ux, speed %s\n", argv0, d->dir, p, sts, sp);

	if(devctl(d, "newdev %s %d", sp, p) < 0){
		fprint(2, "%s: %s: port %d: newdev: %r\n", argv0, d->dir, p);
		return -1;
	}
	seek(d->cfd, 0, 0);
	nr = read(d->cfd, buf, sizeof(buf)-1);
	if(nr <= 0){
		if(nr == 0) werrstr("eof");
		fprint(2, "%s: %s: port %d: newdev: %r\n", argv0, d->dir, p);
		return -1;
	}
	buf[nr] = 0;
	snprint(fname, sizeof(fname), "/dev/usb/%s", buf);
	nd = opendev(fname);
	if(nd == nil){
		fprint(2, "%s: %s: port %d: opendev: %r\n", argv0, d->dir, p);
		return -1;
	}
	pp->dev = nd;
	nd->depth = h->dev->depth+1;
	nd->isusb3 = h->dev->isusb3;
	if(usbdebug > 2)
		devctl(nd, "debug 1");
	for(i=1;; i++){
		if(opendevdata(nd, ORDWR) >= 0)
			break;
		if(i >= 10){
			fprint(2, "%s: %s: opendevdata: %r\n", argv0, nd->dir);
			return -1;
		}
		sts = portstatus(h, p);
		if(sts == -1 || (sts & PSenable) == 0)
			return -1;
		sleep(i*50);
	}

	/*
	 * for xhci, this command is ignored by the driver as the device address
	 * has already been assigned by the controller firmware.
	 */
	if(usbcmd(nd, Rh2d|Rstd|Rdev, Rsetaddress, nd->id&0x7f, 0, nil, 0) < 0){
		dprint(2, "%s: %s: port %d: setaddress: %r\n", argv0, d->dir, p);
		return -1;
	}
	if(devctl(nd, "address") < 0){
		dprint(2, "%s: %s: port %d: set address: %r\n", argv0, d->dir, p);
		return -1;
	}

	mp=getmaxpkt(nd);
	if(mp < 0){
		dprint(2, "%s: %s: port %d: getmaxpkt: %r\n", argv0, d->dir, p);
		return -1;
	}
	dprint(2, "%s; %s: port %d: maxpkt %d\n", argv0, d->dir, p, mp);

	/* force reopen in configdev() after setting maxpkt */
	close(nd->dfd);
	nd->dfd = -1;

	devctl(nd, "maxpkt %d", mp);

	if(configdev(nd) < 0){
		dprint(2, "%s: %s: port %d: configdev: %r\n", argv0, d->dir, p);
		return -1;
	}

	/*
	 * We always set conf #1. BUG.
	 */
	if(usbcmd(nd, Rh2d|Rstd|Rdev, Rsetconf, 1, 0, nil, 0) < 0){
		dprint(2, "%s: %s: port %d: setconf: %r\n", argv0, d->dir, p);
		if(usbcmd(nd, Rh2d|Rstd|Rdev, Rsetconf, 1, 0, nil, 0) < 0)
			return -1;
	}

	pp->state = Pconfigured;
	dprint(2, "%s: %s: port %d: configured: %s\n", argv0, d->dir, p, nd->dir);

	/* assign stable name based on device descriptor */
	assignhname(nd);

	/*
	 * Hubs are handled directly by this process avoiding
	 * concurrent operation so that at most one device
	 * has the config address in use.
	 */
	if(nd->usb->class == Clhub){
		pp->hub = newhub(nd->dir, nd);
		if(pp->hub == nil)
			return -1;

		return 0;
	}

	/* close control endpoint so driver can open it */
	close(nd->dfd);
	nd->dfd = -1;

	/* set device info for ctl file */
	devctl(nd, "info %s csp %#08lux vid %#.4ux did %#.4ux %q %q %s",
		classname(Class(nd->usb->csp)), nd->usb->csp, nd->usb->vid, nd->usb->did,
		nd->usb->vendor, nd->usb->product, nd->hname);

	/* notify driver */
	attachdev(nd);

	return 0;
}

static void
portdetach(Hub *h, int p)
{
	Dev *d;
	Port *pp;
	d = h->dev;
	pp = &h->port[p];

	if(pp->state == Pdisabled)
		return;
	dprint(2, "%s: %s: port %d: detached\n", argv0, d->dir, p);
	if(pp->dev != nil){
		devctl(pp->dev, "detach");

		if(pp->state == Pconfigured
		&& pp->dev->usb->class != Clhub)
			detachdev(pp->dev);

		closedev(pp->dev);
		pp->dev = nil;
	}
	if(pp->hub != nil){
		closehub(pp->hub);
		pp->hub = nil;
	}
	pp->state = Pdisabled;
}

static void
portfail(Hub *h, int p, char *what)
{
	dprint(2, "%s: %s: port %d: failed: %s\n", argv0, h->dev->dir, p, what);
	portdetach(h, p);
	if(!h->dev->isusb3)
		portfeature(h, p, Fportenable, 0);
}

static int
portresetwanted(Hub *h, int p)
{
	char buf[5];
	Port *pp;
	Dev *nd;

	pp = &h->port[p];
	nd = pp->dev;
	if(nd != nil && nd->cfd >= 0 && pread(nd->cfd, buf, 5, 0LL) == 5)
		return memcmp(buf, "reset", 5) == 0;
	else
		return 0;
}

static int
portgone(Port *pp, u32int sts)
{
	if(sts == -1)
		return 1;
	/*
	 * If it was enabled and it's not now then it may be reconnect.
	 * We pretend it's gone and later we'll see it as attached.
	 */
	if((pp->sts & PSenable) != 0 && (sts & PSenable) == 0)
		return 1;
	return (pp->sts & PSpresent) != 0 && (sts & PSpresent) == 0;
}

static int
enumhub(Hub *h, int p)
{
	Dev *d;
	Port *pp;
	u32int sts;
	int onhubs;

	if(h->failed)
		return 0;
	d = h->dev;
	if(usbdebug > 3)
		fprint(2, "%s: %s: port %d enumhub\n", argv0, d->dir, p);

	pp = &h->port[p];
	if(portresetwanted(h, p)){
		pp->sts = 0;	/* pretend its gone to force a new attach */
		portfail(h, p, "reset");
	}

	sts = portstatus(h, p);
	if(sts == -1){
		hubfail(h);		/* avoid delays on detachment */
		return -1;
	}
	if(!h->dev->isusb3){
		if((sts & PSsuspend) != 0){
			if(portfeature(h, p, Fportsuspend, 0) < 0)
				dprint(2, "%s: %s: port %d: unsuspend: %r\n", argv0, d->dir, p);
			sleep(Enabledelay);
			sts = portstatus(h, p);
			if(sts == -1){
				hubfail(h);
				return -1;
			}
			fprint(2, "%s: %s: port %d: unsuspended sts: %s %#ux\n", argv0, d->dir, p,
				stsstr(sts, h->dev->isusb3), sts);
		}
	}

	onhubs = nhubs;

	if(portgone(pp, sts)){
		pp->sts = sts;
		portdetach(h, p);
	} else if((pp->sts & PSpresent) == 0 && (sts & PSpresent) != 0){
		pp->sts = sts;
		if(portattach(h, p, sts) < 0)
			portfail(h, p, "attach");
	} else if(pp->sts != sts){
		dprint(2, "%s: %s port %d: sts %s %#ux ->",
			argv0, d->dir, p, stsstr(pp->sts, h->dev->isusb3), pp->sts);
		dprint(2, " %s %#ux\n", stsstr(sts, h->dev->isusb3), sts);
		pp->sts = sts;
	}

	return onhubs != nhubs;
}

static void
dump(void)
{
	Hub *h;
	int i;

	mustdump = 0;
	for(h = hubs; h != nil; h = h->next)
		for(i = 1; i <= h->nport; i++)
			fprint(2, "%s: hub %#p %s port %d: %U\n",
				argv0, h, h->dev->dir, i, h->port[i].dev);
}

void
work(void)
{
	Hub *h;
	int i;

	/*
	 * Enumerate (and acknowledge after first enumeration).
	 * Do NOT perform enumeration concurrently for the same
	 * controller. new devices attached respond to a default
	 * address (0) after reset, thus enumeration has to work
	 * one device at a time at least before addresses have been
	 * assigned.
	 * Do not use hub interrupt endpoint because we
	 * have to poll the root hub(s) in any case.
	 */
	for(;;nowms += pollms){
		qlock(&hublock);
Again:
		for(h = hubs; h != nil; h = h->next)
			for(i = 1; i <= h->nport; i++)
				if(enumhub(h, i)){
					/* changes in hub list; repeat */
					goto Again;
				}
		qunlock(&hublock);
		checkidle();
		sleep(pollms);
		if(mustdump)
			dump();
	}
}

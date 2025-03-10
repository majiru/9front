#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include "ip.h"
#include "ipv6.h"

#define DPRINT if(0)print

enum {
	Maxmedia	= 32,
	Nself		= Maxmedia*5,
	NHASH		= 1<<6,
	NCACHE		= 256,
	QMAX		= 192*1024-1,
};

static Medium *media[Maxmedia] = { 0 };
void (*multicastreportfn)(Fs*, Ipifc*, uchar*, uchar*, int);

/*
 *  cache of local addresses (addresses we answer to)
 */
struct Ipself
{
	uchar	a[IPaddrlen];
	Ipself	*next;		/* next address in the hash table */
	Iplink	*link;		/* binding twixt Ipself and Ipifc */
	ulong	expire;
	uchar	type;		/* type of address */
};

struct Ipselftab
{
	QLock;
	int	inited;
	int	acceptall;	/* true if an interface has the null address */
	Ipself	*hash[NHASH];	/* hash chains */
};

/* quick hash for ip addresses */
#define hashipa(a) (((a)[IPaddrlen-2] + (a)[IPaddrlen-1])%NHASH)

static char tifc[] = "ifc ";

static void	addselfcache(Fs *f, Ipifc *ifc, Iplifc *lifc, uchar *a, int type);
static void	delselfcache(Fs *f, Ipifc *ifc, Iplifc *lifc, uchar *a);
static void	ipifcregisteraddr(Fs*, Ipifc*, Iplifc*, uchar*);
static void	ipifcregisterproxy(Fs*, Ipifc*, uchar*, int);
static char*	ipifcdellifc(Ipifc*, Iplifc**);

static char Ebound[] = "interface already bound";
static char Eunbound[] = "interface not bound";

enum {
	unknownv6,		/* UGH */
	unspecifiedv6,
	linklocalv6,
	ulav6,
	globalv6,
};

static int
v6addrtype(uchar *addr)
{
	if(isv4(addr) || ipcmp(addr, IPnoaddr) == 0)
		return unknownv6;
	else if(islinklocal(addr) || ipcmp(addr, v6loopback) == 0 ||
	    isv6mcast(addr) && (addr[1] & 0xF) <= Link_local_scop)
		return linklocalv6;
	else if(isula(addr))
		return ulav6;
	else
		return globalv6;
}

static int
comprefixlen(uchar *a, uchar *b, int n)
{
	int i, c;

	for(i = 0; i < n; i++){
		if((c = a[i] ^ b[i]) == 0)
			continue;
		for(i <<= 3; (c & 0x80) == 0; i++)
			c <<= 1;
		return i;
	}
	return i << 3;
}

/*
 *  link in a new medium
 */
void
addipmedium(Medium *med)
{
	int i;

	for(i = 0; i < nelem(media)-1; i++)
		if(media[i] == nil){
			media[i] = med;
			return;
		}
	panic("addipmedium: too many mediums");
}

/*
 *  find the medium with this name
 */
Medium*
ipfindmedium(char *name)
{
	Medium **mp;

	for(mp = media; *mp != nil; mp++)
		if(strcmp((*mp)->name, name) == 0)
			break;
	return *mp;
}

/* same as nullmedium, to prevent unbind while bind or unbind is in progress */
extern Medium unboundmedium;

/*
 *  attach a device (or pkt driver) to the interface.
 *  called with c locked
 */
static char*
ipifcbind(Conv *c, char **argv, int argc)
{
	Ipifc *ifc;
	Medium *m;

	if(argc < 2)
		return Ebadarg;

	/* bind the device to the interface */
	m = ipfindmedium(argv[1]);
	if(m == nil)
		return "unknown interface type";

	ifc = (Ipifc*)c->ptcl;
	wlock(ifc);
	if(ifc->m != nil){
		wunlock(ifc);
		return Ebound;
	}
	ifc->m = &unboundmedium;	/* fake until bound */
	ifc->arg = nil;

	/* set the bound device name */
	if(argc > 2)
		strncpy(ifc->dev, argv[2], sizeof(ifc->dev));
	else
		snprint(ifc->dev, sizeof ifc->dev, "%s%d", m->name, c->x);
	ifc->dev[sizeof(ifc->dev)-1] = 0;

	/* set up parameters */
	ifc->maxtu = m->maxtu;
	ifc->delay = 40;
	ifc->speed = 0;

	/* default router paramters */
	ifc->rp = c->p->f->v6p->rp;
	wunlock(ifc);

	if(waserror()){
		wlock(ifc);
		ifc->m = nil;
		wunlock(ifc);
		nexterror();
	}
	(*m->bind)(ifc, argc, argv);
	poperror();

	wlock(ifc);
	/* switch to real medium */
	ifc->m = m;
	if(m->unbindonclose == 0)
		c->inuse++;

	/* any ancillary structures (like routes) no longer pertain */
	flushrouteifc(c->p->f, ifc);

	/* reopen all the queues closed by a previous unbind */
	qreopen(c->rq);
	qreopen(c->eq);
	qreopen(c->sq);
	wunlock(ifc);

	return nil;
}

/*
 *  detach a medium from an interface, close the interface
 *  ifc->conv is locked, ifc is wlocked
 */
static char*
ipifcunbindmedium(Ipifc *ifc, Medium *m)
{
	Conv *c = ifc->conv;

	if(m == nil || m == &unboundmedium)
		return Eunbound;

	/* disassociate logical interfaces */
	while(ifc->lifc != nil)
		ipifcdellifc(ifc, &ifc->lifc);

	/* disassociate device */
	if(m->unbind != nil){
		ifc->m = &unboundmedium;	/* fake until unbound */
		wunlock(ifc);
		qunlock(c);
		if(!waserror()){
			(*m->unbind)(ifc);
			poperror();
		}
		qlock(c);
		wlock(ifc);
	}

	memset(ifc->dev, 0, sizeof(ifc->dev));

	ifc->reflect = 0;
	ifc->reassemble = 0;

	/* close queues to stop queuing of packets */
	qclose(c->rq);
	qclose(c->wq);
	qclose(c->sq);

	/* dissociate routes */
	flushrouteifc(c->p->f, ifc);
	ifc->m = nil;
	ifc->arg = nil;

	if(m->unbindonclose == 0)
		c->inuse--;

	return nil;
}

/*
 *  called from Ipifcproto->unbind,
 *  with ifc->conv locked.
 */
static char*
ipifcunbind(Ipifc *ifc)
{
	char *err;

	wlock(ifc);
	err = ipifcunbindmedium(ifc, ifc->m);
	wunlock(ifc);

	return err;
}

/*
 *  called from medium at any time to unbind
 *  an interface in case of an error such as
 *  usb ethernet being un-plugged.
 */
char*
mediumunbindifc(Ipifc *ifc)
{
	Medium *m;
	char *err;

	rlock(ifc);
	m = ifc->m;
	runlock(ifc);

	err = Eunbound;
	if(m != nil && m != &unboundmedium){
		qlock(ifc->conv);
		wlock(ifc);
		if(ifc->m == m)
			err = ipifcunbindmedium(ifc, m);
		wunlock(ifc);
		qunlock(ifc->conv);
	}
	return err;
}

static char sfixedformat[] = "device %s maxtu %d sendra %d recvra %d mflag %d oflag %d"
" maxraint %d minraint %d linkmtu %d reachtime %d rxmitra %d ttl %d routerlt %d"
" pktin %lud pktout %lud errin %lud errout %lud speed %d delay %d\n";

static char slineformat[] = "	%-40I %-10M %-40I %-12lud %-12lud\n";

static int
ipifcstate(Conv *c, char *state, int n)
{
	Ipifc *ifc;
	Iplifc *lifc;
	int m;

	ifc = (Ipifc*)c->ptcl;
	rlock(ifc);
	m = snprint(state, n, sfixedformat,
		ifc->dev, ifc->maxtu, ifc->sendra6, ifc->recvra6,
		ifc->rp.mflag, ifc->rp.oflag, ifc->rp.maxraint,
		ifc->rp.minraint, ifc->rp.linkmtu, ifc->rp.reachtime,
		ifc->rp.rxmitra, ifc->rp.ttl, ifc->rp.routerlt,
		ifc->in, ifc->out, ifc->inerr, ifc->outerr,
		ifc->speed, ifc->delay);

	for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next)
		m += snprint(state+m, n - m, slineformat, lifc->local,
			lifc->mask, lifc->remote, lifc->validlt, lifc->preflt);
	if(ifc->lifc == nil)
		m += snprint(state+m, n - m, "\n");
	runlock(ifc);
	return m;
}

static int
ipifclocal(Conv *c, char *state, int n)
{
	Ipifc *ifc;
	Iplifc *lifc;
	Iplink *link;
	int m;

	ifc = (Ipifc*)c->ptcl;
	rlock(ifc);
	m = 0;
	for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next){
		m += snprint(state+m, n - m, "%-40.40I ->", lifc->local);
		for(link = lifc->link; link != nil; link = link->lifclink)
			m += snprint(state+m, n - m, " %-40.40I", link->self->a);
		m += snprint(state+m, n - m, "\n");
	}
	runlock(ifc);
	return m;
}

static int
ipifcinuse(Conv *c)
{
	Ipifc *ifc;

	ifc = (Ipifc*)c->ptcl;
	return ifc->m != nil;
}

static void
ipifcadjustburst(Ipifc *ifc)
{
	int burst;

	burst = ((vlong)ifc->delay * ifc->speed) / 8000;
	if(burst < ifc->maxtu)
		burst = ifc->maxtu;
	ifc->burst = burst;
}

/*
 *  change an interface's burst delay
 *  ifc must be bound and wlock'd
 */
static void
ipifcsetdelay(Ipifc *ifc, int delay)
{
	if(delay < 0)
		delay = 0;
	else if(delay > 1000)
		delay = 1000;
	ifc->delay = delay;
	ipifcadjustburst(ifc);
}

/*
 *  change an interface's baud rate
 *  ifc must be bound and wlock'd
 */
static void
ipifcsetspeed(Ipifc *ifc, int speed)
{
	if(speed < 0)
		speed = 0;
	ifc->speed = speed;
	ifc->load = 0;
	ipifcadjustburst(ifc);
}

/*
 *  change an interface's mtu
 *  ifc must be bound and wlock'd
 */
static char*
ipifcsetmtu(Ipifc *ifc, int mtu)
{
	if(mtu < ifc->m->mintu || mtu > ifc->m->maxtu)
		return Ebadarg;
	ifc->maxtu = mtu;
	ipifcadjustburst(ifc);
	return nil;
}

void
ipifcoput(Ipifc *ifc, Block *bp, int version, uchar *ip, Routehint *rh)
{
	if(ifc->speed){
		ulong now = MACHP(0)->ticks;
		int dt = TK2MS(now - ifc->ticks);
		ifc->ticks = now;
		ifc->load -= ((vlong)dt * ifc->speed) / 8000;
		if(ifc->load < 0 || dt < 0 || dt > 1000)
			ifc->load = 0;
		else if(ifc->load > ifc->burst){
			freeblist(bp);
			return;
		}
	}
	bp = concatblock(bp);
	ifc->load += BLEN(bp);
	ifc->m->bwrite(ifc, bp, version, ip, rh);
}


/*
 *  called when a process writes to an interface's 'data'
 */
static void
ipifckick(void *x)
{
	Conv *c = x;
	Block *bp;
	Ipifc *ifc;

	bp = qget(c->wq);
	if(bp == nil)
		return;

	ifc = (Ipifc*)c->ptcl;
	rlock(ifc);
	if(waserror()){
		runlock(ifc);
		nexterror();
	}
	if(ifc->m != nil && ifc->m->pktin != nil)
		(*ifc->m->pktin)(c->p->f, ifc, bp);
	else
		freeb(bp);
	runlock(ifc);
	poperror();
}

/*
 *  called when a new ipifc structure is created
 */
static void
ipifccreate(Conv *c)
{
	Ipifc *ifc;

	c->rq = qopen(QMAX, 0, 0, 0);
	c->wq = qopen(QMAX, Qkick, ipifckick, c);
	c->sq = qopen(QMAX, 0, 0, 0);
	if(c->rq == nil || c->wq == nil || c->sq == nil)
		error(Enomem);
	ifc = (Ipifc*)c->ptcl;
	ifc->conv = c;
	ifc->m = nil;
	ifc->reflect = 0;
	ifc->reassemble = 0;
}

/*
 *  called after last close of ipifc data or ctl
 */
static void
ipifcclose(Conv *c)
{
	Ipifc *ifc = (Ipifc*)c->ptcl;
	Medium *m = ifc->m;

	if(m != nil && m->unbindonclose)
		ipifcunbind(ifc);
}

/*
 *  add an address to an interface.
 */
char*
ipifcadd(Ipifc *ifc, char **argv, int argc, int tentative, Iplifc *lifcp)
{
	uchar ip[IPaddrlen], mask[IPaddrlen], rem[IPaddrlen];
	uchar bcast[IPaddrlen], net[IPaddrlen];
	Iplifc *lifc, **l;
	int i, type, mtu;
	Fs *f;

	mtu = 0;
	type = Rifc;
	memset(ip, 0, IPaddrlen);
	memset(mask, 0, IPaddrlen);
	memset(rem, 0, IPaddrlen);
	switch(argc){
	case 6:
		if(strstr(argv[5], "proxy") != 0)
			type |= Rproxy;
		if(strstr(argv[5], "trans") != 0)
			type |= Rtrans;
		/* fall through */
	case 5:
		mtu = strtoul(argv[4], 0, 0);
		/* fall through */
	case 4:
		if (parseipandmask(ip, mask, argv[1], argv[2]) == -1 || parseip(rem, argv[3]) == -1)
			return Ebadip;
		maskip(rem, mask, net);
		break;
	case 3:
		if (parseipandmask(ip, mask, argv[1], argv[2]) == -1)
			return Ebadip;
		maskip(ip, mask, rem);
		maskip(rem, mask, net);
		break;
	case 2:
		if (parseip(ip, argv[1]) == -1)
			return Ebadip;
		memmove(mask, defmask(ip), IPaddrlen);
		maskip(ip, mask, rem);
		maskip(rem, mask, net);
		break;
	default:
		return Ebadarg;
	}

	/* check for point-to-point interface */
	if(ipcmp(ip, v6loopback) != 0) /* skip v6 loopback, it's a special address */
	if(ipcmp(mask, IPallbits) == 0)
		type |= Rptpt;

	if(isv4(ip) || ipcmp(ip, IPnoaddr) == 0){
		type |= Rv4;
		tentative = 0;
	}

	wlock(ifc);
	if(ifc->m == nil || ifc->m == &unboundmedium){
		wunlock(ifc);
		return Eunbound;
	}
	f = ifc->conv->p->f;
	if(waserror()){
		wunlock(ifc);
		return up->errstr;
	}

	if(mtu > 0)
		ipifcsetmtu(ifc, mtu);

	/* ignore if this is already a local address for this ifc */
	if((lifc = iplocalonifc(ifc, ip)) != nil){
		if(lifcp != nil) {
			if(!lifc->onlink && lifcp->onlink){
				lifc->onlink = 1;
				addroute(f, lifc->remote, lifc->mask, ip, IPallbits,
					lifc->remote, lifc->type&~Rtrans, ifc, tifc);
				if(v6addrtype(ip) != linklocalv6)
					addroute(f, lifc->remote, lifc->mask, ip, IPnoaddr,
						lifc->remote, lifc->type, ifc, tifc);
			}
			lifc->autoflag = lifcp->autoflag;
			lifc->validlt = lifcp->validlt;
			lifc->preflt = lifcp->preflt;
			lifc->origint = lifcp->origint;
		}
		if(lifc->tentative != tentative){
			lifc->tentative = tentative;
			goto done;
		}
		wunlock(ifc);
		poperror();
		return nil;
	}

	/* add the address to the list of logical ifc's for this ifc */
	lifc = smalloc(sizeof(Iplifc));
	ipmove(lifc->local, ip);
	ipmove(lifc->mask, mask);
	ipmove(lifc->remote, rem);
	ipmove(lifc->net, net);
	lifc->type = type;
	lifc->tentative = tentative;
	if(lifcp != nil) {
		lifc->onlink = lifcp->onlink;
		lifc->autoflag = lifcp->autoflag;
		lifc->validlt = lifcp->validlt;
		lifc->preflt = lifcp->preflt;
		lifc->origint = lifcp->origint;
	} else {		/* default values */
		lifc->onlink = lifc->autoflag = 1;
		lifc->validlt = lifc->preflt = ~0UL;
		lifc->origint = NOW / 1000;
	}
	lifc->next = nil;

	for(l = &ifc->lifc; *l != nil; l = &(*l)->next)
		;
	*l = lifc;

	/* add route for this logical interface */
	if(lifc->onlink){
		addroute(f, rem, mask, ip, IPallbits, rem, type&~Rtrans, ifc, tifc);
		if(v6addrtype(ip) != linklocalv6)
			addroute(f, rem, mask, ip, IPnoaddr, rem, type, ifc, tifc);
	}

	addselfcache(f, ifc, lifc, ip, Runi);

	/* register point-to-point proxy */
	if(type & Rptpt){
		if(type & Rproxy)
			ipifcregisterproxy(f, ifc, rem, 1);
		goto done;
	}

	/* register local ip if proxy */
	if(type & Rproxy)
		ipifcregisterproxy(f, ifc, ip, 1);

	if(type & Rv4) {
		/* add subnet directed broadcast address to the self cache */
		for(i = 0; i < IPaddrlen; i++)
			bcast[i] = (ip[i] & mask[i]) | ~mask[i];
		addselfcache(f, ifc, lifc, bcast, Rbcast);

		/* add subnet directed network address to the self cache */
		for(i = 0; i < IPaddrlen; i++)
			bcast[i] = (ip[i] & mask[i]) & mask[i];
		addselfcache(f, ifc, lifc, bcast, Rbcast);

		addselfcache(f, ifc, lifc, IPv4bcast, Rbcast);

		/* add all nodes multicast address */
		addselfcache(f, ifc, lifc, IPv4allsys, Rmulti);
	} else {
		if(ipcmp(ip, v6loopback) == 0) {
			/* add node-local mcast address */
			addselfcache(f, ifc, lifc, v6allnodesN, Rmulti);

			/* add route for all node multicast */
			addroute(f, v6allnodesN, v6allnodesNmask,
				ip, IPallbits,
				v6allnodesN, Rmulti, ifc, tifc);
		}

		/* add all nodes multicast address */
		addselfcache(f, ifc, lifc, v6allnodesL, Rmulti);

		/* add route for all nodes multicast */
		addroute(f, v6allnodesL, v6allnodesLmask,
			ip, IPallbits,
			v6allnodesL, Rmulti, ifc, tifc);

		/* add solicited-node multicast address */
		ipv62smcast(bcast, ip);
		addselfcache(f, ifc, lifc, bcast, Rmulti);
	}

done:
	wunlock(ifc);
	poperror();

	rlock(ifc);
	ipifcregisteraddr(f, ifc, lifc, ip);
	runlock(ifc);

	return nil;
}

/*
 *  remove a logical interface from an ifc
 *	called with ifc wlock'd
 */
static char*
ipifcdellifc(Ipifc *ifc, Iplifc **l)
{
	Iplifc *lifc = *l;
	Fs *f = ifc->conv->p->f;

	if(lifc == nil)
		return "address not on this interface";
	*l = lifc->next;

	/* disassociate any addresses */
	while(lifc->link != nil)
		delselfcache(f, ifc, lifc, lifc->link->self->a);

	/* remove the route for this logical interface */
	if(lifc->onlink){
		delroute(f, lifc->remote, lifc->mask,
			lifc->local, IPallbits,
			lifc->remote, lifc->type&~Rtrans, ifc, tifc);
		if(v6addrtype(lifc->local) != linklocalv6)
			delroute(f, lifc->remote, lifc->mask,
				lifc->local, IPnoaddr,
				lifc->remote, lifc->type, ifc, tifc);
	}

	/* unregister point-to-point proxy */
	if(lifc->type & Rptpt){
		if(lifc->type & Rproxy)
			ipifcregisterproxy(f, ifc, lifc->remote, 0);
		goto done;
	}

	/* unregister local ip if proxy */
	if(lifc->type & Rproxy)
		ipifcregisterproxy(f, ifc, lifc->local, 0);

	/* remove route for all nodes multicast */
	if((lifc->type & Rv4) == 0){
		if(ipcmp(lifc->local, v6loopback) == 0){
			delroute(f, v6allnodesN, v6allnodesNmask,
				lifc->local, IPallbits,
				v6allnodesN, Rmulti, ifc, tifc);
		}
		delroute(f, v6allnodesL, v6allnodesLmask,
			lifc->local, IPallbits,
			v6allnodesL, Rmulti, ifc, tifc);
	}

done:
	free(lifc);
	return nil;
}

/*
 *  remove an address from an interface.
 */
char*
ipifcdel(Ipifc *ifc, char **argv, int argc)
{
	uchar ip[IPaddrlen], mask[IPaddrlen], rem[IPaddrlen];
	Iplifc *lifc, **l;
	char *err;

	if(argc < 3)
		return Ebadarg;
	if(parseipandmask(ip, mask, argv[1], argv[2]) == -1)
		return Ebadip;
	if(argc < 4)
		maskip(ip, mask, rem);
	else if(parseip(rem, argv[3]) == -1)
		return Ebadip;

	/*
	 *  find address on this interface and remove from chain.
	 *  for pt to pt we actually specify the remote address as the
	 *  addresss to remove.
	 */
	wlock(ifc);
	l = &ifc->lifc;
	for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next) {
		if(ipcmp(ip, lifc->local) == 0
		&& ipcmp(mask, lifc->mask) == 0
		&& ipcmp(rem, lifc->remote) == 0)
			break;
		l = &lifc->next;
	}
	err = ipifcdellifc(ifc, l);
	wunlock(ifc);
	return err;
}

/*
 *  associate an address with the interface.  This wipes out any previous
 *  addresses.  This is a macro that means, remove all the old interfaces
 *  and add a new one.
 */
static char*
ipifcconnect(Conv* c, char **argv, int argc)
{
	Ipifc *ifc = (Ipifc*)c->ptcl;
	char *err;

	wlock(ifc);
	while(ifc->lifc != nil)
		ipifcdellifc(ifc, &ifc->lifc);
	wunlock(ifc);

	err = ipifcadd(ifc, argv, argc, 0, nil);
	if(err != nil)
		return err;

	Fsconnected(c, nil);
	return nil;
}

/*
 *  change ipv6 router parameters
 *  ifc must be bound and wlock'd
 */
static char*
ipifcra6(Ipifc *ifc, char **argv, int argc)
{
	int i, argsleft;
	uchar sendra, recvra;
	Routerparams rp;

	i = 1;
	argsleft = argc - 1;
	if((argsleft % 2) != 0)
		return Ebadarg;

	sendra = ifc->sendra6;
	recvra = ifc->recvra6;
	rp = ifc->rp;

	while (argsleft > 1) {
		if(strcmp(argv[i], "recvra") == 0)
			recvra = atoi(argv[i+1]) != 0;
		else if(strcmp(argv[i], "sendra") == 0)
			sendra = atoi(argv[i+1]) != 0;
		else if(strcmp(argv[i], "mflag") == 0)
			rp.mflag = atoi(argv[i+1]) != 0;
		else if(strcmp(argv[i], "oflag") == 0)
			rp.oflag = atoi(argv[i+1]) != 0;
		else if(strcmp(argv[i], "maxraint") == 0)
			rp.maxraint = atoi(argv[i+1]);
		else if(strcmp(argv[i], "minraint") == 0)
			rp.minraint = atoi(argv[i+1]);
		else if(strcmp(argv[i], "linkmtu") == 0)
			rp.linkmtu = atoi(argv[i+1]);
		else if(strcmp(argv[i], "reachtime") == 0)
			rp.reachtime = atoi(argv[i+1]);
		else if(strcmp(argv[i], "rxmitra") == 0)
			rp.rxmitra = atoi(argv[i+1]);
		else if(strcmp(argv[i], "ttl") == 0)
			rp.ttl = atoi(argv[i+1]);
		else if(strcmp(argv[i], "routerlt") == 0)
			rp.routerlt = atoi(argv[i+1]);
		else
			return Ebadarg;
		argsleft -= 2;
		i += 2;
	}

	/* consistency check */
	if(rp.maxraint < rp.minraint)
		return Ebadarg;

	ifc->rp = rp;
	ifc->sendra6 = sendra;
	ifc->recvra6 = recvra;

	return nil;
}

/*
 *  non-standard control messages.
 */
static char*
ipifcctl(Conv* c, char **argv, int argc)
{
	Ipifc *ifc = (Ipifc*)c->ptcl;
	char *err = nil;

	if(strcmp(argv[0], "iprouting") == 0)
		iprouting(c->p->f, argc>1? atoi(argv[1]): 1);
	else if(strcmp(argv[0], "add") == 0)
		err = ipifcadd(ifc, argv, argc, 0, nil);
	else if(strcmp(argv[0], "try") == 0)
		err = ipifcadd(ifc, argv, argc, 1, nil);
	else if(strcmp(argv[0], "del") == 0 || strcmp(argv[0], "remove") == 0)
		err = ipifcdel(ifc, argv, argc);
	else if(strcmp(argv[0], "unbind") == 0)
		err = ipifcunbind(ifc);
	else if(strcmp(argv[0], "add6") == 0)
		err = ipifcadd6(ifc, argv, argc);
	else if(strcmp(argv[0], "del6") == 0 || strcmp(argv[0], "remove6") == 0)
		err = ipifcdel6(ifc, argv, argc);
	else {
		wlock(ifc);
		if(ifc->m == nil || ifc->m == &unboundmedium){
			wunlock(ifc);
			return Eunbound;
		}
		if(strcmp(argv[0], "mtu") == 0)
			err = ipifcsetmtu(ifc, argc>1? strtoul(argv[1], 0, 0): 0);
		else if(strcmp(argv[0], "speed") == 0)
			ipifcsetspeed(ifc, argc>1? atoi(argv[1]): 0);
		else if(strcmp(argv[0], "delay") == 0)
			ipifcsetdelay(ifc, argc>1? atoi(argv[1]): 0);
		else if(strcmp(argv[0], "reflect") == 0)
			ifc->reflect = argc>1? atoi(argv[1]): 1;
		else if(strcmp(argv[0], "reassemble") == 0)
			ifc->reassemble = argc>1? atoi(argv[1]): 1;
		else if(strcmp(argv[0], "ra6") == 0)
			err = ipifcra6(ifc, argv, argc);
		else
			err = "unsupported ctl";
		wunlock(ifc);
	}
	return err;
}

int
ipifcstats(Proto *ipifc, char *buf, int len)
{
	return ipstats(ipifc->f, buf, len);
}

void
ipifcinit(Fs *f)
{
	Proto *ipifc;

	ipifc = smalloc(sizeof(Proto));
	ipifc->name = "ipifc";
	ipifc->connect = ipifcconnect;
	ipifc->announce = nil;
	ipifc->bind = ipifcbind;
	ipifc->state = ipifcstate;
	ipifc->create = ipifccreate;
	ipifc->close = ipifcclose;
	ipifc->rcv = nil;
	ipifc->ctl = ipifcctl;
	ipifc->advise = nil;
	ipifc->stats = ipifcstats;
	ipifc->inuse = ipifcinuse;
	ipifc->local = ipifclocal;
	ipifc->ipproto = -1;
	ipifc->nc = Maxmedia;
	ipifc->ptclsize = sizeof(Ipifc);

	f->ipifc = ipifc;
	f->self = smalloc(sizeof(Ipselftab));	/* hack for ipforme */

	Fsproto(f, ipifc);
}

/*
 *  add to self routing cache
 */
static void
addselfcache(Fs *f, Ipifc *ifc, Iplifc *lifc, uchar *a, int type)
{
	Iplink *lp;
	Ipself *p;
	int h;

	if(type == Runi) type |= (lifc->type & Rproxy);
	type |= (lifc->type & Rv4);

	qlock(f->self);
	if(waserror()){
		qunlock(f->self);
		nexterror();
	}

	/* see if the address already exists */
	h = hashipa(a);
	for(p = f->self->hash[h]; p != nil; p = p->next)
		if(ipcmp(a, p->a) == 0)
			break;

	/* allocate a local address and add to hash chain */
	if(p == nil){
		p = smalloc(sizeof(*p));
		ipmove(p->a, a);
		p->type = type;
		p->next = f->self->hash[h];
		f->self->hash[h] = p;

		/* if the null address, accept all packets */
		if(ipcmp(a, v4prefix) == 0 || ipcmp(a, IPnoaddr) == 0)
			f->self->acceptall = 1;
	}

	/* look for a link for this lifc */
	for(lp = p->link; lp != nil; lp = lp->selflink)
		if(lp->lifc == lifc)
			break;

	/* allocate a lifc-to-local link and link to both */
	if(lp == nil){
		lp = smalloc(sizeof(*lp));
		lp->ref = 1;
		lp->lifc = lifc;
		lp->self = p;
		lp->selflink = p->link;
		p->link = lp;
		lp->lifclink = lifc->link;
		lifc->link = lp;

		/* add to routing table */
		addroute(f, a, IPallbits,
			lifc->local, 
			((type & (Rbcast|Rmulti)) != 0 || v6addrtype(a) == linklocalv6) ?
				IPallbits : IPnoaddr,
			a, type, ifc, tifc);

		if(type & Rmulti){
			if(ifc->m->addmulti != nil){
				if(!waserror()){
					(*ifc->m->addmulti)(ifc, a, lifc->local);
					poperror();
				}
			}
			if(multicastreportfn != nil){
				if(!waserror()){
					(*multicastreportfn)(f, ifc, a, lifc->local, 0);
					poperror();
				}
			}
		}
	} else
		lp->ref++;

	qunlock(f->self);
	poperror();
}

/*
 *  These structures are unlinked from their chains while
 *  other threads may be using them.  To avoid excessive locking,
 *  just put them aside for a while before freeing them.
 *	called with f->self locked
 */
static Iplink *freeiplink;
static Ipself *freeipself;

static void
iplinkfree(Iplink *p)
{
	Iplink **l, *np;
	ulong now = NOW;

	l = &freeiplink;
	for(np = *l; np != nil; np = *l){
		if((long)(now - np->expire) >= 0){
			*l = np->next;
			free(np);
			continue;
		}
		l = &np->next;
	}
	p->expire = now + 5000;	/* give other threads 5 secs to get out */
	p->next = nil;
	*l = p;
}

static void
ipselffree(Ipself *p)
{
	Ipself **l, *np;
	ulong now = NOW;

	l = &freeipself;
	for(np = *l; np != nil; np = *l){
		if((long)(now - np->expire) >= 0){
			*l = np->next;
			free(np);
			continue;
		}
		l = &np->next;
	}
	p->expire = now + 5000;	/* give other threads 5 secs to get out */
	p->next = nil;
	*l = p;
}

/*
 *  Decrement reference for this address on this link.
 *  Unlink from selftab if this is the last ref.
 */
static void
delselfcache(Fs *f, Ipifc *ifc, Iplifc *lifc, uchar *a)
{
	Ipself *p, **l;
	Iplink *link, **l_self, **l_lifc;

	qlock(f->self);

	/* find the unique selftab entry */
	l = &f->self->hash[hashipa(a)];
	for(p = *l; p != nil; p = *l){
		if(ipcmp(p->a, a) == 0)
			break;
		l = &p->next;
	}

	if(p == nil)
		goto out;

	/*
	 *  walk down links from an ifc looking for one
	 *  that matches the selftab entry
	 */
	l_lifc = &lifc->link;
	for(link = *l_lifc; link != nil; link = *l_lifc){
		if(link->self == p)
			break;
		l_lifc = &link->lifclink;
	}

	if(link == nil)
		goto out;

	/*
	 *  walk down the links from the selftab looking for
	 *  the one we just found
	 */
	l_self = &p->link;
	for(link = *l_self; link != nil; link = *l_self){
		if(link == *l_lifc)
			break;
		l_self = &link->selflink;
	}

	if(link == nil)
		panic("delselfcache");

	if(--(link->ref) != 0)
		goto out;

	/* ref == 0, remove from both chains and free the link */
	*l_lifc = link->lifclink;
	*l_self = link->selflink;
	iplinkfree(link);

	if((p->type & Rmulti) != 0){
		if(multicastreportfn != nil){
			if(!waserror()){
				(*multicastreportfn)(f, ifc, a, lifc->local, 1);
				poperror();
			}
		}
		if(ifc->m->delmulti != nil){
			if(!waserror()){
				(*ifc->m->delmulti)(ifc, a, lifc->local);
				poperror();
			}
		}
	}

	/* remove from routing table */
	delroute(f, a, IPallbits,
		lifc->local, 
		((p->type & (Rbcast|Rmulti)) != 0 || v6addrtype(a) == linklocalv6) ?
			IPallbits : IPnoaddr,
		a, p->type, ifc, tifc);

	if(p->link != nil)
		goto out;

	/* if null address, forget */
	if(ipcmp(a, v4prefix) == 0 || ipcmp(a, IPnoaddr) == 0)
		f->self->acceptall = 0;

	/* no more links, remove from hash and free */
	*l = p->next;
	ipselffree(p);

out:
	qunlock(f->self);
}

long
ipselftabread(Fs *f, char *cp, ulong offset, int n)
{
	int i, m, nifc, off;
	Ipself *p;
	Iplink *link;
	char state[8];

	m = 0;
	off = offset;
	for(i = 0; i < NHASH && m < n; i++){
		for(p = f->self->hash[i]; p != nil && m < n; p = p->next){
			nifc = 0;
			for(link = p->link; link != nil; link = link->selflink)
				nifc++;
			routetype(p->type, state);
			m += snprint(cp + m, n - m, "%-44.44I %2.2d %4.4s\n",
				p->a, nifc, state);
			if(off > 0){
				off -= m;
				m = 0;
			}
		}
	}
	return m;
}

/*
 *  returns
 *	0		- no match
 *	Runi
 *	Rbcast
 *	Rmulti
 */
int
ipforme(Fs *f, uchar *addr)
{
	Ipself *p;

	for(p = f->self->hash[hashipa(addr)]; p != nil; p = p->next)
		if(ipcmp(addr, p->a) == 0)
			return p->type & (Runi|Rbcast|Rmulti);

	/* hack to say accept anything */
	if(f->self->acceptall)
		return Runi;

	return 0;
}

/*
 *  find the ifc on same net as the remote system.
 *  returns the rlock'd ifc, otherwise nil.
 */
Ipifc*
findipifc(Fs *f, uchar *local, uchar *remote, int type)
{
	uchar gnet[IPaddrlen];
	int spec, xspec;
	uchar xifcid;
	Ipifc *x, *ifc;
	Iplifc *lifc;
	Conv **cp;

	x = nil;
	xifcid = 0;
	xspec = 0;
	for(cp = f->ipifc->conv; *cp != nil; cp++){
		ifc = (Ipifc*)(*cp)->ptcl;
		rlock(ifc);
		for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next){
			if(type & Runi){
				if(ipcmp(remote, lifc->local) == 0)
					return ifc;
			} else if(type & (Rbcast|Rmulti)) {
				if(ipcmp(local, lifc->local) == 0)
					return ifc;
			}
			maskip(remote, lifc->mask, gnet);
			if(ipcmp(gnet, lifc->net) == 0){
				spec = comprefixlen(remote, lifc->local, IPaddrlen)<<8;
				spec += comprefixlen(local, lifc->local, IPaddrlen);
				if(spec > xspec){
					x = ifc;
					xifcid = ifc->ifcid;
					xspec = spec;
				}
			}
		}
		runlock(ifc);
	}
	if(x != nil){
		rlock(x);
		if(x->ifcid == xifcid)
			return x;
		runlock(x);
	}
	return nil;
}

/*
 *  find the ifc bound to device name dev.
 *  returns the rlock'd ifc, otherwise nil.
 */
Ipifc*
findipifcdev(Fs *f, char *dev)
{
	Ipifc *ifc;
	Conv **cp;

	for(cp = f->ipifc->conv; *cp != nil; cp++){
		ifc = (Ipifc*)(*cp)->ptcl;
		rlock(ifc);
		if(ifc->m != nil
		&& ifc->m != &unboundmedium
		&& strcmp(ifc->dev, dev) == 0)
			return ifc;
		runlock(ifc);
	}
	return nil;
}

/*
 *  find a ifc based on:
 *  - its /net/ipifc/X directory number
 *  - a local ip address on that interface
 *  - bound device name
 *  returns the rlock'd ifc, otherwise nil.
 */
Ipifc*
findipifcstr(Fs *f, char *s)
{
	uchar ip[IPaddrlen];
	Ipifc *ifc;
	Conv *c;
	char *p;
	long x;

	ifc = nil;
	x = strtol(s, &p, 10);
	if(p > s && *p == '\0'){
		if(x < 0)
			return nil;
		if(x < f->ipifc->nc && (c = f->ipifc->conv[x]) != nil){
			ifc = (Ipifc*)c->ptcl;
			rlock(ifc);
			if(ifc->m == nil || ifc->m == &unboundmedium){
				runlock(ifc);
				ifc = nil;
			}
		}
	} else if(parseip(ip, s) != -1)
		ifc = findipifc(f, ip, ip, Runi);
	else
		ifc = findipifcdev(f, s);
	return ifc;
}

/*
 *  find "best" (global > ula > link local > unspecified)
 *  local address; address must be current.
 */
static void
findprimaryipv6(Fs *f, uchar *local)
{
	ulong now = NOW/1000;
	int atype, atypel;
	Iplifc *lifc;
	Ipifc *ifc;
	Conv **cp;

	ipmove(local, v6Unspecified);
	atype = unspecifiedv6;

	for(cp = f->ipifc->conv; *cp != nil; cp++){
		ifc = (Ipifc*)(*cp)->ptcl;
		rlock(ifc);
		for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next){
			atypel = v6addrtype(lifc->local);
			if(atypel > atype)
			if(lifc->preflt == ~0UL || lifc->preflt >= now-lifc->origint) {
				ipmove(local, lifc->local);
				atype = atypel;
				if(atype == globalv6){
					runlock(ifc);
					return;
				}
			}
		}
		runlock(ifc);
	}
}

/*
 *  returns first v4 address configured
 */
static void
findprimaryipv4(Fs *f, uchar *local)
{
	Iplifc *lifc;
	Ipifc *ifc;
	Conv **cp;

	/* find first ifc local address */
	for(cp = f->ipifc->conv; *cp != nil; cp++){
		ifc = (Ipifc*)(*cp)->ptcl;
		rlock(ifc);
		for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next){
			if((lifc->type & Rv4) != 0){
				ipmove(local, lifc->local);
				runlock(ifc);
				return;
			}
		}
		runlock(ifc);
	}
	ipmove(local, IPnoaddr);
}

/*
 * ipv4local, ipv6local:
 *  return a local address associated with an interface close to remote.
 *  prefixlen is the number of leading bits in the local address that
 *  have to match an interface address to be considered. this is used
 *  by source specific routes to filter on the source address.
 *  return non-zero on success or zero when no address was found.
 *
 *  for ipv4local, all addresses are 4 byte format.
 */
int
ipv4local(Ipifc *ifc, uchar *local, int prefixlen, uchar *remote)
{
	Iplifc *lifc;
	int a, b;

	b = -1;
	for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next){
		if((lifc->type & Rv4) == 0 || ipcmp(lifc->local, IPnoaddr) == 0)
			continue;

		if(prefixlen && comprefixlen(lifc->local+IPv4off, local, IPv4addrlen) < prefixlen)
			continue;
		
		a = comprefixlen(lifc->local+IPv4off, remote, IPv4addrlen);
		if(a > b){
			b = a;
			memmove(local, lifc->local+IPv4off, IPv4addrlen);
		}
	}
	return b >= 0;
}

int
ipv6local(Ipifc *ifc, uchar *local, int prefixlen, uchar *remote)
{
	struct {
		int	atype;
		int	deprecated;
		int	comprefixlen;
	} a, b;
	int atype;
	ulong now;
	Iplifc *lifc;

	if(isv4(remote)){
		memmove(local, v4prefix, IPv4off);
		if((prefixlen -= IPv4off*8) < 0)
			prefixlen = 0;
		return ipv4local(ifc, local+IPv4off, prefixlen, remote+IPv4off);
	}

	atype = v6addrtype(remote);
	b.atype = unknownv6;
	b.deprecated = 1;
	b.comprefixlen = 0;

	now = NOW/1000;
	for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next){
		if(lifc->tentative)
			continue;

		if(prefixlen && comprefixlen(lifc->local, local, IPaddrlen) < prefixlen)
			continue;

		a.atype = v6addrtype(lifc->local);
		a.deprecated = lifc->preflt != ~0UL && lifc->preflt < now-lifc->origint;
		a.comprefixlen = comprefixlen(lifc->local, remote, IPaddrlen);

		/* prefer appropriate scope */
		if(a.atype != b.atype){
			if(a.atype > b.atype && b.atype < atype ||
			   a.atype < b.atype && b.atype > atype)
				goto Good;
			continue;
		}
		/* prefer non-deprecated addresses */
		if(a.deprecated != b.deprecated){
			if(b.deprecated)
				goto Good;
			continue;
		}
		/* prefer longer common prefix */
		if(a.comprefixlen != b.comprefixlen){
			if(a.comprefixlen > b.comprefixlen)
				goto Good;
			continue;
		}
		continue;
	Good:
		b = a;
		ipmove(local, lifc->local);
	}

	return b.atype >= atype;
}

/*
 *  find the local address for a remote destination
 */
void
findlocalip(Fs *f, uchar *local, uchar *remote)
{
	if(isv4(remote)) {
		memmove(local, v4prefix, IPv4off);
		if(v4source(f, remote+IPv4off, local+IPv4off) == nil)
			findprimaryipv4(f, local);
	} else {
		if(v6source(f, remote, local) == nil)
			findprimaryipv6(f, local);
	}
}

/*
 *  see if this address is bound to the interface
 */
Iplifc*
iplocalonifc(Ipifc *ifc, uchar *ip)
{
	Iplifc *lifc;

	for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next)
		if(ipcmp(ip, lifc->local) == 0)
			return lifc;

	return nil;
}

Iplifc*
ipremoteonifc(Ipifc *ifc, uchar *ip)
{
	uchar net[IPaddrlen];
	Iplifc *lifc;

	for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next){
		maskip(ip, lifc->mask, net);
		if(ipcmp(net, lifc->remote) == 0)
			return lifc;
	}
	return nil;
}

/*
 *  return link-local interface
 */
Iplifc*
iplinklocalifc(Ipifc *ifc)
{
	Iplifc *lifc;

	for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next)
		if(islinklocal(lifc->local))
			return lifc;

	return nil;
}

/*
 *  Return a copy of the multicast addresses on the
 *  specified interface and multicast address.
 */
Ipmulti*
ipifcgetmulti(Fs *f, Ipifc *ifc, uchar *ma)
{
	Ipmulti *head, *tail, *mcast;
	Iplifc *lifc;
	Iplink *link;
	Ipself *self;
	int type;

	type = Rmulti;
	if(isv4(ma))
		type |= Rv4;
	if(ipismulticast(ma) == ((type & Rv4)? V4: V6)){
		if(ipforme(f, ma) != Rmulti)
			return nil;
	} else {
		ma = nil;
	}

	head = tail = nil;
	for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next) {
		if((type & Rv4) == 0 && !islinklocal(lifc->local))
			continue;

		for(link = lifc->link; link != nil; link = link->lifclink) {
			self = link->self;
			if((self->type & (Rv4|Rmulti)) != type)
				continue;

			if(ma != nil){
				if(ipcmp(self->a, ma) != 0)
					continue;
			} else {
				if(ipcmp(self->a, (type & Rv4)? IPv4allsys: v6allnodesL) == 0)
					continue;
			}

			mcast = smalloc(sizeof(Ipmulti));
			mcast->next = nil;
			ipmove(mcast->ma, self->a);
			ipmove(mcast->ia, lifc->local);

			if(ma != nil)
				return mcast;

			if(head == nil)
				head = mcast;
			else
				tail->next = mcast;
			tail = mcast;
		}
	}
	return head;
}

/*
 *  add a multicast address to an interface.
 */
void
ipifcaddmulti(Conv *c, uchar *ma, uchar *ia)
{
	Ipmulti *multi, **l;
	Iplifc *lifc;
	Ipifc *ifc;
	Fs *f;

	if(!ipismulticast(ma))
		error("addmulti for a non multicast address");
	if(isv4(ma) != isv4(ia))
		error("incompatible multicast/interface ip address");

	for(l = &c->multi; *l != nil; l = &(*l)->next)
		if(ipcmp(ma, (*l)->ma) == 0 && ipcmp(ia, (*l)->ia) == 0)
			return;		/* it's already there */

	f = c->p->f;
	if((ifc = findipifc(f, ia, ma, Rmulti)) != nil){
		if(waserror()){
			runlock(ifc);
			nexterror();
		}
		if((lifc = iplocalonifc(ifc, ia)) != nil)
			addselfcache(f, ifc, lifc, ma, Rmulti);

		/* for IPv6, must add also add to the link-local address */
		if(!isv4(ia) && !islinklocal(ia) && (lifc = iplinklocalifc(ifc)) != nil)
			addselfcache(f, ifc, lifc, ma, Rmulti);

		runlock(ifc);
		poperror();
	}

	multi = smalloc(sizeof(*multi));
	ipmove(multi->ma, ma);
	ipmove(multi->ia, ia);
	multi->next = nil;
	*l = multi;
}


/*
 *  remove a multicast address from an interface.
 */
void
ipifcdelmulti(Conv *c, uchar *ma, uchar *ia)
{
	Ipmulti *multi, **l;
	Iplifc *lifc;
	Ipifc *ifc;
	Fs *f;

	for(l = &c->multi; *l != nil; l = &(*l)->next)
		if(ipcmp(ma, (*l)->ma) == 0 && ipcmp(ia, (*l)->ia) == 0)
			break;

	multi = *l;
	if(multi == nil)
		return; 	/* we don't have it open */

	*l = multi->next;
	multi->next = nil;

	f = c->p->f;
	if((ifc = findipifc(f, ia, ma, Rmulti)) != nil){
		if(!waserror()){
			if((lifc = iplocalonifc(ifc, ia)) != nil)
				delselfcache(f, ifc, lifc, ma);
			poperror();
		}
		if(!isv4(ia) && !islinklocal(ia) && !waserror()){
			if((lifc = iplinklocalifc(ifc)) != nil)
				delselfcache(f, ifc, lifc, ma);
			poperror();
		}
		runlock(ifc);
	}
	free(multi);
}

/* register the address on this network for address resolution */
static void
ipifcregisteraddr(Fs *f, Ipifc *ifc, Iplifc *lifc, uchar *ip)
{
	if(waserror()){
		print("ipifcregisteraddr %s %I %I: %s\n", ifc->dev, lifc->local, ip, up->errstr);
		return;
	}
	if(ifc->m != nil && ifc->m->areg != nil)
		(*ifc->m->areg)(f, ifc, lifc, ip);
	poperror();
}

static void
ipifcregisterproxy(Fs *f, Ipifc *ifc, uchar *ip, int add)
{
	uchar a[IPaddrlen];
	Iplifc *lifc;
	Ipifc *nifc;
	Conv **cp;

	/* register the address on any interface that will proxy for the ip */
	for(cp = f->ipifc->conv; *cp != nil; cp++){
		nifc = (Ipifc*)(*cp)->ptcl;
		if(nifc == ifc || !canrlock(nifc))
			continue;

		if(nifc->m == nil || nifc->m == &unboundmedium
		|| (lifc = ipremoteonifc(nifc, ip)) == nil
		|| (lifc->type & Rptpt) != 0
		|| waserror()){
			runlock(nifc);
			continue;
		}
		if((lifc->type & Rv4) == 0){
			/* add solicited-node multicast addr */
			ipv62smcast(a, ip);
			if(add)
				addselfcache(f, nifc, lifc, a, Rmulti);
			else
				delselfcache(f, nifc, lifc, a);
		}
		if(add)
			ipifcregisteraddr(f, nifc, lifc, ip);
		runlock(nifc);
		poperror();
	}
}

char*
ipifcadd6(Ipifc *ifc, char **argv, int argc)
{
	int plen = 64;
	char addr[40], preflen[6];
	char *params[3];
	uchar prefix[IPaddrlen];
	Iplifc lifc;

	lifc.onlink = 1;
	lifc.autoflag = 1;
	lifc.validlt = lifc.preflt = ~0UL;
	lifc.origint = NOW / 1000;

	switch(argc) {
	case 7:
		lifc.preflt = strtoul(argv[6], 0, 10);
		/* fall through */
	case 6:
		lifc.validlt = strtoul(argv[5], 0, 10);
		/* fall through */
	case 5:
		lifc.autoflag = atoi(argv[4]) != 0;
		/* fall through */
	case 4:
		lifc.onlink = atoi(argv[3]) != 0;
		/* fall through */
	case 3:
		plen = atoi(argv[2]);
		/* fall through */
	case 2:
		break;
	default:
		return Ebadarg;
	}

	if (parseip(prefix, argv[1]) != 6 || lifc.validlt < lifc.preflt || plen < 0 ||
	    plen > 64 || islinklocal(prefix))
		return Ebadarg;

	rlock(ifc);
	if(ifc->m == nil || ifc->m == &unboundmedium){
		runlock(ifc);
		return Eunbound;
	}
	if(ifc->m->pref2addr != nil && ifc->m->maclen > 0) {
		/* mac → v6 link-local addr */
		(*ifc->m->pref2addr)(prefix, ifc->mac);
	} else {
		/* copy from existing v6 link-local address */
		Iplifc *llifc = iplinklocalifc(ifc);
		if(llifc == nil){
			runlock(ifc);
			return Eunbound;
		}
		memmove(prefix+8, llifc->local+8, 8);
	}
	runlock(ifc);

	/* issue "add" ctl msg for v6 link-local addr and prefix len */
	snprint(addr, sizeof addr, "%I", prefix);
	snprint(preflen, sizeof preflen, "/%d", plen);
	params[0] = "add";
	params[1] = addr;
	params[2] = preflen;

	return ipifcadd(ifc, params, 3, 0, &lifc);
}

char*
ipifcdel6(Ipifc *ifc, char**, int argc)
{
	Iplifc *lifc, **l;
	ulong now;

	if(argc != 1)
		return Ebadarg;

	wlock(ifc);
	now = NOW/1000;
	for(l = &ifc->lifc; (lifc = *l) != nil;) {
		if((lifc->type & Rv4) == 0)
		if(lifc->validlt != ~0UL && lifc->validlt < now-lifc->origint)
			if(ipifcdellifc(ifc, l) == nil)
				continue;
		l = &lifc->next;
	}
	wunlock(ifc);

	return nil;
}

#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"../port/netif.h"

static int netown(Netfile*, char*, int);
static int openfile(Netif*, int, int);
static char* matchtoken(char*, char*);
static void netmulti(Netif*, Netfile*, uchar*, int);
static int parseaddr(uchar*, char*, int);

/*
 *  set up a new network interface
 */
void
netifinit(Netif *nif, char *name, int nfile, int limit)
{
	if(strlen(name) >= sizeof nif->name)
		panic("netifinit: name too long: %s", name);
	strcpy(nif->name, name);
	nif->nfile = nfile;
	nif->f = xalloc(nfile*sizeof(Netfile*));
	if (nif->f == nil)
		panic("netifinit: no memory");
	memset(nif->f, 0, nfile*sizeof(Netfile*));
	nif->limit = limit;
}

/*
 * adjust input queue sizes for all files
 */
void
netifsetlimit(Netif *nif, int limit)
{
	Netfile *f;
	int i;

	qlock(nif);
	if(nif->limit != limit){
		nif->limit = limit;
		for(i = 0; i < nif->nfile; i++){
			f = nif->f[i];
			if(f == nil || !f->inuse)
				continue;
			qsetlimit(f->in, nif->limit);
		}
	}
	qunlock(nif);
}

/*
 *  generate a 3 level directory
 */
static int
netifgen(Chan *c, char*, Dirtab *vp, int, int i, Dir *dp)
{
	Qid q;
	Netif *nif = (Netif*)vp;
	Netfile *f;
	int t;
	int perm;
	char *o;

	q.type = QTFILE;
	q.vers = 0;

	/* top level directory contains the name of the network */
	if(c->qid.path == 0){
		switch(i){
		case DEVDOTDOT:
			q.path = 0;
			q.type = QTDIR;
			devdir(c, q, ".", 0, eve, 0555, dp);
			break;
		case 0:
			q.path = N2ndqid;
			q.type = QTDIR;
			strcpy(up->genbuf, nif->name);
			devdir(c, q, up->genbuf, 0, eve, 0555, dp);
			break;
		default:
			return -1;
		}
		return 1;
	}

	/* second level contains clone plus all the conversations */
	t = NETTYPE(c->qid.path);
	if(t == N2ndqid || t == Ncloneqid || t == Naddrqid || t == Nstatqid || t == Nifstatqid){
		switch(i) {
		case DEVDOTDOT:
			q.type = QTDIR;
			q.path = 0;
			devdir(c, q, ".", 0, eve, 0555, dp);
			break;
		case 0:
			q.path = Ncloneqid;
			devdir(c, q, "clone", 0, eve, 0666, dp);
			break;
		case 1:
			q.path = Naddrqid;
			devdir(c, q, "addr", 0, eve, 0444, dp);
			break;
		case 2:
			q.path = Nstatqid;
			devdir(c, q, "stats", 0, eve, 0444, dp);
			break;
		case 3:
			q.path = Nifstatqid;
			devdir(c, q, "ifstats", 0, eve, 0444, dp);
			break;
		default:
			i -= 4;
			if(i >= nif->nfile)
				return -1;
			if(nif->f[i] == nil)
				return 0;
			q.type = QTDIR;
			q.path = NETQID(i, N3rdqid);
			snprint(up->genbuf, sizeof up->genbuf, "%d", i);
			devdir(c, q, up->genbuf, 0, eve, 0555, dp);
			break;
		}
		return 1;
	}

	/* third level */
	f = nif->f[NETID(c->qid.path)];
	if(f == nil)
		return 0;
	if(*f->owner){
		o = f->owner;
		perm = f->mode;
	} else {
		o = eve;
		perm = 0666;
	}
	switch(i){
	case DEVDOTDOT:
		q.type = QTDIR;
		q.path = N2ndqid;
		strcpy(up->genbuf, nif->name);
		devdir(c, q, up->genbuf, 0, eve, 0555, dp);
		break;
	case 0:
		q.path = NETQID(NETID(c->qid.path), Ndataqid);
		devdir(c, q, "data", 0, o, perm, dp);
		break;
	case 1:
		q.path = NETQID(NETID(c->qid.path), Nctlqid);
		devdir(c, q, "ctl", 0, o, perm, dp);
		break;
	case 2:
		q.path = Nstatqid;
		devdir(c, q, "stats", 0, eve, 0444, dp);
		break;
	case 3:
		q.path = NETQID(NETID(c->qid.path), Ntypeqid);
		devdir(c, q, "type", 0, eve, 0444, dp);
		break;
	case 4:
		q.path = Nifstatqid;
		devdir(c, q, "ifstats", 0, eve, 0444, dp);
		break;
	default:
		return -1;
	}
	return 1;
}

Walkqid*
netifwalk(Netif *nif, Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, (Dirtab *)nif, 0, netifgen);
}

Chan*
netifopen(Netif *nif, Chan *c, int omode)
{
	int id;

	if(c->qid.type & QTDIR){
		if(omode != OREAD)
			error(Eperm);
	} else {
		switch(NETTYPE(c->qid.path)){
		case Ndataqid:
		case Nctlqid:
			id = NETID(c->qid.path);
			openfile(nif, id, omode);
			break;
		case Ncloneqid:
			id = openfile(nif, -1, omode);
			c->qid.path = NETQID(id, Nctlqid);
			break;
		default:
			if(omode != OREAD)
				error(Ebadarg);
		}
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	c->iounit = qiomaxatomic;
	return c;
}

long
netifread(Netif *nif, Chan *c, void *a, long n, ulong offset)
{
	int i, j;
	Netfile *f;
	char *p;

	if(c->qid.type&QTDIR)
		return devdirread(c, a, n, (Dirtab*)nif, 0, netifgen);

	switch(NETTYPE(c->qid.path)){
	case Ndataqid:
		f = nif->f[NETID(c->qid.path)];
		return qread(f->in, a, n);
	case Nctlqid:
		return readnum(offset, a, n, NETID(c->qid.path), NUMSIZE);
	case Nstatqid:
		if(offset >= READSTR)
			return 0;
		p = smalloc(READSTR);
		if(waserror()){
			free(p);
			nexterror();
		}
		p[0] = '\0';
		if(nif->ifstat != nil && !waserror()){
			(*nif->ifstat)(nif->arg, p, p);
			poperror();
		}
		j = snprint(p, READSTR, "in: %llud\n", nif->inpackets);
		j += snprint(p+j, READSTR-j, "link: %d\n", nif->link);
		j += snprint(p+j, READSTR-j, "out: %llud\n", nif->outpackets);
		j += snprint(p+j, READSTR-j, "crc errs: %d\n", nif->crcs);
		j += snprint(p+j, READSTR-j, "overflows: %d\n", nif->overflows);
		j += snprint(p+j, READSTR-j, "soft overflows: %d\n", nif->soverflows);
		j += snprint(p+j, READSTR-j, "framing errs: %d\n", nif->frames);
		j += snprint(p+j, READSTR-j, "buffer errs: %d\n", nif->buffs);
		j += snprint(p+j, READSTR-j, "output errs: %d\n", nif->oerrs);
		j += snprint(p+j, READSTR-j, "prom: %d\n", nif->prom);
		j += snprint(p+j, READSTR-j, "mbps: %d\n", nif->mbps);
		j += snprint(p+j, READSTR-j, "addr: ");
		for(i = 0; i < nif->alen; i++)
			j += snprint(p+j, READSTR-j, "%2.2ux", nif->addr[i]);
		snprint(p+j, READSTR-j, "\n");
		n = readstr(offset, a, n, p);
		free(p);
		poperror();
		return n;
	case Naddrqid:
		j = 0;
		for(i = 0; i < nif->alen; i++)
			j += snprint(up->genbuf+j, sizeof(up->genbuf)-j, "%2.2ux", nif->addr[i]);
		return readstr(offset, a, n, up->genbuf);
	case Ntypeqid:
		f = nif->f[NETID(c->qid.path)];
		return readnum(offset, a, n, f->type, NUMSIZE);
	case Nifstatqid:
		if(nif->ifstat == nil || offset >= READSTR)
			return 0;
		p = smalloc(READSTR);
		if(waserror()){
			free(p);
			nexterror();
		}
		p[0] = '\0';
		(*nif->ifstat)(nif->arg, p, p + READSTR);
		n = readstr(offset, a, n, p);
		free(p);
		poperror();
		return n;
	}
	error(Ebadarg);
}

Block*
netifbread(Netif *nif, Chan *c, long n, ulong offset)
{
	if((c->qid.type & QTDIR) || NETTYPE(c->qid.path) != Ndataqid)
		return devbread(c, n, offset);

	return qbread(nif->f[NETID(c->qid.path)]->in, n);
}

/*
 *  make sure this type isn't already in use on this device
 */
static int
typeinuse(Netif *nif, int type)
{
	Netfile *f, **fp, **efp;

	if(type <= 0)
		return 0;

	efp = &nif->f[nif->nfile];
	for(fp = nif->f; fp < efp; fp++){
		f = *fp;
		if(f == nil)
			continue;
		if(f->type == type)
			return 1;
	}
	return 0;
}

/*
 *  the devxxx.c that calls us handles writing data, it knows best
 */
long
netifwrite(Netif *nif, Chan *c, void *a, long n)
{
	Netfile *f;
	int type;
	char *p, buf[64];
	uchar binaddr[Nmaxaddr];

	if(NETTYPE(c->qid.path) != Nctlqid)
		error(Eperm);

	if(n >= sizeof(buf))
		n = sizeof(buf)-1;
	memmove(buf, a, n);
	buf[n] = 0;

	if(waserror()){
		qunlock(nif);
		nexterror();
	}
	qlock(nif);

	f = nif->f[NETID(c->qid.path)];
	if((p = matchtoken(buf, "connect")) != 0){
		type = strtoul(p, 0, 0);
		if(typeinuse(nif, type))
			error(Einuse);
		f->type = type;
		if(f->type < 0)
			nif->all++;
	} else if(matchtoken(buf, "promiscuous")){
		if(f->prom == 0){
			if(nif->prom == 0 && nif->promiscuous != nil)
				(*nif->promiscuous)(nif->arg, 1);
			f->prom = 1;
			nif->prom++;
		}
	} else if((p = matchtoken(buf, "scanbs")) != 0){
		/* scan for base stations */
		if(f->scan == 0){
			type = atoi(p);
			if(type < 5)
				type = 5;
			if(nif->scanbs != nil)
				(*nif->scanbs)(nif->arg, type);
			f->scan = type;
			nif->scan++;
		}
	} else if(matchtoken(buf, "bridge")){
		f->bridge = 1;
	} else if(matchtoken(buf, "bypass")){
		if(nif->bypass != nil)
			error(Einuse);
		f->bypass = 1;
		nif->bypass = f;
	} else if(matchtoken(buf, "headersonly")){
		f->headersonly = 1;
	} else if((p = matchtoken(buf, "addmulti")) != nil){
		if(parseaddr(binaddr, p, nif->alen) < 0)
			error("bad address");
		netmulti(nif, f, binaddr, 1);
	} else if((p = matchtoken(buf, "delmulti")) != nil
		||(p = matchtoken(buf, "remmulti")) != nil){
		if(parseaddr(binaddr, p, nif->alen) < 0)
			error("bad address");
		netmulti(nif, f, binaddr, 0);
	} else
		n = -1;
	qunlock(nif);
	poperror();
	return n;
}

int
netifwstat(Netif *nif, Chan *c, uchar *db, int n)
{
	Netfile *f;
	Dir *dir;
	int m;

	dir = smalloc(sizeof(Dir)+n);
	if(waserror()){
		free(dir);
		nexterror();
	}
	m = convM2D(db, n, &dir[0], (char*)&dir[1]);
	if(m == 0)
		error(Eshortstat);
	if(waserror()){
		qunlock(nif);
		nexterror();
	}
	qlock(nif);
	f = nif->f[NETID(c->qid.path)];
	if(f == nil)
		error(Enonexist);
	if(netown(f, up->user, OWRITE) < 0)
		error(Eperm);
	if(!emptystr(dir[0].uid)){
		strncpy(f->owner, dir[0].uid, KNAMELEN-1);
		f->owner[KNAMELEN-1] = 0;
	}
	if(dir[0].mode != ~0UL)
		f->mode = dir[0].mode;
	qunlock(nif);
	free(dir);
	poperror();
	poperror();
	return m;
}

int
netifstat(Netif *nif, Chan *c, uchar *db, int n)
{
	return devstat(c, db, n, (Dirtab *)nif, 0, netifgen);
}

void
netifclose(Netif *nif, Chan *c)
{
	Netfile *f;
	int t;
	Netaddr *ap;

	if((c->flag & COPEN) == 0)
		return;

	t = NETTYPE(c->qid.path);
	if(t != Ndataqid && t != Nctlqid)
		return;

	qlock(nif);
	f = nif->f[NETID(c->qid.path)];
	if(f == nil || --(f->inuse) != 0){
		qunlock(nif);
		return;
	}
	if(f->bypass){
		nif->bypass = nil;
		f->bypass = 0;
	}
	if(f->prom){
		if(--(nif->prom) == 0 && nif->promiscuous != nil && !waserror()){
			(*nif->promiscuous)(nif->arg, 0);
			poperror();
		}
		f->prom = 0;
	}
	if(f->scan){
		if(--(nif->scan) == 0 && nif->scanbs != nil && !waserror()){
			(*nif->scanbs)(nif->arg, 0);
			poperror();
		}
		f->prom = 0;
		f->scan = 0;
	}
	if(f->nmaddr){
		t = 0;
		for(ap = nif->maddr; ap != nil; ap = ap->next){
			if(f->maddr[t/8] & (1<<(t%8)) && !waserror()){
				netmulti(nif, f, ap->addr, 0);
				poperror();
			}
		}
		f->nmaddr = 0;
	}
	if(f->type < 0)
		nif->all--;
	f->owner[0] = 0;
	f->type = 0;
	f->bridge = 0;
	f->headersonly = 0;
	qclose(f->in);
	qunlock(nif);
}

static int
netown(Netfile *p, char *o, int omode)
{
	static int access[] = { 0400, 0200, 0600, 0100 };
	int mode;
	int t;

	if(*p->owner){
		if(strncmp(o, p->owner, KNAMELEN) == 0)	/* User */
			mode = p->mode;
		else if(strncmp(o, eve, KNAMELEN) == 0)	/* Bootes is group */
			mode = p->mode<<3;
		else
			mode = p->mode<<6;		/* Other */

		t = access[omode&3];
		if((t & mode) == t)
			return 0;
		else
			return -1;
	}
	strncpy(p->owner, o, KNAMELEN-1);
	p->owner[KNAMELEN-1] = 0;
	p->mode = 0660;
	return 0;
}

/*
 *  Increment the reference count of a network device.
 *  If id < 0, return an unused ether device.
 */
static int
openfile(Netif *nif, int id, int omode)
{
	Netfile *f, **fp, **efp;

	qlock(nif);
	if(waserror()){
		qunlock(nif);
		nexterror();
	}
	if(id >= 0){
		f = nif->f[id];
		if(f == nil)
			error(Enodev);
		if(netown(f, up->user, omode&7) < 0)
			error(Eperm);
		f->inuse++;
		qreopen(f->in);
		qsetlimit(f->in, nif->limit);
		qunlock(nif);
		poperror();
		return id;
	}
	efp = &nif->f[nif->nfile];
	for(fp = nif->f; fp < efp; fp++){
		f = *fp;
		if(f == nil){
			f = malloc(sizeof(Netfile));
			if(f == nil)
				exhausted("memory");
			f->in = qopen(nif->limit, Qmsg, 0, 0);
			if(f->in == nil){
				free(f);
				exhausted("memory");
			}
			coherence();
			*fp = f;
		} else if(f->inuse)
			continue;
		f->inuse = 1;
		qreopen(f->in);
		qsetlimit(f->in, nif->limit);
		netown(f, up->user, 0);
		qunlock(nif);
		poperror();
		return fp - nif->f;
	}
	error(Enodev);
}

/*
 *  look for a token starting a string,
 *  return a pointer to first non-space char after it
 */
static char*
matchtoken(char *p, char *token)
{
	int n;

	n = strlen(token);
	if(strncmp(p, token, n))
		return 0;
	p += n;
	if(*p == 0)
		return p;
	if(*p != ' ' && *p != '\t' && *p != '\n')
		return 0;
	while(*p == ' ' || *p == '\t' || *p == '\n')
		p++;
	return p;
}

void
hnputv(void *p, uvlong v)
{
	uchar *a;

	a = p;
	hnputl(a, v>>32);
	hnputl(a+4, v);
}

void
hnputl(void *p, uint v)
{
	uchar *a;

	a = p;
	a[0] = v>>24;
	a[1] = v>>16;
	a[2] = v>>8;
	a[3] = v;
}

void
hnputs(void *p, ushort v)
{
	uchar *a;

	a = p;
	a[0] = v>>8;
	a[1] = v;
}

uvlong
nhgetv(void *p)
{
	uchar *a;

	a = p;
	return ((vlong)nhgetl(a) << 32) | nhgetl(a+4);
}

uint
nhgetl(void *p)
{
	uchar *a;

	a = p;
	return (a[0]<<24)|(a[1]<<16)|(a[2]<<8)|(a[3]<<0);
}

ushort
nhgets(void *p)
{
	uchar *a;

	a = p;
	return (a[0]<<8)|(a[1]<<0);
}

static ulong
hash(uchar *a, int len)
{
	ulong sum = 0;

	while(len-- > 0)
		sum = (sum << 1) + *a++;
	return sum%Nmhash;
}

int
activemulti(Netif *nif, uchar *addr, int alen)
{
	Netaddr *hp;

	for(hp = nif->mhash[hash(addr, alen)]; hp != nil; hp = hp->hnext)
		if(memcmp(addr, hp->addr, alen) == 0)
			return hp->ref > 0;
	return 0;
}

static int
parseaddr(uchar *to, char *from, int alen)
{
	char nip[4];
	char *p;
	int i;

	p = from;
	for(i = 0; i < alen; i++){
		if(*p == 0)
			return -1;
		nip[0] = *p++;
		if(*p == 0)
			return -1;
		nip[1] = *p++;
		nip[2] = 0;
		to[i] = strtoul(nip, 0, 16);
		if(*p == ':')
			p++;
	}
	return 0;
}

/*
 *  keep track of multicast addresses
 */
static void
netmulti(Netif *nif, Netfile *f, uchar *addr, int add)
{
	Netaddr **l, *ap;
	ulong h;
	int i;

	if(nif->multicast == nil)
		error("interface does not support multicast");

	i = 0;
	l = &nif->maddr;
	for(ap = *l; ap != nil; ap = *l){
		if(memcmp(addr, ap->addr, nif->alen) == 0)
			break;
		i++;
		l = &ap->next;
	}

	if(add){
		if(ap == nil){
			ap = malloc(sizeof(*ap));
			if(ap == nil)
				error(Enomem);
			memmove(ap->addr, addr, nif->alen);
			h = hash(ap->addr, nif->alen);
			ap->hnext = nif->mhash[h];
			ap->ref = 1;
			coherence();
			nif->mhash[h] = ap;
			*l = ap;
		} else {
			ap->ref++;
		}
		if(ap->ref == 1){
			nif->nmaddr++;
			(*nif->multicast)(nif->arg, addr, 1);
		}
		if(i < 8*sizeof(f->maddr)){
			if((f->maddr[i/8] & (1<<(i%8))) == 0)
				f->nmaddr++;
			f->maddr[i/8] |= 1<<(i%8);
		}
	} else {
		if(ap == nil || ap->ref == 0)
			return;
		ap->ref--;
		if(ap->ref == 0){
			nif->nmaddr--;
			(*nif->multicast)(nif->arg, addr, 0);
		}
		if(i < 8*sizeof(f->maddr)){
			if((f->maddr[i/8] & (1<<(i%8))) != 0)
				f->nmaddr--;
			f->maddr[i/8] &= ~(1<<(i%8));
		}
	}
}

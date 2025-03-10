/*
 * ipconfig for IPv6
 *	RS means Router Solicitation
 *	RA means Router Advertisement
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ip.h>
#include <ndb.h>
#include "ipconfig.h"
#include "../icmp.h"

#include <libsec.h>	/* for sha1 */

enum {
	IsRouter 	= 1,
	IsHostRecv	= 2,
	IsHostNoRecv	= 3,

	ICMP6_RS	= 133,
	ICMP6_RA	= 134,

	MFMASK = 1 << 7,
	OCMASK = 1 << 6,

	OLMASK = 1 << 7,
	AFMASK = 1 << 6,
	RFMASK = 1 << 5,

	MAXTTL		= 255,
	DEFMTU		= 1500,
};

typedef struct Routeradv Routeradv;
typedef struct Routersol Routersol;
typedef struct Lladdropt Lladdropt;
typedef struct Prefixopt Prefixopt;
typedef struct Mtuopt Mtuopt;
typedef struct Ipaddrsopt Ipaddrsopt;

struct Routersol {
	uchar	vcf[4];		/* version:4, traffic class:8, flow label:20 */
	uchar	ploadlen[2];	/* payload length: packet length - 40 */
	uchar	proto;		/* next header	type */
	uchar	ttl;		/* hop limit */
	uchar	src[16];
	uchar	dst[16];
	uchar	type;
	uchar	code;
	uchar	cksum[2];
	uchar	res[4];
};

struct Routeradv {
	uchar	vcf[4];		/* version:4, traffic class:8, flow label:20 */
	uchar	ploadlen[2];	/* payload length: packet length - 40 */
	uchar	proto;		/* next header	type */
	uchar	ttl;		/* hop limit */
	uchar	src[16];
	uchar	dst[16];
	uchar	type;
	uchar	code;
	uchar	cksum[2];
	uchar	cttl;
	uchar	mor;
	uchar	routerlt[2];
	uchar	rchbltime[4];
	uchar	rxmtimer[4];
};

struct Lladdropt {
	uchar	type;
	uchar	len;
	uchar	lladdr[6];
};

struct Prefixopt {
	uchar	type;
	uchar	len;
	uchar	plen;
	uchar	lar;
	uchar	validlt[4];
	uchar	preflt[4];
	uchar	reserv[4];
	uchar	pref[16];
};

struct Mtuopt {
	uchar	type;
	uchar	len;
	uchar	reserv[2];
	uchar	mtu[4];
};

struct Ipaddrsopt {
	uchar	type;
	uchar	len;
	uchar	reserv[2];
	uchar	lifetime[4];
	uchar	addrs[];
};

uchar v6allroutersL[IPaddrlen] = {
	0xff, 0x02, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0x02
};

uchar v6allnodesL[IPaddrlen] = {
	0xff, 0x02, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0x01
};

uchar v6Unspecified[IPaddrlen] = {
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0
};

uchar v6loopback[IPaddrlen] = {
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 1
};

uchar v6glunicast[IPaddrlen] = {
	0x08, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0
};

uchar v6linklocal[IPaddrlen] = {
	0xfe, 0x80, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0
};

uchar v6solpfx[IPaddrlen] = {
	0xff, 0x02, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 1,
	/* last 3 bytes filled with low-order bytes of addr being solicited */
	0xff, 0, 0, 0,
};

#define isula(addr)       (((addr)[0] & 0xfe) == 0xfc)

void
v6paraminit(Conf *cf)
{
	cf->sendra = cf->recvra = 0;
	cf->mflag = 0;
	cf->oflag = 0;
	cf->linkmtu = DEFMTU;
	cf->maxraint = Maxv6initraintvl;
	cf->minraint = Maxv6initraintvl / 4;
	cf->reachtime = V6reachabletime;
	cf->rxmitra = V6retranstimer;
	cf->ttl = MAXTTL;

	cf->routerlt = 0;

	cf->prefixlen = 64;
	cf->onlink = cf->autoflag = 1;
	cf->validlt = cf->preflt = ~0L;
}

void
parse6pref(int argc, char **argv)
{
	switch(argc){
	case 6:
		conf.preflt = strtoul(argv[5], 0, 10);
		/* fall through */
	case 5:
		conf.validlt = strtoul(argv[4], 0, 10);
		/* fall through */
	case 4:
		conf.autoflag = (atoi(argv[3]) != 0);
		/* fall through */
	case 3:
		conf.onlink = (atoi(argv[2]) != 0);
		/* fall through */
	case 2:
		conf.prefixlen = atoi(argv[1]);
		/* fall through */
	case 1:
		if (parseip(conf.v6pref, argv[0]) == -1)
			sysfatal("bad address %s", argv[0]);
		break;
	}
	genipmask(conf.v6mask, conf.prefixlen);
	DEBUG("parse6pref: pref %I len %d", conf.v6pref, conf.prefixlen);
}

/* parse router advertisement (keyword, value) pairs */
void
parse6ra(int argc, char **argv)
{
	int i, argsleft;
	char *kw, *val;

	if (argc % 2 != 0)
		usage();

	i = 0;
	for (argsleft = argc; argsleft > 1; argsleft -= 2) {
		kw =  argv[i];
		val = argv[i+1];
		if (strcmp(kw, "recvra") == 0)
			conf.recvra = (atoi(val) != 0);
		else if (strcmp(kw, "sendra") == 0)
			conf.sendra = (atoi(val) != 0);
		else if (strcmp(kw, "mflag") == 0)
			conf.mflag = (atoi(val) != 0);
		else if (strcmp(kw, "oflag") == 0)
			conf.oflag = (atoi(val) != 0);
		else if (strcmp(kw, "maxraint") == 0)
			conf.maxraint = atoi(val);
		else if (strcmp(kw, "minraint") == 0)
			conf.minraint = atoi(val);
		else if (strcmp(kw, "linkmtu") == 0)
			conf.linkmtu = atoi(val);
		else if (strcmp(kw, "reachtime") == 0)
			conf.reachtime = atoi(val);
		else if (strcmp(kw, "rxmitra") == 0)
			conf.rxmitra = atoi(val);
		else if (strcmp(kw, "ttl") == 0)
			conf.ttl = atoi(val);
		else if (strcmp(kw, "routerlt") == 0)
			conf.routerlt = atoi(val);
		else {
			warning("bad ra6 keyword %s", kw);
			usage();
		}
		i += 2;
	}

	/* consistency check */
	if (conf.maxraint < conf.minraint)
		sysfatal("maxraint %d < minraint %d",
			conf.maxraint, conf.minraint);
}

void
ea2lla(uchar *lla, uchar *ea)
{
	memset(lla, 0, IPaddrlen);
	lla[0]  = 0xFE;
	lla[1]  = 0x80;
	lla[8]  = ea[0] ^ 0x2;
	lla[9]  = ea[1];
	lla[10] = ea[2];
	lla[11] = 0xFF;
	lla[12] = 0xFE;
	lla[13] = ea[3];
	lla[14] = ea[4];
	lla[15] = ea[5];
}

int
findllip(uchar *ip, Ipifc *ifc)
{
	Iplifc *lifc;

	for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next){
		if(ISIPV6LINKLOCAL(lifc->ip)){
			ipmove(ip, lifc->ip);
			return 1;
		}
	}
	ipmove(ip, v6Unspecified);
	return 0;
}

static int
dialicmpv6(uchar *ip, int port)
{
	char addr[128], local[128];
	int fd, cfd;

	snprint(addr, sizeof(addr), "%s/icmpv6!%I!%d!r", conf.mpoint, ip, port);
	snprint(local, sizeof(local), "%I!%d", conf.lladdr, port);
	if((fd = dial(addr, local, nil, &cfd)) < 0)
		sysfatal("dialicmp6: %r");
	fprint(cfd, "headers");
	fprint(cfd, "ignoreadvice");
	if(ISIPV6MCAST(ip))
		fprint(cfd, "addmulti %I", conf.lladdr);
	close(cfd);
	return fd;
}

static int
arpenter(uchar *ip, uchar *mac)
{
	char buf[256];
	int fd, n;

	if(!validip(ip))
		return 0;
	snprint(buf, sizeof buf, "%s/arp", conf.mpoint);
	if((fd = open(buf, OWRITE)) < 0){
		warning("couldn't open %s: %r", buf);
		return -1;
	}
	n = snprint(buf, sizeof buf, "add %s %I %E %I\n", conf.type, ip, mac, conf.lladdr);
	if(write(fd, buf, n) != n) {
		warning("arpenter: %s: %r", buf);
		close(fd);
		return 0;
	}
	close(fd);
	return 1;
}

static int
arpcheck(uchar *ip)
{
	char buf[256], *f[5], *p;
	uchar addr[IPaddrlen];
	Biobuf *bp;
	int rv;

	snprint(buf, sizeof buf, "%s/arp", conf.mpoint);
	bp = Bopen(buf, OREAD);
	if(bp == nil){
		warning("couldn't open %s: %r", buf);
		return -1;
	}
	rv = 0;
	while((p = Brdline(bp, '\n')) != nil){
		p[Blinelen(bp)-1] = 0;
		if(tokenize(p, f, nelem(f)) < 3)
			continue;
		if(parseip(addr, f[2]) != -1)
			continue;
		if(ipcmp(addr, ip) == 0){
			rv = 1;
			break;
		}
	}
	Bterm(bp);
	return rv;
}

/* add ipv6 addr to an interface */
int
ip6cfg(void)
{
	char buf[256];
	int tentative, n;

	if(!validip(conf.laddr) || isv4(conf.laddr))
		return -1;

	tentative = dupl_disc && isether();

Again:
	if(tentative)
		n = sprint(buf, "try");
	else
		n = sprint(buf, "add");

	n += snprint(buf+n, sizeof buf-n, " %I", conf.laddr);
	if(!validip(conf.mask))
		ipmove(conf.mask, defmask(conf.laddr));
	n += snprint(buf+n, sizeof buf-n, " %M", conf.mask);
	if(!validip(conf.raddr) || isv4(conf.raddr))
		maskip(conf.laddr, conf.mask, conf.raddr);
	n += snprint(buf+n, sizeof buf-n, " %I", conf.raddr);
	n += snprint(buf+n, sizeof buf-n, " %d", conf.mtu);
	if(yflag)
		n += snprint(buf+n, sizeof buf-n, " proxy");

	DEBUG("ip6cfg: %.*s", n, buf);
	if(write(conf.cfd, buf, n) < 0){
		warning("write(%s): %r", buf);
		return -1;
	}

	if(!tentative){
		if(validip(conf.gaddr) && !isv4(conf.gaddr)
		&& ipcmp(conf.gaddr, conf.laddr) != 0
		&& ipcmp(conf.gaddr, conf.lladdr) != 0)
			adddefroute(conf.gaddr, conf.laddr, conf.laddr, conf.raddr, conf.mask);
		return 0;
	}

	sleep(1000);

	if(arpcheck(conf.laddr) <= 0) {
		tentative = 0;
		goto Again;
	}

	warning("found dup entry in arp cache");
	ipunconfig();
	return -1;
}

static int
recvra6on(Ipifc *ifc)
{
	if(!myip(ifc, conf.lladdr))
		return 0;
	else if(ifc->sendra6 > 0)
		return IsRouter;
	else if(ifc->recvra6 > 0 || noconfig)
		return IsHostRecv;
	else
		return IsHostNoRecv;
}

static void
sendrs(int fd, uchar *dst)
{
	Routersol *rs;
	Lladdropt *llao;
	uchar buf[1024];
	int pktlen;

	memset(buf, 0, sizeof buf);

	rs = (Routersol*)buf;
	rs->type = ICMP6_RS;
	ipmove(rs->dst, dst);
	ipmove(rs->src, conf.lladdr);
	pktlen = sizeof *rs;

	if(conf.hwalen > 0){
		llao = (Lladdropt*)&buf[pktlen];
		llao->type = V6nd_srclladdr;
		llao->len = (2+7+conf.hwalen)/8;
		memmove(llao->lladdr, conf.hwa, conf.hwalen);
		pktlen += 8 * llao->len;
	}

	if(write(fd, rs, pktlen) != pktlen){
		DEBUG("sendrs: write failed, pkt size %d", pktlen);
	} else {
		DEBUG("sendrs: sent solicitation to %I from %I on %s",
			rs->dst, rs->src, conf.dev);
	}
}

static void
ewrite(int fd, char *str)
{
	int n;

	if(noconfig){
		DEBUG("ewrite: %s\n", str);
		return;
	}

	if(fd < 0)
		return;

	n = strlen(str);
	if(write(fd, str, n) != n)
		warning("write(%s) failed: %r", str);
}

static void
issuebasera6(Conf *cf)
{
	char *cfg;

	cfg = smprint("ra6 mflag %d oflag %d reachtime %d rxmitra %d "
		"ttl %d routerlt %d linkmtu %d",
		cf->mflag, cf->oflag, cf->reachtime, cf->rxmitra,
		cf->ttl, cf->routerlt, cf->linkmtu);
	ewrite(cf->cfd, cfg);
	free(cfg);
}

static void
issuerara6(Conf *cf)
{
	char *cfg;

	myifc->sendra6 = cf->sendra;
	myifc->recvra6 = cf->recvra;

	cfg = smprint("ra6 sendra %d recvra %d maxraint %d minraint %d",
		cf->sendra, cf->recvra, cf->maxraint, cf->minraint);
	ewrite(cf->cfd, cfg);
	free(cfg);
}

static void
issueadd6(Conf *cf)
{
	char *cfg;

	cfg = smprint("add6 %I %d %d %d %lud %lud", cf->v6pref, cf->prefixlen,
		cf->onlink, cf->autoflag, cf->validlt, cf->preflt);
	ewrite(cf->cfd, cfg);
	free(cfg);
}

static void
issuedel6(Conf *cf)
{
	/* use "remove6" verb instead of "del6" for older kernels */
	ewrite(cf->cfd, "remove6");
}

static int
masklen(uchar *mask)
{
	int len;

	for(len=0; len < 128; len += 8){
		if(*mask != 255)
			break;
		mask++;
	}
	while(len < 128 && (*mask & (0x80 >> (len & 7))) != 0)
		len++;
	return len;
}

void
genipmask(uchar *mask, int len)
{
	memset(mask, 0, IPaddrlen);
	if(len < 0)
		len = 0;
	else if(len > 128)
		len = 128;
	for(; len >= 8; len -= 8)
		*mask++ = 255;
	if(len > 0)
		*mask = ~((1<<(8-len))-1);
}

typedef struct Route Route;
struct Route
{
	Route	*next;
	ulong	time;

	ulong	prefixlt;
	ulong	routerlt;

	uchar	src[IPaddrlen];
	uchar	gaddr[IPaddrlen];
	uchar	laddr[IPaddrlen];
	uchar	mask[IPaddrlen];

	uchar	hash[SHA1dlen];
};

static Route	*routelist;

int
validv6prefix(uchar *ip)
{
	if(!validip(ip))
		return 0;
	if(isv4(ip))
		return 0;
	if(ipcmp(ip, v6loopback) == 0)
		return 0;
	if(ISIPV6MCAST(ip))
		return 0;
	if(ISIPV6LINKLOCAL(ip))
		return 0;
	return 1;
}

/*
 * host receiving a router advertisement calls this
 */
static int
recvrahost(uchar buf[], int pktlen, ulong now)
{
	char dnsdomain[sizeof(conf.dnsdomain)];
	int m, n, optype, seen, needrefresh;
	Lladdropt *llao;
	Mtuopt *mtuo;
	Prefixopt *prfo;
	Ipaddrsopt *addrso;
	Routeradv *ra;
	uchar raddr[IPaddrlen];
	uchar hash[SHA1dlen];
	Route *r, **rr;

	m = sizeof *ra;
	ra = (Routeradv*)buf;
	if(pktlen < m)
		return -1;

	if(!ISIPV6LINKLOCAL(ra->src))
		return -1;

	DEBUG("got RA from %I on %s; flags %x", ra->src, conf.dev, ra->mor);

	/*
	 * ignore all non-managed-flag router-advertisements
	 * when dhcpv6 is active so we do not trash conf data.
	 */
	if(dodhcp && conf.state && (MFMASK & ra->mor) == 0)
		return -1;

	conf.ttl = ra->cttl;
	conf.mflag = (MFMASK & ra->mor) != 0;
	conf.oflag = (OCMASK & ra->mor) != 0;
	conf.routerlt = nhgets(ra->routerlt);
	conf.reachtime = nhgetl(ra->rchbltime);
	conf.rxmitra = nhgetl(ra->rxmtimer);
	conf.linkmtu = DEFMTU;

	ipmove(conf.v6router, conf.routerlt? ra->src: IPnoaddr);

	memset(conf.dns, 0, sizeof(conf.dns));
	memset(conf.fs, 0, sizeof(conf.fs));
	memset(conf.auth, 0, sizeof(conf.auth));
	memset(conf.dnsdomain, 0, sizeof(conf.dnsdomain));

	/* process options */
	while(pktlen - m >= 8) {
		n = m;
		optype = buf[n];
		m += 8 * buf[n+1];
		if(m <= n || pktlen < m)
			return -1;

		switch (optype) {
		case V6nd_srclladdr:
			llao = (Lladdropt*)&buf[n];
			if(llao->len == 1 && conf.hwalen == 6)
				arpenter(ra->src, llao->lladdr);
			break;
		case V6nd_mtu:
			mtuo = (Mtuopt*)&buf[n];
			conf.linkmtu = nhgetl(mtuo->mtu);
			break;

		case V6nd_rdnssl:
			addrso = (Ipaddrsopt*)&buf[n];
			if(gnames(dnsdomain, sizeof(dnsdomain),
				addrso->addrs, (addrso->len - 1)*8) <= 0)
				break;
			addnames(conf.dnsdomain, dnsdomain, sizeof(conf.dnsdomain));
			break;

		case V6nd_rdns:
			addrso = (Ipaddrsopt*)&buf[n];
			n = (addrso->len - 1) * 8;
			if(n == 0 || n % IPaddrlen)
				break;
			addaddrs(conf.dns, sizeof(conf.dns), addrso->addrs, n);
			break;

		case V6nd_9fs:
			addrso = (Ipaddrsopt*)&buf[n];
			n = (addrso->len - 1) * 8;
			if(n == 0 || n % IPaddrlen || !plan9)
				break;
			addaddrs(conf.fs, sizeof(conf.fs), addrso->addrs, n);
			break;
		case V6nd_9auth:
			addrso = (Ipaddrsopt*)&buf[n];
			n = (addrso->len - 1) * 8;
			if(n == 0 || n % IPaddrlen || !plan9)
				break;
			addaddrs(conf.auth, sizeof(conf.auth), addrso->addrs, n);
			break;
		}
	}

	issuebasera6(&conf);

	/* remove expired default routes */
	m = 0;
	for(rr = &routelist; (r = *rr) != nil;){
		if(m > 100
		|| r->prefixlt != ~0UL && r->prefixlt < now-r->time
		|| r->routerlt != ~0UL && r->routerlt < now-r->time){
			DEBUG("purging RA from %I on %s; pfx %I %M",
				r->src, conf.dev, r->laddr, r->mask);
			if(validip(r->gaddr)){
				maskip(r->laddr, r->mask, raddr);
				deldefroute(r->gaddr, conf.lladdr, r->laddr, raddr, r->mask);
			}
			*rr = r->next;
			free(r);
			continue;
		}
		rr = &r->next;
		m++;
	}

	/* remove expired prefixes */
	issuedel6(&conf);
	ipmove(conf.laddr, IPnoaddr);

	/* managed network: prefixes are acquired with dhcpv6 */
	if(dodhcp && conf.mflag)
		return 0;

	/* process new prefixes */
	needrefresh = 0;
	m = sizeof *ra;
	while(pktlen - m >= 8) {
		n = m;
		optype = buf[n];
		m += 8 * buf[n+1];
		if(m <= n || pktlen < m)
			return -1;

		if(optype != V6nd_pfxinfo)
			continue;

		prfo = (Prefixopt*)&buf[n];
		if(prfo->len != 4)
			continue;

		if((prfo->lar & AFMASK) == 0)
			continue;

		conf.prefixlen = prfo->plen & 127;
		if(conf.prefixlen < 1
		|| conf.prefixlen > 64)
			continue;

		genipmask(conf.v6mask, conf.prefixlen);
		maskip(prfo->pref, conf.v6mask, conf.v6pref);
		if(!validv6prefix(conf.v6pref))
			continue;
		ipmove(conf.raddr, conf.v6pref);
		ipmove(conf.mask, conf.v6mask);
		memmove(conf.laddr, conf.v6pref, 8);
		memmove(conf.laddr+8, conf.lladdr+8, 8);
		ipmove(conf.gaddr, (prfo->lar & RFMASK) != 0? prfo->pref: conf.v6router);
		conf.onlink = (prfo->lar & OLMASK) != 0;
		conf.autoflag = (prfo->lar & AFMASK) != 0;
		conf.validlt = nhgetl(prfo->validlt);
		conf.preflt = nhgetl(prfo->preflt);
		if(conf.preflt > conf.validlt)
			continue;

		if(conf.preflt == 0
		|| isula(conf.laddr)
		|| ipcmp(conf.gaddr, conf.laddr) == 0
		|| ipcmp(conf.gaddr, conf.lladdr) == 0)
			ipmove(conf.gaddr, IPnoaddr);

		sha1((uchar*)&conf, sizeof(conf), hash, nil);
		for(rr = &routelist; (r = *rr) != nil; rr = &r->next){
			if(ipcmp(r->src, ra->src) == 0
			&& ipcmp(r->laddr, conf.laddr) == 0){
				*rr = r->next;
				break;
			}
		}
		if(r != nil){
			/* rfc4862, section 5.5.3, e) */
			ulong remaining = r->prefixlt != ~0UL ? r->time + r->prefixlt : ~0UL;
			ulong signaled = conf.validlt != ~0UL ? now + conf.validlt : ~0UL;
			ulong timeout = now + 2*60*60;	/* 2 horus from now */
			if(signaled <= timeout && signaled <= remaining) {
				if(remaining <= timeout)
					conf.validlt = remaining - now;
				else
					conf.validlt = timeout - now;
				if (conf.preflt > conf.validlt)
					conf.preflt = conf.validlt;
			}
			seen = memcmp(r->hash, hash, SHA1dlen) == 0;
			if(!seen && validip(r->gaddr) && ipcmp(r->gaddr, conf.gaddr) != 0){
				DEBUG("changing router %I->%I", r->gaddr, conf.gaddr);
				maskip(r->laddr, r->mask, raddr);
				deldefroute(r->gaddr, conf.lladdr, r->laddr, raddr, r->mask);
			}
		} else {
			seen = 0;
			if(conf.validlt == 0)
				continue;
			r = malloc(sizeof(*r));
		}
		memmove(r->hash, hash, SHA1dlen);

		r->time = now;
		r->routerlt = conf.routerlt;
		r->prefixlt = conf.validlt;
		ipmove(r->src, ra->src);
		ipmove(r->gaddr, conf.gaddr);
		ipmove(r->laddr, conf.laddr);
		ipmove(r->mask, conf.mask);

		r->next = routelist;
		routelist = r;
	
		/* add prefix and update parameters */
		issueadd6(&conf);

		/* report only once */
		if(seen)
			continue;

		/* must not be deprecated */
		if(conf.preflt == 0)
			continue;

		DEBUG("got prefix %I %M via %I on %s",
			conf.v6pref, conf.mask, conf.gaddr, conf.dev);

		if(validip(conf.gaddr))
			adddefroute(conf.gaddr, conf.lladdr, conf.laddr, conf.raddr, conf.mask);

		needrefresh |= 1<<(putndb(1) != 0);
	}
	if(needrefresh > 1 || (needrefresh == 0 && putndb(0)))
		refresh();
	return 0;
}

/*
 * daemon to receive router advertisements from routers
 */
static int
recvra6(void)
{
	int fd, n, sendrscnt, gotra, pktcnt, sleepfor;
	uchar buf[4096];
	ulong now;

	fd = dialicmpv6(v6allnodesL, ICMP6_RA);
	if(fd < 0)
		sysfatal("can't open icmp_ra connection: %r");

	switch(rfork(RFPROC|RFFDG|RFNOWAIT|RFNOTEG)){
	case -1:
		sysfatal("can't fork: %r");
	default:
		close(fd);
		DEBUG("recvra6 on %s", conf.dev);

		/* wait for initial RA */
		return (int)(uintptr)rendezvous(recvra6, (void*)0);
	case 0:
		break;
	}
	procsetname("recvra6 on %s %I", conf.dev, conf.lladdr);
	notify(catch);

	gotra = 0;
restart:
	sendrscnt = 0;
	if(recvra6on(myifc) == IsHostRecv){
		sendrs(fd, v6allroutersL);
		sendrscnt = Maxv6rss;
	}
	pktcnt = 0;
	sleepfor = Minv6interradelay;
	conf.state = 0;	/* dhcpv6 off */

	for (;;) {
		alarm(sleepfor);
		n = read(fd, buf, sizeof buf);
		sleepfor = alarm(0);

		/* wait for alarm to expire */
		if(sleepfor > 100){
			if(pktcnt > 100)
				continue;
			pktcnt++;
		} else {
			pktcnt = 1;
		}
		sleepfor = Maxv6radelay;
		now = time(nil);

		myifc = readipifc(conf.mpoint, myifc, myifc->index);
		if(myifc == nil) {
			warning("recvra6: can't read router params on %s, quitting on %s",
				conf.mpoint, conf.dev);
			if(!gotra)
				rendezvous(recvra6, (void*)-1);
			exits(nil);
		}

		switch(recvra6on(myifc)){
		case IsHostRecv:
			break;
		default:
			warning("recvra6: recvra off, quitting on %s", conf.dev);
			if(!gotra)
				rendezvous(recvra6, (void*)-1);
			exits(nil);
		}

		if(n <= 0) {
			if(sendrscnt > 0) {
				sendrscnt--;
				sendrs(fd, v6allroutersL);
				sleepfor = V6rsintvl + nrand(100);
			} else if(!gotra) {
				warning("recvra6: no router advs after %d sols on %s",
					Maxv6rss, conf.dev);
				rendezvous(recvra6, (void*)0);
			} else if(dodhcp && conf.state)
				goto renewdhcp;
			continue;
		}

		if(recvrahost(buf, n, now) < 0)
			continue;

		if(dodhcp && conf.mflag){
			if(conf.state){
				if(validip(conf.gaddr) && ipcmp(conf.gaddr, conf.v6router) != 0){
					warning("dhcpv6: default router changed %I -> %I on %s",
						conf.gaddr, conf.v6router, conf.dev);
				} else {
renewdhcp:
					/* when renewing, wait for lease to time-out */
					if(now < conf.timeout)
						continue;
				}
			}
			if(dhcpv6query(conf.state) < 0){
				if(conf.state == 0)
					continue;

				DEBUG("dhcpv6 failed renew for %I with prefix %I %M via %I on %s",
					conf.laddr, conf.v6pref, conf.v6mask, conf.gaddr, conf.dev);

				if(!noconfig){
					fprint(conf.rfd, "tag dhcp");
					if(validip(conf.gaddr))
						if(conf.preflt && validv6prefix(conf.v6pref) && !isula(conf.v6pref))
							deldefroute(conf.gaddr, conf.lladdr, IPnoaddr, conf.v6pref, conf.v6mask);
					ipunconfig();
					fprint(conf.rfd, "tag ra6");
				}
				/* restart sending router solitications */
				conf.state = 0;
				goto restart;
			}
			now = time(nil);
			conf.state = 1;	/* dhcpv6 active */
			sendrscnt = 0;	/* stop sending router solicitations */

			DEBUG("dhcpv6 got %I with prefix %I %M via %I on %s for lease %lud",
				conf.laddr, conf.v6pref, conf.v6mask, conf.v6router, conf.dev, conf.lease);

			if(!noconfig){
				fprint(conf.rfd, "tag dhcp");

				if(validip(conf.gaddr) && ipcmp(conf.gaddr, conf.v6router) != 0){
					deldefroute(conf.gaddr, conf.lladdr, conf.laddr, conf.raddr, conf.mask);
					if(conf.preflt && validv6prefix(conf.v6pref) && !isula(conf.v6pref))
						deldefroute(conf.gaddr, conf.lladdr, IPnoaddr, conf.v6pref, conf.v6mask);
				}
				ipmove(conf.gaddr, conf.v6router);

				if(ip6cfg() < 0){
					fprint(conf.rfd, "tag ra6");
					continue;
				}
				putndb(1);
				if(conf.preflt && validv6prefix(conf.v6pref)){
					uchar save[IPaddrlen];

					/* if we got a delegated prefix, add the default route */
					if(validip(conf.gaddr) && !isula(conf.v6pref))
						adddefroute(conf.gaddr, conf.lladdr, IPnoaddr, conf.v6pref, conf.v6mask);

					/* store prefix info in /net/ndb */
					ipmove(save, conf.laddr);
					ipmove(conf.laddr, IPnoaddr);		/* we don't have an address there */
					ipmove(conf.raddr, conf.v6pref);
					ipmove(conf.mask, conf.v6mask);
					putndb(1);
					ipmove(conf.laddr, save);
					ipmove(conf.raddr, save);		/* was same as laddr as /128 */
					memset(conf.mask, 0xFF, IPaddrlen);
				}
				refresh();
				fprint(conf.rfd, "tag ra6");
			}
			/* infinity */
			if(conf.lease == ~0UL){
				if(!gotra)
					rendezvous(recvra6, (void*)1);
				exits(nil);
			}
			if(conf.lease < 60)
				conf.lease = 60;
			conf.timeout = now + conf.lease;
		}

		/* got at least initial ra; no whining */
		if(!gotra){
			gotra = 1;
			rendezvous(recvra6, (void*)1);
		}
	}
}

/*
 * return -1 -- error, reading/writing some file,
 *         0 -- no arp table updates
 *         1 -- successful arp table update
 */
static int
recvrs(uchar *buf, int pktlen, uchar *sol)
{
	int n;
	Routersol *rs;
	Lladdropt *llao;

	n = sizeof *rs + sizeof *llao;
	rs = (Routersol*)buf;
	if(pktlen < n)
		return 0;

	llao = (Lladdropt*)&buf[sizeof *rs];
	if(llao->type != V6nd_srclladdr || llao->len != 1 || conf.hwalen != 6)
		return 0;

	if(!validip(rs->src)
	|| isv4(rs->src)
	|| ipcmp(rs->src, v6loopback) == 0
	|| ISIPV6MCAST(rs->src))
		return 0;

	if((n = arpenter(rs->src, llao->lladdr)) <= 0)
		return n;

	ipmove(sol, rs->src);
	return 1;
}

static void
sendra(int fd, uchar *dst, int rlt, Ipifc *ifc, Ndb *db)
{
	uchar dns[sizeof(conf.dns)], fs[sizeof(conf.fs)], auth[sizeof(conf.auth)];
	char dnsdomain[sizeof(conf.dnsdomain)];
	Ipaddrsopt *addrso;
	Prefixopt *prfo;
	Iplifc *lifc;
	Routeradv *ra;
	uchar buf[1024];
	int pktlen, n;

	memset(dns, 0, sizeof(dns));
	memset(fs, 0, sizeof(fs));
	memset(auth, 0, sizeof(auth));
	memset(dnsdomain, 0, sizeof(dnsdomain));

	memset(buf, 0, sizeof buf);

	ra = (Routeradv*)buf;
	ipmove(ra->dst, dst);
	ipmove(ra->src, conf.lladdr);
	ra->type = ICMP6_RA;
	ra->cttl = conf.ttl;
	if(conf.mflag > 0)
		ra->mor |= MFMASK;
	if(conf.oflag > 0)
		ra->mor |= OCMASK;
	if(rlt > 0)
		hnputs(ra->routerlt, conf.routerlt);
	else
		hnputs(ra->routerlt, 0);
	hnputl(ra->rchbltime, conf.reachtime);
	hnputl(ra->rxmtimer, conf.rxmitra);
	pktlen = sizeof *ra;

	/*
	 * include link layer address (mac address for now) in
	 * link layer address option
	 */
	if(conf.hwalen > 0){
		Lladdropt *llao = (Lladdropt *)&buf[pktlen];
		llao->type = V6nd_srclladdr;
		llao->len = (2+7+conf.hwalen)/8;
		memmove(llao->lladdr, conf.hwa, conf.hwalen);
		pktlen += 8 * llao->len;
	}

	/* include all global unicast prefixes on interface in prefix options */
	for (lifc = (ifc != nil? ifc->lifc: nil); lifc != nil; lifc = lifc->next) {
		if(pktlen > sizeof buf - 4*8)
			break;

		if(!validv6prefix(lifc->ip))
			continue;

		prfo = (Prefixopt*)&buf[pktlen];
		prfo->type = V6nd_pfxinfo;
		prfo->len = 4;
		prfo->plen = masklen(lifc->mask) & 127;
		if(prfo->plen == 0)
			continue;
		ipmove(prfo->pref, lifc->net);
		prfo->lar = AFMASK|OLMASK;
		hnputl(prfo->validlt, lifc->validlt);
		hnputl(prfo->preflt, lifc->preflt);
		pktlen += 8 * prfo->len;

		/* get ndb configuration for this prefix */
		ipmove(conf.laddr, lifc->ip);
		ndb2conf(db, lifc->net);

		addaddrs(dns, sizeof(dns), conf.dns, sizeof(conf.dns));
		addaddrs(fs, sizeof(fs), conf.fs, sizeof(conf.fs));
		addaddrs(auth, sizeof(auth), conf.auth, sizeof(conf.auth));

		addnames(dnsdomain, conf.dnsdomain, sizeof(dnsdomain));
	}

	addrso = (Ipaddrsopt*)&buf[pktlen];
	n = pnames(addrso->addrs, sizeof buf - 8 - pktlen, dnsdomain);
	if(n > 0){
		addrso->type = V6nd_rdnssl;
		addrso->len = 1 + ((n + 7) / 8);
		hnputl(addrso->lifetime, ~0L);
		pktlen += 8 * addrso->len;
	}

	if((n = countaddrs(dns, sizeof(dns))) > 0 && pktlen+8+n*IPaddrlen <= sizeof buf) {
		addrso = (Ipaddrsopt*)&buf[pktlen];
		addrso->type = V6nd_rdns;
		addrso->len = 1 + n*2;
		memmove(addrso->addrs, dns, n*IPaddrlen);
		hnputl(addrso->lifetime, ~0L);
		pktlen += 8 * addrso->len;
	}

	if(!plan9)
		goto send;

	/* send plan9 specific options */
	if((n = countaddrs(fs, sizeof(fs))) > 0 && pktlen+8+n*IPaddrlen <= sizeof buf) {
		addrso = (Ipaddrsopt*)&buf[pktlen];
		addrso->type = V6nd_9fs;
		addrso->len = 1 + n*2;
		memmove(addrso->addrs, fs, n*IPaddrlen);
		hnputl(addrso->lifetime, ~0L);
		pktlen += 8 * addrso->len;
	}
	if((n = countaddrs(auth, sizeof(auth))) > 0 && pktlen+8+n*IPaddrlen <= sizeof buf) {
		addrso = (Ipaddrsopt*)&buf[pktlen];
		addrso->type = V6nd_9auth;
		addrso->len = 1 + n*2;
		memmove(addrso->addrs, auth, n*IPaddrlen);
		hnputl(addrso->lifetime, ~0L);
		pktlen += 8 * addrso->len;
	}

send:
	write(fd, buf, pktlen);
}

/*
 * daemon to send router advertisements to hosts
 */
static void
sendra6(void)
{
	int fd, n, sleepfor, nquitmsgs;
	uchar buf[4096], dst[IPaddrlen];
	Ndb *db;

	db = opendatabase();
	if(db == nil)
		warning("couldn't open ndb: %r");

	fd = dialicmpv6(v6allroutersL, ICMP6_RS);
	if(fd < 0)
		sysfatal("can't open icmp_rs connection: %r");

	switch(rfork(RFPROC|RFFDG|RFNOWAIT|RFNOTEG)){
	case -1:
		sysfatal("can't fork: %r");
	default:
		close(fd);
		DEBUG("sendra6 on %s", conf.dev);
		return;
	case 0:
		break;
	}
	procsetname("sendra6 on %s %I", conf.dev, conf.lladdr);
	notify(catch);

	nquitmsgs = Maxv6finalras;
	sleepfor = 100 + jitter();

	for (;;) {
		alarm(sleepfor);
		n = read(fd, buf, sizeof buf);
		sleepfor = alarm(0);

		if(myifc->sendra6 > 0 && n > 0 && recvrs(buf, n, dst) > 0)
			sendra(fd, dst, 1, myifc, db);

		/* wait for alarm to expire */
		if(sleepfor > 100)
			continue;
		sleepfor = Minv6interradelay;

		myifc = readipifc(conf.mpoint, myifc, myifc->index);
		if(myifc == nil || !myip(myifc, conf.lladdr)) {
			warning("sendra6: can't read router params on %s, quitting on %s",
				conf.mpoint, conf.dev);
			exits(nil);
		}
		if(myifc->sendra6 <= 0){
			if(nquitmsgs > 0) {
				nquitmsgs--;
				sendra(fd, v6allnodesL, 0, myifc, nil);
				continue;
			}
			warning("sendra6: sendra off on %s, quitting on %s",
				conf.mpoint, conf.dev);
			exits(nil);
		}
		db = opendatabase();
		sendra(fd, v6allnodesL, 1, myifc, db);
		sleepfor = randint(myifc->rp.minraint, myifc->rp.maxraint);
	}
}

static void
startra6(void)
{
	if(!findllip(conf.lladdr, myifc))
		sysfatal("no link local address");

	if(conf.recvra > 0)
		recvra6();

	dolog = 1;
	if(conf.sendra > 0) {
		ewrite(conf.cfd, "iprouting 1");
		sendra6();
		if(conf.recvra <= 0)
			recvra6();
	}
}

void
doipv6(int what)
{
	fprint(conf.rfd, "tag ra6");
	switch (what) {
	default:
		sysfatal("unknown IPv6 verb");
	case Vaddpref6:
		issueadd6(&conf);
		refresh();
		break;
	case Vra6:
		issuebasera6(&conf);
		issuerara6(&conf);
		startra6();
		break;
	}
}

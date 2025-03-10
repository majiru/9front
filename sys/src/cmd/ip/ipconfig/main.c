/*
 * ipconfig - configure parameters of an ip stack
 */
#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ip.h>
#include <ndb.h>
#include "ipconfig.h"
#include "../dhcp.h"

#include <libsec.h> /* genrandom() */

Conf	conf;
Ipifc*	myifc;
int	beprimary = -1;
int	noconfig;
Ctl	*firstctl, **ctll = &firstctl;

int	debug;
int	dolog;

int	plan9 = 1;
int	Oflag;
int	rflag;
int	tflag;
int	yflag;

int	dodhcp;
int	nodhcpwatch;
int	sendhostname;
char	*ndboptions;
char	*ipnet;

int	ipv6auto;
int	dupl_disc = 1;		/* flag: V6 duplicate neighbor discovery */

int	dondbconfig;
char	*dbfile;

static char logfile[] = "ipconfig";

static void	openiproute(void);
static void	binddevice(void);
static void	controldevice(void);
extern void	pppbinddev(void);

static void	doadd(void);
static void	dodel(void);
static void	dounbind(void);
static void	ndbconfig(void);

static int	Ufmt(Fmt*);
#pragma varargck type "U" char*

void
usage(void)
{
	fprint(2, "usage: %s [-6dDGnNOpPrtuXy][-b baud][-c ctl]* [-U duid] [-g gw]"
		"[-h host][-i ipnet][-m mtu][-s dns]...\n"
		"\t[-f dbfile][-x mtpt][-o dhcpopt] type dev [verb] [laddr [mask "
		"[raddr [fs [auth]]]]]\n", argv0);
	exits("usage");
}

static void
init(void)
{
	srand(truerand());

	fmtinstall('H', encodefmt);
	fmtinstall('E', eipfmt);
	fmtinstall('I', eipfmt);
	fmtinstall('M', eipfmt);
	fmtinstall('V', eipfmt);
	fmtinstall('U', Ufmt);
	fmtinstall('$', ndbvalfmt);

	nsec();			/* make sure time file is open before forking */

	conf.cfd = -1;
	conf.rfd = -1;

	setnetmtpt(conf.mpoint, sizeof conf.mpoint, nil);
	conf.cputype = getenv("cputype");
	if(conf.cputype == nil)
		conf.cputype = "386";

	v6paraminit(&conf);

	dhcpinit();
}

void
warning(char *fmt, ...)
{
	char buf[1024];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf + sizeof buf, fmt, arg);
	va_end(arg);
	if (dolog)
		syslog(0, logfile, "%s", buf);
	else
		fprint(2, "%s: %s\n", argv0, buf);
}

static void
parsenorm(int argc, char **argv)
{
	switch(argc){
	case 5:
		 if (parseip(conf.auth, argv[4]) == -1)
			usage();
		/* fall through */
	case 4:
		 if (parseip(conf.fs, argv[3]) == -1)
			usage();
		/* fall through */
	case 3:
		 if (parseip(conf.raddr, argv[2]) == -1)
			usage();
		/* fall through */
	case 2:
		if (strcmp(argv[1], "0") != 0){
			if (parseipandmask(conf.laddr, conf.mask, argv[0], argv[1]) == -1)
				usage();
			break;
		}
		/* fall through */
	case 1:
		 if (parseip(conf.laddr, argv[0]) == -1)
			usage();
		/* fall through */
	case 0:
		break;
	default:
		usage();
	}
}

static char*
finddev(char *dir, char *name, char *dev)
{
	int fd, i, nd;
	Dir *d;

	fd = open(dir, OREAD);
	if(fd >= 0){
		d = nil;
		nd = dirreadall(fd, &d);
		close(fd);
		for(i=0; i<nd; i++){
			if(strncmp(d[i].name, name, strlen(name)))
				continue;
			if(strstr(d[i].name, "ctl") != nil)
				continue;	/* ignore ctl files */
			dev = smprint("%s/%s", dir, d[i].name);
			break;
		}
		free(d);
	}
	return dev;
}

/* look for an action */
static int
parseverb(char *name)
{
	static char *verbs[] = {
		[Vadd]		"add",
		[Vdel]		"del",
		[Vunbind]	"unbind",
		[Vether]	"ether",
		[Vgbe]		"gbe",
		[Vppp]		"ppp",
		[Vloopback]	"loopback",
		[Vaddpref6]	"add6",
		[Vra6]		"ra6",
		[Vtorus]	"torus",
		[Vtree]		"tree",
		[Vpkt]		"pkt",
		[Vnull]		"null",
	};
	int i;

	for(i = 0; i < nelem(verbs); i++)
		if(verbs[i] != nil && strcmp(name, verbs[i]) == 0)
			return i;

	if(strcmp(name, "remove")==0)
		return Vdel;

	return -1;
}

static int
parseargs(int argc, char **argv)
{
	char *p;
	int action, verb;

	/* default to any host name we already have */
	if(*conf.hostname == 0){
		p = getenv("sysname");
		if(p == nil || *p == 0)
			p = sysname();
		if(p != nil)
			utf2idn(p, conf.hostname, sizeof(conf.hostname));
	}

	/* defaults */
	conf.type = "ether";
	conf.dev = nil;
	action = Vadd;

	/* get optional medium and device */
	if (argc > 0){
		verb = parseverb(*argv);
		switch(verb){
		case Vether:
		case Vgbe:
		case Vloopback:
		case Vpkt:
		case Vppp:
		case Vtorus:
		case Vtree:
		case Vnull:
			conf.type = *argv++;
			argc--;
			if(argc > 0){
				conf.dev = *argv++;
				argc--;
			} else if(verb == Vppp)
				conf.dev = finddev("/dev", "eia", "/dev/eia0");
			break;
		}
	}
	if(conf.dev == nil){
		if(!isether())
			sysfatal("no device specified for medium %s", conf.type);
		conf.dev = finddev(conf.mpoint, "ether", "/net/ether0");
	}

	/* get optional verb */
	if (argc > 0){
		verb = parseverb(*argv);
		switch(verb){
		case Vether:
		case Vgbe:
		case Vppp:
		case Vloopback:
		case Vtorus:
		case Vtree:
		case Vpkt:
		case Vnull:
			sysfatal("medium %s already specified", conf.type);
		case Vadd:
		case Vdel:
		case Vunbind:
		case Vaddpref6:
		case Vra6:
			argv++;
			argc--;
			action = verb;
			break;
		}
	}

	/* get verb-dependent arguments */
	switch (action) {
	case Vadd:
	case Vdel:
	case Vunbind:
		parsenorm(argc, argv);
		break;
	case Vaddpref6:
		parse6pref(argc, argv);
		break;
	case Vra6:
		parse6ra(argc, argv);
		break;
	}
	return action;
}

/* all interfaces on conf.mpoint stack */
static Ipifc *allifcs;

static Ipifc*
findmyifc(void)
{
	Ipifc *nifc;

	allifcs = readipifc(conf.mpoint, allifcs, -1);
	for(nifc = allifcs; nifc != nil; nifc = nifc->next){
		if(strcmp(nifc->dev, conf.dev) == 0){
			myifc = readipifc(conf.mpoint, myifc, nifc->index);
			return myifc;
		}
	}
	return nil;
}

int
myip(Ipifc *ifc, uchar *ip)
{
	for(; ifc != nil; ifc = ifc->next) {
		if(iplocalonifc(ifc, ip) != nil)
			return 1;
	}
	return 0;
}

int
isether(void)
{
	switch(parseverb(conf.type)){
	case Vether:
	case Vgbe:
		return 1;
	}
	return 0;
}

/* create a client id */
static void
mkclientid(void)
{
	if(isether() && myetheraddr(conf.hwa, conf.dev) == 0){
		conf.hwalen = 6;
		conf.hwatype = 1;
		conf.cid[0] = conf.hwatype;
		memmove(&conf.cid[1], conf.hwa, conf.hwalen);
		conf.cidlen = conf.hwalen+1;
		if(conf.duidlen == 0){
			/* DUID-LL */
			conf.duid[0] = 0x00;
			conf.duid[1] = 0x03;
			conf.duid[2] = conf.hwatype >> 8;
			conf.duid[3] = conf.hwatype;
			memmove(conf.duid+4, conf.hwa, conf.hwalen);
			conf.duidlen = conf.hwalen+4;
		}
	} else {
		conf.hwatype = -1;
		snprint((char*)conf.cid, sizeof conf.cid,
			"plan9_%ld.%d", lrand(), getpid());
		conf.cidlen = strlen((char*)conf.cid);
		genrandom(conf.hwa, sizeof(conf.hwa));
	}
	ea2lla(conf.lladdr, conf.hwa);
}

void
main(int argc, char **argv)
{
	uchar ip[IPaddrlen];
	int action;
	Ctl *cp;
	char *s;

	init();
	ARGBEGIN {
	case '6': 			/* IPv6 auto config */
		ipv6auto = 1;
		break;
	case 'b':
		conf.baud = EARGF(usage());
		break;
	case 'c':
		cp = malloc(sizeof *cp);
		if(cp == nil)
			sysfatal("%r");
		*ctll = cp;
		ctll = &cp->next;
		cp->next = nil;
		cp->ctl = EARGF(usage());
		break;
	case 'd':
		dodhcp = 1;
		break;
	case 'D':
		debug = 1;
		break;
	case 'f':
		dbfile = EARGF(usage());
		break;
	case 'g':
		if (parseip(conf.gaddr, EARGF(usage())) == -1)
			usage();
		break;
	case 'G':
		plan9 = 0;
		break;
	case 'h':
		if(utf2idn(EARGF(usage()), conf.hostname, sizeof(conf.hostname)) <= 0)
			sysfatal("bad hostname");
		sendhostname = 1;
		break;
	case 'i':
		ipnet = EARGF(usage());
		break;
	case 'm':
		conf.mtu = atoi(EARGF(usage()));
		break;
	case 'n':
		noconfig = 1;
		break;
	case 'N':
		dondbconfig = 1;
		break;
	case 'o':
		if(addoption(EARGF(usage())) < 0)
			usage();
		break;
	case 'O':
		Oflag = 1;
		break;
	case 'p':
		beprimary = 1;
		break;
	case 'P':
		beprimary = 0;
		break;
	case 'r':
		rflag = 1;
		break;
	case 's':
		if(parseip(ip, EARGF(usage())) == -1)
			usage();
		addaddrs(conf.dns, sizeof(conf.dns), ip, sizeof(ip));
		break;
	case 't':
		tflag = 1;
		break;
	case 'u':		/* IPv6: duplicate neighbour disc. off */
		dupl_disc = 0;
		break;
	case 'U':		/* device unique id used for dhcpv6 */
		s = EARGF(usage());
		conf.duidlen = dec16(conf.duid, sizeof(conf.duid), s, strlen(s));
		if(conf.duidlen <= 0)
			usage();
		break;
	case 'x':
		setnetmtpt(conf.mpoint, sizeof conf.mpoint, EARGF(usage()));
		break;
	case 'X':
		nodhcpwatch = 1;
		break;
	case 'y':
		yflag = 1;
		break;
	default:
		usage();
	} ARGEND;

	action = parseargs(argc, argv);

	if(beprimary == -1 && (ipv6auto || ISIPV6LINKLOCAL(conf.laddr) || parseverb(conf.type) == Vloopback))
		beprimary = 0;

	openiproute();
	if(findmyifc() == nil) {
		switch(action){
		default:
			if(noconfig)
				break;
			/* bind new interface */
			controldevice();
			binddevice();
			findmyifc();
		case Vunbind:
			break;
		case Vdel:
			/*
			 * interface gone, just remove
			 * default route and ndb entries.
			 */
			dodel();
			exits(nil);
		}
		if(myifc == nil)
			sysfatal("interface not found for: %s", conf.dev);
	} else if(!noconfig) {
		/* open old interface */
		binddevice();
	}

	switch(action){
	case Vadd:
		mkclientid();
		if(dondbconfig){
			dodhcp = 0;
			beprimary = 0;
			ndbconfig();
			break;
		}
		doadd();
		break;
	case Vra6:
	case Vaddpref6:
		mkclientid();
		doipv6(action);
		break;
	case Vdel:
		dodel();
		break;
	case Vunbind:
		dounbind();
		break;
	}
	exits(nil);
}

static void
doadd(void)
{
	if(!validip(conf.laddr)){
		if(ipv6auto){
			ipmove(conf.laddr, conf.lladdr);
			dodhcp = 0;
		} else
			dodhcp = 1;
	}

	/* run dhcp if we need something */
	if(dodhcp){
		fprint(conf.rfd, "tag dhcp");
		dhcpquery(!noconfig, Sselecting);
	}

	if(!validip(conf.laddr)){
		if(rflag && dodhcp && !noconfig){
			warning("couldn't determine ip address, retrying");
			dhcpwatch(1);
			return;
		} else
			sysfatal("no success with DHCP");
	}

	DEBUG("adding address %I %M on %s", conf.laddr, conf.mask, conf.dev);
	if(noconfig)
		return;

	if(!isv4(conf.laddr)){
		if(ip6cfg() < 0)
			sysfatal("can't start IPv6 on %s, address %I", conf.dev, conf.laddr);
	} else {
		if(ip4cfg() < 0)
			sysfatal("can't start IPv4 on %s, address %I", conf.dev, conf.laddr);
		else if(dodhcp && conf.lease != Lforever)
			dhcpwatch(0);
	}

	/* leave everything we've learned somewhere other procs can find it */
	putndb(1);
	refresh();
}

static void
dodel(void)
{
	if(!validip(conf.laddr))
		sysfatal("del requires an address");

	DEBUG("deleting address %I %M on %s", conf.laddr, conf.mask, conf.dev);
	if(noconfig)
		return;

	if(validip(conf.gaddr))
		deldefroute(conf.gaddr, conf.laddr, conf.laddr, conf.raddr, conf.mask);

	/* use "remove" verb instead of "del" for older kernels */
	if(conf.cfd >= 0 && fprint(conf.cfd, "remove %I %M", conf.laddr, conf.mask) < 0)
		warning("can't delete %I %M: %r", conf.laddr, conf.mask);

	/* remove ndb entries matching our ip address */
	putndb(0);
	refresh();
}

static void
dounbind(void)
{
	if(conf.cfd < 0)
		return;

	if(fprint(conf.cfd, "unbind") < 0)
		warning("can't unbind %s: %r", conf.dev);
}

static void
openiproute(void)
{
	char buf[127];

	if(noconfig)
		return;
	snprint(buf, sizeof buf, "%s/iproute", conf.mpoint);
	conf.rfd = open(buf, OWRITE);
}

/* send some ctls to a device */
static void
controldevice(void)
{
	char ctlfile[256];
	int fd;
	Ctl *cp;

	if (firstctl == nil || !isether())
		return;

	snprint(ctlfile, sizeof ctlfile, "%s/clone", conf.dev);
	fd = open(ctlfile, ORDWR);
	if(fd < 0)
		sysfatal("can't open %s", ctlfile);

	for(cp = firstctl; cp != nil; cp = cp->next){
		if(write(fd, cp->ctl, strlen(cp->ctl)) < 0)
			sysfatal("ctl message %s: %r", cp->ctl);
		seek(fd, 0, 0);
	}
//	close(fd);		/* or does it need to be left hanging? */
}

/* bind an ip stack to a device, leave the control channel open */
static void
binddevice(void)
{
	char buf[256];

	if(myifc != nil){
		/* open the old interface */
		snprint(buf, sizeof buf, "%s/ipifc/%d/ctl", conf.mpoint, myifc->index);
		conf.cfd = open(buf, ORDWR);
		if(conf.cfd < 0)
			sysfatal("open %s: %r", buf);
	} else if(strcmp(conf.type, "ppp") == 0)
		pppbinddev();
	else {
		/* get a new ip interface */
		snprint(buf, sizeof buf, "%s/ipifc/clone", conf.mpoint);
		conf.cfd = open(buf, ORDWR);
		if(conf.cfd < 0)
			sysfatal("opening %s/ipifc/clone: %r", conf.mpoint);

		/* specify medium as ethernet, bind the interface to it */
		if(fprint(conf.cfd, "bind %s %s", conf.type, conf.dev) < 0)
			sysfatal("%s: bind %s %s: %r", buf, conf.type, conf.dev);
	}
}

/* add a logical interface to the ip stack */
int
ip4cfg(void)
{
	char buf[256];
	int n;

	if(!validip(conf.laddr) || !isv4(conf.laddr))
		return -1;

	n = sprint(buf, "add");
	n += snprint(buf+n, sizeof buf-n, " %I", conf.laddr);

	if(!validip(conf.mask))
		ipmove(conf.mask, defmask(conf.laddr));
	n += snprint(buf+n, sizeof buf-n, " %M", conf.mask);

	if(!validip(conf.raddr) || !isv4(conf.raddr))
		maskip(conf.laddr, conf.mask, conf.raddr);
	n += snprint(buf+n, sizeof buf-n, " %I", conf.raddr);
	n += snprint(buf+n, sizeof buf-n, " %d", conf.mtu);
	if(yflag)
		n += snprint(buf+n, sizeof buf-n, " proxy");
	if(tflag)
		n += snprint(buf+n, sizeof buf-n, "%strans", yflag? ",": " ");

	DEBUG("ip4cfg: %.*s", n, buf);
	if(write(conf.cfd, buf, n) < 0){
		warning("write(%s): %r", buf);
		return -1;
	}

	if(validip(conf.gaddr) && isv4(conf.gaddr)
	&& ipcmp(conf.gaddr, conf.laddr) != 0)
		adddefroute(conf.gaddr, conf.laddr, conf.laddr, conf.laddr, conf.mask);

	if(tflag)
		fprint(conf.cfd, "iprouting 1");

	return 0;
}

/* remove a logical interface from the ip stack */
void
ipunconfig(void)
{
	if(!validip(conf.laddr))
		return;

	if(!validip(conf.mask))
		ipmove(conf.mask, defmask(conf.laddr));

	dodel();

	ipmove(conf.laddr, IPnoaddr);
	ipmove(conf.raddr, IPnoaddr);
	ipmove(conf.mask, IPnoaddr);
}

/* return true if this is not a null address */
int
validip(uchar *addr)
{
	return ipcmp(addr, IPnoaddr) != 0 && ipcmp(addr, v4prefix) != 0;
}

/* put server ip addresses into the ndb entry */
static char*
putaddrs(char *p, char *e, char *attr, uchar *a, int len)
{
	int i;

	for(i = 0; i < len && validip(a); i += IPaddrlen, a += IPaddrlen)
		p = seprint(p, e, "%s=%I\n", attr, a);
	return p;
}

/* put space separated names into ndb entry */
static char*
putnames(char *p, char *e, char *attr, char *s)
{
	char *x;

	for(; *s != 0; s = x+1){
		if((x = strchr(s, ' ')) != nil)
			*x = 0;
		p = seprint(p, e, "%s=%U\n", attr, s);
		if(x == nil)
			break;
		*x = ' ';
	}
	return p;
}

/* make an ndb entry and put it into /net/ndb for the servers to see */
int
putndb(int doadd)
{
	static char buf[16*1024];
	char file[64], *p, *e, *np;
	Ndbtuple *t, *nt;
	Ndb *db;
	int fd;
	static uchar csum[SHA1dlen];
	uchar newcsum[SHA1dlen];

	if(beprimary == 0 || noconfig)
		return 0;

	p = buf;
	e = buf + sizeof buf;

	if(doadd){
		if(ipnet != nil && validip(conf.raddr)){
			p = seprint(p, e, "ipnet=%$ ip=%I ipmask=%M ipgw=%I\n",
				ipnet, conf.raddr, conf.mask, conf.gaddr);
		}
		if(validip(conf.laddr)){
			p = seprint(p, e, "ip=%I ipmask=%M ipgw=%I\n",
				conf.laddr, conf.mask, conf.gaddr);
			if(np = strchr(conf.hostname, '.')){
				if(*conf.domainname == 0)
					strcpy(conf.domainname, np+1);
				*np = 0;
			}
			if(*conf.hostname)
				p = seprint(p, e, "\tsys=%U\n", conf.hostname);
			if(*conf.domainname)
				p = seprint(p, e, "\tdom=%U.%U\n",
					conf.hostname, conf.domainname);
			if(*conf.dnsdomain)
				p = putnames(p, e, "\tdnsdomain", conf.dnsdomain);
			if(validip(conf.dns))
				p = putaddrs(p, e, "\tdns", conf.dns, sizeof conf.dns);
			if(validip(conf.fs))
				p = putaddrs(p, e, "\tfs", conf.fs, sizeof conf.fs);
			if(validip(conf.auth))
				p = putaddrs(p, e, "\tauth", conf.auth, sizeof conf.auth);
			if(validip(conf.ntp))
				p = putaddrs(p, e, "\tntp", conf.ntp, sizeof conf.ntp);
			if(ndboptions)
				p = seprint(p, e, "%s\n", ndboptions);
		}
	}

	/* for myip() */
	allifcs = readipifc(conf.mpoint, allifcs, -1);

	/* write valid pre-existing entries not matching our ip */
	snprint(file, sizeof file, "%s/ndb", conf.mpoint);
	db = ndbopen(file);
	if(db != nil ){
		while((t = ndbparse(db)) != nil){
			uchar ip[IPaddrlen];

			nt = ndbfindattr(t, t, "ipnet");
			if(nt != nil && (ipnet == nil || strcmp(nt->val, ipnet) != 0)
			|| nt == nil && ((nt = ndbfindattr(t, t, "ip")) == nil
				|| parseip(ip, nt->val) == -1
				|| (!doadd || !validip(conf.laddr) || ipcmp(ip, conf.laddr) != 0) 
					 && myip(allifcs, ip))){
				if(p > buf)
					p = seprint(p, e, "\n");
				for(nt = t; nt != nil; nt = nt->entry)
					p = seprint(p, e, "%s=%$%s", nt->attr, nt->val,
						nt->entry==nil? "\n": nt->line!=nt->entry? "\n\t": " ");
			}
			ndbfree(t);
		}
		ndbclose(db);
	}

	/* only write if something has changed since last time */
	sha1((uchar *)buf, p-buf, newcsum, nil);
	if(memcmp(csum, newcsum, SHA1dlen) == 0)
		return 0;
	memcpy(csum, newcsum, SHA1dlen);

	if((fd = open(file, OWRITE|OTRUNC)) < 0)
		return 0;
	write(fd, buf, p-buf);
	close(fd);
	return 1;
}

static int
issrcspec(uchar *src, uchar *smask)
{
	return isv4(src)? memcmp(smask+IPv4off, IPnoaddr+IPv4off, 4): ipcmp(smask, IPnoaddr);
}

static void
routectl(char *cmd, uchar *dst, uchar *mask, uchar *gate, char *flags, uchar *ia, uchar *src, uchar *smask)
{
	char *ctl;

	if(*flags == '\0'){
		if(!issrcspec(src, smask))
			ctl = "%s %I %M %I %I";
		else
			ctl = "%s %I %M %I %I %I %M";
		DEBUG(ctl, cmd, dst, mask, gate, ia, src, smask);
		fprint(conf.rfd, ctl, cmd, dst, mask, gate, ia, src, smask);
		return;
	}
	ctl = "%s %I %M %I %s %I %I %M";
	DEBUG(ctl, cmd, dst, mask, gate, flags, ia, src, smask);
	fprint(conf.rfd, ctl, cmd, dst, mask, gate, flags, ia, src, smask);
}

static void
defroutectl(char *cmd, uchar *gaddr, uchar *ia, uchar *laddr, uchar *src, uchar *smask)
{
	uchar dst[IPaddrlen], mask[IPaddrlen];

	if(isv4(gaddr)){
		parseipandmask(dst, mask, "0.0.0.0", "0.0.0.0");
		if(src == nil)
			src = dst;
		if(smask == nil)
			smask = mask;
	} else {
		parseipandmask(dst, mask, "2000::", "/3");
		if(src == nil)
			src = IPnoaddr;
		if(smask == nil)
			smask = IPnoaddr;
	}

	if(tflag && isv4(gaddr)){
		/* add route for everyone with source translation */
		routectl(cmd, dst, mask, gaddr, "4t", ia, dst, mask);
	} else {
		/* add route for subnet */
		routectl(cmd, dst, mask, gaddr, "", ia, src, smask);
	}

	/* add source specific route for us */
	if(validip(laddr))
		routectl(cmd, dst, mask, gaddr, "", ia, laddr, IPallbits);
}

void
adddefroute(uchar *gaddr, uchar *ia, uchar *laddr, uchar *src, uchar *smask)
{
	defroutectl("add", gaddr, ia, laddr, src, smask);
}

void
deldefroute(uchar *gaddr, uchar *ia, uchar *laddr, uchar *src, uchar *smask)
{
	/* use "remove" verb instead of "del" for older kernels */
	defroutectl("remove", gaddr, ia, laddr, src, smask);
}

void
refresh(void)
{
	char file[64];
	int fd;

	if(noconfig)
		return;

	snprint(file, sizeof file, "%s/cs", conf.mpoint);
	if((fd = open(file, OWRITE)) >= 0){
		write(fd, "refresh", 7);
		close(fd);
	}

	/* dns unaffected, no need to refresh dns */
	if(!beprimary && !dondbconfig)
		return;

	snprint(file, sizeof file, "%s/dns", conf.mpoint);
	if((fd = open(file, OWRITE)) >= 0){
		write(fd, "refresh", 7);
		close(fd);
	}
}

void
catch(void*, char *msg)
{
	if(strstr(msg, "alarm"))
		noted(NCONT);
	exits(msg);
}

/* return pseudo-random integer in range low...(hi-1) */
ulong
randint(ulong low, ulong hi)
{
	if (hi < low)
		return low;
	return low + nrand(hi - low);
}

long
jitter(void)		/* compute small pseudo-random delay in ms */
{
	return randint(0, 10*1000);
}

int
countaddrs(uchar *a, int len)
{
	int i;

	for(i = 0; i < len && validip(a); i += IPaddrlen, a += IPaddrlen)
		;
	return i / IPaddrlen;
}

void
addaddrs(uchar *to, int nto, uchar *from, int nfrom)
{
	int i, j;

	for(i = 0; i < nfrom; i += IPaddrlen, from += IPaddrlen){
		if(!validip(from))
			continue;
		for(j = 0; j < nto && validip(to+j); j += IPaddrlen){
			if(ipcmp(to+j, from) == 0)
				return;
		}
		if(j == nto)
			return;
		ipmove(to+j, from);
	}
}

void
addnames(char *d, char *s, int len)
{
	char *p, *e, *f;
	int n;

	for(;;s++){
		if((e = strchr(s, ' ')) == nil)
			e = strchr(s, 0);
		n = e - s;
		if(n == 0)
			goto next;
		for(p = d;;p++){
			if((f = strchr(p, ' ')) == nil)
				f = strchr(p, 0);
			if(f - p == n && memcmp(s, p, n) == 0)
				goto next;
			p = f;
			if(*p == 0)
				break;
		}
		if(1 + n + p - d >= len)
			break;
		if(p > d)
			*p++ = ' ';
		p[n] = 0;
		memmove(p, s, n);
next:
		s = e;
		if(*s == 0)
			break;
	}
}

int
pnames(uchar *d, int nd, char *s)
{
	uchar *de = d + nd;
	int l;

	if(nd < 1)
		return -1;
	for(; *s != 0; s++){
		for(l = 0; *s != 0 && *s != '.' && *s != ' '; l++)
			s++;

		d += l+1;
		if(d >= de || l > 077)
			return -1;

		d[-l-1] = l;
		memmove(d-l, s-l, l);

		if(*s != '.')
			*d++ = 0;
	}
	return d - (de - nd);
}

int
gnames(char *d, int nd, uchar *s, int ns)
{
	char  *de = d + nd;
	uchar *se = s + ns;
	uchar *c = nil;
	int l, p = 0;

	if(ns < 1 || nd < 1)
		return -1;
	while(s < se){
		l = *s++;
		if((l & 0300) == 0300){
			if(++p > 100 || s >= se)
				break;
			l = (l & 077)<<8 | *s++;
			if(c == nil)
				c = s;
			s = (se - ns) + l;
			continue;
		}
		l &= 077;
		if(l == 0){
			if(d <= de - nd)
				break;
			d[-1] = ' ';
			if(c != nil){
				s = c;
				c = nil;
				p = 0;
			}
			continue;
		}
		if(s+l >= se || d+l >= de)
			break;
		memmove(d, s, l);
		s += l;
		d += l;
		*d++ = '.';
	}
	if(p != 0 || s != se || d <= de - nd || d[-1] != ' ')
		return -1;
	*(--d) = 0;
	return d - (de - nd);
}

static int
Ufmt(Fmt *f)
{
	char d[256], *s;

	s = va_arg(f->args, char*);
	if(idn2utf(s, d, sizeof(d)) >= 0)
		s = d;
	fmtprint(f, "%s", s);
	return 0;
}

static Ndbtuple*
uniquent(Ndbtuple *t)
{
	Ndbtuple **l, *x;

	l = &t->entry;
	while((x = *l) != nil){
		if(strcmp(t->attr, x->attr) != 0){
			l = &x->entry;
			continue;
		}
		*l = x->entry;
		x->entry = nil;
		ndbfree(x);
	}
	return t;
}

/* my ips from ndb, read by ndbconfig() below */
static uchar dbips[128*IPaddrlen];

static int
ipindb(uchar *ip)
{
	uchar *a;

	for(a = dbips; a < &dbips[sizeof(dbips)]; a += IPaddrlen){
		if(!validip(a))
			break;
		if(ipcmp(ip, a) == 0)
			return 1;
	}
	return 0;
}

/* read configuration (except laddr) for myip from ndb */
void
ndb2conf(Ndb *db, uchar *myip)
{
	int nattr;
	char *attrs[10], val[256];
	uchar ip[IPaddrlen];
	Ndbtuple *t, *nt;

	ipmove(conf.mask, defmask(conf.laddr));

	memset(conf.raddr, 0, sizeof(conf.raddr));
	memset(conf.gaddr, 0, sizeof(conf.gaddr));
	memset(conf.dns, 0, sizeof(conf.dns));
	memset(conf.ntp, 0, sizeof(conf.ntp));
	memset(conf.fs, 0, sizeof(conf.fs));
	memset(conf.auth, 0, sizeof(conf.auth));
	memset(conf.dnsdomain, 0, sizeof(conf.dnsdomain));

	if(db == nil)
		return;

	nattr = 0;
	attrs[nattr++] = "ipmask";
	attrs[nattr++] = "@ipgw";

	attrs[nattr++] = "@dns";
	attrs[nattr++] = "@ntp";
	attrs[nattr++] = "@fs";
	attrs[nattr++] = "@auth";

	attrs[nattr++] = "dnsdomain";

	snprint(val, sizeof(val), "%I", myip);
	t = ndbipinfo(db, "ip", val, attrs, nattr);
	for(nt = t; nt != nil; nt = nt->entry) {
		if(strcmp(nt->attr, "dnsdomain") == 0) {
			if(utf2idn(nt->val, val, sizeof(val)) <= 0)
				continue;
			addnames(conf.dnsdomain, val, sizeof(conf.dnsdomain));
			continue;
		}
		if(strcmp(nt->attr, "ipmask") == 0) {
			nt = uniquent(nt);
			if(parseipmask(conf.mask, nt->val, isv4(myip)) == -1)
				goto Badip;
			continue;
		}
		if(parseip(ip, nt->val) == -1) {
		Badip:
			fprint(2, "%s: bad %s address in ndb: %s\n", argv0, nt->attr, nt->val);
			continue;
		}
		if(strcmp(nt->attr, "ipgw") == 0) {
			/* ignore in case we are the gateway */
			if(ipindb(ip))
				continue;
			ipmove(conf.gaddr, ip);
			nt = uniquent(nt);
		} else if(strcmp(nt->attr, "dns") == 0) {
			addaddrs(conf.dns, sizeof(conf.dns), ip, IPaddrlen);
		} else if(strcmp(nt->attr, "ntp") == 0) {
			addaddrs(conf.ntp, sizeof(conf.ntp), ip, IPaddrlen);
		} else if(strcmp(nt->attr, "fs") == 0) {
			addaddrs(conf.fs, sizeof(conf.fs), ip, IPaddrlen);
		} else if(strcmp(nt->attr, "auth") == 0) {
			addaddrs(conf.auth, sizeof(conf.auth), ip, IPaddrlen);
		}
	}
	ndbfree(t);
}

Ndb*
opendatabase(void)
{
	static Ndb *db;

	if(db != nil)
		ndbclose(db);
	db = ndbopen(dbfile);
	return db;
}

/* add addresses for my ethernet address from ndb */
static void
ndbconfig(void)
{
	char etheraddr[32], *attr;
	Ndbtuple *t, *nt;
	Ndb *db;
	int n, i;

	db = opendatabase();
	if(db == nil)
		sysfatal("can't open ndb: %r");

	if(validip(conf.laddr)){
		ndb2conf(db, conf.laddr);
		doadd();
		return;
	}

	memset(dbips, 0, sizeof(dbips));

	if(conf.hwatype != 1)
		sysfatal("can't read hardware address");
	snprint(etheraddr, sizeof(etheraddr), "%E", conf.hwa);

	attr = "ip";
	t = ndbipinfo(db, "ether", etheraddr, &attr, 1);
	for(nt = t; nt != nil; nt = nt->entry) {
		if(parseip(conf.laddr, nt->val) == -1){
			fprint(2, "%s: bad %s address in ndb: %s\n", argv0,
				nt->attr, nt->val);
			continue;
		}
		addaddrs(dbips, sizeof(dbips), conf.laddr, IPaddrlen);
	}
	ndbfree(t);

	n = countaddrs(dbips, sizeof(dbips));
	if(n == 0)
		sysfatal("no ip addresses found in ndb");

	/* add link local address first, if not already done */
	if(!findllip(conf.lladdr, myifc)){
		for(i = 0; i < n; i++){
			ipmove(conf.laddr, dbips+i*IPaddrlen);
			if(ISIPV6LINKLOCAL(conf.laddr)){
				ipv6auto = 0;
				ipmove(conf.lladdr, conf.laddr);
				ndb2conf(db, conf.laddr);
				doadd();
				break;
			}
		}
		if(ipv6auto){
			ipmove(conf.laddr, IPnoaddr);
			doadd();
		}
	}

	/* add v4 addresses and v6 if link local address is available */
	for(i = 0; i < n; i++){
		ipmove(conf.laddr, dbips+i*IPaddrlen);
		if(isv4(conf.laddr) || ipcmp(conf.laddr, conf.lladdr) != 0){
			ndb2conf(db, conf.laddr);
			doadd();
		}
	}
}

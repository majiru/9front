/* possible verbs */
enum
{
	/* commands */
	Vadd,
	Vdel,
	Vunbind,
	Vaddpref6,
	Vra6,

	/* media */
	Vether,
	Vgbe,
	Vppp,
	Vloopback,
	Vtorus,
	Vtree,
	Vpkt,
	Vnull,
};

typedef struct Conf Conf;
typedef struct Ctl Ctl;

struct Conf
{
	/* locally generated */
	char	*type;
	char	*dev;
	char	mpoint[32];
	int	cfd;			/* ifc control channel */
	int	rfd;			/* iproute control channel */
	char	*cputype;
	uchar	hwa[32];		/* hardware address */
	int	hwatype;
	int	hwalen;
	uchar	cid[32];
	int	cidlen;
	uchar	duid[2+128];
	int	duidlen;
	char	*baud;

	/* learned info */
	uchar	gaddr[IPaddrlen];
	uchar	laddr[IPaddrlen];
	uchar	mask[IPaddrlen];
	uchar	raddr[IPaddrlen];

	uchar	dns[8*IPaddrlen];
	uchar	fs[2*IPaddrlen];
	uchar	auth[2*IPaddrlen];
	uchar	ntp[2*IPaddrlen];
	int	mtu;

	/* dhcp specific */
	int	state;
	int	fd;
	ulong	xid;
	ulong	starttime;
	char	sname[64];
	char	hostname[256];
	char	domainname[256];
	char	dnsdomain[256];
	uchar	server[IPaddrlen];	/* server IP address */
	ulong	offered;		/* offered lease time */
	ulong	lease;			/* lease time */
	ulong	resend;			/* # of resends for current state */
	ulong	timeout;		/* time to timeout - seconds */

	/*
	 * IPv6
	 */
	uchar	v6router[IPaddrlen];

	/* router-advertisement related */
	uchar	sendra;
	uchar	recvra;
	uchar	mflag;
	uchar	oflag;
	int 	maxraint; /* rfc2461, p.39: 4sec ≤ maxraint ≤ 1800sec, def 600 */
	int	minraint;	/* 3sec ≤ minraint ≤ 0.75*maxraint */
	int	linkmtu;
	int	routerlt;	/* router life time */
	int	reachtime;	/* 3,600,000 msec, default 0 */
	int	rxmitra;	/* default 0 */
	int	ttl;		/* default 0 (unspecified) */

	/* prefix related */
	uchar	lladdr[IPaddrlen];
	uchar	v6pref[IPaddrlen];
	uchar	v6mask[IPaddrlen];
	int	prefixlen;
	uchar	onlink;		/* flag: address is `on-link' */
	uchar	autoflag;	/* flag: autonomous */
	ulong	validlt;	/* valid lifetime (seconds) */
	ulong	preflt;		/* preferred lifetime (seconds) */
};

struct Ctl
{
	Ctl	*next;
	char	*ctl;
};

/* my interface on conf.mpoint stack */
extern Ipifc*	myifc;

extern Conf	conf;
extern int	noconfig;
extern int	dodhcp;

extern int	debug;
extern int	dolog;

extern int	plan9;
extern int	Oflag;
extern int	yflag;

extern int	dupl_disc;

extern int	nodhcpwatch;
extern int	sendhostname;
extern char	*ndboptions;
extern char	*ipnet;		/* put ipnet= tuple in ndb for raddr */

void	usage(void);
int	ip4cfg(void);
void	ipunconfig(void);

void	adddefroute(uchar*, uchar*, uchar*, uchar*, uchar*);
void	deldefroute(uchar*, uchar*, uchar*, uchar*, uchar*);

int	myip(Ipifc*, uchar*);
int	isether(void);
long	jitter(void);
void	catch(void*, char*);
int	countaddrs(uchar *a, int len);
void	addaddrs(uchar *to, int nto, uchar *from, int nfrom);
void	addnames(char *d, char *s, int len);
int	pnames(uchar*, int, char*);
int	gnames(char*, int, uchar*, int);
Ndb*	opendatabase(void);
void	ndb2conf(Ndb *db, uchar *ip);
int	putndb(int);
void	refresh(void);
ulong	randint(ulong low, ulong hi);
int	validip(uchar*);
void	warning(char *fmt, ...);
#define DEBUG if(debug)warning
#pragma	varargck argpos	warning 1

/* for ndbvalfmt */
#pragma varargck type "$" char*

/*
 * DHCP
 */
void	dhcpinit(void);
void	dhcpquery(int, int);
void	dhcpwatch(int);
int	addoption(char*);

/*
 * IPv6
 */
void	v6paraminit(Conf*);
void	parse6pref(int argc, char **argv);
void	parse6ra(int argc, char **argv);
void	doipv6(int);
void	ea2lla(uchar *lla, uchar *ea);
int	findllip(uchar *ip, Ipifc *ifc);
int	ip6cfg(void);
int	validv6prefix(uchar *ip);
void	genipmask(uchar *mask, int len);

/*
 * DHCPv6
 */
int	dhcpv6query(int);

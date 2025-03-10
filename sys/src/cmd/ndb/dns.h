typedef struct Ndbtuple Ndbtuple;

enum
{
	/* RR types; see: http://www.iana.org/assignments/dns-parameters */
	Ta=		1,
	Tns=		2,
	Tmd=		3,
	Tmf=		4,
	Tcname=		5,
	Tsoa=		6,
	Tmb=		7,
	Tmg=		8,
	Tmr=		9,
	Tnull=		10,
	Twks=		11,
	Tptr=		12,
	Thinfo=		13,
	Tminfo=		14,
	Tmx=		15,
	Ttxt=		16,
	Trp=		17,
	Tafsdb=		18,
	Tx25=		19,
	Tisdn=		20,
	Trt=		21,
	Tnsap=		22,
	Tnsapptr=	23,
	Tsig=		24,
	Tkey=		25,
	Tpx=		26,
	Tgpos=		27,
	Taaaa=		28,
	Tloc=		29,
	Tnxt=		30,
	Teid=		31,
	Tnimloc=	32,
	Tsrv=		33,
	Tatma=		34,
	Tnaptr=		35,
	Tkx=		36,
	Tcert=		37,
	Ta6=		38,
	Tdname=		39,
	Tsink=		40,
	Topt=		41,
	Tapl=		42,
	Tds=		43,
	Tsshfp=		44,
	Tipseckey=	45,
	Trrsig=		46,
	Tnsec=		47,
	Tdnskey=	48,

	Tspf=		99,
	Tuinfo=		100,
	Tuid=		101,
	Tgid=		102,
	Tunspec=	103,

	/* query types (all RR types are also queries) */
	Ttkey=	249,	/* transaction key */
	Ttsig=	250,	/* transaction signature */
	Tixfr=	251,	/* incremental zone transfer */
	Taxfr=	252,	/* zone transfer */
	Tmailb=	253,	/* { Tmb, Tmg, Tmr } */
	Tmaila= 254,	/* obsolete */
	Tall=	255,	/* all records */
	Tcaa=	257,	/* certification authority authorization */

	/* classes */
	Csym=	0,	/* internal symbols */
	Cin=	1,	/* internet */
	Ccs,		/* CSNET (obsolete) */
	Cch,		/* Chaos net */
	Chs,		/* Hesiod (?) */

	/* class queries (all class types are also queries) */
	Call=	255,	/* all classes */

	/* opcodes */
	Oquery=		0<<11,		/* normal query */
	Oinverse=	1<<11,		/* inverse query (retired) */
	Ostatus=	2<<11,		/* status request */
	Onotify=	4<<11,		/* notify slaves of updates */
	Oupdate=	5<<11,
	Omask=		0xf<<11,	/* mask for opcode */

	/* response codes */
	Rok=		0,
	Rformat=	1,	/* format error */
	Rserver=	2,	/* server failure (e.g. no answer from something) */
	Rname=		3,	/* bad name */
	Runimplimented=	4,	/* unimplemented */
	Rrefused=	5,	/* we don't like you */
	Ryxdomain=	6,	/* name exists when it should not */
	Ryxrrset=	7,	/* rr set exists when it should not */
	Rnxrrset=	8,	/* rr set that should exist does not */
	Rnotauth=	9,	/* not authoritative */
	Rnotzone=	10,	/* name not in zone */
	Rbadvers=	16,	/* bad opt version */
/*	Rbadsig=	16, */	/* also tsig signature failure */
	Rbadkey=	17,		/* key not recognized */
	Rbadtime=	18,		/* signature out of time window */
	Rbadmode=	19,		/* bad tkey mode */
	Rbadname=	20,		/* duplicate key name */
	Rbadalg=	21,		/* algorithm not supported */
	Rmask=		0x1f,	/* mask for response */
	Rtimeout=	1<<5,	/* timeout sending (for internal use only) */

	/* bits in flag word (other than opcode and response) */
	Fresp=		1<<15,	/* message is a response */
	Fauth=		1<<10,	/* true if an authoritative response */
	Ftrunc=		1<<9,	/* truncated message */
	Frecurse=	1<<8,	/* request recursion */
	Fcanrec=	1<<7,	/* server can recurse */

	/* EDNS flags (eflags) */
	Ercode=		0xff<<24,
	Evers=		0xff<<16,
	Edo=		1<<15,	/* DNSSEC ok */

	Domlen=		256,	/* max domain name length (with NULL) */
	Labellen=	64,	/* max domain label length (with NULL) */
	Strlen=		256,	/* max string length (with NULL) */

	/* time to live values (in seconds) */
	Min=		60,
	Hour=		60*Min,		/* */
	Day=		24*Hour,	/* Ta, Tmx */
	Week=		7*Day,		/* Tsoa, Tns */
	Year=		52*Week,
	DEFTTL=		Day,

	/* packet sizes */
	Maxudp=		8*1024,
	Maxtcp=		0xfffe,
	Maxpkt=		0x10000,

	Maxpath=	128,	/* size of mntpt */
	Maxlcks=	10,	/* max. query-type locks per domain name */

	/* parallelism: tune; was 32; allow lots */
	Maxactive=	250,

	/* tune; was 8*1000; that was too short */
	Maxreqtm=	15*1000,	/* max. ms to process a request */
	Minreqtm=	100,		/* min. ms to attempt a request */
	Maxtcpdialtm=	4000,		/* max. ms to dial() tcp connection */

	Notauthoritative = 0,
	Authoritative,
};

typedef struct Area	Area;
typedef struct Block	Block;
typedef struct Cert	Cert;
typedef struct DN	DN;
typedef struct DNSmsg	DNSmsg;
typedef struct Key	Key;
typedef struct Null	Null;
typedef struct RR	RR;
typedef struct Request	Request;
typedef struct SOA	SOA;
typedef struct Server	Server;
typedef struct Sig	Sig;
typedef struct Srv	Srv;
typedef struct Txt	Txt;
typedef struct Caa	Caa;
typedef struct Unknown	Unknown;
typedef struct Opt	Opt;

/*
 *  a structure to track a request and any slave process handling it
 */
struct Request
{
	int	isslave;	/* pid of slave */
	uvlong	aborttime;	/* time in ms at which we give up */
	jmp_buf	mret;		/* where master jumps to after starting a slave */
	ushort	id;		/* internal id of request (just for logging) */
	uchar	mark;
	char	*from;		/* who asked us? */
	void	*aux;
};

/*
 *  a domain name
 */
struct DN
{
	DN	*next;		/* hash collision list */
	RR	*rr;		/* resource records off this name */
	ulong	ordinal;
	ushort	class;		/* RR class */
	uchar	respcode;	/* response code */
	uchar	mark;		/* for mark and sweep */
	char	name[];		/* owner */
};

/*
 *  security info
 */
struct Block
{
	int	dlen;
	uchar	*data;
};
struct Key
{
	int	flags;
	int	proto;
	int	alg;
	Block;
};
struct Caa
{
	int	flags;
	DN	*tag;
	Block;
};
struct Cert
{
	int	type;
	int	tag;
	int	alg;
	Block;
};
struct Sig
{
	Cert;
	int	labels;
	ulong	ttl;
	ulong	exp;
	ulong	incep;
	DN	*signer;
};
struct Null
{
	Block;
};
struct Unknown
{
	Block;
};
struct Opt
{
	Block;
};

/*
 *  text strings
 */
struct Txt
{
	Txt	*next;
	Block;
};

/*
 *  an unpacked resource record
 */
struct RR
{
	RR	*next;
	DN	*owner;		/* domain that owns this resource record */
	uintptr	pc;		/* for tracking memory allocation */
	ulong	ttl;		/* time to live to be passed on */
	ulong	expire;		/* time this entry expires locally */
	ulong	marker;		/* used locally when scanning rrlists */
	ushort	type;		/* RR type */
	ushort	query;		/* query type is in response to */
	uchar	auth;		/* flag: authoritative */
	uchar	db;		/* flag: from database */
	uchar	cached;		/* flag: rr in cache */
	uchar	negative;	/* flag: this is a cached negative response */

	union {			/* discriminated by negative & type */
		DN	*negsoaowner;	/* soa for cached negative response */
		DN	*host;	/* hostname - soa, cname, mb, md, mf, mx, ns, srv */
		DN	*cpu;	/* cpu type - hinfo */
		DN	*mb;	/* mailbox - mg, minfo */
		DN	*ip;	/* ip address - a, aaaa */
		DN	*rp;	/* rp arg - rp */
		ulong	eflags;	/* EDNS(0) flags - opt */
		uintptr	arg0;	/* arg[01] are compared to find dups in dn.c */
	};
	union {			/* discriminated by negative & type */
		int	negrcode; /* response code for cached negative resp. */
		DN	*rmb;	/* responsible maibox - minfo, soa, rp */
		DN	*ptr;	/* pointer to domain name - ptr */
		DN	*os;	/* operating system - hinfo */
		ulong	pref;	/* preference value - mx */
		ulong	local;	/* ns served from local database - ns */
		ushort	port;	/* - srv */
		ushort	udpsize;/* requester's UDP payload size - opt */
		uintptr	arg1;	/* arg[01] are compared to find dups in dn.c */
	};
	union {			/* discriminated by type */
		SOA	*soa;	/* soa timers - soa */
		Srv	*srv;
		Key	*key;
		Caa	*caa;
		Cert	*cert;
		Sig	*sig;
		Null	*null;
		Txt	*txt;
		Unknown	*unknown;
		Opt	*opt;
	};
};

/*
 *  list of servers
 */
struct Server
{
	Server	*next;
	char	*name;
};

/*
 *  timers for a start-of-authority record.  all ulongs are in seconds.
 */
struct SOA
{
	ulong	serial;		/* zone serial # */
	ulong	refresh;	/* zone refresh interval */
	ulong	retry;		/* zone retry interval */
	ulong	expire;		/* time to expiration */
	ulong	minttl;		/* min. time to live for any entry */

	Server	*slaves;	/* slave servers */
};

/*
 * srv (service location) record (rfc2782):
 * _service._proto.name ttl class(IN) 'SRV' priority weight port target
 */
struct Srv
{
	ushort	pri;
	ushort	weight;
};

/*
 *  domain messages
 */
struct DNSmsg
{
	ushort	id;
	int	flags;
	int	qdcount;	/* questions */
	RR 	*qd;
	int	ancount;	/* answers */
	RR	*an;
	int	nscount;	/* name servers */
	RR	*ns;
	int	arcount;	/* hints */
	RR	*ar;
	RR	*edns;		/* edns option */
};

/*
 *  definition of local area for dblookup
 */
struct Area
{
	Area	*next;

	RR	*soarr;		/* soa defining this area */
	int	len;		/* strlen(area->soarr->owner->name) */
	uchar	neednotify;
	uchar	needrefresh;
};

typedef struct Cfg Cfg;
struct Cfg {
	char	cachedb;
	char	resolver;
	char	justforw;	/* flag: pure resolver, just forward queries */
	char	serve;		/* flag: serve tcp udp queries */
	char	nonrecursive;	/* flag: never serve recursive queries */
	char	localrecursive;	/* flag: serve recursive queries for local ip's */
};

/* query stats */
typedef struct {
	ulong	slavehiwat;	/* procs */
	ulong	qrecvd9p;	/* query counts */
	ulong	qrecvdudp;
	ulong	qrecvdtcp;
	ulong	qsentudp;
	ulong	qsenttcp;
	ulong	qrecvd9prpc;	/* packet count */
	/* reply times by count */
	ulong	under10ths[3*10+2];	/* under n*0.1 seconds, n is index */
	ulong	tmout;
	ulong	tmoutcname;
	ulong	tmoutv6;

	ulong	answinmem;	/* answers in memory */
	ulong	negans;		/* negative answers received */
	ulong	negserver;	/* neg ans with Rserver set */
	ulong	negbaddeleg;	/* neg ans with bad delegations */
	ulong	negbdnoans;	/* ⋯ and no answers */
	ulong	negnorname;	/* neg ans with no Rname set */
	ulong	negcached;	/* neg ans cached */
} Stats;

Stats stats;

enum
{
	Recurse,
	Dontrecurse,
	NOneg,
	OKneg,
};

extern char	*dbfile;
extern char	*logfile;
extern Cfg	cfg;
extern int	debug;
extern char	mntpt[];

#pragma	varargck	type	"\\"	uchar*
#pragma	varargck	type	"R"	RR*
#pragma	varargck	type	"Q"	RR*

/* dn.c */
extern int	needrefresh;
extern ulong	now;		/* time base seconds */
extern uvlong	nowms;		/* time base milliseconds */
extern int	maxage;		/* age of oldest entry in cache (secs) */
extern ulong	target;

RR*	getdnsservers(int);

void	addserver(Server**, char*);
int	bslashfmt(Fmt*);
Server*	copyserverlist(Server*);
void	db2cache(int);
void	dnageall(int);
void	dnagedb(void);
void	dnauthdb(void);
void	dninit(void);
DN*	dnlookup(char*, int, int);
DN*	idnlookup(char*, int, int);
DN*	ipalookup(uchar*, int, int);
void	dnptr(uchar*, uchar*, char*, int, int, int);
void	dnpurge(void);
void	dnslog(char*, ...);
void	dnstats(char *file);
void*	emalloc(int);
char*	estrdup(char*);
void	freeanswers(DNSmsg *mp);
void	freeserverlist(Server*);
void	getactivity(Request*);
void	putactivity(Request*);
RR*	randomize(RR*);
char*	rcname(int);
RR*	rralloc(int);
void	rrattach(RR*, int);
int	rravfmt(Fmt*);
RR*	rrcat(RR**, RR*);
RR**	rrcopy(RR*, RR**);
int	rrfmt(Fmt*);
void	rrfree(RR*);
void	rrfreelist(RR*);
RR*	rrlookup(DN*, int, int);
RR*	rrgetzone(char*);
char*	rrname(int, char*, int);
RR*	rrremneg(RR**);
RR*	rrremtype(RR**, int);
RR*	rrremowner(RR**, DN*);
RR*	rrremfilter(RR**, int (*)(RR*, void*), void*);
int	rrsupported(int);
int	rrtype(char*);
void	slave(Request*);
int	subsume(char*, char*);
uvlong	timems(void);
int	tsame(int, int);
void	unique(RR*);
void	warning(char*, ...);

/* dnarea.c */
extern Area	*delegated;
extern Area	*owned;
void	addarea(RR *rp, Ndbtuple *t);
void	freeareas(Area**);
Area*	inmyarea(char*, Area**);

/* dblookup.c */
int	baddelegation(RR*, RR*, uchar*);
RR*	dblookup(char*, int, int, int, int);
RR*	dnsservers(int);
RR*	domainlist(int);
int	myip(uchar *);
int	localip(uchar *);
int	opendatabase(void);

/* dns.c */
char*	walkup(char*);
void	logreply(int, char*, uchar*, DNSmsg*);
void	logrequest(int, int, char*, uchar*, char*, char*, int);

/* dnresolve.c */
RR*	dnresolve(char*, int, int, Request*, RR**, int, int, int, int*);
int	udpport(char *);
int	mkreq(DN*, int type, uchar *pkt, int flags, ushort);
RR*	mkednsopt(void);
RR*	getednsopt(DNSmsg*, int*);
int	getercode(DNSmsg*);
void	setercode(DNSmsg*, int);

/* dnserver.c */
void	dnserver(DNSmsg*, DNSmsg*, Request*, uchar *, int);

/* dnudpserver.c */
void	dnudpserver(char*, char*);

/* dntcpserver.c */
void	dntcpserver(char*, char*, char*);

/* dnnotify.c */
void	dnnotify(DNSmsg*, DNSmsg*, Request*);
void	notifyproc(char*);

/* convDNS2M.c */
int	convDNS2M(DNSmsg*, uchar*, int);

/* convM2DNS.c */
char*	convM2DNS(uchar*, int, DNSmsg*, int*);

#pragma varargck argpos dnslog 1
#pragma varargck argpos warning 1

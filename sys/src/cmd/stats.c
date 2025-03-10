#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <draw.h>
#include <event.h>
#include <keyboard.h>

#define	MAXNUM	10	/* maximum number of numbers on data line */

typedef struct Graph	Graph;
typedef struct Machine	Machine;

struct Graph
{
	int		colindex;
	Rectangle	r;
	int		*data;
	int		ndata;
	char		*label;
	void		(*newvalue)(Machine*, uvlong*, uvlong*, int);
	void		(*update)(Graph*, uvlong, uvlong);
	Machine		*mach;
	int		overflow;
	Image		*overtmp;
	uvlong		hiwater;
};

enum
{
	/* /dev/swap */
	Mem		= 0,
	Maxmem,
	Swap,
	Maxswap,
	Reclaim,
	Maxreclaim,
	Kern,
	Maxkern,
	Draw,
	Maxdraw,

	/* /dev/sysstats */
	Procno	= 0,
	Context,
	Interrupt,
	Syscall,
	Fault,
	TLBfault,
	TLBpurge,
	Load,
	Idle,
	InIntr,

	/* /net/ether0/stats */
	In		= 0,
	Link,
	Out,
	Err0,
	Overflows,
	Soverflows,
};

struct Machine
{
	char		*name;
	char		*shortname;
	int		remote;
	int		statsfd;
	int		swapfd;
	int		etherfd[10];
	int		batteryfd;
	int		bitsybatfd;
	int		tempfd;
	int		disable;

	uvlong		devswap[10];
	uvlong		devsysstat[10];
	uvlong		prevsysstat[10];
	int		nproc;
	int		lgproc;
	uvlong		netetherstats[9];
	uvlong		prevetherstats[9];
	uvlong		batterystats[2];
	uvlong		temp[10];

	/* big enough to hold /dev/sysstat even with many processors */
	char		buf[8*1024];
	char		*bufp;
	char		*ebufp;
};

enum
{
	Mainproc,
	Inputproc,
	NPROC,
};

enum
{
	Ncolor		= 6,
	Ysqueeze	= 2,	/* vertical squeezing of label text */
	Labspace	= 2,	/* room around label */
	Dot		= 2,	/* height of dot */
	Opwid		= 5,	/* strlen("add  ") or strlen("drop ") */
	Nlab		= 3,	/* max number of labels on y axis */
	Lablen		= 16,	/* max length of label */
	Lx		= 4,	/* label tick length */
};

enum Menu2
{
	Mbattery,
	Mcontext,
	Mether,
	Methererr,
	Metherin,
	Metherout,
	Metherovf,
	Mfault,
	Midle,
	Minintr,
	Mintr,
	Mload,
	Mmem,
	Mswap,
	Mreclaim,
	Mkern,
	Mdraw,
	Msyscall,
	Mtlbmiss,
	Mtlbpurge,
	Mtemp,
	Nmenu2,
};

char	*menu2str[Nmenu2+1] = {
	"add  battery ",
	"add  context ",
	"add  ether   ",
	"add  ethererr",
	"add  etherin ",
	"add  etherout",
	"add  etherovf",
	"add  fault   ",
	"add  idle    ",
	"add  inintr  ",
	"add  intr    ",
	"add  load    ",
	"add  mem     ",
	"add  swap    ",
	"add  reclaim ",
	"add  kern    ",
	"add  draw    ",
	"add  syscall ",
	"add  tlbmiss ",
	"add  tlbpurge",
	"add  temp    ",
	nil,
};


void	contextval(Machine*, uvlong*, uvlong*, int),
	etherval(Machine*, uvlong*, uvlong*, int),
	ethererrval(Machine*, uvlong*, uvlong*, int),
	etherovfval(Machine*, uvlong*, uvlong*, int),
	etherinval(Machine*, uvlong*, uvlong*, int),
	etheroutval(Machine*, uvlong*, uvlong*, int),
	faultval(Machine*, uvlong*, uvlong*, int),
	intrval(Machine*, uvlong*, uvlong*, int),
	inintrval(Machine*, uvlong*, uvlong*, int),
	loadval(Machine*, uvlong*, uvlong*, int),
	idleval(Machine*, uvlong*, uvlong*, int),
	memval(Machine*, uvlong*, uvlong*, int),
	swapval(Machine*, uvlong*, uvlong*, int),
	reclaimval(Machine*, uvlong*, uvlong*, int),
	kernval(Machine*, uvlong*, uvlong*, int),
	drawval(Machine*, uvlong*, uvlong*, int),
	syscallval(Machine*, uvlong*, uvlong*, int),
	tlbmissval(Machine*, uvlong*, uvlong*, int),
	tlbpurgeval(Machine*, uvlong*, uvlong*, int),
	batteryval(Machine*, uvlong*, uvlong*, int),
	tempval(Machine*, uvlong*, uvlong*, int);

Menu	menu2 = {menu2str, nil};
int	present[Nmenu2];
void	(*newvaluefn[Nmenu2])(Machine*, uvlong*, uvlong*, int init) = {
	batteryval,
	contextval,
	etherval,
	ethererrval,
	etherinval,
	etheroutval,
	etherovfval,
	faultval,
	idleval,
	inintrval,
	intrval,
	loadval,
	memval,
	swapval,
	reclaimval,
	kernval,
	drawval,
	syscallval,
	tlbmissval,
	tlbpurgeval,
	tempval,
};

Image	*cols[Ncolor][3];
Graph	*graph;
Machine	*mach;
char	*mysysname;
char	argchars[] = "8bcdeEfiIkmlnprstwz";
int	pids[NPROC];
int 	parity;	/* toggled to avoid patterns in textured background */
int	nmach;
int	ngraph;	/* totaly number is ngraph*nmach */
double	scale = 1.0;
int	logscale = 0;
int	ylabels = 0;
int	sleeptime = 1000;
int	batteryperiod = 1000;
int	tempperiod = 1000;

char	*procnames[NPROC] = {"main", "input"};

void
killall(char *s)
{
	int i, pid;

	pid = getpid();
	for(i=0; i<NPROC; i++)
		if(pids[i] && pids[i]!=pid)
			postnote(PNPROC, pids[i], "kill");
	exits(s);
}

void*
emalloc(ulong sz)
{
	void *v;
	v = malloc(sz);
	if(v == nil) {
		fprint(2, "stats: out of memory allocating %ld: %r\n", sz);
		killall("mem");
	}
	memset(v, 0, sz);
	return v;
}

void*
erealloc(void *v, ulong sz)
{
	v = realloc(v, sz);
	if(v == nil) {
		fprint(2, "stats: out of memory reallocating %ld: %r\n", sz);
		killall("mem");
	}
	return v;
}

char*
estrdup(char *s)
{
	char *t;
	if((t = strdup(s)) == nil) {
		fprint(2, "stats: out of memory in strdup(%.10s): %r\n", s);
		killall("mem");
	}
	return t;
}

void
mkcol(int i, int c0, int c1, int c2)
{
	cols[i][0] = allocimagemix(display, c0, DWhite);
	cols[i][1] = allocimage(display, Rect(0,0,1,1), CMAP8, 1, c1);
	cols[i][2] = allocimage(display, Rect(0,0,1,1), CMAP8, 1, c2);
}

void
colinit(void)
{
	/* Peach */
	mkcol(0, 0xFFAAAAFF, 0xFFAAAAFF, 0xBB5D5DFF);
	/* Aqua */
	mkcol(1, DPalebluegreen, DPalegreygreen, DPurpleblue);
	/* Yellow */
	mkcol(2, DPaleyellow, DDarkyellow, DYellowgreen);
	/* Green */
	mkcol(3, DPalegreen, DMedgreen, DDarkgreen);
	/* Blue */
	mkcol(4, 0x00AAFFFF, 0x00AAFFFF, 0x0088CCFF);
	/* Grey */
	cols[5][0] = allocimage(display, Rect(0,0,1,1), CMAP8, 1, 0xEEEEEEFF);
	cols[5][1] = allocimage(display, Rect(0,0,1,1), CMAP8, 1, 0xCCCCCCFF);
	cols[5][2] = allocimage(display, Rect(0,0,1,1), CMAP8, 1, 0x888888FF);
}

int
loadbuf(Machine *m, int *fd)
{
	int n;


	if(*fd < 0)
		return 0;
	seek(*fd, 0, 0);
	n = read(*fd, m->buf, sizeof m->buf-1);
	if(n <= 0){
		close(*fd);
		*fd = -1;
		return 0;
	}
	m->bufp = m->buf;
	m->ebufp = m->buf+n;
	m->buf[n] = 0;
	return 1;
}

void
label(Point p, int dy, char *text)
{
	char *s;
	Rune r[2];
	int w, maxw, maxy;

	p.x += Labspace;
	maxy = p.y+dy;
	maxw = 0;
	r[1] = '\0';
	for(s=text; *s; ){
		if(p.y+font->height-Ysqueeze > maxy)
			break;
		w = chartorune(r, s);
		s += w;
		w = runestringwidth(font, r);
		if(w > maxw)
			maxw = w;
		runestring(screen, p, display->black, ZP, font, r);
		p.y += font->height-Ysqueeze;
	}
}

Point
paritypt(int x)
{
	return Pt(x+parity, 0);
}

Point
datapoint(Graph *g, int x, uvlong v, uvlong vmax)
{
	Point p;
	double y;

	p.x = x;
	y = ((double)v)/(vmax*scale);
	if(logscale){
		/*
		 * Arrange scale to cover a factor of 1000.
		 * vmax corresponds to the 100 mark.
		 * 10*vmax is the top of the scale.
		 */
		if(y <= 0.)
			y = 0;
		else{
			y = log10(y);
			/* 1 now corresponds to the top; -2 to the bottom; rescale */
			y = (y+2.)/3.;
		}
	}
	if(y >= 1.)
		y = 1;
	if(y <= 0.)
		y = 0;
	p.y = g->r.max.y - Dy(g->r)*y - Dot;
	if(p.y < g->r.min.y)
		p.y = g->r.min.y;
	if(p.y > g->r.max.y-Dot)
		p.y = g->r.max.y-Dot;
	return p;
}

void
drawdatum(Graph *g, int x, uvlong prev, uvlong v, uvlong vmax)
{
	int c;
	Point p, q;

	c = g->colindex;
	p = datapoint(g, x, v, vmax);
	q = datapoint(g, x, prev, vmax);
	if(p.y < q.y){
		draw(screen, Rect(p.x, g->r.min.y, p.x+1, p.y), cols[c][0], nil, paritypt(p.x));
		draw(screen, Rect(p.x, p.y, p.x+1, q.y+Dot), cols[c][2], nil, ZP);
		draw(screen, Rect(p.x, q.y+Dot, p.x+1, g->r.max.y), cols[c][1], nil, ZP);
	}else{
		draw(screen, Rect(p.x, g->r.min.y, p.x+1, q.y), cols[c][0], nil, paritypt(p.x));
		draw(screen, Rect(p.x, q.y, p.x+1, p.y+Dot), cols[c][2], nil, ZP);
		draw(screen, Rect(p.x, p.y+Dot, p.x+1, g->r.max.y), cols[c][1], nil, ZP);
	}

}

/* round vmax such that ylabel's have at most 3 non-zero digits */
uvlong
roundvmax(uvlong v)
{
	int e, o;

	e = ceil(log10(v)) - 2;
	if(e <= 0)
		return v + (v % 4);
	o = pow10(e);
	v /= o;
	v &= ~1;
	return v * o;
}


void
labelstrs(Graph *g, char strs[Nlab][Lablen], int *np)
{
	int j;
	uvlong v, vmax;

	g->newvalue(g->mach, &v, &vmax, 1);
	if(vmax == 0)
		vmax = 1;
	if(g->hiwater > vmax)
		vmax = g->hiwater;
	vmax = roundvmax(vmax);
	if(logscale){
		for(j=1; j<=2; j++)
			sprint(strs[j-1], "%g", scale*pow10(j)*(double)vmax/100.);
		*np = 2;
	}else{
		for(j=1; j<=3; j++)
			sprint(strs[j-1], "%g", scale*j*(double)vmax/4.0);
		*np = 3;
	}
}

int
labelwidth(void)
{
	int i, j, n, w, maxw;
	char strs[Nlab][Lablen];

	maxw = 0;
	for(i=0; i<ngraph; i++){
		/* choose value for rightmost graph */
		labelstrs(&graph[ngraph*(nmach-1)+i], strs, &n);
		for(j=0; j<n; j++){
			w = stringwidth(font, strs[j]);
			if(w > maxw)
				maxw = w;
		}
	}
	return maxw;
}

int
drawlabels(int maxx)
{
	int x, j, k, y, dy, dx, starty, startx, nlab, ly;
	int wid;
	Graph *g;
	char labs[Nlab][Lablen];
	Rectangle r;

	/* label left edge */
	x = screen->r.min.x;
	y = screen->r.min.y + Labspace+font->height+Labspace;
	dy = (screen->r.max.y - y)/ngraph;
	dx = Labspace+stringwidth(font, "0")+Labspace;
	startx = x+dx+1;
	starty = y;
	dx = (screen->r.max.x - startx)/nmach;

	if(!dy>Nlab*(font->height+1))
		return maxx;

	/* if there's not enough room */
	if((wid = labelwidth()) >= dx-10)
		return maxx;

	maxx -= 1+Lx+wid;
	draw(screen, Rect(maxx, starty, maxx+1, screen->r.max.y), display->black, nil, ZP);
	y = starty;
	for(j=0; j<ngraph; j++, y+=dy){
		/* choose value for rightmost graph */
		g = &graph[ngraph*(nmach-1)+j];
		labelstrs(g, labs, &nlab);
		r = Rect(maxx+1, y, screen->r.max.x, y+dy-1);
		if(j == ngraph-1)
			r.max.y = screen->r.max.y;
		draw(screen, r, cols[g->colindex][0], nil, paritypt(r.min.x));
		for(k=0; k<nlab; k++){
			ly = y + (dy*(nlab-k)/(nlab+1));
			draw(screen, Rect(maxx+1, ly, maxx+1+Lx, ly+1), display->black, nil, ZP);
			ly -= font->height/2;
			string(screen, Pt(maxx+1+Lx, ly), display->black, ZP, font, labs[k]);
		}
	}
	return maxx;
}


void
redraw(Graph *g, uvlong vmax)
{
	int i, c;

	c = g->colindex;
	draw(screen, g->r, cols[c][0], nil, paritypt(g->r.min.x));
	for(i=1; i<Dx(g->r); i++)
		drawdatum(g, g->r.max.x-i, g->data[i-1], g->data[i], vmax);
	drawdatum(g, g->r.min.x, g->data[i], g->data[i], vmax);
	g->overflow = 0;
}

void
update1(Graph *g, uvlong v, uvlong vmax)
{
	char buf[48];
	int overflow;

	overflow = 0;
	if(v > g->hiwater){
		g->hiwater = v;
		if(v > vmax){
			overflow = 1;
			g->hiwater = v;
			if(ylabels)
				drawlabels(screen->r.max.x);
			redraw(g, g->hiwater);
		}
	}
	if(g->hiwater > vmax)
		vmax = g->hiwater;
	if(g->overflow && g->overtmp!=nil)
		draw(screen, g->overtmp->r, g->overtmp, nil, g->overtmp->r.min);
	draw(screen, g->r, screen, nil, Pt(g->r.min.x+1, g->r.min.y));
	drawdatum(g, g->r.max.x-1, g->data[0], v, vmax);
	memmove(g->data+1, g->data, (g->ndata-1)*sizeof(g->data[0]));
	g->data[0] = v;
	g->overflow = 0;
	if(overflow && g->overtmp!=nil){
		g->overflow = 1;
		draw(g->overtmp, g->overtmp->r, screen, nil, g->overtmp->r.min);
		sprint(buf, "%llud", v);
		string(screen, g->overtmp->r.min, display->black, ZP, font, buf);
	}
}

/* read one line of text from buffer and process integers */
int
readnums(Machine *m, int n, uvlong *a, int spanlines)
{
	int i;
	char *p, *ep;

	if(spanlines)
		ep = m->ebufp;
	else
		for(ep=m->bufp; ep<m->ebufp; ep++)
			if(*ep == '\n')
				break;
	p = m->bufp;
	for(i=0; i<n && p<ep; i++){
		while(p<ep && (!isascii(*p) || !isdigit(*p)) && *p!='-')
			p++;
		if(p == ep)
			break;
		a[i] = strtoull(p, &p, 10);
	}
	if(ep < m->ebufp)
		ep++;
	m->bufp = ep;
	return i == n;
}

int
readswap(Machine *m, uvlong *a)
{
	static int xxx = 0;

	if(strstr(m->buf, "memory\n")){
		/* new /dev/swap - skip first 3 numbers */
		if(!readnums(m, 7, a, 1))
			return 0;

		a[Mem] = a[3];
		a[Maxmem] = a[4];
		a[Swap] = a[5];
		a[Maxswap] = a[6];

		a[Reclaim] = 0;
		a[Maxreclaim] = 0;
		if(m->bufp = strstr(m->buf, "reclaim")){
			while(m->bufp > m->buf && m->bufp[-1] != '\n')
				m->bufp--;
			a[Reclaim] = strtoull(m->bufp, &m->bufp, 10);
			while(*m->bufp++ == '/')
				a[Maxreclaim] = strtoull(m->bufp, &m->bufp, 10);
		}

		a[Kern] = 0;
		a[Maxkern] = 0;
		if(m->bufp = strstr(m->buf, "kernel malloc")){
			while(m->bufp > m->buf && m->bufp[-1] != '\n')
				m->bufp--;
			a[Kern] = strtoull(m->bufp, &m->bufp, 10);
			while(*m->bufp++ == '/')
				a[Maxkern] = strtoull(m->bufp, &m->bufp, 10);
		}

		a[Draw] = 0;
		a[Maxdraw] = 0;
		if(m->bufp = strstr(m->buf, "kernel draw")){
			while(m->bufp > m->buf && m->bufp[-1] != '\n')
				m->bufp--;
			a[Draw] = strtoull(m->bufp, &m->bufp, 10);
			while(*m->bufp++ == '/')
				a[Maxdraw] = strtoull(m->bufp, &m->bufp, 10);
		}

		return 1;
	}

	a[Reclaim] = 0;
	a[Maxreclaim] = 0;
	a[Kern] = 0;
	a[Maxkern] = 0;
	a[Draw] = 0;
	a[Maxdraw] = 0;

	return readnums(m, 4, a, 0);
}

char*
shortname(char *s)
{
	char *p, *e;

	p = estrdup(s);
	e = strchr(p, '.');
	if(e)
		*e = 0;
	return p;
}

int
ilog10(uvlong j)
{
	int i;

	for(i = 0; j >= 10; i++)
		j /= 10;
	return i;
}

int
initmach(Machine *m, char *name)
{
	int n, i, j, fd;
	uvlong a[MAXNUM];
	char *p, mpt[256], buf[256];
	Dir *d;

	p = strchr(name, '!');
	if(p)
		p++;
	else
		p = name;
	m->name = estrdup(p);
	m->shortname = shortname(p);
	m->remote = (strcmp(p, mysysname) != 0);
	if(m->remote == 0)
		strcpy(mpt, "");
	else{
		Waitmsg *w;
		int pid;

		snprint(mpt, sizeof mpt, "/n/%s", p);

		pid = fork();
		switch(pid){
		case -1:
			fprint(2, "can't fork: %r\n");
			return 0;
		case 0:
			execl("/bin/rimport", "rimport", name, "/", mpt, nil);
			fprint(2, "can't exec: %r\n");
			exits("exec");
		}
		w = wait();
		if(w == nil || w->pid != pid || w->msg[0] != '\0'){
			free(w);
			return 0;
		}
		free(w);
	}

	snprint(buf, sizeof buf, "%s/dev/swap", mpt);
	m->swapfd = open(buf, OREAD);
	if(loadbuf(m, &m->swapfd) && readswap(m, a))
		memmove(m->devswap, a, sizeof m->devswap);

	snprint(buf, sizeof buf, "%s/dev/sysstat", mpt);
	m->statsfd = open(buf, OREAD);
	if(loadbuf(m, &m->statsfd)){
		for(n=0; readnums(m, nelem(m->devsysstat), a, 0); n++)
			;
		m->nproc = n;
	}else
		m->nproc = 1;
	m->lgproc = ilog10(m->nproc);

	/* find all the ethernets */
	n = 0;
	snprint(buf, sizeof buf, "%s/net/", mpt);
	if((fd = open(buf, OREAD)) >= 0){
		for(d = nil; (i = dirread(fd, &d)) > 0; free(d)){
			for(j=0; j<i; j++){
				if(strncmp(d[j].name, "ether", 5))
					continue;
				snprint(buf, sizeof buf, "%s/net/%s/stats", mpt, d[j].name);
				if((m->etherfd[n] = open(buf, OREAD)) < 0)
					continue;
				if(++n >= nelem(m->etherfd))
					break;
			}
			if(n >= nelem(m->etherfd))
				break;
		}
		close(fd);
	}
	while(n < nelem(m->etherfd))
		m->etherfd[n++] = -1;

	snprint(buf, sizeof buf, "%s/mnt/apm/battery", mpt);
	m->batteryfd = open(buf, OREAD);
	if(m->batteryfd < 0){
		snprint(buf, sizeof buf, "%s/mnt/pm/battery", mpt);
		m->batteryfd = open(buf, OREAD);
	}
	m->bitsybatfd = -1;
	if(m->batteryfd >= 0){
		batteryperiod = 10000;
		if(loadbuf(m, &m->batteryfd) && readnums(m, nelem(m->batterystats), a, 0))
			memmove(m->batterystats, a, sizeof(m->batterystats));
	}else{
		snprint(buf, sizeof buf, "%s/dev/battery", mpt);
		m->bitsybatfd = open(buf, OREAD);
		if(loadbuf(m, &m->bitsybatfd) && readnums(m, 1, a, 0))
			memmove(m->batterystats, a, sizeof(m->batterystats));
	}
	snprint(buf, sizeof buf, "%s/dev/cputemp", mpt);
	m->tempfd = open(buf, OREAD);
	if(m->tempfd < 0){
		tempperiod = 5000;
		snprint(buf, sizeof buf, "%s/mnt/pm/cputemp", mpt);
		m->tempfd = open(buf, OREAD);
	}
	if(loadbuf(m, &m->tempfd))
		for(n=0; n < nelem(m->temp) && readnums(m, 2, a, 0); n++)
			 m->temp[n] = a[0];
	return 1;
}

jmp_buf catchalarm;

int
alarmed(void *a, char *s)
{
	if(strcmp(s, "alarm") == 0)
		notejmp(a, catchalarm, 1);
	return 0;
}

int
needswap(int init)
{
	return init | present[Mmem] | present[Mswap] | present[Mreclaim] | present[Mkern] | present[Mdraw];
}


int
needstat(int init)
{
	return init | present[Mcontext]  | present[Mfault] | present[Mintr] | present[Mload] | present[Midle] |
		present[Minintr] | present[Msyscall] | present[Mtlbmiss] | present[Mtlbpurge];
}


int
needether(int init)
{
	return init | present[Mether] | present[Metherin] | present[Metherout] | present[Methererr] | present[Metherovf];
}

int
needbattery(int init)
{
	static uint step = 0;

	if(++step*sleeptime >= batteryperiod){
		step = 0;
		return init | present[Mbattery];
	}

	return 0;
}

int
needtemp(int init)
{
	static uint step = 0;

	if(++step*sleeptime >= tempperiod){
		step = 0;
		return init | present[Mtemp];
	}

	return 0;
}

void
vadd(uvlong *a, uvlong *b, int n)
{
	int i;

	for(i=0; i<n; i++)
		a[i] += b[i];
}

void
readmach(Machine *m, int init)
{
	int n;
	uvlong a[nelem(m->devsysstat)];
	char buf[32];

	if(m->remote && (m->disable || setjmp(catchalarm))){
		if (m->disable++ >= 5)
			m->disable = 0; /* give it another chance */
		memmove(m->devsysstat, m->prevsysstat, sizeof m->devsysstat);
		memmove(m->netetherstats, m->prevetherstats, sizeof m->netetherstats);
		return;
	}
	snprint(buf, sizeof buf, "%s", m->name);
	if (strcmp(m->name, buf) != 0){
		free(m->name);
		m->name = estrdup(buf);
		free(m->shortname);
		m->shortname = shortname(buf);
		if(display != nil)	/* else we're still initializing */
			eresized(0);
	}
	if(m->remote){
		atnotify(alarmed, 1);
		alarm(5000);
	}
	if(needswap(init) && loadbuf(m, &m->swapfd) && readswap(m, a))
		memmove(m->devswap, a, sizeof m->devswap);
	if(needstat(init) && loadbuf(m, &m->statsfd)){
		memmove(m->prevsysstat, m->devsysstat, sizeof m->devsysstat);
		memset(m->devsysstat, 0, sizeof m->devsysstat);
		for(n=0; n<m->nproc && readnums(m, nelem(m->devsysstat), a, 0); n++)
			vadd(m->devsysstat, a, nelem(m->devsysstat));
	}
	if(needether(init)){
		memmove(m->prevetherstats, m->netetherstats, sizeof m->netetherstats);
		memset(m->netetherstats, 0, sizeof(m->netetherstats));
		for(n=0; n<nelem(m->etherfd) && m->etherfd[n] >= 0; n++){
			if(loadbuf(m, &m->etherfd[n]) && readnums(m, nelem(m->netetherstats), a, 1))
				vadd(m->netetherstats, a, nelem(m->netetherstats));
		}
	}
	if(needbattery(init)){
		if(loadbuf(m, &m->batteryfd) && readnums(m, nelem(m->batterystats), a, 0))
			memmove(m->batterystats, a, sizeof(m->batterystats));
		else if(loadbuf(m, &m->bitsybatfd) && readnums(m, 1, a, 0))
			memmove(m->batterystats, a, sizeof(m->batterystats));
	}
	if(needtemp(init) && loadbuf(m, &m->tempfd))
		for(n=0; n < nelem(m->temp) && readnums(m, 2, a, 0); n++)
			 m->temp[n] = a[0];
	if(m->remote){
		alarm(0);
		atnotify(alarmed, 0);
	}
}

void
memval(Machine *m, uvlong *v, uvlong *vmax, int)
{
	*v = m->devswap[Mem];
	*vmax = m->devswap[Maxmem];
	if(*vmax == 0)
		*vmax = 1;
}

void
swapval(Machine *m, uvlong *v, uvlong *vmax, int)
{
	*v = m->devswap[Swap];
	*vmax = m->devswap[Maxswap];
	if(*vmax == 0)
		*vmax = 1;
}

void
reclaimval(Machine *m, uvlong *v, uvlong *vmax, int)
{
	*v = m->devswap[Reclaim];
	*vmax = m->devswap[Maxreclaim];
	if(*vmax == 0)
		*vmax = 1;
}

void
kernval(Machine *m, uvlong *v, uvlong *vmax, int)
{
	*v = m->devswap[Kern];
	*vmax = m->devswap[Maxkern];
	if(*vmax == 0)
		*vmax = 1;
}

void
drawval(Machine *m, uvlong *v, uvlong *vmax, int)
{
	*v = m->devswap[Draw];
	*vmax = m->devswap[Maxdraw];
	if(*vmax == 0)
		*vmax = 1;
}

void
contextval(Machine *m, uvlong *v, uvlong *vmax, int init)
{
	*v = (m->devsysstat[Context]-m->prevsysstat[Context])&0xffffffff;
	*vmax = sleeptime*m->nproc;
	if(init)
		*vmax = sleeptime;
}

/*
 * bug: need to factor in HZ
 */
void
intrval(Machine *m, uvlong *v, uvlong *vmax, int init)
{
	*v = (m->devsysstat[Interrupt]-m->prevsysstat[Interrupt])&0xffffffff;
	*vmax = sleeptime*m->nproc*10;
	if(init)
		*vmax = sleeptime*10;
}

void
syscallval(Machine *m, uvlong *v, uvlong *vmax, int init)
{
	*v = (m->devsysstat[Syscall]-m->prevsysstat[Syscall])&0xffffffff;
	*vmax = sleeptime*m->nproc;
	if(init)
		*vmax = sleeptime;
}

void
faultval(Machine *m, uvlong *v, uvlong *vmax, int init)
{
	*v = (m->devsysstat[Fault]-m->prevsysstat[Fault])&0xffffffff;
	*vmax = sleeptime*m->nproc;
	if(init)
		*vmax = sleeptime;
}

void
tlbmissval(Machine *m, uvlong *v, uvlong *vmax, int init)
{
	*v = (m->devsysstat[TLBfault]-m->prevsysstat[TLBfault])&0xffffffff;
	*vmax = (sleeptime/1000)*10*m->nproc;
	if(init)
		*vmax = (sleeptime/1000)*10;
}

void
tlbpurgeval(Machine *m, uvlong *v, uvlong *vmax, int init)
{
	*v = (m->devsysstat[TLBpurge]-m->prevsysstat[TLBpurge])&0xffffffff;
	*vmax = (sleeptime/1000)*10*m->nproc;
	if(init)
		*vmax = (sleeptime/1000)*10;
}

void
loadval(Machine *m, uvlong *v, uvlong *vmax, int init)
{
	*v = m->devsysstat[Load];
	*vmax = 1000*m->nproc;
	if(init)
		*vmax = 1000;
}

void
idleval(Machine *m, uvlong *v, uvlong *vmax, int)
{
	*v = m->devsysstat[Idle]/m->nproc;
	*vmax = 100;
}

void
inintrval(Machine *m, uvlong *v, uvlong *vmax, int)
{
	*v = m->devsysstat[InIntr]/m->nproc;
	*vmax = 100;
}

void
etherval(Machine *m, uvlong *v, uvlong *vmax, int)
{
	*v = m->netetherstats[In]-m->prevetherstats[In] + m->netetherstats[Out]-m->prevetherstats[Out];
	*vmax = sleeptime;
}

void
etherinval(Machine *m, uvlong *v, uvlong *vmax, int)
{
	*v = m->netetherstats[In]-m->prevetherstats[In];
	*vmax = sleeptime;
}

void
etheroutval(Machine *m, uvlong *v, uvlong *vmax, int)
{
	*v = m->netetherstats[Out]-m->prevetherstats[Out];
	*vmax = sleeptime;
}

void
ethererrval(Machine *m, uvlong *v, uvlong *vmax, int)
{
	int i;

	*v = 0;
	for(i=Err0; i<nelem(m->netetherstats); i++)
		*v += m->netetherstats[i]-m->prevetherstats[i];
	*vmax = (sleeptime/1000)*10;
}

void
etherovfval(Machine *m, uvlong *v, uvlong *vmax, int)
{
	int i;

	*v = 0;
	for(i=Overflows; i<=Soverflows; i++)
		*v += m->netetherstats[i]-m->prevetherstats[i];
	*vmax = (sleeptime/1000)*10;
}

void
batteryval(Machine *m, uvlong *v, uvlong *vmax, int)
{
	*v = m->batterystats[0];
	if(m->bitsybatfd >= 0)
		*vmax = 184;		// at least on my bitsy...
	else
		*vmax = 100;
}

void
tempval(Machine *m, uvlong *v, uvlong *vmax, int)
{
	ulong l;

	*vmax = 100;
	l = m->temp[0];
	if(l == ~0 || l == 0)
		*v = 0;
	else
		*v = l;
}

void
usage(void)
{
	fprint(2, "usage: stats [-O] [-S scale] [-LY] [-%s] [machine...]\n", argchars);
	exits("usage");
}

void
addgraph(int n)
{
	Graph *g, *ograph;
	int i, j;
	static int nadd;

	if(n > nelem(menu2str))
		abort();
	/* avoid two adjacent graphs of same color */
	if(ngraph>0 && graph[ngraph-1].colindex==nadd%Ncolor)
		nadd++;
	ograph = graph;
	graph = emalloc(nmach*(ngraph+1)*sizeof(Graph));
	for(i=0; i<nmach; i++)
		for(j=0; j<ngraph; j++)
			graph[i*(ngraph+1)+j] = ograph[i*ngraph+j];
	free(ograph);
	ngraph++;
	for(i=0; i<nmach; i++){
		g = &graph[i*ngraph+(ngraph-1)];
		memset(g, 0, sizeof(Graph));
		g->label = menu2str[n]+Opwid;
		g->newvalue = newvaluefn[n];
		g->update = update1;	/* no other update functions yet */
		g->mach = &mach[i];
		g->colindex = nadd%Ncolor;
	}
	present[n] = 1;
	nadd++;
}

void
dropgraph(int which)
{
	Graph *ograph;
	int i, j, n;

	if(which > nelem(menu2str))
		abort();
	/* convert n to index in graph table */
	n = -1;
	for(i=0; i<ngraph; i++)
		if(strcmp(menu2str[which]+Opwid, graph[i].label) == 0){
			n = i;
			break;
		}
	if(n < 0){
		fprint(2, "stats: internal error can't drop graph\n");
		killall("error");
	}
	ograph = graph;
	graph = emalloc(nmach*(ngraph-1)*sizeof(Graph));
	for(i=0; i<nmach; i++){
		for(j=0; j<n; j++)
			graph[i*(ngraph-1)+j] = ograph[i*ngraph+j];
		free(ograph[i*ngraph+j].data);
		freeimage(ograph[i*ngraph+j].overtmp);
		for(j++; j<ngraph; j++)
			graph[i*(ngraph-1)+j-1] = ograph[i*ngraph+j];
	}
	free(ograph);
	ngraph--;
	present[which] = 0;
}

int
addmachine(char *name)
{
	if(ngraph > 0){
		fprint(2, "stats: internal error: ngraph>0 in addmachine()\n");
		usage();
	}
	if(mach == nil)
		nmach = 0;	/* a little dance to get us started with local machine by default */
	mach = erealloc(mach, (nmach+1)*sizeof(Machine));
	memset(mach+nmach, 0, sizeof(Machine));
	if (initmach(mach+nmach, name)){
		nmach++;
		return 1;
	} else
		return 0;
}

void
resize(void)
{
	int i, j, n, startx, starty, x, y, dx, dy, ondata, maxx;
	Graph *g;
	Rectangle machr, r;
	uvlong v, vmax;
	char buf[128];

	draw(screen, screen->r, display->white, nil, ZP);

	/* label left edge */
	x = screen->r.min.x;
	y = screen->r.min.y + Labspace+font->height+Labspace;
	dy = (screen->r.max.y - y)/ngraph;
	dx = Labspace+stringwidth(font, "0")+Labspace;
	startx = x+dx+1;
	starty = y;
	for(i=0; i<ngraph; i++,y+=dy){
		draw(screen, Rect(x, y-1, screen->r.max.x, y), display->black, nil, ZP);
		draw(screen, Rect(x, y, x+dx, screen->r.max.y), cols[graph[i].colindex][0], nil, paritypt(x));
		label(Pt(x, y), dy, graph[i].label);
		draw(screen, Rect(x+dx, y, x+dx+1, screen->r.max.y), cols[graph[i].colindex][2], nil, ZP);
	}

	/* label top edge */
	dx = (screen->r.max.x - startx)/nmach;
	for(x=startx, i=0; i<nmach; i++,x+=dx){
		draw(screen, Rect(x-1, starty-1, x, screen->r.max.y), display->black, nil, ZP);
		j = dx/stringwidth(font, "0");
		n = mach[i].nproc;
		if(n>1 && j>=1+3+mach[i].lgproc){	/* first char of name + (n) */
			j -= 3+mach[i].lgproc;
			if(j <= 0)
				j = 1;
			snprint(buf, sizeof buf, "%.*s(%d)", j, mach[i].shortname, n);
		}else
			snprint(buf, sizeof buf, "%.*s", j, mach[i].shortname);
		string(screen, Pt(x+Labspace, screen->r.min.y + Labspace), display->black, ZP, font, buf);
	}

	maxx = screen->r.max.x;
	if(ylabels)
		maxx = drawlabels(maxx);

	/* create graphs */
	for(i=0; i<nmach; i++){
		machr = Rect(startx+i*dx, starty, startx+(i+1)*dx - 1, screen->r.max.y);
		if(i == nmach-1)
			machr.max.x = maxx;
		y = starty;
		for(j=0; j<ngraph; j++, y+=dy){
			g = &graph[i*ngraph+j];
			/* allocate data */
			ondata = g->ndata;
			g->ndata = Dx(machr)+1;	/* may be too many if label will be drawn here; so what? */
			g->data = erealloc(g->data, g->ndata*sizeof(ulong));
			if(g->ndata > ondata)
				memset(g->data+ondata, 0, (g->ndata-ondata)*sizeof(ulong));
			/* set geometry */
			g->r = machr;
			g->r.min.y = y;
			g->r.max.y = y+dy - 1;
			if(j == ngraph-1)
				g->r.max.y = screen->r.max.y;
			draw(screen, g->r, cols[g->colindex][0], nil, paritypt(g->r.min.x));
			g->overflow = 0;
			r = g->r;
			r.max.y = r.min.y+font->height;
			r.max.x = r.min.x+stringwidth(font, "999999999999");
			freeimage(g->overtmp);
			g->overtmp = nil;
			if(r.max.x <= g->r.max.x)
				g->overtmp = allocimage(display, r, screen->chan, 0, -1);
			g->newvalue(g->mach, &v, &vmax, 0);
			if(vmax == 0)
				vmax = 1;
			if(g->hiwater > vmax)
				vmax = g->hiwater;
			vmax = roundvmax(vmax);
			redraw(g, vmax);
		}
	}

	flushimage(display, 1);
}

void
eresized(int new)
{
	lockdisplay(display);
	if(new && getwindow(display, Refnone) < 0) {
		fprint(2, "stats: can't reattach to window\n");
		killall("reattach");
	}
	resize();
	unlockdisplay(display);
}

void
inputproc(void)
{
	Event e;
	int i;

	for(;;){
		switch(eread(Emouse|Ekeyboard, &e)){
		case Emouse:
			if(e.mouse.buttons == 4){
				lockdisplay(display);
				for(i=0; i<Nmenu2; i++)
					if(present[i])
						memmove(menu2str[i], "drop ", Opwid);
					else
						memmove(menu2str[i], "add  ", Opwid);
				i = emenuhit(3, &e.mouse, &menu2);
				if(i >= 0){
					if(!present[i])
						addgraph(i);
					else if(ngraph > 1)
						dropgraph(i);
					resize();
				}
				unlockdisplay(display);
			}
			break;
		case Ekeyboard:
			if(e.kbdc==Kdel || e.kbdc=='q')
				killall(nil);
			break;
		}
	}
}

void
startproc(void (*f)(void), int index)
{
	int pid;

	switch(pid = rfork(RFPROC|RFMEM|RFNOWAIT)){
	case -1:
		fprint(2, "stats: fork failed: %r\n");
		killall("fork failed");
	case 0:
		f();
		fprint(2, "stats: %s process exits\n", procnames[index]);
		if(index >= 0)
			killall("process died");
		exits(nil);
	}
	if(index >= 0)
		pids[index] = pid;
}

void
main(int argc, char *argv[])
{
	int i, j;
	double secs;
	uvlong v, vmax, nargs;
	char args[100];

	quotefmtinstall();

	nmach = 1;
	mysysname = getenv("sysname");
	if(mysysname == nil){
		fprint(2, "stats: can't find $sysname: %r\n");
		exits("sysname");
	}

	nargs = 0;
	ARGBEGIN{
	case 'T':
		secs = atof(EARGF(usage()));
		if(secs > 0)
			sleeptime = 1000*secs;
		break;
	case 'S':
		scale = atof(EARGF(usage()));
		if(scale <= 0)
			usage();
		break;
	case 'L':
		logscale++;
		break;
	case 'Y':
		ylabels++;
		break;
	case 'O':
		break;
	default:
		if(nargs>=sizeof args || strchr(argchars, ARGC())==nil)
			usage();
		args[nargs++] = ARGC();
	}ARGEND

	if(argc == 0){
		mach = emalloc(nmach*sizeof(Machine));
		initmach(&mach[0], mysysname);
		readmach(&mach[0], 1);
	}else{
		rfork(RFNAMEG);
		for(i=j=0; i<argc; i++){
			if (addmachine(argv[i]))
				readmach(&mach[j++], 1);
		}
		if (j == 0)
			exits("connect");
	}

	for(i=0; i<nargs; i++)
	switch(args[i]){
	default:
		fprint(2, "stats: internal error: unknown arg %c\n", args[i]);
		usage();
	case 'b':
		addgraph(Mbattery);
		break;
	case 'c':
		addgraph(Mcontext);
		break;
	case 'e':
		addgraph(Mether);
		break;
	case 'E':
		addgraph(Metherin);
		addgraph(Metherout);
		break;
	case 'f':
		addgraph(Mfault);
		break;
	case 'i':
		addgraph(Mintr);
		break;
	case 'I':
		addgraph(Mload);
		addgraph(Midle);
		addgraph(Minintr);
		break;
	case 'l':
		addgraph(Mload);
		break;
	case 'm':
		addgraph(Mmem);
		break;
	case 'n':
		addgraph(Metherin);
		addgraph(Metherout);
		addgraph(Methererr);
		addgraph(Metherovf);
		break;
	case 'p':
		addgraph(Mtlbpurge);
		break;
	case 'r':
		addgraph(Mreclaim);
		break;
	case 's':
		addgraph(Msyscall);
		break;
	case 't':
		addgraph(Mtlbmiss);
		addgraph(Mtlbpurge);
		break;
	case 'w':
		addgraph(Mswap);
		break;
	case 'k':
		addgraph(Mkern);
		break;
	case 'd':
		addgraph(Mdraw);
		break;
	case 'z':
		addgraph(Mtemp);
		break;
	}

	if(ngraph == 0)
		addgraph(Mload);

	for(i=0; i<nmach; i++)
		for(j=0; j<ngraph; j++)
			graph[i*ngraph+j].mach = &mach[i];

	if(initdraw(nil, nil, "stats") < 0){
		fprint(2, "stats: initdraw failed: %r\n");
		exits("initdraw");
	}
	display->locking = 1;	/* tell library we're using the display lock */
	colinit();
	einit(Emouse|Ekeyboard);
	startproc(inputproc, Inputproc);
	pids[Mainproc] = getpid();

	resize();

	unlockdisplay(display); /* display is still locked from initdraw() */
	for(;;){
		for(i=0; i<nmach; i++)
			readmach(&mach[i], 0);
		lockdisplay(display);
		parity = 1-parity;
		for(i=0; i<nmach*ngraph; i++){
			graph[i].newvalue(graph[i].mach, &v, &vmax, 0);
			if(vmax == 0)
				vmax = 1;
			vmax = roundvmax(vmax);
			graph[i].update(&graph[i], v, vmax);
		}
		flushimage(display, 1);
		unlockdisplay(display);
		sleep(sleeptime);
	}
}

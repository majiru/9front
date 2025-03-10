#include <u.h>
#include <libc.h>
#include <bio.h>
#include <auth.h>
#include <fcall.h>
#include <disk.h>
#include <regexp.h>

enum {
	LEN	= 8*1024,
	HUNKS	= 128,
};

typedef struct File File;
struct File{
	char	*new;
	char	*elem;
	char	*old;
	char	*uid;
	char	*gid;
	ulong	mode;
};

typedef void Mkfserr(char*, void*);
typedef void Mkfsenum(char*, char*, Dir*, void*);

typedef struct Name Name;
struct Name {
	int n;
	char *s;
};

typedef struct Opt Opt;
struct Opt {
	int level;
	long mode;
	long mask;
	Reprog *skip;
	char *uid;
	char *gid;
	Opt *prev;
};

typedef struct Mkaux Mkaux;
struct Mkaux {
	Mkfserr *warn;
	Mkfsenum *mkenum;
	char *root;
	char *proto;
	jmp_buf jmp;
	Biobuf *b;

	Name oldfile;
	Name fullname;
	int	lineno;
	int	indent;

	Opt *opt;

	void *a;
};

static void domkfs(Mkaux *mkaux, File *me, int level);

static int	copyfile(Mkaux*, File*, Dir*, int);
static void	freefile(File*);
static void	freeoptptr(Opt*, void*);
static char*	getline(Mkaux*);
static File*	getfile(Mkaux*, File*);
static char*	getmode(Mkaux*, char*, ulong*);
static char*	getname(Mkaux*, char*, char**);
static char*	getpath(Mkaux*, char*);
static int	mkfile(Mkaux*, File*);
static char*	mkpath(Mkaux*, char*, char*);
static void	mktree(Mkaux*, File*, int);
static void	setname(Mkaux*, Name*, File*);
static void	setopt(Mkaux*, char*, char*);
static void	skipdir(Mkaux*);
static void	warn(Mkaux*, char *, ...);
static void	popopt(Mkaux *mkaux);

int
rdproto(char *proto, char *root, Mkfsenum *mkenum, Mkfserr *mkerr, void *a)
{
	Mkaux mx, *m;
	File file;
	int rv;

	m = &mx;
	memset(&mx, 0, sizeof mx);
	if(root == nil)
		root = "/";

	m->root = root;
	m->warn = mkerr;
	m->mkenum = mkenum;
	m->a = a;
	m->proto = proto;
	m->lineno = 0;
	m->indent = 0;
	m->opt = nil;
	if((m->b = Bopen(proto, OREAD)) == nil) {
		werrstr("open '%s': %r", proto);
		return -1;
	}

	memset(&file, 0, sizeof file);
	file.new = "";
	file.old = nil;

	rv = 0;
	if(setjmp(m->jmp) == 0)
		domkfs(m, &file, -1);
	else
		rv = -1;
	free(m->oldfile.s);
	free(m->fullname.s);
	m->indent = -1;
	popopt(m);
	return rv;
}

static void*
emalloc(Mkaux *mkaux, ulong n)
{
	void *v;

	v = malloc(n);
	if(v == nil)
		longjmp(mkaux->jmp, 1);	/* memory leak */
	memset(v, 0, n);
	return v;
}

static char*
estrdup(Mkaux *mkaux, char *s)
{
	s = strdup(s);
	if(s == nil)
		longjmp(mkaux->jmp, 1);	/* memory leak */
	return s;
}

static void
domkfs(Mkaux *mkaux, File *me, int level)
{
	File *child;
	int rec;

	child = getfile(mkaux, me);
	if(child == nil)
		return;
	if((child->elem[0] == '+' || child->elem[0] == '*') && child->elem[1] == '\0'){
		rec = child->elem[0] == '+';
		free(child->new);
		child->new = estrdup(mkaux, me->new);
		setname(mkaux, &mkaux->oldfile, child);
		mktree(mkaux, child, rec);
		freefile(child);
		child = getfile(mkaux, me);
	}
	while(child != nil && mkaux->indent > level){
		if(mkfile(mkaux, child))
			domkfs(mkaux, child, mkaux->indent);
		freefile(child);
		child = getfile(mkaux, me);
	}
	if(child != nil){
		freefile(child);
		Bseek(mkaux->b, -Blinelen(mkaux->b), 1);
		mkaux->lineno--;
	}
}

static void
mktree(Mkaux *mkaux, File *me, int rec)
{
	File child;
	Dir *d;
	int i, n, fd;

	fd = open(mkaux->oldfile.s, OREAD);
	if(fd < 0){
		warn(mkaux, "can't open %s: %r", mkaux->oldfile.s);
		return;
	}
	child = *me;
	while((n = dirread(fd, &d)) > 0){
		for(i = 0; i < n; i++){
			if(mkaux->opt && mkaux->opt->skip){
				Resub m[8];

				memset(m, 0, sizeof(m));
				if(regexec(mkaux->opt->skip, d[i].name, m, nelem(m)))
					continue;
			}
			child.new = mkpath(mkaux, me->new, d[i].name);
			if(me->old != nil)
				child.old = mkpath(mkaux, me->old, d[i].name);
			child.elem = d[i].name;
			setname(mkaux, &mkaux->oldfile, &child);
			if((!(d[i].mode&DMDIR) || rec) && copyfile(mkaux, &child, &d[i], 1) && rec)
				mktree(mkaux, &child, rec);
			free(child.new);
			free(child.old);
		}
		free(d);
	}
	close(fd);
}

static int
mkfile(Mkaux *mkaux, File *f)
{
	Dir *d;

	if((d = dirstat(mkaux->oldfile.s)) == nil){
		warn(mkaux, "can't stat file %s: %r", mkaux->oldfile.s);
		skipdir(mkaux);
		return 0;
	}
	return copyfile(mkaux, f, d, 0);
}

enum {
	SLOP = 30
};

static void
setname(Mkaux *mkaux, Name *name, File *f)
{
	char *s1, *s2;
	int n;
	
	s1 = mkaux->root;
	s2 = "";
	if(f->old){
		/* if old is not a absolute path, dont append root to it */
		if(f->old[0] != '/')
			s1 = f->old;
		else
			s2 = f->old;
	}else
		s2 = f->new;
	n = strlen(s1) + strlen(s2) + 2;
	if(name->n < n+SLOP/2) {
		free(name->s);
		name->s = emalloc(mkaux, n+SLOP);
		name->n = n+SLOP;
	}
	snprint(name->s, name->n, "%s/%s", s1, s2);
	cleanname(name->s);
}


static int
copyfile(Mkaux *mkaux, File *f, Dir *d, int permonly)
{
	Dir *nd;
	Opt *o;
	ulong xmode;
	char *p;

	setname(mkaux, &mkaux->fullname, f);

	/*
	 * Extra stat here is inefficient but accounts for binds.
	 */
	if((nd = dirstat(mkaux->fullname.s)) != nil)
		d = nd;

	d->name = f->elem;
	o = mkaux->opt;
	if(strcmp(f->uid, "-") != 0)
		d->uid = f->uid;
	else if(o != nil && o->uid != nil)
		d->uid = o->uid;
	if(strcmp(f->gid, "-") != 0)
		d->gid = f->gid;
	else if(o != nil && o->gid != nil)
		d->gid = o->gid;
	if(f->mode != ~0){
		if(permonly)
			d->mode = (d->mode & ~0666) | (f->mode & 0666);
		else if((d->mode&DMDIR) != (f->mode&DMDIR))
			warn(mkaux, "inconsistent mode for %s", f->new);
		else
			d->mode = f->mode;
	} else if(o != nil && o->mask)
		d->mode = (d->mode & ~o->mask) | (o->mode & o->mask);

	if((p = strrchr(f->new, '/')) != nil)
		d->name = p+1;
	else
		d->name = f->new;
	mkaux->mkenum(f->new, mkaux->fullname.s, d, mkaux->a);
	xmode = d->mode;
	free(nd);
	return (xmode&DMDIR) != 0;
}

static char *
mkpath(Mkaux *mkaux, char *prefix, char *elem)
{
	char *p;
	int n;

	n = strlen(prefix) + strlen(elem) + 2;
	p = emalloc(mkaux, n);
	snprint(p, n, "%s/%s", prefix, elem);
	return cleanname(p);
}

static int
parsemode(char *spec, long *pmask, long *pmode)
{
	char op, set, *s;
	long mode;
	long mask;

	s = spec;
	op = set = 0;
	mode = 0;
	mask = DMAPPEND | DMEXCL | DMTMP;

	if(*s >= '0' && *s <= '7'){
		mask = 0666;
		mode = strtoul(s, 0, 8);
		op = '=';
		s = "!";
	}

	for(; *s && op == 0; s++){
		switch(*s){
		case 'a':
			mask |= 0666;
			break;
		case 'u':
			mask |= 0600;
			break;
		case 'g':
			mask |= 060;
			break;
		case 'o':
			mask |= 06;
			break;
		case '-':
		case '+':
		case '=':
			op = *s;
			break;
		default:
			return 0;
		}
	}
	if(s == spec)
		mask |= 0666;

	for(; *s; s++){
		switch(*s){
		case 'r':
			mode |= 0444;
			break;
		case 'w':
			mode |= 0222;
			break;
		case 'x':
			mode |= 0111;
			break;
		case 'a':
			mode |= DMAPPEND;
			break;
		case 'l':
			mode |= DMEXCL;
			break;
		case 't':
			mode |= DMTMP;
			break;
		case '!':
			set = 1;
			break;
		default:
			return 0;
		}
	}

	if(op == '+' || op == '-')
		mask &= mode;
	if(op == '-')
		mode = ~mode;
	if(set)
		*pmask = 0;

	*pmask |= mask;
	*pmode = (*pmode & ~mask) | (mode & mask);

	return 1;
}

static void
setopt(Mkaux *mkaux, char *key, char *val)
{
	Opt *o;

	o = mkaux->opt;
	if(o == nil || mkaux->indent > o->level){
		o = emalloc(mkaux, sizeof(*o));
		if(mkaux->opt != nil)
			*o = *mkaux->opt;
		o->level = mkaux->indent;
		o->prev = mkaux->opt;
		mkaux->opt = o;
	} else if(mkaux->indent < o->level)
		return;
	if(strcmp(key, "skip") == 0){
		freeoptptr(o, &o->skip);
		if((o->skip = regcomp(val)) == nil)
			warn(mkaux, "bad regular expression %s", val);
	} else if(strcmp(key, "uid") == 0){
		freeoptptr(o, &o->uid);
		o->uid = *val ? estrdup(mkaux, val) : nil;
	} else if(strcmp(key, "gid") == 0){
		freeoptptr(o, &o->gid);
		o->gid = *val ? estrdup(mkaux, val) : nil;
	} else if(strcmp(key, "mode") == 0){
		if(!parsemode(val, &o->mask, &o->mode))
			warn(mkaux, "bad mode specification %s", val);
	} else {
		warn(mkaux, "bad option %s=%s", key, val);
	}
}

static void
popopt(Mkaux *mkaux)
{
	Opt *o;

	while((o = mkaux->opt) != nil){
		if(o->level <= mkaux->indent)
			break;
		mkaux->opt = o->prev;
		freeoptptr(o, &o->skip);
		freeoptptr(o, &o->uid);
		freeoptptr(o, &o->gid);
		free(o);
	}
}

static void
freeoptptr(Opt *o, void *p)
{
	int x = (void**)p - (void**)o;
	void *v = ((void**)o)[x];
	if(v == nil)
		return;
	((void**)o)[x] = nil;
	if((o = o->prev) != nil)
		if(((void**)o)[x] == v)
			return;
	free(v);
}


static void
freefile(File *f)
{
	free(f->old);
	free(f->new);
	free(f);
}

/*
 * skip all files in the proto that
 * could be in the current dir
 */
static void
skipdir(Mkaux *mkaux)
{
	int level;

	level = mkaux->indent;
	while(getline(mkaux) != nil){
		if(mkaux->indent <= level){
			popopt(mkaux);
			Bseek(mkaux->b, -Blinelen(mkaux->b), 1);
			mkaux->lineno--;
			return;
		}
	}
}

static char*
getline(Mkaux *mkaux)
{
	char *p;
	int c;

	if(mkaux->indent < 0)
		return nil;
loop:
	mkaux->indent = 0;
	p = Brdline(mkaux->b, '\n');
	mkaux->lineno++;
	if(p == nil){
		mkaux->indent = -1;
		return nil;
	}
	if(memchr(p, 0, Blinelen(mkaux->b)) != nil){
		warn(mkaux, "null bytes in proto");
		longjmp(mkaux->jmp, 1);
	}
	while((c = *p++) != '\n')
		if(c == ' ')
			mkaux->indent++;
		else if(c == '\t')
			mkaux->indent += 8;
		else
			break;
	if(c == '\n' || c == '#')
		goto loop;
	return --p;
}

static File*
getfile(Mkaux *mkaux, File *old)
{
	File *f;
	char *elem;
	char *p, *s;

loop:
	if((p = getline(mkaux)) == nil)
		return nil;
	popopt(mkaux);

	*strchr(p, '\n') = 0;
	if((s = strchr(p, '=')) != nil){
		*s++ = 0;
		setopt(mkaux, p, s);
		goto loop;
	}else
		p[strlen(p)] = '\n';

	if((p = getname(mkaux, p, &elem)) == nil)
		return nil;

	f = emalloc(mkaux, sizeof *f);
	f->new = mkpath(mkaux, old->new, elem);
	if((s = strrchr(f->new, '/')) != nil)
		f->elem = s+1;
	else
		f->elem = f->new;
	free(elem);

	if((p = getmode(mkaux, p, &f->mode)) == nil){
		freefile(f);
		return nil;
	}

	if((p = getname(mkaux, p, &f->uid)) == nil){
		freefile(f);
		return nil;
	}
	if(*f->uid == 0)
		strcpy(f->uid, "-");

	if((p = getname(mkaux, p, &f->gid)) == nil){
		freefile(f);
		return nil;
	}
	if(*f->gid == 0)
		strcpy(f->gid, "-");

	f->old = getpath(mkaux, p);
	if(f->old != nil && strcmp(f->old, "-") == 0){
		free(f->old);
		f->old = nil;
	}

	setname(mkaux, &mkaux->oldfile, f);

	return f;
}

static char*
getpath(Mkaux *mkaux, char *p)
{
	char *q, *new;
	int c, n;

	while((c = *p) == ' ' || c == '\t')
		p++;
	q = p;
	while((c = *q) != '\n' && c != ' ' && c != '\t')
		q++;
	if(q == p)
		return nil;
	n = q - p;
	new = emalloc(mkaux, n + 1);
	memcpy(new, p, n);
	new[n] = 0;
	return new;
}

static char*
getname(Mkaux *mkaux, char *p, char **buf)
{
	char *s, *start;
	int c;

	while((c = *p) == ' ' || c == '\t')
		p++;

	start = p;
	while((c = *p) != '\n' && c != ' ' && c != '\t')
		p++;

	*buf = malloc(p+2-start);	/* +2: need at least 2 bytes; might strcpy "-" into buf */
	if(*buf == nil)
		return nil;
	memmove(*buf, start, p-start);

	(*buf)[p-start] = 0;

	if(**buf == '$'){
		s = getenv(*buf+1);
		if(s == nil){
			warn(mkaux, "can't read environment variable %s", *buf+1);
			free(*buf);
			skipdir(mkaux);
			return nil;
		}
		free(*buf);
		*buf = s;
	}
	return p;
}

static char*
getmode(Mkaux *mkaux, char *p, ulong *xmode)
{
	char *buf, *s;
	ulong m;

	*xmode = ~0;
	if((p = getname(mkaux, p, &buf)) == nil)
		return nil;

	s = buf;
	if(*s == 0 || strcmp(s, "-") == 0)
		return p;
	m = 0;
	if(*s == 'd'){
		m |= DMDIR;
		s++;
	}
	if(*s == 'a'){
		m |= DMAPPEND;
		s++;
	}
	if(*s == 'l'){
		m |= DMEXCL;
		s++;
	}
	if(s[0] < '0' || s[0] > '7'
	|| s[1] < '0' || s[1] > '7'
	|| s[2] < '0' || s[2] > '7'
	|| s[3]){
		warn(mkaux, "bad mode specification %s", buf);
		free(buf);
		return p;
	}
	*xmode = m | strtoul(s, 0, 8);
	free(buf);
	return p;
}

static void
warn(Mkaux *mkaux, char *fmt, ...)
{
	char buf[256];
	va_list va;

	va_start(va, fmt);
	vseprint(buf, buf+sizeof(buf), fmt, va);
	va_end(va);

	if(mkaux->warn != nil)
		mkaux->warn(buf, mkaux->a);
	else
		fprint(2, "warning: %s\n", buf);
}

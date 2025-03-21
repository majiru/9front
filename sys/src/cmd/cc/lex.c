#include	"cc.h"
#include	"y.tab.h"

#ifndef	CPP
#define	CPP	"/bin/cpp"
#endif

/*
 * known debug flags
 * flags without a description may be exclusive to certain architectures
 *	-.		Inhibit search for includes in source directory
 *	-0		R0ISZERO - qc
 *	-4
 *	-8
 *	-<

 *	-A		!B
 *	-B		non ANSI
 *	-C
 *	-D name		define
 *	-F		format specification check
 *	-G
 *	-H
 *	-I path		include
 *	-L		print every NAME symbol
 *	-M		constant multiplication
 *	-N
 *	-P		peephole
 *	-Q
 *	-R		print registerization
 *	-S		print assembly
 *	-T
 *	-V		enable void* conversion warnings
 *	-W
 *	-X		abort on error
 *	-Y
 *	-Z		pickle

 *	-a		acid declaration output
 *	-c		print const if
 *	-d		print declarations
 *	-e
 *	-f
 *	-g
 *	-h
 *	-i		print initialization
 *	-l		generate little-endian code
 *	-m		print add/sub/mul trees
 *	-n		print acid to file (%.c=%.acid) (with -a or -aa)
 *	-o file		output file
 *	-p		use standard cpp ANSI preprocessor (not on windows)
 *	-s		print structure offsets (with -a or -aa)
 *	-t		print type trees
 *	-v		verbose printing
 *	-w		print warnings
 *	-x
 *	-y
 */

void
main(int argc, char *argv[])
{
	char **defs, *p;
	int nproc, nout, status, i, c, ndef;

	memset(debug, 0, sizeof(debug));
	tinit();
	cinit();
	ginit();
	arginit();

	profileflg = 1;	/* #pragma can turn it off */
	tufield = simplet((1L<<tfield->etype) | BUNSIGNED);
	defs = 0;
	ndef = 0;
	outfile = 0;
	include[ninclude++] = ".";
	ARGBEGIN {
	default:
		c = ARGC();
		if(c >= 0 && c < sizeof(debug))
			debug[c]++;
		break;

	case 'l':			/* for little-endian mips */
		if(thechar != 'v'){
			print("can only use -l with vc");
			errorexit();
		}
		thechar = '0';
		thestring = "spim";
		break;

	case 'o':
		outfile = ARGF();
		break;

	case 'D':
		p = ARGF();
		if(p) {
			if((ndef & 15) == 0)
				defs = allocn(defs, ndef*sizeof(defs[0]), 16*sizeof(defs[0]));
			defs[ndef++] = p;
			dodefine(p);
		}
		break;

	case 'I':
		p = ARGF();
		if(p)
			setinclude(p);
		break;
	} ARGEND
	if(argc < 1 && outfile == 0) {
		print("usage: %Cc [-options] files\n", thechar);
		errorexit();
	}
	if(argc > 1 && systemtype(Windows)){
		print("can't compile multiple files on windows\n");
		errorexit();
	}
	if(argc > 1 && !systemtype(Windows)) {
		nproc = 1;
		/*
		 * if we're writing acid to standard output, don't compile
		 * concurrently, to avoid interleaving output.
		 */
		if(((!debug['a'] && !debug['Z']) || debug['n']) &&
		    (p = getenv("NPROC")) != nil)
			nproc = atol(p);	/* */
		c = 0;
		nout = 0;
		for(;;) {
			while(nout < nproc && argc > 0) {
				i = myfork();
				if(i < 0) {
					i = mywait(&status);
					if(i < 0) {
						print("cannot create a process\n");
						errorexit();
					}
					if(status)
						c++;
					nout--;
					continue;
				}
				if(i == 0) {
					fprint(2, "%s:\n", *argv);
					if (compile(*argv, defs, ndef))
						errorexit();
					exits(0);
				}
				nout++;
				argc--;
				argv++;
			}
			i = mywait(&status);
			if(i < 0) {
				if(c)
					errorexit();
				exits(0);
			}
			if(status)
				c++;
			nout--;
		}
	}

	if(argc == 0)
		c = compile("stdin", defs, ndef);
	else
		c = compile(argv[0], defs, ndef);

	if(c)
		errorexit();
	exits(0);
}

int
compile(char *file, char **defs, int ndef)
{
	char *ofile, *p, **av;
	int i, c, fd[2], status;
	static int first = 1;

	ofile = strdup(file);
	p = utfrrune(ofile, pathchar());
	if(p) {
		*p++ = 0;
		if(!debug['.'])
			include[0] = strdup(ofile);
	} else
		p = ofile;

	if(outfile == 0) {
		if(p) {
			outfile = p;
			if(p = utfrrune(outfile, '.'))
				if(p[1] == 'c' && p[2] == 0)
					p[0] = 0;
			if(debug['a'] && debug['n'])
				outfile = smprint("%s.acid", outfile);
			else if(debug['Z'] && debug['n'])
				outfile = smprint("%s_pickle.c", outfile);
			else
				outfile = smprint("%s.%C", outfile, thechar);
		} else
			outfile = "/dev/null";
	}

	if(p = getenv("INCLUDE")) {
		setinclude(p);
	} else {
		if(systemtype(Plan9)) {
			setinclude(smprint("/%s/include", thestring));
			setinclude("/sys/include");
		}
	}
	if (first)
		Binit(&diagbuf, 1, OWRITE);
	/*
	 * if we're writing acid to standard output, don't keep scratching
	 * outbuf.
	 */
	if((debug['a'] || debug['Z']) && !debug['n']) {
		if (first) {
			outfile = 0;
			Binit(&outbuf, dup(1, -1), OWRITE);
			dup(2, 1);
		}
	} else {
		c = mycreat(outfile, 0664 | DMTMP);
		if(c < 0) {
			diag(Z, "cannot open %s - %r", outfile);
			outfile = 0;
			errorexit();
		}
		Binit(&outbuf, c, OWRITE);
	}
	newio();
	first = 0;

	/* Use an ANSI preprocessor */
	if(debug['p']) {
		if(systemtype(Windows)) {
			diag(Z, "-p option not supported on windows");
			errorexit();
		}
		if(myaccess(file) < 0) {
			diag(Z, "%s does not exist", file);
			errorexit();
		}
		if(mypipe(fd) < 0) {
			diag(Z, "pipe failed");
			errorexit();
		}
		switch(myfork()) {
		case -1:
			diag(Z, "fork failed");
			errorexit();
		case 0:
			close(fd[0]);
			mydup(fd[1], 1);
			close(fd[1]);

			i = 8+ndef+ninclude;
			av = alloc(i*sizeof(av[0]));

			i = 0;
			av[i++] = CPP;
			av[i++] = "-+";
			if(debug['.'])
				av[i++] = "-.";
			for(c = 0; c < ndef; c++)
				av[i++] = smprint("-D%s", defs[c]);
			for(c = 0; c < ninclude; c++)
				av[i++] = smprint("-I%s", include[c]);
			if(strcmp(file, "stdin") != 0)
				av[i++] = file;
			av[i] = 0;
			if(debug['p'] > 1) {
				for(c = 0; c < i; c++)
					fprint(2, "%s ", av[c]);
				fprint(2, "\n");
			}
			myexec(av[0], av);
			fprint(2, "can't exec C preprocessor %s: %r\n", CPP);
			errorexit();
		default:
			close(fd[1]);
			newfile(file, fd[0]);
			break;
		}
	} else {
		if(strcmp(file, "stdin") == 0)
			newfile(file, 0);
		else
			newfile(file, -1);
	}
	yyparse();
	if(!debug['a'] && !debug['Z'])
		gclean();
	if(nerrors == 0 && mywait(&status) > 0 && status != 0)
		nerrors++;
	return nerrors;
}

void
errorexit(void)
{
	if(outfile)
		remove(outfile);
	exits("error");
}

void
pushio(void)
{
	Io *i;

	i = iostack;
	if(i == I) {
		yyerror("botch in pushio");
		errorexit();
	}
	i->p = fi.p;
	i->c = fi.c;
}

void
newio(void)
{
	Io *i;
	static int pushdepth = 0;

	i = iofree;
	if(i == I) {
		pushdepth++;
		if(pushdepth > 1000) {
			yyerror("macro/io expansion too deep");
			errorexit();
		}
		i = alloc(sizeof(*i));
	} else
		iofree = i->link;
	i->c = 0;
	i->f = -1;
	ionext = i;
}

void
newfile(char *s, int f)
{
	Io *i;

	if(debug['e'])
		print("%L: %s\n", lineno, s);

	i = ionext;
	i->link = iostack;
	iostack = i;
	i->f = f;
	if(f < 0)
		i->f = open(s, 0);
	if(i->f < 0) {
		yyerror("%Cc: %r: %s", thechar, s);
		errorexit();
	}
	fi.c = 0;
	linehist(s, 0);
}

Sym*
slookup(char *s)
{
	strncpy(symb, s, NSYMB);
	if(symb[NSYMB-1] != '\0'){
		yyerror("symbol too long: %s", s);
		errorexit();
	}
	return lookup();
}

Sym*
lookup(void)
{
	Sym *s;
	ulong h;
	char *p;
	int c, n;

	h = 0;
	for(p=symb; *p;) {
		h = h * 3;
		h += *p++;
	}
	n = (p - symb) + 1;
	if((long)h < 0)
		h = ~h;
	h %= NHASH;
	c = symb[0];
	for(s = hash[h]; s != S; s = s->link) {
		if(s->name[0] != c)
			continue;
		if(strcmp(s->name, symb) == 0)
			return s;
	}
	s = alloc(sizeof(*s));
	s->name = alloc(n);
	memmove(s->name, symb, n);

	s->link = hash[h];
	hash[h] = s;
	syminit(s);

	return s;
}

void
syminit(Sym *s)
{
	s->lexical = LNAME;
	s->block = 0;
	s->offset = 0;
	s->type = T;
	s->suetag = T;
	s->class = CXXX;
	s->aused = 0;
	s->sig = SIGNONE;
}

#define	EOF	(-1)
#define	IGN	(-2)
#define	ESC	(1<<20)

enum
{
	Numdec		= 1<<0,
	Numlong		= 1<<1,
	Numuns		= 1<<2,
	Numvlong	= 1<<3,
	Numflt		= 1<<4,
};

long
yylex(void)
{
	vlong vv;
	long c, c1, t, w;
	char *cp;
	Rune rune;
	Sym *s;

	if(peekc != IGN) {
		c = peekc;
		peekc = IGN;
		goto l1;
	}
l0:
	c = GETC();

l1:
	if(c >= Runeself) {
		/*
		 * extension --
		 *	all multibyte runes are alpha
		 */
		cp = symb;
		goto talph;
	}
	if(isspace(c)) {
		if(c == '\n')
			lineno++;
		goto l0;
	}
	if(isalpha(c)) {
		cp = symb;
		if(c != 'L')
			goto talph;
		*cp++ = c;
		c = GETC();
		if(c == '\'') {
			/* L'x' */
			c = escchar('\'', 1, 0);
			if(c == EOF)
				c = '\'';
			c1 = escchar('\'', 1, 0);
			if(c1 != EOF) {
				yyerror("missing '");
				peekc = c1;
			}
			yylval.vval = convvtox(c, TRUNE);
			return LUCONST;
		}
		if(c == '"') {
			goto caselq;
		}
		goto talph;
	}
	if(isdigit(c))
		goto tnum;
	switch(c)
	{

	case EOF:
		peekc = EOF;
		return -1;

	case '_':
		cp = symb;
		goto talph;

	case '#':
		domacro();
		goto l0;

	case '.':
		c1 = GETC();
		if(c1 == '.'){
			c = GETC();
			if(c == '.')
				return LDOTDOTDOT;
			else
				yyerror(".. instead of ...");
				
		} else if(isdigit(c1)) {
			cp = symb;
			*cp++ = c;
			c = c1;
			c1 = 0;
			goto casedot;
		}
		break;

	case '"':
		strcpy(symb, "\"<string>\"");
		cp = alloc(0);
		c1 = 0;

		/* "..." */
		for(;;) {
			c = escchar('"', 0, 1);
			if(c == EOF)
				break;
			if(c & ESC) {
				cp = allocn(cp, c1, 1);
				cp[c1++] = c;
			} else {
				rune = c;
				c = runelen(rune);
				cp = allocn(cp, c1, c);
				runetochar(cp+c1, &rune);
				c1 += c;
			}
		}
		yylval.sval.l = c1;
		do {
			cp = allocn(cp, c1, 1);
			cp[c1++] = 0;
		} while(c1 & MAXALIGN);
		yylval.sval.s = cp;
		return LSTRING;

	caselq:
		/* L"..." */
		strcpy(symb, "\"L<string>\"");
		cp = alloc(0);
		c1 = 0;
		for(;;) {
			c = escchar('"', 1, 0);
			if(c == EOF)
				break;
			cp = allocn(cp, c1, sizeof(Rune));
			*(Rune*)(cp + c1) = c;
			c1 += sizeof(Rune);
		}
		yylval.sval.l = c1;
		do {
			cp = allocn(cp, c1, sizeof(Rune));
			*(Rune*)(cp + c1) = 0;
			c1 += sizeof(Rune);
		} while(c1 & MAXALIGN);
		yylval.sval.s = cp;
		return LLSTRING;

	case '\'':
		/* '.' */
		c = escchar('\'', 0, 0);
		if(c == EOF)
			c = '\'';
		c1 = escchar('\'', 0, 0);
		if(c1 != EOF) {
			yyerror("missing '");
			peekc = c1;
		}
		vv = c;
		yylval.vval = convvtox(vv, TUCHAR);
		if(yylval.vval != vv)
			yyerror("overflow in character constant: 0x%lx", c);
		else
		if(c & 0x80){
			nearln = lineno;
			warn(Z, "sign-extended character constant");
		}
		yylval.vval = convvtox(vv, TCHAR);
		return LCONST;

	case '/':
		c1 = GETC();
		if(c1 == '*') {
			for(;;) {
				c = getr();
				while(c == '*') {
					c = getr();
					if(c == '/')
						goto l0;
				}
				if(c == EOF) {
					yyerror("eof in comment");
					errorexit();
				}
			}
		}
		if(c1 == '/') {
			for(;;) {
				c = getr();
				if(c == '\n')
					goto l0;
				if(c == EOF) {
					yyerror("eof in comment");
					errorexit();
				}
			}
		}
		if(c1 == '=')
			return LDVE;
		break;

	case '*':
		c1 = GETC();
		if(c1 == '=')
			return LMLE;
		break;

	case '%':
		c1 = GETC();
		if(c1 == '=')
			return LMDE;
		break;

	case '+':
		c1 = GETC();
		if(c1 == '+')
			return LPP;
		if(c1 == '=')
			return LPE;
		break;

	case '-':
		c1 = GETC();
		if(c1 == '-')
			return LMM;
		if(c1 == '=')
			return LME;
		if(c1 == '>')
			return LMG;
		break;

	case '>':
		c1 = GETC();
		if(c1 == '>') {
			c = LRSH;
			c1 = GETC();
			if(c1 == '=')
				return LRSHE;
			break;
		}
		if(c1 == '=')
			return LGE;
		break;

	case '<':
		c1 = GETC();
		if(c1 == '<') {
			c = LLSH;
			c1 = GETC();
			if(c1 == '=')
				return LLSHE;
			break;
		}
		if(c1 == '=')
			return LLE;
		break;

	case '=':
		c1 = GETC();
		if(c1 == '=')
			return LEQ;
		break;

	case '!':
		c1 = GETC();
		if(c1 == '=')
			return LNE;
		break;

	case '&':
		c1 = GETC();
		if(c1 == '&')
			return LANDAND;
		if(c1 == '=')
			return LANDE;
		break;

	case '|':
		c1 = GETC();
		if(c1 == '|')
			return LOROR;
		if(c1 == '=')
			return LORE;
		break;

	case '^':
		c1 = GETC();
		if(c1 == '=')
			return LXORE;
		break;

	default:
		return c;
	}
	peekc = c1;
	return c;

talph:
	/*
	 * cp is set to symb and some
	 * prefix has been stored
	 */
	for(;;) {
		if(cp >= &symb[NSYMB-UTFmax-1])
			goto toolong;
		if(c >= Runeself) {
			for(c1=0;;) {
				cp[c1++] = c;
				if(fullrune(cp, c1))
					break;
				c = GETC();
			}
			cp += c1;
			c = GETC();
			continue;
		}
		if(!isalnum(c) && c != '_')
			break;
		*cp++ = c;
		c = GETC();
	}
	*cp = 0;
	if(debug['L'])
		print("%L: %s\n", lineno, symb);
	peekc = c;
	s = lookup();
	if(s->macro) {
		newio();
		cp = ionext->b;
		if(macexpand(s, cp, sizeof(ionext->b)-1)){
			pushio();
			ionext->link = iostack;
			iostack = ionext;
			fi.p = cp;
			fi.c = strlen(cp);
			if(peekc != IGN) {
				cp[fi.c++] = peekc;
				cp[fi.c] = 0;
				peekc = IGN;
			}
			goto l0;
		} else {
			ionext->link = iofree;
			iofree = ionext;
		}
	}
	yylval.sym = s;
	if(s->class == CTYPEDEF || s->class == CTYPESTR)
		return LTYPE;
	return s->lexical;

tnum:
	c1 = 0;
	cp = symb;
	vv = 0;
	if(c != '0') {
		c1 |= Numdec;
		for(;;) {
			if(!isdigit(c))
				goto dc;

			vv = vv*10 + c-'0';
			if(cp >= &symb[NSYMB-1])
				goto toolong;
			*cp++ = c;
			c = GETC();
		}
	}
	*cp++ = c;
	c = GETC();
	if(c == 'x' || c == 'X')
		for(;;) {
			if(cp >= &symb[NSYMB-1])
				goto toolong;
			*cp++ = c;
			c = GETC();
			if(isdigit(c)){
				vv = vv*16 + c-'0';
				continue;
			}
			if(c >= 'a' && c <= 'f'){
				vv = vv*16 + c + 10 - 'a';
				continue;
			}
			if(c >= 'A' && c <= 'F'){
				vv = vv*16 + c + 10 - 'A';
				continue;
			}
			if(cp == symb+2)
				yyerror("malformed hex constant");
			goto ncu;
		}
	if(c == 'b' || c == 'B')
		for(;;) {
			if(cp >= &symb[NSYMB-1])
				goto toolong;
			*cp++ = c;
			c = GETC();
			if(c == '0' || c == '1'){
				vv = vv*2 + c-'0';
				continue;
			}
			if(cp == symb+2)
				yyerror("malformed binary constant");
			goto ncu;
		}
	if(c < '0' || c > '7')
		goto dc;
	for(;;) {
		if(c < '0' || c > '7')
			goto ncu;

		vv = vv*8 + c-'0';
		if(cp >= &symb[NSYMB-1])
			goto toolong;
		*cp++ = c;
		c = GETC();
	}

dc:
	if(c == '.')
		goto casedot;
	if(c == 'e' || c == 'E')
		goto casee;

ncu:
	if((c == 'U' || c == 'u') && !(c1 & Numuns)) {
		c = GETC();
		c1 |= Numuns;
		goto ncu;
	}
	if((c == 'L' || c == 'l') && !(c1 & Numvlong)) {
		c = GETC();
		if(c1 & Numlong)
			c1 |= Numvlong;
		c1 |= Numlong;
		goto ncu;
	}
	*cp = 0;
	peekc = c;

	/*
	 * c99 is silly: decimal constants stay signed,
	 * hex and octal go unsigned before widening.
	 */
	w = 32;
	if((c1 & (Numdec|Numuns)) == Numdec)
		w = 31;
	if(c1 & Numvlong || (c1 & Numlong) == 0 && (uvlong)vv >= 1ULL<<w){
		if((c1&(Numdec|Numvlong)) == Numdec && vv < 1ULL<<32)
			warn(Z, "int constant widened to vlong: %s", symb);
		if((c1 & Numuns) || convvtox(vv, TVLONG) < 0) {
			c = LUVLCONST;
			t = TUVLONG;
			goto nret;
		}
		c = LVLCONST;
		t = TVLONG;
		goto nret;
	}
	if(c1 & Numlong) {
		if((c1 & Numuns) || convvtox(vv, TLONG) < 0) {
			c = LULCONST;
			t = TULONG;
			goto nret;
		}
		c = LLCONST;
		t = TLONG;
		goto nret;
	}
	if((c1 & Numuns) || convvtox(vv, TINT) < 0) {
		c = LUCONST;
		t = TUINT;
		goto nret;
	}
	c = LCONST;
	t = TINT;
	goto nret;

nret:
	yylval.vval = convvtox(vv, t);
	if(yylval.vval != vv){
		nearln = lineno;
		warn(Z, "truncated constant: %T %s", types[t], symb);
	}
	return c;

casedot:
	for(;;) {
		if(cp >= &symb[NSYMB-1])
			goto toolong;
		*cp++ = c;
		c = GETC();
		if(!isdigit(c))
			break;
	}
	if(c != 'e' && c != 'E')
		goto caseout;

casee:
	if(cp >= &symb[NSYMB-2])
		goto toolong;
	*cp++ = 'e';
	c = GETC();
	if(c == '+' || c == '-') {
		*cp++ = c;
		c = GETC();
	}
	if(!isdigit(c))
		yyerror("malformed fp constant exponent");
	while(isdigit(c)) {
		if(cp >= &symb[NSYMB-1])
			goto toolong;
		*cp++ = c;
		c = GETC();
	}

caseout:
	if(c == 'L' || c == 'l') {
		c = GETC();
		c1 |= Numlong;
	} else
	if(c == 'F' || c == 'f') {
		c = GETC();
		c1 |= Numflt;
	}
	*cp = 0;
	peekc = c;
	yylval.dval = strtod(symb, nil);
	if(isInf(yylval.dval, 1) || isInf(yylval.dval, -1)) {
		yyerror("overflow in float constant");
		yylval.dval = 0;
	}
	if(c1 & Numflt)
		return LFCONST;
	return LDCONST;

toolong:
	yyerror("token too long: %.*s...", utfnlen(symb, cp-symb), symb);
	errorexit();
	return -1;
}

int
getc(void)
{
	int c;

	if(peekc != IGN) {
		c = peekc;
		peekc = IGN;
	} else
		c = GETC();
	if(c == '\n')
		lineno++;
	if(c == EOF) {
		yyerror("End of file");
		errorexit();
	}
	return c;
}

long
getr(void)
{
	int c, i;
	char str[UTFmax+1];
	Rune rune;


	c = getc();
	if(c < Runeself)
		return c;
	i = 0;
	str[i++] = c;

loop:
	c = getc();
	str[i++] = c;
	if(!fullrune(str, i))
		goto loop;
	c = chartorune(&rune, str);
	if(rune == Runeerror && c == 1) {
		nearln = lineno;
		diag(Z, "illegal rune in string");
		for(c=0; c<i; c++)
			print(" %.2x", *(uchar*)(str+c));
		print("\n");
	}
	return rune;
}

int
getnsc(void)
{
	int c;

	if(peekc != IGN) {
		c = peekc;
		peekc = IGN;
	} else
		c = GETC();
	for(;;) {
		if(c >= Runeself || !isspace(c))
			return c;
		if(c == '\n') {
			lineno++;
			return c;
		}
		c = GETC();
	}
}

void
unget(int c)
{

	peekc = c;
	if(c == '\n')
		lineno--;
}

long
escchar(long e, int longflg, int escflg)
{
	long c, l;
	int i;

loop:
	c = getr();
	if(c == '\n') {
		yyerror("newline in string");
		return EOF;
	}
	if(c != '\\') {
		if(c == e)
			c = EOF;
		return c;
	}
	c = getr();
	if(c == 'x') {
		/*
		 * note this is not ansi,
		 * supposed to only accept 2 hex
		 */
		i = 2;
		if(longflg)
			i = 6;
		l = 0;
		for(; i>0; i--) {
			c = getc();
			if(c >= '0' && c <= '9') {
				l = l*16 + c-'0';
				continue;
			}
			if(c >= 'a' && c <= 'f') {
				l = l*16 + c-'a' + 10;
				continue;
			}
			if(c >= 'A' && c <= 'F') {
				l = l*16 + c-'A' + 10;
				continue;
			}
			unget(c);
			break;
		}
		if(escflg)
			l |= ESC;
		return l;
	}
	if(c >= '0' && c <= '7') {
		/*
		 * note this is not ansi,
		 * supposed to only accept 3 oct
		 */
		i = 2;
		if(longflg)
			i = 6;
		l = c - '0';
		for(; i>0; i--) {
			c = getc();
			if(c >= '0' && c <= '7') {
				l = l*8 + c-'0';
				continue;
			}
			unget(c);
		}
		if(escflg)
			l |= ESC;
		return l;
	}
	switch(c)
	{
	case '\n':	goto loop;
	case 'n':	return '\n';
	case 't':	return '\t';
	case 'b':	return '\b';
	case 'r':	return '\r';
	case 'f':	return '\f';
	case 'a':	return '\a';
	case 'v':	return '\v';
	}
	return c;
}

struct
{
	char	*name;
	ushort	lexical;
	ushort	type;
} itab[] =
{
	"auto",		LAUTO,		0,
	"break",	LBREAK,		0,
	"case",		LCASE,		0,
	"char",		LCHAR,		TCHAR,
	"const",	LCONSTNT,	0,
	"continue",	LCONTINUE,	0,
	"default",	LDEFAULT,	0,
	"do",		LDO,		0,
	"double",	LDOUBLE,	TDOUBLE,
	"else",		LELSE,		0,
	"enum",		LENUM,		0,
	"extern",	LEXTERN,	0,
	"float",	LFLOAT,		TFLOAT,
	"for",		LFOR,		0,
	"goto",		LGOTO,		0,
	"if",		LIF,		0,
	"inline",	LINLINE,	0,
	"int",		LINT,		TINT,
	"long",		LLONG,		TLONG,
	"_Noreturn",	LNORET,		0,
	"register",	LREGISTER,	0,
	"restrict",	LRESTRICT,	0,
	"return",	LRETURN,	0,
	"SET",		LSET,		0,
	"short",	LSHORT,		TSHORT,
	"signed",	LSIGNED,	0,
	"signof",	LSIGNOF,	0,
	"sizeof",	LSIZEOF,	0,
	"static",	LSTATIC,	0,
	"struct",	LSTRUCT,	0,
	"switch",	LSWITCH,	0,
	"typedef",	LTYPEDEF,	0,
	"typestr",	LTYPESTR,	0,
	"union",	LUNION,		0,
	"unsigned",	LUNSIGNED,	0,
	"USED",		LUSED,		0,
	"void",		LVOID,		TVOID,
	"volatile",	LVOLATILE,	0,
	"while",	LWHILE,		0,
	0
};

void
cinit(void)
{
	Sym *s;
	int i;
	Type *t;

	nerrors = 0;
	lineno = 1;
	iostack = I;
	iofree = I;
	peekc = IGN;

	types[TXXX] = T;
	types[TCHAR] = typ(TCHAR, T);
	types[TUCHAR] = typ(TUCHAR, T);
	types[TSHORT] = typ(TSHORT, T);
	types[TUSHORT] = typ(TUSHORT, T);
	types[TINT] = typ(TINT, T);
	types[TUINT] = typ(TUINT, T);
	types[TLONG] = typ(TLONG, T);
	types[TULONG] = typ(TULONG, T);
	types[TVLONG] = typ(TVLONG, T);
	types[TUVLONG] = typ(TUVLONG, T);
	types[TFLOAT] = typ(TFLOAT, T);
	types[TDOUBLE] = typ(TDOUBLE, T);
	types[TVOID] = typ(TVOID, T);
	types[TENUM] = typ(TENUM, T);
	types[TFUNC] = typ(TFUNC, types[TINT]);
	types[TIND] = typ(TIND, types[TVOID]);

	for(i=0; i<NHASH; i++)
		hash[i] = S;
	for(i=0; itab[i].name; i++) {
		s = slookup(itab[i].name);
		s->lexical = itab[i].lexical;
		if(itab[i].type != 0)
			s->type = types[itab[i].type];
	}
	blockno = 0;
	autobn = 0;
	autoffset = 0;

	t = typ(TARRAY, types[TCHAR]);
	t->width = 0;
	symstring = slookup(".string");
	symstring->class = CSTATIC;
	symstring->type = t;

	t = typ(TARRAY, types[TCHAR]);
	t->width = 0;

	nodproto = new(OPROTO, Z, Z);
	dclstack = D;

	pathname = allocn(pathname, 0, 100);
	if(mygetwd(pathname, 99) == 0) {
		pathname = allocn(pathname, 100, 900);
		if(mygetwd(pathname, 999) == 0)
			strcpy(pathname, "/???");
	}

	fmtinstall('O', Oconv);
	fmtinstall('T', Tconv);
	fmtinstall('F', FNconv);
	fmtinstall('L', Lconv);
	fmtinstall('Q', Qconv);
	fmtinstall('|', VBconv);
}

int
filbuf(void)
{
	Io *i;

loop:
	i = iostack;
	if(i == I)
		return EOF;
	if(i->f < 0)
		goto pop;
	fi.c = read(i->f, i->b, BUFSIZ) - 1;
	if(fi.c < 0) {
		close(i->f);
		linehist(0, 0);
		goto pop;
	}
	fi.p = i->b + 1;
	return i->b[0] & 0xff;

pop:
	iostack = i->link;
	i->link = iofree;
	iofree = i;
	i = iostack;
	if(i == I)
		return EOF;
	fi.p = i->p;
	fi.c = i->c;
	if(--fi.c < 0)
		goto loop;
	return *fi.p++ & 0xff;
}

int
Oconv(Fmt *fp)
{
	int a;

	a = va_arg(fp->args, int);
	if(a < OXXX || a > OEND)
		return fmtprint(fp, "***badO %d***", a);

	return fmtstrcpy(fp, onames[a]);
}

int
Lconv(Fmt *fp)
{
	Hist *h;
	struct
	{
		Hist*	incl;	/* start of this include file */
		long	idel;	/* delta line number to apply to include */
		Hist*	line;	/* start of this #line directive */
		long	ldel;	/* delta line number to apply to #line */
	} a[HISTSZ];
	long l, d;
	int i, n;

	l = va_arg(fp->args, long);
	n = 0;
	for(h = hist; h != H; h = h->link) {
		if(l < h->line)
			break;
		if(h->name) {
			if(h->offset != 0) {		/* #line directive, not #pragma */
				if(n > 0 && n < HISTSZ && h->offset >= 0) {
					a[n-1].line = h;
					a[n-1].ldel = h->line - h->offset + 1;
				}
			} else {
				if(n < HISTSZ) {	/* beginning of file */
					a[n].incl = h;
					a[n].idel = h->line;
					a[n].line = 0;
				}
				n++;
			}
			continue;
		}
		n--;
		if(n > 0 && n < HISTSZ) {
			d = h->line - a[n].incl->line;
			a[n-1].ldel += d;
			a[n-1].idel += d;
		}
	}
	if(n > HISTSZ)
		n = HISTSZ;
	if(n == 0)
		return fmtprint(fp, "<eof>");
	for(i=n-1; i>=0; i--) {
		if(i != n-1) {
			if(fp->flags & ~(FmtWidth|FmtPrec))	/* BUG ROB - was f3 */
				break;
			fmtrune(fp, ' ');
		}
		if(a[i].line)
			fmtprint(fp, "%s:%ld[%s:%ld]",
				a[i].line->name, l-a[i].ldel+1,
				a[i].incl->name, l-a[i].idel+1);
		else
			fmtprint(fp, "%s:%ld",
				a[i].incl->name, l-a[i].idel+1);
		l = a[i].incl->line - 1;	/* now print out start of this file */
	}
	return 0;
}

int
Tconv(Fmt *fp)
{
	Type *t, *t1;
	int et;
	long n;

	t = va_arg(fp->args, Type*);
	while(t != T) {
		if(t->garb&~GINCOMPLETE)
			fmtprint(fp, "%s ", gnames[t->garb&~GINCOMPLETE]);
		et = t->etype;
		fmtprint(fp, "%s", tnames[et]);
		if(et == TFUNC && (t1 = t->down) != T) {
			fmtprint(fp, "(%T", t1);
			while((t1 = t1->down) != T)
				fmtprint(fp, ", %T", t1);
			fmtprint(fp, ")");
		}
		if(et == TARRAY) {
			n = t->width;
			if(t->link != T && t->link->width)
				n /= t->link->width;
			fmtprint(fp, "[%ld]", n);
		}
		if(t->nbits)
			fmtprint(fp, " %d:%d", t->shift, t->nbits);
		if(typesu[et]) {
			fmtprint(fp, " %s", t->tag? t->tag->name: "{}");
			break;
		}
		if((t = t->link) == T)
			break;
		fmtrune(fp, ' ');
	}
	return 0;
}

int
FNconv(Fmt *fp)
{
	char *str;
	Node *n;

	n = va_arg(fp->args, Node*);
	str = "<indirect>";
	if(n != Z && (n->op == ONAME || n->op == ODOT || n->op == OELEM))
		str = n->sym->name;
	return fmtstrcpy(fp, str);
}

int
Qconv(Fmt *fp)
{
	long b;
	int i;

	b = va_arg(fp->args, long);
	while(b) {
		i = bitno(b);
		b &= ~(1L << i);
		fmtprint(fp, "%s%s", qnames[i], b? " ": "");
	}
	return 0;
}

int
VBconv(Fmt *fp)
{
	int n, t, pc;

	n = va_arg(fp->args, int);
	pc = 0;	/* BUG: was printcol */
	while(pc < n) {
		t = (pc+4) & ~3;
		if(t <= n) {
			fmtrune(fp, '\t');
			pc = t;
		} else {
			fmtrune(fp, ' ');
			pc++;
		}
	}
	return 0;
}

void
setinclude(char *p)
{
	int i;
	char *e;

	while(*p != 0) {
		e = strchr(p, ' ');
		if(e != 0)
			*e = '\0';

		for(i=1; i < ninclude; i++)
			if(strcmp(p, include[i]) == 0)
				break;

		if(i >= ninclude){
			if(ninclude >= nelem(include)) {
				diag(Z, "ninclude too small %d", nelem(include));
				exits("ninclude");
			}
			include[ninclude++] = p;
		}

		if(e == 0)
			break;
		p = e+1;
	}
}

#include <u.h>
#include <libc.h>
#include <bio.h>
#include "tapefs.h"

/*
 * File system for cpio tapes (read-only)
 */

union hblock {
	char tbuf[Maxbuf];
} dblock;

typedef  void HdrReader(Fileinf *);

Biobuf	*tape;

static void
addrfatal(char *fmt, va_list arg)
{
	char buf[1024];

	vseprint(buf, buf+sizeof(buf), fmt, arg);
	fprint(2, "%s: %#llx: %s\n", argv0, Bseek(tape, 0, 1), buf);
	exits(buf);
}

static int
egetc(void)
{
	int c;

	if((c = Bgetc(tape)) == Beof)
		sysfatal("unexpected eof");
	if(c < 0)
		sysfatal("read error: %r");
	return c;
}

static ushort
rd16le()
{
	ushort x;

	x = egetc();
	x |= egetc()<<8;
	return x;
}

static ulong
rd3211()
{
	ulong x;

	x = egetc()<<16;
	x |= egetc()<<24;
	x |= egetc();
	x |= egetc()<<8;
	return x;
}

/* sysvr3 and sysvr4 skip records with names longer than 256. pwb 1.0,
32V, sysiii, sysvr1, and sysvr2 overrun their 256 byte buffer */
static void
rdpwb(Fileinf *f, ushort (*rd16)(void), ulong (*rd32)(void))
{
	int namesz, n;
	static char buf[256];

	rd16();	/* dev */
	rd16();	/* ino */
	f->mode = rd16();
	f->uid = rd16();
	f->gid = rd16();
	rd16();	/* nlink */
	rd16();	/* rdev */
	f->mdate = rd32();
	namesz = rd16();
	f->size = rd32();

	/* namesz include the trailing nul */
	if(namesz == 0)
		sysfatal("name too small");
	if(namesz > sizeof(buf))
		sysfatal("name too big");

	if((n = Bread(tape, buf, namesz)) < 0)
		sysfatal("read error: %r");
	if(n < namesz)
		sysfatal("unexpected eof");

	if(buf[n-1] != '\0')
		sysfatal("no nul after file name");
	if((n = strlen(buf)) != namesz-1)
		sysfatal("mismatched name length: saw %d; expected %d", n, namesz-1);
	f->name = buf;

	/* skip padding */
	if(Bseek(tape, 0, 1) & 1)
		egetc();
}

static void
rdpwb11(Fileinf *f)
{
	rdpwb(f, rd16le, rd3211);
}

static vlong
rdasc(int n)
{
	vlong x;
	int y;

	for(x = 0; n > 0; n--) {
		if((y = egetc() - '0') & ~7)
			sysfatal("rdasc:%#c not octal", y);
		x = x<<3 | y;
	}
	return x;
}

static vlong
rdascx(int n)
{
	vlong x;
	int y;

	for(x = 0; n > 0; n--) {
		y = egetc();
		if ((y >= '0') && (y <= '9')){
			x = x << 4 | (y - '0');
			continue;
		}
		if ((y >= 'a') && (y <= 'f')){
			x = x << 4 | 10 + (y - 'a');
			continue;
		}
		if ((y >= 'A') && (y <= 'F')){
			x = x << 4 | 10 + (y - 'A');
			continue;
		}
		sysfatal("rdascx:%#x:not hex", y);
	}
	return x;
}

/* sysvr3 and sysvr4 skip records with names longer than 256. sysiii,
sysvr1, and sysvr2 overrun their 256 byte buffer */
static void
rdsysiii(Fileinf *f)
{
	int namesz, n;
	static char buf[256];

	rdasc(6);	/* dev */
	rdasc(6);	/* ino */
	f->mode = rdasc(6);
	f->uid = rdasc(6);
	f->gid = rdasc(6);
	rdasc(6);	/* nlink */
	rdasc(6);	/* rdev */
	f->mdate = rdasc(11);
	namesz = rdasc(6);
	f->size = rdasc(11);

	/* namesz includes the trailing nul */
	if(namesz == 0)
		sysfatal("name too small");
	if(namesz > sizeof (buf))
		sysfatal("name too big");

	if((n = Bread(tape, buf, namesz)) < 0)
		sysfatal("read error: %r");
	if(n < namesz)
		sysfatal("unexpected eof");

	if(buf[n-1] != '\0')
		sysfatal("no nul after file name");
	if((n = strlen(buf)) != namesz-1)
		sysfatal("mismatched name length: saw %d; expected %d", n, namesz-1);
	f->name = buf;
}

/*
struct cpio_newc_header {
                   char    c_magic[6];
                   char    c_ino[8];
                   char    c_mode[8];
                   char    c_uid[8];
                   char    c_gid[8];
                   char    c_nlink[8];
                   char    c_mtime[8];
                   char    c_filesize[8];
                   char    c_devmajor[8];
                   char    c_devminor[8];
                   char    c_rdevmajor[8];
                   char    c_rdevminor[8];
                   char    c_namesize[8];
                   char    c_check[8];
           };
*/
static void
rdnewc(Fileinf *f)
{
	int namesz, n;
	static char buf[256];

	rdascx(8);	/* ino */
	f->mode = rdascx(8);
	f->uid = rdascx(8);
	f->gid = rdascx(8);
	rdascx(8);	/* nlink */
	f->mdate = rdascx(8);
	f->size = rdascx(8);
	rdascx(8); //devmajor
	rdascx(8); //devminor
	rdascx(8); //rdevmajor
	rdascx(8); //rdevminor
	namesz = rdascx(8);
	rdascx(8); // checksum

	/* namesz includes the trailing nul */
	if(namesz == 0)
		sysfatal("name too small");
	if(namesz > sizeof (buf))
		sysfatal("name too big");
	if((n = Bread(tape, buf, namesz)) < 0)
		sysfatal("read error: %r");
	if(n < namesz)
		sysfatal("unexpected eof");

	if(buf[n-1] != '\0')
		sysfatal("no nul after file name");
	if((n = strlen(buf)) != namesz-1)
		sysfatal("mismatched name length: saw %d; expected %d", n, namesz-1);
	f->name = buf;
	if((Bseek(tape, 0, 1) & 3)) {
		int skip = 4-(Bseek(tape, 0, 1) & 3);
		for(int i = 0; i < skip; i++)
			egetc();
	}
}

static HdrReader *
rdmagic(void)
{
	uchar buf[8];

	buf[0] = egetc();
	buf[1] = egetc();
	if(buf[0] == 0xc7 && buf[1] == 0x71)
		return rdpwb11;

	buf[2] = egetc();
	buf[3] = egetc();
	buf[4] = egetc();
	buf[5] = egetc();
	buf[6] = 0;
	if(memcmp(buf, "070707", 6) == 0)
		return rdsysiii;

	if(memcmp(buf, "070701", 6) == 0)
		return rdnewc;

	for(int i = 0; i < 6; i++)
		print("%#x,", buf[i]);

	sysfatal("Out of phase(%s)--get MERT help", buf);
}

void
populate(char *name)
{
	HdrReader *rdhdr, *prevhdr;
	Fileinf f;

	/* the tape buffer may not be the ideal size for scanning the
	record headers */
	if((tape = Bopen(name, OREAD)) == nil)
		sysfatal("Can't open argument file");
	extern void (*_sysfatal)(char *, va_list);
	_sysfatal = addrfatal;

	prevhdr = nil;
	replete = 1;
	for(;;) {
		/* sysiii and sysv implementations don't allow
		multiple header types within a single tape, so we
		won't either */
		rdhdr = rdmagic();
		if(prevhdr != nil && rdhdr != prevhdr)
			sysfatal("mixed headers");
		rdhdr(&f);

		while(f.name[0] == '/')
			f.name++;
		if(f.name[0] == '\0')
			sysfatal("nameless record");
		if(strcmp(f.name, "TRAILER!!!") == 0)
			break;
		switch(f.mode & 0170000) {
		case 0040000:
			f.mode = DMDIR | f.mode&0777;
			break;
		case 0100000:	/* normal file */
		case 0120000:	/* symlink */
			f.mode &= 0777;
			break;
		default:	/* sockets, pipes, devices */
			f.mode = 0;
			break;
		}
		f.addr = Bseek(tape, 0, 1);
		poppath(f, 1);

		Bseek(tape, f.size, 1);
		/* skip padding */
		if(((rdhdr == rdpwb11)||(rdhdr == rdnewc)) && (Bseek(tape, 0, 1) & 1))
			egetc();

		/* sleazy alignment hack. Who needs a for loop? */
		if(rdhdr == rdnewc && (Bseek(tape, 0, 1) & 2)) {
				egetc();
				egetc();
		}

	}
}

void
dotrunc(Ram *r)
{
	USED(r);
}

void
docreate(Ram *r)
{
	USED(r);
}

char *
doread(Ram *r, vlong off, long cnt)
{
	Bseek(tape, r->addr+off, 0);
	if (cnt>sizeof(dblock.tbuf))
		sysfatal("read too big");
	Bread(tape, dblock.tbuf, cnt);
	return dblock.tbuf;
}

void
popdir(Ram *r)
{
	USED(r);
}

void
dowrite(Ram *r, char *buf, long off, long cnt)
{
	USED(r); USED(buf); USED(off); USED(cnt);
}

int
dopermw(Ram *r)
{
	USED(r);
	return 0;
}

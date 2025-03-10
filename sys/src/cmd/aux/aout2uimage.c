#include <u.h>
#include <libc.h>
#include <bio.h>
#include <mach.h>
#include <flate.h>

int infd, outfd;
ulong dcrc;
ulong *tab;
ulong lie = 23;	/* plan9 */
uchar buf[65536];

enum {
	IH_ARCH_INVALID		= 0,
	IH_ARCH_ALPHA,
	IH_ARCH_ARM,
	IH_ARCH_I386,
	IH_ARCH_IA64,
	IH_ARCH_MIPS,
	IH_ARCH_MIPS64,
	IH_ARCH_PPC,
	IH_ARCH_S390,
	IH_ARCH_SH,
	IH_ARCH_SPARC,
	IH_ARCH_SPARC64,
	IH_ARCH_M68K,
	IH_ARCH_NIOS,
	IH_ARCH_MICROBLAZE,
	IH_ARCH_NIOS2,
	IH_ARCH_BLACKFIN,
	IH_ARCH_AVR32,
	IH_ARCH_ST200,
	IH_ARCH_SANDBOX,
	IH_ARCH_NDS32,
	IH_ARCH_OPENRISC,
	IH_ARCH_ARM64,
	IH_ARCH_ARC,
	IH_ARCH_X86_64,
	IH_ARCH_XTENSA,
	IH_ARCH_RISCV,
};

uchar archtab[] = {
	[MMIPS] IH_ARCH_MIPS,
	[MSPARC] IH_ARCH_SPARC,
	[MI386] IH_ARCH_I386,
	[MMIPS2] IH_ARCH_MIPS,
	[NMIPS2] IH_ARCH_MIPS,
	[MARM] IH_ARCH_ARM,
	[MPOWER] IH_ARCH_PPC,
	[MALPHA] IH_ARCH_ALPHA,
	[NMIPS] IH_ARCH_MIPS,
	[MSPARC64] IH_ARCH_SPARC64,
	[MAMD64] IH_ARCH_X86_64,
	[MPOWER64] IH_ARCH_PPC,
	[MARM64] IH_ARCH_ARM64,
};

void
put(uchar *p, u32int v)
{
	*p++ = v >> 24;
	*p++ = v >> 16;
	*p++ = v >> 8;
	*p = v;
}

void
usage(void)
{
	fprint(2, "usage: %s [-o outfile] [-Z kzero] [-l ostype] a.out\n", argv0);
	exits("usage");
}

void
block(int n)
{
	int rc;

	rc = readn(infd, buf, n);
	if(rc < 0) sysfatal("read: %r");
	if(rc < n) sysfatal("input file truncated");
	if(write(outfd, buf, n) < 0) sysfatal("write error");
	dcrc = blockcrc(tab, dcrc, buf, n);
}

void
copy(int n)
{
	int i;

	for(i = sizeof(buf) - 1; i < n; i += sizeof(buf))
		block(sizeof(buf));
	i = n & sizeof(buf) - 1;
	if(i > 0)
		block(i);
}

void
main(int argc, char **argv)
{
	Fhdr fhdr;
	u64int kzero;
	ulong rtext;
	uchar header[64];
	char *ofile, *iname;
	int arch;

	kzero = 0xF0000000;
	ofile = nil;
	ARGBEGIN {
	case 'Z': kzero = strtoull(EARGF(usage()), 0, 0); break;
	case 'l': lie = strtoul(EARGF(usage()), 0, 0); break;
	case 'o': ofile = strdup(EARGF(usage())); break;
	default: usage();
	} ARGEND;
	
	if(argc != 1) usage();
	infd = open(argv[0], OREAD);
	if(infd < 0) sysfatal("infd: %r");
	if(crackhdr(infd, &fhdr) == 0) sysfatal("crackhdr: %r");
	if((uint)mach->mtype >= nelem(archtab) || archtab[mach->mtype] == 0)
		sysfatal("archloch");
	arch = archtab[mach->mtype];
	assert(sizeof(buf) >= mach->pgsize);
	iname = strrchr(argv[0], '/');
	if(iname != nil)
		iname++;
	else
		iname = argv[0];
	if(ofile == nil) ofile = smprint("%s.u", iname);
	outfd = create(ofile, OWRITE|OTRUNC, 0666);
	if(outfd < 0) sysfatal("create: %r");
	
	tab = mkcrctab(0xEDB88320);
	seek(infd, fhdr.hdrsz, 0);	/* a.out header not part of the image */
	seek(outfd, sizeof(header), 0);
	dcrc = 0;
	copy(fhdr.txtsz);

	/* round text out to page boundary (see rebootcmd()) */
	rtext = ((fhdr.entry + fhdr.txtsz + mach->pgsize-1) & -mach->pgsize) - fhdr.entry;
	if(rtext > fhdr.txtsz){
		memset(buf, 0, rtext - fhdr.txtsz);
		if(write(outfd, buf, rtext - fhdr.txtsz) < 0) sysfatal("write: %r");
		dcrc = blockcrc(tab, dcrc, buf, rtext - fhdr.txtsz);
	}
	copy(fhdr.datsz);
	
	memset(header, 0, sizeof(header));
	put(&header[0], 0x27051956); /* magic */
	put(&header[8], time(0)); /* time */
	put(&header[12], rtext + fhdr.datsz); /* image size */
	put(&header[16], fhdr.txtaddr - kzero); /* load address */
	put(&header[20], fhdr.entry - kzero); /* entry point */
	put(&header[24], dcrc); /* data crc */
	header[28] = lie;
	header[29] = arch;
	header[30] = 2; /* type = kernel */
	header[31] = 0; /* compressed = no */
	
	strncpy((char*)&header[32], iname, sizeof(header)-32);
	put(&header[4], blockcrc(tab, 0, header, sizeof(header)));
	
	seek(outfd, 0, 0);
	if(write(outfd, header, sizeof(header)) < sizeof(header)) sysfatal("write: %r");
	
	exits(nil);
}

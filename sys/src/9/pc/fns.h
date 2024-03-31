#include "../port/portfns.h"

Dirtab*	addarchfile(char*, int, long(*)(Chan*,void*,long,vlong), long(*)(Chan*,void*,long,vlong));
void	archinit(void);
void	archreset(void);
int	bios32call(BIOS32ci*, u16int[3]);
int	bios32ci(BIOS32si*, BIOS32ci*);
void	bios32close(BIOS32si*);
BIOS32si* bios32open(char*);
void	bootargsinit(void);
ulong	cankaddr(ulong);
int	checksum(void *, int);
void	clockintr(Ureg*, void*);
int	(*cmpswap)(long*, long, long);
int	cmpswap486(long*, long, long);
void	(*coherence)(void);
void	cpuid(int, int, ulong regs[]);
void	fpuinit(void);
void	fpuprocsetup(Proc*);
void	fpuprocfork(Proc*);
void	fpuprocsave(Proc*);
void	fpuprocrestore(Proc*);
int	cpuidentify(void);
void	cpuidprint(void);
void	(*cycles)(uvlong*);
void	delay(int);
void	delayloop(int);
void*	dmabva(int);
#define	dmaflush(clean, addr, len)
int	dmacount(int);
int	dmadone(int);
void	dmaend(int);
int	dmainit(int, int);
#define DMAWRITE 0
#define DMAREAD 1
#define DMALOOP 2
long	dmasetup(int, void*, long, int);
void	dumpmcregs(void);
int	ecinit(int cmdport, int dataport);
int	ecread(uchar addr);
int	ecwrite(uchar addr, uchar val);
#define	evenaddr(x)				/* x86 doesn't care */
void	fpclear(void);
void	fpinit(void);
void	fpoff(void);
void	(*fprestore)(FPsave*);
void	(*fpsave)(FPsave*);
ulong	getcr0(void);
ulong	getcr2(void);
ulong	getcr3(void);
ulong	getcr4(void);
u32int	getdr6(void);
char*	getconf(char*);
void	halt(void);
void	mwait(void*);
int	i8042auxcmd(int);
void	i8042auxenable(void (*)(int, int));
void	i8042reset(void);
void	i8250console(void);
void*	i8250alloc(int, int, int);
void	i8253enable(void);
void	i8253init(void);
void	i8253reset(void);
uvlong	i8253read(uvlong*);
void	i8253timerset(uvlong);
void	idle(void);
void	idlehands(void);
int	inb(int);
void	insb(int, void*, int);
ushort	ins(int);
void	inss(int, void*, int);
ulong	inl(int);
void	insl(int, void*, int);
void	intrdisable(int, void (*)(Ureg *, void *), void*, int, char*);
void	intrenable(int, void (*)(Ureg*, void*), void*, int, char*);
void	introff(void);
void	intron(void);
void	invlpg(ulong);
void	ioinit(void);
int	iprint(char*, ...);
int	isaconfig(char*, int, ISAConf*);
void*	kaddr(ulong);
#define	kmapinval()
void	lgdt(ushort[3]);
void	lldt(ulong);
void	lidt(ushort[3]);
void	links(void);
void	ltr(ulong);
void	mach0init(void);
void	mathinit(void);
void	mb386(void);
void	mb586(void);
void	meminit(void);
void	meminit0(void);
void	memreserve(uintptr, uintptr);
void	mfence(void);
#define mmuflushtlb(pdb) putcr3(pdb)
void	mmuinit(void);
ulong*	mmuwalk(ulong*, ulong, int, int);
char*	mtrr(uvlong, uvlong, char *);
char*	mtrrattr(uvlong, uvlong *);
void	mtrrclock(void);
int	mtrrprint(char *, long);
void	mtrrsync(void);
void	nmienable(void);
uchar	nvramread(int);
void	nvramwrite(int, uchar);
void	outb(int, int);
void	outsb(int, void*, int);
void	outs(int, ushort);
void	outss(int, void*, int);
void	outl(int, ulong);
void	outsl(int, void*, int);
ulong	paddr(void*);
void	patwc(void*, int);
void	pcicfginit(void);
int	(*pcicfgrw8)(int, int, int, int);
int	(*pcicfgrw16)(int, int, int, int);
int	(*pcicfgrw32)(int, int, int, int);
void	pcmcisread(PCMslot*);
int	pcmcistuple(int, int, int, void*, int);
PCMmap*	pcmmap(int, ulong, int, int);
int	pcmspecial(char*, ISAConf*);
int	(*_pcmspecial)(char *, ISAConf *);
void	pcmspecialclose(int);
void	(*_pcmspecialclose)(int);
void	pcmunmap(int, PCMmap*);
void	pmap(ulong, ulong, int);
void	punmap(ulong, int);
void	procrestore(Proc*);
void	procsave(Proc*);
void	procsetup(Proc*);
void	procfork(Proc*);
void	putcr0(ulong);
void	putcr2(ulong);
void	putcr3(ulong);
void	putcr4(ulong);
void	putxcr0(ulong);
void	putdr(u32int*);
void	putdr01236(uintptr*);
void	putdr6(u32int);
void	putdr7(u32int);
void*	rampage(void);
int	rdmsr(int, vlong*);
void	realmode(Ureg*);
void*	rsdsearch(void);
void	screeninit(void);
void	(*screenputs)(char*, int);
void	setconfenv(void);
void*	sigsearch(char*, int);
void	syncclock(void);
void*	tmpmap(Page*);
void	tmpunmap(void*);
void	touser(void*);
void	trapenable(int, void (*)(Ureg*, void*), void*, char*);
void	trapinit(void);
void	trapinit0(void);
int	tas(void*);
uvlong	tscticks(uvlong*);
ulong	umballoc(ulong, ulong, ulong);
void	umbfree(ulong, ulong);
uvlong	upaalloc(uvlong, uvlong, uvlong);
uvlong	upaallocwin(uvlong, uvlong, uvlong, uvlong);
void	upafree(uvlong, uvlong);
void	vectortable(void);
void*	vmap(uvlong, vlong);
int	vmapsync(ulong);
void	vmxprocrestore(Proc *);
void	vmxshutdown(void);
void	vunmap(void*, vlong);
void	wbinvd(void);
void	writeconf(void);
int	wrmsr(int, vlong);
int	xchgw(ushort*, int);
void	rdrandbuf(void*, ulong);

#define	userureg(ur)	(((ur)->cs & 3) == 3)
#define	KADDR(a)	kaddr(a)
#define PADDR(a)	paddr((void*)(a))

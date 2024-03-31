#include "../port/portfns.h"

Dirtab*	addarchfile(char*, int, long(*)(Chan*,void*,long,vlong), long(*)(Chan*,void*,long,vlong));
void	archinit(void);
void	archreset(void);
int	bios32call(BIOS32ci*, u16int[3]);
int	bios32ci(BIOS32si*, BIOS32ci*);
void	bios32close(BIOS32si*);
BIOS32si* bios32open(char*);
void	bootargsinit(void);
uintptr	cankaddr(uintptr);
int	checksum(void *, int);
void	clockintr(Ureg*, void*);
int	(*cmpswap)(long*, long, long);
int	cmpswap486(long*, long, long);
void	(*coherence)(void);
void	cpuid(int, int, ulong regs[]);
void	fpuinit(void);
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
void	(*fprestore)(FPsave*);
void	(*fpsave)(FPsave*);
void	fpinit(void);
FPsave*	fpukenter(Ureg*);
void	fpukexit(Ureg*, FPsave*);
void	fpuprocfork(Proc*);
void	fpuprocrestore(Proc*);
void	fpuprocsave(Proc*);
void	fpuprocsetup(Proc*);

u64int	getcr0(void);
u64int	getcr2(void);
u64int	getcr3(void);
u64int	getcr4(void);
u64int	getxcr0(void);
u64int	getdr6(void);
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
void	invlpg(uintptr);
void	ioinit(void);
int	iprint(char*, ...);
int	isaconfig(char*, int, ISAConf*);
void*	kaddr(uintptr);
KMap*	kmap(Page*);
void	kunmap(KMap*);
#define	kmapinval()
void	lgdt(void*);
void	lidt(void*);
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
#define mmuflushtlb(pml4) putcr3(pml4)
void	mmuinit(void);
uintptr	*mmuwalk(uintptr*, uintptr, int, int);
char*	mtrr(uvlong, uvlong, char *);
char*	mtrrattr(uvlong, uvlong *);
void	mtrrclock(void);
int	mtrrprint(char *, long);
void	mtrrsync(void);
void	nmienable(void);
void	noteret(void);
uchar	nvramread(int);
void	nvramwrite(int, uchar);
void	outb(int, int);
void	outsb(int, void*, int);
void	outs(int, ushort);
void	outss(int, void*, int);
void	outl(int, ulong);
void	outsl(int, void*, int);
uintptr	paddr(void*);
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
void	pmap(uintptr, uintptr, vlong);
void	punmap(uintptr, vlong);
void	preallocpages(void);
void	procrestore(Proc*);
void	procsave(Proc*);
void	procsetup(Proc*);
void	procfork(Proc*);
void	putcr0(u64int);
void	putcr2(u64int);
void	putcr3(u64int);
void	putcr4(u64int);
void	putxcr0(u64int);
void	putdr(u64int*);
void	putdr01236(u64int*);
void	putdr6(u64int);
void	putdr7(u64int);
void*	rampage(void);
int	rdmsr(int, vlong*);
void	realmode(Ureg*);
void*	rsdsearch(void);
void	screeninit(void);
void	(*screenputs)(char*, int);
void	setconfenv(void);
void*	sigsearch(char*, int);
void	syncclock(void);
void	syscallentry(void);
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
void	vmxprocrestore(Proc *);
void	vmxshutdown(void);
void*	vmap(uvlong, vlong);
void	vunmap(void*, vlong);
void	wbinvd(void);
void	writeconf(void);
int	wrmsr(int, vlong);
int	xchgw(ushort*, int);
void	rdrandbuf(void*, ulong);

#define	userureg(ur)	(((ur)->cs & 3) == 3)
#define	KADDR(a)	kaddr(a)
#define PADDR(a)	paddr((void*)(a))

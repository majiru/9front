#include "../port/portfns.h"

/* l.s */
extern void sev(void);
extern int tas(void *);
extern int cmpswap(long*, long, long);
extern void coherence(void);
extern void idlehands(void);
extern uvlong vcycles(void);
#define cycles(ip) *(ip) = vcycles()
extern int splfhi(void);
extern void splflo(void);
extern void touser(uintptr sp);
extern void forkret(void);
extern void noteret(void);
extern void returnto(void*);
extern void fpsaveregs(void*);
extern void fploadregs(void*);
extern void smccall(Ureg*);

extern void setttbr(uintptr pa);
extern uintptr getfar(void);

extern void flushasidva(uintptr asidva);
extern void tlbivae1is(uintptr asidva);

extern void flushasidvall(uintptr asidva);
extern void tlbivale1is(uintptr asidva);

extern void flushasid(uintptr asid);
extern void tlbiaside1is(uintptr asid);

extern void flushtlb(void);
extern void tlbivmalle1(void);

extern void flushlocaltlb(void);
extern void tlbivmalle1(void);

/* cache */
extern ulong cachesize(int level);

extern void cacheiinvse(void*, int);
extern void cacheuwbinv(void);
extern void cacheiinv(void);

extern void cachedwbse(void*, int);
extern void cacheduwbse(void*, int);
extern void cachedinvse(void*, int);
extern void cachedwbinvse(void*, int);

extern void cachedwb(void);
extern void cachedinv(void);
extern void cachedwbinv(void);

extern void l2cacheuwb(void);
extern void l2cacheuinv(void);
extern void l2cacheuwbinv(void);

/* mmu */
#define	getpgcolor(a)	0
extern uintptr paddr(void*);
#define PADDR(a) paddr((void*)(a))
extern uintptr cankaddr(uintptr);
extern void* kaddr(uintptr);
#define KADDR(a) kaddr(a)
extern void kmapinval(void);
#define	VA(k)	((uintptr)(k))
extern KMap *kmap(Page*);
extern void kunmap(KMap*);
extern uintptr mmukmap(uintptr, uintptr, usize);
extern void* vmap(uvlong, vlong);
extern void vunmap(void*, vlong);

extern void mmu0init(uintptr*);
extern void mmu0clear(uintptr*);
extern void mmuidmap(uintptr*);
extern void mmu1init(void);
extern void meminit(void);

extern void putasid(Proc*);

extern void* ucalloc(usize);
extern void* fbmemalloc(usize);

/* clock */
extern void clockinit(void);
extern void synccycles(void);
extern void armtimerset(int);
extern void clockshutdown(void);

/* fpu */
extern void fpuinit(void);
extern void fpoff(void);
extern void fpinit(void);
extern void fpclear(void);
extern void fpsave(FPsave*);
extern void fprestore(FPsave*);
extern void mathtrap(Ureg*);

/* trap */
extern void trapinit(void);
extern int userureg(Ureg*);
extern void evenaddr(uintptr);
extern void setkernur(Ureg*, Proc*);
extern void procfork(Proc*);
extern void procsetup(Proc*);
extern void procsave(Proc*);
extern void procrestore(Proc *);
extern void trap(Ureg*);
extern void syscall(Ureg*);
extern void noted(Ureg*, ulong);
extern void faultarm64(Ureg*);
extern void dumpstack(void);
extern void dumpregs(Ureg*);

/* irq */
extern void intrinit(void);
extern void intrcpushutdown(void);
extern void intrsoff(void);
extern void intrenable(int, void (*)(Ureg*, void*), void*, int, char*);
extern void intrdisable(int, void (*)(Ureg*, void*), void*, int, char*);
extern int irq(Ureg*);
extern void fiq(Ureg*);

/* sysreg */
extern uvlong	sysrd(ulong);
extern void	syswr(ulong, uvlong);

/* uartimx */
extern void uartconsinit(void);

/* dma */
extern void dmaflush(int, void*, ulong);

/* main */
extern char *getconf(char *name);
extern void setconfenv(void);
extern void writeconf(void);

extern int isaconfig(char*, int, ISAConf*);
extern void links(void);

/* ccm */
extern void setclkgate(char *name, int on);
extern void setclkrate(char *name, char *source, int freq);
extern int getclkrate(char *name);

/* gpc */
extern void powerup(char *dom);

/* lcd */
extern void lcdinit(void);

/* iomux */
extern void iomuxpad(char *pads, char *sel, char *cfg);
extern uint iomuxgpr(int gpr, uint set, uint mask);

/* gpio */
#define GPIO_PIN(n, m)	((n)<<5 | (m))
extern void gpioout(uint pin, int set);
extern int gpioin(uint pin);

/* pciimx */
extern int pcicfgrw8(int tbdf, int rno, int data, int read);
extern int pcicfgrw16(int tbdf, int rno, int data, int read);
extern int pcicfgrw32(int tbdf, int rno, int data, int read);
extern void pciintrenable(int tbdf, void (*f)(Ureg*, void*), void *a);
extern void pciintrdisable(int tbdf, void (*f)(Ureg*, void*), void *a);

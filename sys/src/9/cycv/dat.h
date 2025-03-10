typedef struct Conf	Conf;
typedef struct Confmem	Confmem;
typedef struct FPsave	FPsave;
typedef struct PFPU	PFPU;
typedef struct L1	L1;
typedef struct Label	Label;
typedef struct Lock	Lock;
typedef struct KMap	KMap;
typedef struct MMMU	MMMU;
typedef struct Mach	Mach;
typedef struct Page	Page;
typedef struct Proc	Proc;
typedef struct PMMU	PMMU;
typedef struct Ureg	Ureg;
typedef struct ISAConf	ISAConf;
typedef struct DMAC	DMAC;
typedef uvlong		Tval;

#pragma incomplete Ureg

#define MAXSYSARG	5	/* for mount(fd, mpt, flag, arg, srv) */

#define AOUT_MAGIC	(E_MAGIC)

struct Lock
{
	ulong	key;
	u32int	sr;
	uintptr	pc;
	Proc*	p;
	Mach*	m;
	int	isilock;
};

struct Label
{
	uintptr	sp;
	uintptr	pc;
};

struct FPsave
{
	ulong	exc, scr;
	uchar	regs[256];
};

struct PFPU
{
	int	fpstate;
	FPsave	fpsave[1];
};

enum
{
	FPinit,
	FPactive,
	FPinactive,
	FPillegal = 0x100
};

struct Confmem
{
	uintptr	base;
	ulong	npage;
	uintptr	kbase;
	uintptr	klimit;
};

struct Conf
{
	ulong	nmach;		/* processors */
	ulong	nproc;		/* processes */
	Confmem	mem[2];		/* physical memory */
	ulong	npage;		/* total physical pages of memory */
	ulong	upages;		/* user page pool */
	ulong	copymode;	/* 0 is copy on write, 1 is copy on reference */
	ulong	ialloc;		/* max interrupt time allocation in bytes */
	ulong	pipeqsize;	/* size in bytes of pipe queues */
	ulong	nimage;		/* number of page cache image headers */
	ulong	nswap;		/* number of swap pages */
	int	nswppo;		/* max # of pageouts per segment pass */
	int	monitor;
};

/*
 *  MMU stuff in proc
 */
#define NCOLOR	1

struct PMMU
{
	L1 *l1;
	Page *mmuused, *mmufree;
	
	int nkmap;
	Page *kmaptable;
};

#include "../port/portdat.h"

struct L1
{
	Ref;
	uintptr pa;
	ulong *va;
	L1 *next;
};

struct MMMU
{
	L1 l1;
	L1 *l1free;
	int nfree;
	uchar asid;
};

struct Mach
{
	int	machno;			/* physical id of processor */
	uintptr	splpc;			/* pc of last caller to splhi */
	Proc*	proc;			/* current process */
	ulong	excregs[3];
	ulong	cycleshi;
	/* end of known offsets to assembly */

	MMMU;

	PMach;

	int	lastintr;

	int	cpumhz;
	uvlong	cpuhz;			/* speed of cpu */

	uintptr	stack[1];
};

#define NISAOPT		8
struct ISAConf
{
	char	*type;
	ulong	port;
	int	irq;
	int	nopt;
	char	*opt[NISAOPT];
};
#define BUSUNKNOWN -1

struct
{
	char	machs[MAXMACH];		/* active CPUs */
	int	exiting;		/* shutdown */
}active;

extern register Mach* m;			/* R10 */
extern register Proc* up;			/* R9 */

void nope(void);
#define NOPE nope();

/*
 *  hardware info about a device
 */
typedef struct {
	ulong	port;
	int	size;
} Devport;

struct DevConf
{
	ulong	intnum;			/* interrupt number */
	char	*type;			/* card type, malloced */
	int	nports;			/* Number of ports */
	Devport	*ports;			/* The ports themselves */
};

#define mpcore ((ulong*)MPCORE_BASE)
#define resetmgr ((ulong*)RESETMGR_BASE)
#define sysmgr ((ulong*)SYSMGR_BASE)
#define l3 ((ulong*)L3_BASE)

/*dmacopy*/
#define SRC_INC (1<<0)
#define DST_INC (1<<14)

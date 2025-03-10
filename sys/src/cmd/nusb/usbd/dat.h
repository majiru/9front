typedef struct Hub Hub;
typedef struct DHub DHub;
typedef struct DSSHub DSSHub;
typedef struct Port Port;

enum
{
	Stack	= 32*1024,

	Dhub		= 0x29,		/* hub descriptor type */
	Dhublen		= 9,		/* hub descriptor length */

	Dsshub		= 0x2A,		/* superspeed hub descriptor type */
	Dsshublen	= 12,		/* superspeed hub descriptor length */

	/* hub class feature selectors */
	Fhublocalpower	= 0,
	Fhubovercurrent	= 1,

	Fportconnection	= 0,
	Fportenable	= 1,
	Fportsuspend	= 2,
	Fportovercurrent = 3,
	Fportreset	= 4,
	Fportpower	= 8,
	Fportlowspeed	= 9,
	Fcportconnection	= 16,
	Fcportenable	= 17,
	Fcportsuspend	= 18,
	Fcportovercurrent= 19,
	Fcportreset	= 20,
	Fportindicator	= 22,
	Fbhportreset	= 28,
	

	/* Port status and status change bits
	 * Constants at /sys/src/9/pc/usb.h starting with HP-
	 * must have the same values or root hubs won't work.
	 */
	PSpresent	= 0x0001,
	PSenable	= 0x0002,
	PSsuspend	= 0x0004,
	PSovercurrent	= 0x0008,
	PSreset		= 0x0010,
	PSpower		= 0x0100,
	PSslow		= 0x0200,
	PShigh		= 0x0400,

	PSstatuschg	= 0x10000,	/* PSpresent changed */
	PSchange	= 0x20000,	/* PSenable changed */


	/* port/device state */
	Pdisabled = 0,		/* must be 0 */
	Pattached,
	Pconfigured,

	/* Delays, timeouts (ms) */
	Rootresetdelay	= 100,		/* how much to wait after a root port reset (50ms by standard) */
	Portresetdelay	= 50,		/* how much to wait after a hub port reset (20ms by standard) */
	Resumedelay	= 50,		/* how much to wait after a resume (20ms by standard) */
	Powerdelay	= 100,		/* after powering up ports */
	Pollms		= 250, 		/* port poll interval */

	Attachdelay	= 3000,		/* attach considered repeated if within Attachdelay */
	Attachcount	= 5,		/* maximum number of repeated attaches before giving up */

	/*
	 * device tab for embedded usb drivers.
	 */
	DCL = 0x01000000,		/* csp identifies just class */
	DSC = 0x02000000,		/* csp identifies just subclass */
	DPT = 0x04000000,		/* csp identifies just proto */

};

struct Hub
{
	uchar	pwrmode;
	uchar	compound;
	int	pwrms;		/* time to wait in ms */
	uchar	maxcurrent;	/*    after powering port*/
	uchar	ttt;		/* tt think-time */
	uchar	mtt;		/* muti tt enabled */
	int	leds;		/* has port indicators? */
	int	maxpkt;
	uchar	nport;
	Port	*port;
	int	failed;		/* I/O error while enumerating */
	Dev	*dev;		/* for this hub */
	Hub	*next;		/* in list of hubs */
};

struct Port
{
	int	state;		/* state of the device */
	u32int	sts;		/* old port status */
	ulong	atime;		/* time of last attach in milliseconds */
	int	acount;		/* rapid attach counter */
	uchar	removable;
	uchar	pwrctl;
	Dev	*dev;		/* attached device (if non-nil) */
	Hub	*hub;		/* non-nil if hub attached */
};

/* USB HUB descriptor */
struct DHub
{
	uchar	bLength;
	uchar	bDescriptorType;
	uchar	bNbrPorts;
	uchar	wHubCharacteristics[2];
	uchar	bPwrOn2PwrGood;
	uchar	bHubContrCurrent;
	uchar	DeviceRemovable[1];	/* variable length */
};

/* Superspeed HUB descriptor */
struct DSSHub
{
	uchar	bLength;
	uchar	bDescriptorType;
	uchar	bNbrPorts;
	uchar	wHubCharacteristics[2];
	uchar	bPwrOn2PwrGood;
	uchar	bHubContrCurrent;
	uchar	bHubHdrDecLat;
	uchar	wHubDelay[2];
	uchar	DeviceRemovable[1];	/* variable length */
};

extern Hub *hubs;

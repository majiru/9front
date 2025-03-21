#include <u.h>
#include <libc.h>
#include <thread.h>
#include "usb.h"

int
parsedev(Dev *xd, uchar *b, int n)
{
	Usbdev *d;
	DDev *dd;

	d = xd->usb;
	dd = (DDev*)b;
	if(usbdebug>1)
		fprint(2, "%s: parsedev %s: %.*H\n", argv0, xd->dir, Ddevlen, b);
	if(dd->bLength < Ddevlen){
		werrstr("short dev descr. (%d < %d)", dd->bLength, Ddevlen);
		return -1;
	}
	if(dd->bDescriptorType != Ddev){
		werrstr("%d is not a dev descriptor", dd->bDescriptorType);
		return -1;
	}
	d->csp = CSP(dd->bDevClass, dd->bDevSubClass, dd->bDevProtocol);
	d->ver = GET2(dd->bcdUSB);
	xd->isusb3 = (d->ver >= 0x0300);
	if(xd->isusb3)
		d->ep[0]->maxpkt = xd->maxpkt = 1<<dd->bMaxPacketSize0;
	else
		d->ep[0]->maxpkt = xd->maxpkt = dd->bMaxPacketSize0;
	d->class = dd->bDevClass;
	d->nconf = dd->bNumConfigurations;
	if(d->nconf == 0)
		dprint(2, "%s: %s: no configurations\n", argv0, xd->dir);
	d->vid = GET2(dd->idVendor);
	d->did = GET2(dd->idProduct);
	d->dno = GET2(dd->bcdDev);
	d->vsid = dd->iManufacturer;
	d->psid = dd->iProduct;
	d->ssid = dd->iSerialNumber;
	if(n > Ddevlen && usbdebug>1)
		fprint(2, "%s: %s: parsedev: %d bytes left",
			argv0, xd->dir, n - Ddevlen);
	return Ddevlen;
}

static int
parseiface(Usbdev *d, Conf *c, uchar *b, int n, Iface **ipp)
{
	int class, subclass, proto, alt, id;
	ulong csp;
	DIface *dip;
	Iface *ip;

	if(n < Difacelen){
		werrstr("short interface descriptor");
		return -1;
	}
	dip = (DIface *)b;
	class = dip->bInterfaceClass;
	subclass = dip->bInterfaceSubClass;
	proto = dip->bInterfaceProtocol;
	csp = CSP(class, subclass, proto);
	if(csp == 0)
		csp = d->csp;
	if(d->class == 0)
		d->class = class;
	alt = dip->bAlternateSetting;
	id = dip->bInterfaceNumber;
	if(id < 0 || id >= nelem(c->iface)){
		werrstr("bad interface number %d", id);
		return -1;
	}
	for(ip = c->iface[id]; ip != nil; ip = ip->next)
		if(ip->csp == csp && ip->alt == alt)
			goto Found;
	ip = emallocz(sizeof(Iface), 1);
	ip->id = id;
	ip->csp = csp;
	ip->alt = alt;
	ip->next = c->iface[id];
	c->iface[id] = ip;
	if(d->csp == 0)			/* use csp from 1st iface */
		d->csp = ip->csp;	/* if device has none */
	if(c == d->conf[0] && id == 0)	/* ep0 was already there */
		d->ep[0]->iface = ip;
Found:
	*ipp = ip;
	return Difacelen;
}

extern Ep* mkep(Usbdev *, int);

static int
parseendpt(Usbdev *d, Conf *c, Iface *ip, uchar *b, int n, Ep **epp)
{
	int addr, dir, type, id, i;
	DEp *dep;
	Ep *ep;

	if(n < Deplen){
		werrstr("short endpoint descriptor");
		return -1;
	}
	dep = (DEp *)b;

	type = dep->bmAttributes & 0x03;
	addr = dep->bEndpointAddress;
	if(addr & 0x80)
		dir = Ein;
	else
		dir = Eout;

	/*
	 * the endpoint id is created once, setting
	 * type and direction, meaning the endpoint's
	 * id must be unique for (type, dir) relative
	 * to all other potential endpoints from
	 * interfaces/altsettings on this device.
	 *
	 * the low Epmax bits of the id contains the
	 * endpoint address that must be preserved.
	 */
	id = addr & Epmax;
Again:
	for(ep = d->ep[id & Epmax]; ep != nil; ep = ep->next){
		if(ep->id != id)
			continue;
		if(ep->type != type){
			id += Epmax+1;
			goto Again;
		}
		if(ep->dir == dir)
			break;
		/*
		 * Ein/Eout endpoints from the same
		 * interface/altsetting can be merged
		 * into one. (except for iso).
		 */
		if(ep->iface != ip || type == Eiso){
			id += Epmax+1;
			goto Again;
		}
		dir = Eboth;
		break;
	}

	if(ep == nil || ep->iface != ip)
		ep = mkep(d, id);

	ep->dir = dir;
	ep->type = type;
	ep->iface = ip;
	ep->conf = c;
	ep->maxpkt = GET2(dep->wMaxPacketSize);
	ep->ntds = 1 + ((ep->maxpkt >> 11) & 3);
	ep->maxpkt &= 0x7FF;
	ep->attrib = dep->bmAttributes;
	ep->pollival = dep->bInterval;

	for(i = 0; i < nelem(ip->ep); i++){
		if(ip->ep[i] == nil){
			ip->ep[i] = ep;
			break;
		}
		if(ip->ep[i] == ep)
			break;
	}
	if(i >= nelem(ip->ep))
		fprint(2, "%s: parseendpt: too many endpoints in interface", argv0);

	*epp = ep;
	return Deplen;
}

static char*
dname(int dtype)
{
	switch(dtype){
	case Ddev:	return "device";
	case Dconf: 	return "config";
	case Dstr: 	return "string";
	case Diface:	return "interface";
	case Dep:	return "endpoint";
	case Dreport:	return "report";
	case Dphysical:	return "phys";
	default:	return "desc";
	}
}

int
parsedesc(Usbdev *d, Conf *c, uchar *b, int n)
{
	int	len, nd, tot;
	Iface	*ip;
	Ep 	*ep;

	tot = 0;
	ip = nil;
	ep = nil;
	for(nd = 0; nd < nelem(d->ddesc); nd++)
		if(d->ddesc[nd] == nil)
			break;

	while(n > 2 && (len = b[0]) != 0 && len <= n){
		if(usbdebug>1){
			fprint(2, "%s:\t\tparsedesc %s %x[%d] %.*H\n",
				argv0, dname(b[1]), b[1], b[0], len, b);
		}
		switch(b[1]){
		case Ddev:
		case Dconf:
			werrstr("unexpected descriptor %d", b[1]);
			ddprint(2, "%s\tparsedesc: %r", argv0);
			break;
		case Diface:
			if(parseiface(d, c, b, n, &ip) < 0){
				ddprint(2, "%s\tparsedesc: %r\n", argv0);
				return -1;
			}
			break;
		case Dep:
			if(ip == nil){
				werrstr("unexpected endpoint descriptor");
				break;
			}
			if(parseendpt(d, c, ip, b, n, &ep) < 0){
				ddprint(2, "%s\tparsedesc: %r\n", argv0);
				return -1;
			}
			break;
		default:
			if(nd >= nelem(d->ddesc)){
				fprint(2, "%s: parsedesc: too many "
					"device-specific descriptors for device"
					" %s %s\n",
					argv0, d->vendor, d->product);
				break;
			}
			d->ddesc[nd] = emallocz(sizeof(Desc)+len, 0);
			d->ddesc[nd]->iface = ip;
			d->ddesc[nd]->ep = ep;
			d->ddesc[nd]->conf = c;
			memmove(&d->ddesc[nd]->data, b, len);
			++nd;
		}
		n -= len;
		b += len;
		tot += len;
	}
	return tot;
}

int
parseconf(Usbdev *d, Conf *c, uchar *b, int n)
{
	DConf* dc;
	int	l;
	int	nr;

	dc = (DConf*)b;
	if(usbdebug>1)
		fprint(2, "%s:\tparseconf %.*H\n", argv0, Dconflen, b);
	if(dc->bLength < Dconflen){
		werrstr("short configuration descriptor");
		return -1;
	}
	if(dc->bDescriptorType != Dconf){
		werrstr("not a configuration descriptor");
		return -1;
	}
	c->cval = dc->bConfigurationValue;
	c->attrib = dc->bmAttributes;
	c->milliamps = dc->MaxPower*2;
	l = GET2(dc->wTotalLength);
	if(n < l){
		werrstr("truncated configuration info");
		return -1;
	}
	n -= Dconflen;
	b += Dconflen;
	nr = 0;
	if(n > 0 && (nr=parsedesc(d, c, b, n)) < 0)
		return -1;
	n -= nr;
	if(n > 0 && usbdebug>1)
		fprint(2, "%s:\tparseconf: %d bytes left\n", argv0, n);
	return l;
}

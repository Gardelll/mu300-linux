/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mu300: the one parse of a packet from the modem (docs/FINDINGS.md 192). Plain C with no kernel header, so that
 * tests/test_ims_bridge.py compiles and runs it on the host.
 *
 * The vendor checked such a packet in one place (nothing, or the version byte), classified it with conntrack's own
 * parser (nf_ct_get_tuplepr, which walks IPv6 extension headers its own way and, for an ICMP error, takes the
 * inner header's tuple), routed it with a third reading (ihl and the IPv6 next header taken as the transport protocol,
 * the ports read without a length) and delivered it by the version byte. Here everything that decides what happens
 * to the packet - the family, the length, the addresses, the transport protocol and the ports - comes from this
 * parse, which checks every field against the bytes there are before it reads it, and the packet is trimmed to the
 * length found, so that what the kernel's own input path sees later is what was checked here.
 */
#ifndef IMSBR_PARSE_H
#define IMSBR_PARSE_H

#define IMSBR_L3_MAX_EXTHDRS	8

struct imsbr_l3 {
	unsigned int family;		/* 4 or 6 */
	unsigned int len;		/* the packet's own length: IPv4 total length, IPv6 40 + payload length */
	unsigned int l4off;		/* where the transport header starts (after the IPv4 options, the IPv6 extensions) */
	unsigned char l4proto;		/* the transport protocol */
	unsigned char has_ports;	/* UDP or TCP, not a later fragment, and the ports are within len */
	unsigned short sport, dport;	/* host order */
	unsigned int saddr_off, daddr_off, alen;	/* the addresses, as offsets into the packet */
};

static inline unsigned int imsbr_be16(const unsigned char *p)
{
	return ((unsigned int)p[0] << 8) | p[1];
}

/* 0, or -1 for a packet that is not a whole IPv4 or IPv6 packet within buflen bytes */
static inline int imsbr_l3_parse(const unsigned char *p, unsigned int buflen, struct imsbr_l3 *l3)
{
	unsigned int off, i;
	unsigned char nh;
	int later_frag = 0;

	if (buflen < 1)
		return -1;
	switch (p[0] >> 4) {
	case 4:
		if (buflen < 20 || (p[0] & 0x0f) < 5)
			return -1;
		l3->family = 4;
		off = (p[0] & 0x0f) * 4;
		l3->len = imsbr_be16(p + 2);
		if (l3->len < off || l3->len > buflen)
			return -1;
		nh = p[9];
		later_frag = (imsbr_be16(p + 6) & 0x1fff) != 0;
		l3->saddr_off = 12;
		l3->daddr_off = 16;
		l3->alen = 4;
		break;
	case 6:
		if (buflen < 40)
			return -1;
		l3->family = 6;
		l3->len = 40 + imsbr_be16(p + 4);
		if (l3->len > buflen)
			return -1;
		off = 40;
		nh = p[6];
		/* hop-by-hop, routing, fragment, destination options, AH: each within the payload */
		for (i = 0; ; i++) {
			unsigned int hl;

			if (nh != 0 && nh != 43 && nh != 44 && nh != 60 && nh != 51)
				break;
			if (i == IMSBR_L3_MAX_EXTHDRS || off + 8 > l3->len)
				return -1;
			if (nh == 44) {
				hl = 8;
				if (imsbr_be16(p + off + 2) & 0xfff8)
					later_frag = 1;
			} else if (nh == 51) {
				hl = (p[off + 1] + 2) * 4;
			} else {
				hl = (p[off + 1] + 1) * 8;
			}
			if (off + hl > l3->len)
				return -1;
			nh = p[off];
			off += hl;
			if (later_frag)
				break;
		}
		l3->saddr_off = 8;
		l3->daddr_off = 24;
		l3->alen = 16;
		break;
	default:
		return -1;
	}

	l3->l4off = off;
	l3->l4proto = nh;
	l3->has_ports = 0;
	l3->sport = l3->dport = 0;
	if (!later_frag && (nh == 17 || nh == 6) && off + 4 <= l3->len) {
		l3->has_ports = 1;
		l3->sport = imsbr_be16(p + off);
		l3->dport = imsbr_be16(p + off + 2);
	}
	return 0;
}

#endif

// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2016 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "sprd-imsbr: " fmt

#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/if_ether.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/ratelimit.h>
#include <linux/skbuff.h>
#include <net/flow.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/addrconf.h>
#include <linux/inetdevice.h>
#include <net/ip6_route.h>
#include <net/icmp.h>
#include <net/route.h>
#include <net/udp.h>
#include <uapi/linux/ims_bridge/ims_bridge.h>

#include "imsbr_core.h"
#include "imsbr_sipc.h"
#include "imsbr_hooks.h"
#include "imsbr_packet.h"
#include "imsbr_parse.h"

static int tcpdump_enable __read_mostly;
module_param(tcpdump_enable, int, 0644);

static unsigned int vowifi_in_mark __read_mostly;
module_param(vowifi_in_mark, uint, 0644);

uint imsbr_frag_size __read_mostly = IMSBR_PACKET_MAXSZ;
module_param_named(frag_size, imsbr_frag_size, uint, 0644);

/* XXX - when app vpn is enabled, a specific ip rule will be added:
 * "ip rule from all fwmark 0x0/0x20000 iif lo uidrange 0-99999 lookup tun0".
 * Thus if the packet's input device is "lo", it maybe routed to "tun0" and
 * stolen to the userspace, so use "dummy0" instead of "lo" here.
 */
static char *input_device __read_mostly = "dummy0";
/* mu300: 0444, was 0644: a charp parameter written at run time frees the old string under readers that
 * take no lock (the receive thread looks the device up by it for every packet) */
module_param(input_device, charp, 0444);

static char *backhole_device __read_mostly = "dummy0";
module_param(backhole_device, charp, 0444);

static DEFINE_MUTEX(imsbr_reasm_lock);
static struct sk_buff *imsbr_reasm_skb;

/* Prevent one direction's log suppress another, we do rate limit
 * separately
 */
static DEFINE_RATELIMIT_STATE(rlimit_tocp_pkt, 5 * HZ, 5);
static DEFINE_RATELIMIT_STATE(rlimit_fromcp_pkt, 5 * HZ, 5);

static struct sk_buff *imsbr_allocskb(int size, gfp_t priority)
{
	struct sk_buff *skb;
	int tlen;

	tlen = size + MAX_HEADER;
	skb = alloc_skb(tlen, priority);
	if (!skb) {
		IMSBR_STAT_INC(imsbr_stats->sk_buff_fail);
		pr_err("alloc_skb len=%d failed\n", tlen);
		return NULL;
	}

	skb_reserve(skb, MAX_HEADER - sizeof(struct ethhdr));
	skb_reset_mac_header(skb);
	skb_reserve(skb, sizeof(struct ethhdr));
	skb_reset_network_header(skb);

	return skb;
}

static struct sk_buff *imsbr_pkt2skb(void *pkt, int pktlen)
{
	struct sk_buff *skb;

	skb = imsbr_allocskb(pktlen, GFP_KERNEL);
	if (!skb)
		return NULL;

	memcpy_fromio(skb->data, (void __iomem *)pkt, pktlen);
	skb_put(skb, pktlen);

	return skb;
}

/* mu300: the protocol and ports from the one parse (imsbr_parse.h), not read again here */
static void imsbr_flowi4_init(struct flowi4 *fl4, const struct imsbr_l3 *l3, struct iphdr *ip)
{
	memset(fl4, 0, sizeof(*fl4));
	fl4->flowi4_dscp = inet_dsfield_to_dscp(ip->tos);
	fl4->flowi4_proto = l3->l4proto;
	fl4->flowi4_scope = RT_SCOPE_UNIVERSE;
	fl4->flowi4_flags = FLOWI_FLAG_ANYSRC;
	fl4->saddr = ip->saddr;
	fl4->daddr = ip->daddr;
	if (l3->has_ports) {
		fl4->fl4_sport = htons(l3->sport);
		fl4->fl4_dport = htons(l3->dport);
	}
}

static void imsbr_packet_output_v4(struct sk_buff *skb, const struct imsbr_l3 *l3, struct iphdr *ip)
{
	struct flowi4 fl4;
	struct rtable *rt;
	int err;

	imsbr_flowi4_init(&fl4, l3, ip);
	security_skb_classify_flow(skb, flowi4_to_flowi_common(&fl4));

	rt = ip_route_output_key(&init_net, &fl4);
	if (IS_ERR(rt)) {
		IMSBR_STAT_INC(imsbr_stats->ip_route_fail);
		pr_err_ratelimited("ip_route_output_key s=%pI4 d=%pI4 e=%ld\n",
				   &ip->saddr, &ip->daddr, PTR_ERR(rt));
		kfree_skb(skb);
		return;
	}

	skb_dst_set(skb, &rt->dst);
	err = ip_local_out(&init_net, skb->sk, skb);
	if (unlikely(err)) {
		IMSBR_STAT_INC(imsbr_stats->ip_output_fail);
		pr_err_ratelimited("ip_local_out err=%d\n", err);
	}
}

/* mu300: the protocol after the extension headers and the ports from the one parse; the vendor took the first
 * next header for the protocol */
static void imsbr_flowi6_init(struct flowi6 *fl6, const struct imsbr_l3 *l3, struct ipv6hdr *ip6)
{
	memset(fl6, 0, sizeof(*fl6));
	fl6->flowi6_proto = l3->l4proto;
	fl6->saddr = ip6->saddr;
	fl6->daddr = ip6->daddr;
	if (l3->has_ports) {
		fl6->fl6_sport = htons(l3->sport);
		fl6->fl6_dport = htons(l3->dport);
	}
}

static void imsbr_packet_output_v6(struct sk_buff *skb, const struct imsbr_l3 *l3, struct ipv6hdr *ip6)
{
	struct flowi6 fl6;
	struct dst_entry *dst;
	int err;

	imsbr_flowi6_init(&fl6, l3, ip6);
	security_skb_classify_flow(skb, flowi6_to_flowi_common(&fl6));
	fl6.flowi6_mark = skb->mark;
	dst = ip6_route_output(&init_net, NULL, &fl6);
	if (unlikely(dst->error)) {
		IMSBR_STAT_INC(imsbr_stats->ip6_route_fail);
		pr_err_ratelimited("ip6_route_output s=%pI6c d=%pI6c e=%d\n",
				   &ip6->saddr, &ip6->daddr, dst->error);
		dst_release(dst);
		goto freeit;
	}

	dst = xfrm_lookup(&init_net, dst, flowi6_to_flowi(&fl6), NULL, 0);
	if (IS_ERR(dst)) {
		IMSBR_STAT_INC(imsbr_stats->xfrm_lookup_fail);
		pr_err_ratelimited("xfrm_lookup s=%pI6c d=%pI6c p=%d e=%ld\n",
				   &ip6->saddr, &ip6->daddr, ip6->nexthdr,
				   PTR_ERR(dst));
freeit:
		kfree_skb(skb);
		return;
	}

	skb_dst_set(skb, dst);
	err = ip6_local_out(&init_net, skb->sk, skb);
	if (unlikely(err)) {
		IMSBR_STAT_INC(imsbr_stats->ip6_output_fail);
		pr_err_ratelimited("ip6_local_out err=%d\n", err);
	}
}

static void imsbr_packet_output(struct sk_buff *skb, const struct imsbr_l3 *l3)
{

	/* Packets from cp are treated as the local generated packets, also
	 * CP's mtu is ALWAYS less than AP's, so we need not consider the
	 * TCP fragment issue.
	 */
	skb->ignore_df = 1;

	if (l3->family == 4) {
		imsbr_packet_output_v4(skb, l3, ip_hdr(skb));
	} else {
		/* Ipv6 route rule 11000 can not prevent apps to hit this rule,
		 * so add mark 0x80000000 for the rule to avoid apps hitting.
		 * For vowifi pkts,they need add mark for all.
		 */
		skb->mark |= 0x80000000;
		imsbr_packet_output_v6(skb, l3, ipv6_hdr(skb));
	}
}

/* mu300: is the packet's destination an address of dev? */
static bool imsbr_dst_on_dev(struct sk_buff *skb, const struct imsbr_l3 *l3, struct net_device *dev)
{
	struct in_device *in_dev;
	const struct in_ifaddr *ifa;
	bool found = false;

	if (l3->family == 6)
		return ipv6_chk_addr(&init_net, &ipv6_hdr(skb)->daddr, dev, 0);

	rcu_read_lock();
	in_dev = __in_dev_get_rcu(dev);
	if (in_dev) {
		in_dev_for_each_ifa_rcu(ifa, in_dev) {
			if (ifa->ifa_local == ip_hdr(skb)->daddr) {
				found = true;
				break;
			}
		}
	}
	rcu_read_unlock();
	return found;
}

static void imsbr_packet_input(struct sk_buff *skb, const struct imsbr_l3 *l3)
{
	struct net_device *dev;
	struct ethhdr *eth;

	dev = dev_get_by_name(&init_net, input_device);

	/* mu300: only what is addressed to the AP on the input device - the call's address, which the program
	 * running the media put there. Anything else would go on as if received on it: forwarded (the router
	 * forwards), to the LAN or out of an uplink, around the firewall's idea of where it came from. */
	if (dev && !imsbr_dst_on_dev(skb, l3, dev)) {
		IMSBR_STAT_INC(imsbr_stats->ip_route_fail);
		dev_put(dev);
		kfree_skb(skb);
		return;
	}

	if (dev) {
		eth = (struct ethhdr *)skb_mac_header(skb);
		if (l3->family == 4)
			eth->h_proto = htons(ETH_P_IP);
		else
			eth->h_proto = htons(ETH_P_IPV6);

		eth_zero_addr(eth->h_dest);
		eth_zero_addr(eth->h_source);

		skb->protocol = eth->h_proto;
		skb->dev = dev;
		netif_rx(skb);
		dev_put(dev);
	} else {
		kfree_skb(skb);
		pr_crit_ratelimited("device %s is not exist!\n", input_device);
	}
}

void imsbr_dumpcap(struct sk_buff *skb)
{
	const char mac[ETH_ALEN] = {0x88, 0x88, 0x88, 0x88, 0x88, 0x88};
	struct sk_buff *nskb;
	struct ethhdr *eth;
	struct iphdr *ip;
	struct net_device *dev;

	nskb = skb_realloc_headroom(skb, sizeof(struct ethhdr));
	if (!nskb) {
		IMSBR_STAT_INC(imsbr_stats->sk_buff_fail);
		pr_err("skb realloc headroom fail, len=%d\n", skb->len);
		return;
	}

	skb_push(nskb, sizeof(struct ethhdr));
	skb_reset_mac_header(nskb);

	ip = (struct iphdr *)skb_network_header(nskb);
	eth = (struct ethhdr *)skb_mac_header(nskb);
	if (ip->version == 4)
		eth->h_proto = htons(ETH_P_IP);
	else
		eth->h_proto = htons(ETH_P_IPV6);

	ether_addr_copy(eth->h_dest, mac);
	ether_addr_copy(eth->h_source, mac);

	dev = dev_get_by_name(&init_net, backhole_device);
	if (dev) {
		nskb->protocol = eth->h_proto;
		nskb->dev = dev;
		dev_queue_xmit(nskb);
		dev_put(dev);
	} else {
		kfree_skb(nskb);
	}
}

static void imsbr_packet_info(char *prefix, struct sk_buff *skb)
{
	struct iphdr *ip = ip_hdr(skb);

	if (ip->version == 4) {
		pr_info("%s: src=%pI4 dst=%pI4 p=%u len=%u mark=%#x\n",
			prefix, &ip->saddr, &ip->daddr, ip->protocol,
			ntohs(ip->tot_len), skb->mark);
	} else {
		struct ipv6hdr *ip6 = ipv6_hdr(skb);

		pr_info("%s: src=%pI6c dst=%pI6c p=%u len=%u mark=%#x\n",
			prefix, &ip6->saddr, &ip6->daddr, ip6->nexthdr,
			ntohs(ip6->payload_len), skb->mark);
	}
}

static int imsbr_fragsz(void)
{
	int min_val;

	min_val = min_t(int, imsbr_frag_size, IMSBR_PACKET_MAXSZ);
	if (imsbr_frag_size)
		return min_val;

	return IMSBR_PACKET_MAXSZ;
}

static void imsbr_frag_send(struct sk_buff *skb)
{
	struct imsbr_packet _pkthdr;
	struct imsbr_packet *pkt;
	struct sblock blk;
	char *src;
	int totlen = skb->len;
	int offset = 0;
	int len;

	pr_info_ratelimited("pktlen %d exceed max %d, frag it!\n", skb->len,
			    imsbr_fragsz());

	IMSBR_STAT_INC(imsbr_stats->frag_create);

	while (totlen > 0) {
		len = totlen < imsbr_fragsz() ? totlen : imsbr_fragsz();

		if (imsbr_sblock_get(&imsbr_data, &blk, len)) {
			pr_err("frag %d packet fail!\n", skb->len);
			IMSBR_STAT_INC(imsbr_stats->frag_fail);
			return;
		}

		pr_debug("frag offset=%d len=%d reasm_tlen=%d\n",
			 offset, len, skb->len);

		pkt = blk.addr;

		INIT_IMSBR_PACKET(&_pkthdr, skb->len);
		_pkthdr.frag_off = offset;
		memcpy_toio((void __iomem *)pkt, &_pkthdr, sizeof(_pkthdr));

		src = skb->data + offset;
		memcpy_toio((void __iomem *)pkt->packet, src, len);
		imsbr_sblock_send(&imsbr_data, &blk, len);

		totlen -= len;
		offset += len;
	}

	IMSBR_STAT_INC(imsbr_stats->frag_ok);
}

void imsbr_packet_relay2cp(struct sk_buff *skb)
{
	struct sblock blk;
	int len = skb->len;

	IMSBR_STAT_INC(imsbr_stats->pkts_tocp);
	if (__ratelimit(&rlimit_tocp_pkt))
		imsbr_packet_info("relay packet to cp", skb);

	if (unlikely(tcpdump_enable))
		imsbr_dumpcap(skb);

	if (skb_linearize(skb)) {
		IMSBR_STAT_INC(imsbr_stats->sk_buff_fail);
		pr_err("linearize fail, len=%d, drop it!\n", len);
		goto freeit;
	}

	if (likely(len <= imsbr_fragsz())) {
		struct imsbr_packet _pkthdr;
		struct imsbr_packet *pkt;

		if (imsbr_sblock_get(&imsbr_data, &blk, len))
			goto freeit;

		pkt = blk.addr;

		INIT_IMSBR_PACKET(&_pkthdr, len);
		memcpy_toio((void __iomem *)pkt, &_pkthdr, sizeof(_pkthdr));
		memcpy_toio((void __iomem *)pkt->packet, skb->data, len);

		imsbr_sblock_send(&imsbr_data, &blk, len);
	} else {
		imsbr_frag_send(skb);
	}

freeit:
	kfree_skb(skb);
}

static struct sk_buff *
imsbr_pkt_reasm(struct imsbr_packet *phdr, void *pktdata, int pktlen)
{
	const int huge_pktlen = 8192;
	int reasm_tlen = phdr->reasm_tlen;
	char *dst;

	pr_debug("reasm offset=%d len=%d reasm_tlen=%d\n",
		 phdr->frag_off, pktlen, phdr->reasm_tlen);

	mutex_lock(&imsbr_reasm_lock);
	if (phdr->frag_off == 0) {
		IMSBR_STAT_INC(imsbr_stats->reasm_request);
		if (unlikely(reasm_tlen >= huge_pktlen)) {
			pr_info("start to reasm a huge packet len=%d\n",
				reasm_tlen);
		}

		/* Drop residual sk_buff */
		if (imsbr_reasm_skb)
			kfree_skb(imsbr_reasm_skb);
		imsbr_reasm_skb = imsbr_allocskb(reasm_tlen, GFP_KERNEL);
		if (!imsbr_reasm_skb)
			goto fail;
		/* mu300: a fragment that never came would leave the heap's old bytes in the packet */
		memset(imsbr_reasm_skb->data, 0, reasm_tlen);
	} else if (!imsbr_reasm_skb) {
		pr_err("First frag dropped, cannot reasm!\n");
		goto fail;
	}

	if (reasm_tlen > skb_availroom(imsbr_reasm_skb)) {
		pr_crit("reasm_tlen %d is wrong, first frag alloc room is %d\n",
			reasm_tlen, skb_availroom(imsbr_reasm_skb));
		goto fail;
	}

	dst = imsbr_reasm_skb->data + phdr->frag_off;
	memcpy_fromio(dst, (void __iomem *)pktdata, pktlen);

	if (phdr->frag_off + pktlen == reasm_tlen) {
		struct sk_buff *skb = imsbr_reasm_skb;

		imsbr_reasm_skb = NULL;
		IMSBR_STAT_INC(imsbr_stats->reasm_ok);

		skb_put(skb, reasm_tlen);
		mutex_unlock(&imsbr_reasm_lock);
		return skb;
	}

	/**
	 * XXX - If a packet was fraged to 1#,2#,3# and 3# was lost,
	 * we will *leak* imsbr_reasm_skb util the next frag packet
	 * arrival, but it is not worth to *fix* this "bug", this will
	 * just increase complexity. Also *leak* is very hard to
	 * occur!
	 */
	mutex_unlock(&imsbr_reasm_lock);
	return NULL;

fail:
	IMSBR_STAT_INC(imsbr_stats->reasm_fail);
	if (imsbr_reasm_skb) {
		kfree_skb(imsbr_reasm_skb);
		imsbr_reasm_skb = NULL;
	}

	mutex_unlock(&imsbr_reasm_lock);
	return NULL;
}

static bool imsbr_packet_invalid(struct imsbr_packet *phdr, int pktlen)
{
	if (phdr->version != IMSBR_PACKET_VER) {
		pr_err("pkt version %d is not same as %d\n", phdr->version,
		       IMSBR_PACKET_VER);
		return true;
	}

	if (unlikely(phdr->frag_off + pktlen > phdr->reasm_tlen)) {
		pr_err("offset %d+%d is larger than reasm_tlen %d\n",
		       phdr->frag_off, pktlen, phdr->reasm_tlen);
		return true;
	}

	return false;
}

/* mu300: the one parse of the modem's packet (imsbr_parse.h) feeds the check, the classification (the tuple
 * matched against the flows), the routing and the delivery; the packet is cut to the length it found */
static int imsbr_l3_from_cp(struct sk_buff *skb, struct imsbr_l3 *l3, struct nf_conntrack_tuple *nft)
{
	/* built here, so linear; the whole packet is in the head */
	if (skb_is_nonlinear(skb) || !pskb_may_pull(skb, skb->len))
		return -EINVAL;
	if (imsbr_l3_parse(skb->data, skb->len, l3))
		return -EINVAL;
	if (pskb_trim(skb, l3->len))
		return -ENOMEM;
	skb_reset_network_header(skb);
	skb_set_transport_header(skb, l3->l4off);

	memset(nft, 0, sizeof(*nft));
	nft->src.l3num = l3->family == 4 ? AF_INET : AF_INET6;
	nft->dst.protonum = l3->l4proto;
	memcpy(&nft->src.u3, skb->data + l3->saddr_off, l3->alen);
	memcpy(&nft->dst.u3, skb->data + l3->daddr_off, l3->alen);
	if (l3->has_ports) {
		nft->src.u.all = htons(l3->sport);
		nft->dst.u.all = htons(l3->dport);
	}
	return 0;
}

void imsbr_packet_exit(void)
{
	mutex_lock(&imsbr_reasm_lock);
	kfree_skb(imsbr_reasm_skb);
	imsbr_reasm_skb = NULL;
	mutex_unlock(&imsbr_reasm_lock);
}

void imsbr_process_packet(struct imsbr_sipc *sipc, struct sblock *blk,
			  bool freeit)
{
	struct nf_conntrack_tuple nft_tuple;
	struct imsbr_l3 l3;
	struct imsbr_packet _pkthdr;
	struct imsbr_flow *flow;
	struct sk_buff *skb;
	int socket_type;
	char *pktdata;
	int pktlen;
	int media_type = IMSBR_MEDIA_UNSPEC;

	IMSBR_STAT_INC(imsbr_stats->pkts_fromcp);

	memcpy_fromio(&_pkthdr, (void __iomem *)blk->addr, sizeof(_pkthdr));

	pktdata = (char *)blk->addr + sizeof(_pkthdr);
	pktlen = blk->length - sizeof(_pkthdr);

	if (imsbr_packet_invalid(&_pkthdr, pktlen)) {
		if (likely(freeit))
			imsbr_sblock_release(sipc, blk);
		return;
	}

	/* No frags */
	if (likely(_pkthdr.frag_off == 0 && _pkthdr.reasm_tlen == pktlen))
		skb = imsbr_pkt2skb(pktdata, pktlen);
	else
		skb = imsbr_pkt_reasm(&_pkthdr, pktdata, pktlen);

	if (likely(freeit))
		imsbr_sblock_release(sipc, blk);

	if (unlikely(!skb))
		return;

	/* mu300: the modem's bytes: one parse, checked against what there is, before anything reads a header */
	if (imsbr_l3_from_cp(skb, &l3, &nft_tuple)) {
		IMSBR_STAT_INC(imsbr_stats->sk_buff_fail);
		pr_err_ratelimited("packet from cp: %u bytes, not a whole IP packet\n", skb->len);
		kfree_skb(skb);
		return;
	}

	if (tcpdump_enable)
		imsbr_dumpcap(skb);

	/* Default to local input */
	socket_type = IMSBR_SOCKET_AP;

	imsbr_nfct_debug("fromcp", skb, &nft_tuple);

	rcu_read_lock();
	flow = imsbr_flow_match(&nft_tuple);
	if (flow) {
		socket_type = flow->socket_type;
		media_type = flow->media_type;
	}
	rcu_read_unlock();

	/* mu300: the modem's own IMS packets, which the AP would route out for it (VoWiFi): vowifi=1 only */
	if (socket_type == IMSBR_SOCKET_CP && !imsbr_vowifi) {
		kfree_skb(skb);
		return;
	}
	if (socket_type == IMSBR_SOCKET_CP) {
		if (__ratelimit(&rlimit_fromcp_pkt))
			imsbr_packet_info("process packet from cp out", skb);
		if (media_type == IMSBR_MEDIA_SIP) {
			skb->mark |= IMSBR_SKB_MARK_SIP;
		} else if (media_type == IMSBR_MEDIA_IKE) {
			skb->mark |= IMSBR_SKB_MARK_IKE;
		} else if (media_type == IMSBR_MEDIA_RTP_AUDIO ||
			   media_type == IMSBR_MEDIA_RTCP_AUDIO) {
			skb->mark |= IMSBR_SKB_MARK_VOICE;
		}

		imsbr_packet_output(skb, &l3);
	} else {
		skb->mark = vowifi_in_mark;

		if (__ratelimit(&rlimit_fromcp_pkt))
			imsbr_packet_info("process packet from cp in", skb);

		imsbr_packet_input(skb, &l3);
	}
}

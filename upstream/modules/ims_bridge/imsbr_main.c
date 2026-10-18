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

#include <linux/init.h>
#include <linux/module.h>
#include <net/netfilter/nf_conntrack.h>
#include <uapi/linux/ims_bridge/ims_bridge.h>

#include "imsbr_core.h"
#include "imsbr_hooks.h"
#include "imsbr_netlink.h"
#include "imsbr_packet.h"
#include "imsbr_sipc.h"

static int __init imsbr_init(void)
{
	int err;

	BUILD_BUG_ON(sizeof(union imsbr_inet_addr) !=
		     sizeof(union nf_inet_addr));
	BUILD_BUG_ON(sizeof(struct imsbr_tuple) > IMSBR_MSG_MAXLEN);
	/* Prevent the control message size from being too large! */
	BUILD_BUG_ON(IMSBR_CTRL_BLKSZ > 512);

	/* F50: the netfilter hooks use the core's flow table and statistics and send through the
	 * sblocks, so they come after both and go before both (nf_unregister_net_hooks() waits for
	 * the ones running); the vendor order registered them first and removed them last. */
	err = imsbr_core_init();
	if (err)
		goto err_core;
	err = imsbr_sipc_init();
	if (err)
		goto err_sipc;
	err = imsbr_hooks_init();
	if (err)
		goto err_hooks;
	err = imsbr_netlink_init();
	if (err)
		goto err_netlink;

	return 0;

err_netlink:
	imsbr_hooks_exit();
err_hooks:
	imsbr_sipc_exit();
err_sipc:
	imsbr_core_exit();
err_core:
	return err;
}

static void __exit imsbr_exit(void)
{
	imsbr_netlink_exit();
	imsbr_hooks_exit();
	imsbr_sipc_exit();
	imsbr_packet_exit();	/* mu300: a half-reassembled packet was left behind */
	imsbr_core_exit();
}

module_init(imsbr_init);
module_exit(imsbr_exit);

MODULE_AUTHOR("Liping Zhang <liping.zhang@spreadtrum.com>");
MODULE_DESCRIPTION("IMS bridge for two IMS Stacks in both AP and CP");
MODULE_LICENSE("GPL");

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
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/workqueue.h>
#include <uapi/linux/ims_bridge/ims_bridge.h>

#include "imsbr_core.h"
#include "imsbr_packet.h"
#include "imsbr_sipc.h"

/**
 * Temporary solution, will notify CP imsbr whether the volte video's socket
 * is located at AP (0 - disabled 1 - enabled).
 *
 * -- I hope that this ugly codes will be abandoned one day :(
 */
static unsigned int volte_video_apsk __read_mostly = 1;

int imsbr_notify_ltevideo_apsk(void)
{
	int err;
	u32 enable = volte_video_apsk;
	struct sblock blk;

	err = imsbr_build_cmd("ltevideo-apsk", &blk, &enable, sizeof(enable));
	if (unlikely(err < 0))
		return err;

	err = imsbr_sblock_send(&imsbr_ctrl, &blk, sizeof(enable));
	if (unlikely(err < 0))
		return err;

	pr_info("notify ltevideo-apsk=%d success!\n", enable);
	return 0;
}

static int imsbr_pre_notify(struct imsbr_sipc *sipc)
{
	int err, i, max = 10;

	for (i = 0; i < max; i++) {
		err = imsbr_notify_ltevideo_apsk();
		if (!err)
			break;

		/* Waiting for cp to be truely alive! */
		schedule_timeout_interruptible(HZ / 100);
	}

	if (i == max) {
		pr_err_ratelimited("fail to notify cp with ltevideo-apsk msg\n");
		return err;
	}

	return 0;
}

struct imsbr_sipc imsbr_data __read_mostly = {
	.dst		= SIPC_ID_LTE,
	.channel	= SMSG_CH_IMSBR_DATA,
	.hdrlen		= sizeof(struct imsbr_packet),
	.blksize	= IMSBR_DATA_BLKSZ,
	.blknum		= IMSBR_DATA_BLKNUM,
	.peer_ready	= ATOMIC_INIT(0),
	.process	= imsbr_process_packet,
};

struct imsbr_sipc imsbr_ctrl __read_mostly = {
	.dst		= SIPC_ID_LTE,
	.channel	= SMSG_CH_IMSBR_CTRL,
	.hdrlen		= sizeof(struct imsbr_msghdr),
	.blksize	= IMSBR_CTRL_BLKSZ,
	.blknum		= IMSBR_CTRL_BLKNUM,
	.peer_ready	= ATOMIC_INIT(0),
	.pre_hook	= imsbr_pre_notify,
	.process	= imsbr_process_msg,
};

int imsbr_build_cmd(const char *cmd, struct sblock *blk,
		    void *payload, int paylen)
{
	int err;
	struct imsbr_msghdr _msg;

	if (paylen > IMSBR_MSG_MAXLEN) {
		pr_err("paylen %d is too large, max is %d\n",
		       paylen, (int)IMSBR_MSG_MAXLEN);
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	err = imsbr_sblock_get(&imsbr_ctrl, blk, IMSBR_MSG_MAXLEN);
	if (err)
		return err;

	_msg.imsbr_version = IMSBR_MSG_VERSION;
	_msg.imsbr_paylen = paylen;
	strscpy(_msg.imsbr_cmd, cmd, IMSBR_CMD_MAXSZ);

	memcpy_toio((void __iomem *)blk->addr, &_msg, sizeof(_msg));

	if (payload) {
		struct imsbr_msghdr *m = blk->addr;

		memcpy_toio((void __iomem *)m->imsbr_payload, payload, paylen);
	}

	return 0;
}

/* F50: set by imsbr_sipc_exit() so a still-running init work stops retrying instead of
 * registering a notifier and a kthread into a module that is being unloaded, and so the
 * kthreads leave their receive loop. */
static atomic_t imsbr_sipc_stopping = ATOMIC_INIT(0);

int imsbr_sblock_receive(struct imsbr_sipc *sipc, struct sblock *blk)
{
	int err;

	err = sblock_receive(sipc->dst, sipc->channel, blk, -1);
	if (unlikely(err < 0)) {
		IMSBR_STAT_INC(imsbr_stats->sipc_receive_fail);
		/* -ERESTARTSYS is the unload's signal, not a failure */
		if (err != -ERESTARTSYS)
			pr_err_ratelimited("sblock_receive %s fail, error=%d\n",
					   sipc->desc, err);
		return err;
	}

	return 0;
}

int imsbr_sblock_get(struct imsbr_sipc *sipc, struct sblock *blk, int size)
{
	int err;

	size += sipc->hdrlen;

	err = sblock_get(sipc->dst, sipc->channel, blk, 0);
	if (unlikely(err < 0)) {
		IMSBR_STAT_INC(imsbr_stats->sipc_get_fail);
		pr_err_ratelimited("sblock_get %s fail, error=%d\n",
				   sipc->desc, err);
		return err;
	}

	if (unlikely(blk->length < size)) {
		pr_err("sblock len %d is too short, alloc size is %d\n",
		       blk->length, size);
		imsbr_sblock_put(sipc, blk);
		/* the block went back: the caller must not fill and send it */
		return -ENOSPC;
	}

	return 0;
}

int imsbr_sblock_send(struct imsbr_sipc *sipc, struct sblock *blk, int size)
{
	int err;

	blk->length = sipc->hdrlen + size;

	/* F50: sblock_send() fails before it queues the block only when the sblock is not READY;
	 * any later error is smsg_send()'s, with the block in the ring already, and putting it
	 * back then hands it out twice. So that case is checked here, and a block sblock_send()
	 * was given is never put back (should the state change in between, one block is lost
	 * until the modem restarts the ring, rather than shared). */
	if (unlikely(sblock_query(sipc->dst, sipc->channel))) {
		IMSBR_STAT_INC(imsbr_stats->sipc_send_fail);
		sblock_put(sipc->dst, sipc->channel, blk);
		return -EIO;
	}
	err = sblock_send(sipc->dst, sipc->channel, blk);
	if (unlikely(err < 0)) {
		IMSBR_STAT_INC(imsbr_stats->sipc_send_fail);
		pr_err_ratelimited("sblock_send %s fail, error=%d\n",
				   sipc->desc, err);
		return err;
	}

	return 0;
}

void imsbr_sblock_release(struct imsbr_sipc *sipc, struct sblock *blk)
{
	sblock_release(sipc->dst, sipc->channel, blk);
}

void imsbr_sblock_put(struct imsbr_sipc *sipc, struct sblock *blk)
{
	sblock_put(sipc->dst, sipc->channel, blk);
}

static void imsbr_handler(int event, void *data)
{
	struct imsbr_sipc *sipc = (struct imsbr_sipc *)data;

	WARN_ON(!sipc);

	if (!sipc) {
		pr_err("sipc is null\n");
		return;
	}

	/* Receieve event means peer in CP side is ok! */
	if (!atomic_cmpxchg(&sipc->peer_ready, 0, 1)) {
		pr_info("recv %d event, %s's peer is alive!\n",
			event, sipc->desc);
		complete_all(&sipc->peer_comp);
	}

	switch (event) {
	case SBLOCK_NOTIFY_GET:
	case SBLOCK_NOTIFY_RECV:
	case SBLOCK_NOTIFY_STATUS:
	case SBLOCK_NOTIFY_OPEN:
	case SBLOCK_NOTIFY_CLOSE:
		pr_debug("%s recv event %d\n", sipc->desc, event);
		break;
	default:
		pr_err("%s recv event %d\n", sipc->desc, event);
		break;
	}
}

static int imsbr_kthread(void *arg)
{
	struct imsbr_sipc *sipc = arg;

	set_user_nice(current, -20);
	/* F50: the receive below waits with wait_event_interruptible(), which kthread_stop()'s
	 * wakeup does not end: imsbr_sipc_destroy() sends this signal to get the thread out. */
	allow_signal(SIGKILL);
	wait_for_completion_interruptible(&sipc->peer_comp);
	if (kthread_should_stop() || atomic_read(&imsbr_sipc_stopping))
		return 0;
	pr_info("%s is ok, kthread run!\n", sipc->desc);

	/* F50: the vendor thread returned when this failed, leaving the channel dead for good
	 * (the modem's pool can be empty for a moment just after it opens); keep trying. */
	while (sipc->pre_hook) {
		if (kthread_should_stop() || atomic_read(&imsbr_sipc_stopping))
			return 0;
		/* not while the modem is down: every attempt would log */
		if (!sblock_query(sipc->dst, sipc->channel) && sipc->pre_hook(sipc) >= 0)
			break;
		schedule_timeout_interruptible(HZ);
	}

	while (!kthread_should_stop() && !atomic_read(&imsbr_sipc_stopping)) {
		struct sblock blk;

		/* F50: while the modem restarts the sblock is not READY and sblock_receive() fails at
		 * once (with a line of its own each time); sblock_query() says so quietly. */
		if (sblock_query(sipc->dst, sipc->channel)) {
			schedule_timeout_interruptible(HZ);
			continue;
		}
		if (imsbr_sblock_receive(sipc, &blk)) {
			schedule_timeout_interruptible(HZ / 50);
			continue;
		}

		if (blk.length < sipc->hdrlen) {
			pr_err("%s recv len=%d, min=%d, too short!\n",
			       sipc->desc, blk.length, sipc->hdrlen);
			imsbr_sblock_release(sipc, &blk);
		} else if (blk.length > sipc->blksize) {
			pr_err("%s recv len=%d, max=%d, too long!\n",
			       sipc->desc, blk.length, sipc->blksize);
			imsbr_sblock_release(sipc, &blk);
		} else {
			sipc->process(sipc, &blk, true);
		}
	}

	return 0;
}

/* Exported by sipc-core but not declared in its public sipc.h. */
struct sblock_mgr;
struct sblock_mgr *sblock_register_notifier_ex(u8 dst, u8 channel,
					       void (*handler)(int event, void *data),
					       void *data);

static void imsbr_sipc_release(struct imsbr_sipc *sipc)
{
	if (sipc->created) {
		sblock_destroy(sipc->dst, sipc->channel);
		sipc->created = false;
	} else {
		/* spool created this one at boot; tearing it down would leave the channel
		 * gone until a reboot. sblock_register_notifier() refuses to overwrite a
		 * handler, the _ex variant does not. */
		sblock_register_notifier_ex(sipc->dst, sipc->channel, NULL, NULL);
	}
}

static int imsbr_sipc_query_to_register(struct imsbr_sipc *sipc)
{
	struct task_struct *tsk;
	int err;
	int try = 0;

	snprintf(sipc->desc, sizeof(sipc->desc), "%s[%d-%d]",
		 sipc == &imsbr_ctrl ? "ctrl" : "data",
		 sipc->dst, sipc->channel);

	init_completion(&sipc->peer_comp);

	/* F50: nothing on this kernel creates the IMSBR sblocks. This 5.15 version only waits for
	 * someone else's (sblock_query), which on the stock Android kernel is fine; the older
	 * versions of this file (4.14 / kernel-sprd) created them here. Do that when the channel
	 * does not exist at all, then fall through to the same wait for the CP to open its end. */
	err = sblock_query(sipc->dst, sipc->channel);
	if (err == -ENODEV) {
		err = sblock_create(sipc->dst, sipc->channel, sipc->blknum,
				    sipc->blksize, sipc->blknum, sipc->blksize);
		if (err) {
			pr_err("sblock_create %s fail, error=%d\n",
			       sipc->desc, err);
			return err;
		}
		sipc->created = true;
		pr_info("sblock %s created, waiting for the CP to open it\n",
			sipc->desc);
	}
again:
	err = sblock_query(sipc->dst, sipc->channel);
	if (err) {
		//v-huangchaobing@vanyol.com 5378359, 20230321,vowifi reg fail
		if (atomic_read(&imsbr_sipc_stopping))
			return -ESHUTDOWN;
		/* F50: loaded at boot with the modem stack, before the modem runs: its end opens when it
		 * has booted, however long that takes. Waiting on (rather than giving up after 120 s, as
		 * the vendor did for an sblock someone else creates) keeps a slow modem start from leaving
		 * video calls without media until a reload; unloading still ends the wait. */
		if (++try == 120)
			pr_info("sblock [%d-%u] not ready after %d s, still waiting, err %d\n",
				sipc->dst, sipc->channel, try, err);
		msleep(1000);
		goto again;
	}

	err = sblock_register_notifier(sipc->dst, sipc->channel,
				       imsbr_handler, sipc);
	if (err) {
		pr_err("sblock_register_notifier %s fail, error=%d\n",
		       sipc->desc, err);
		/* F50: the handler there is someone else's; leave it (release would clear it) */
		if (sipc->created) {
			sblock_destroy(sipc->dst, sipc->channel);
			sipc->created = false;
		}
		return err;
	}

	/* F50: the thread may end by itself (unloading) before kthread_stop() gets to it, so the
	 * reference is taken before it runs at all */
	tsk = kthread_create(imsbr_kthread, sipc, "imsbr-%s", sipc->desc);
	if (IS_ERR(tsk)) {
		err = PTR_ERR(tsk);
		pr_err("%s kthread_create fail: %d\n", sipc->desc, err);
		goto fail;
	}
	get_task_struct(tsk);
	sipc->task = tsk;
	wake_up_process(tsk);

	/* F50: loaded after boot, so the CP opened its end long ago and the OPEN notify that
	 * would complete peer_comp went to whoever held the sblock then (spool). READY means
	 * the open handshake is done; don't sit waiting for an event that already happened. */
	if (!sblock_query(sipc->dst, sipc->channel) &&
	    !atomic_cmpxchg(&sipc->peer_ready, 0, 1)) {
		pr_info("%s already open on the CP side\n", sipc->desc);
		complete_all(&sipc->peer_comp);
	}
	return 0;

fail:
	imsbr_sipc_release(sipc);
	return err;
}

static void imsbr_sipc_destroy(struct imsbr_sipc *sipc)
{
	if (sipc->task) {
		/* The kthread waits interruptibly, for the CP (peer_comp) and then in every
		 * sblock_receive(); kthread_stop()'s wakeup ends neither (only a signal does), and
		 * rmmod hung for ever. imsbr_sipc_stopping is set already, which the thread checks
		 * after each wait. */
		complete_all(&sipc->peer_comp);
		send_sig(SIGKILL, sipc->task, 1);
		kthread_stop(sipc->task);
		put_task_struct(sipc->task);
		sipc->task = NULL;
	}

	imsbr_sipc_release(sipc);
}

static void imsbr_sipc_init_work(struct work_struct *wk)
{
	struct imsbr_sipc *sipc = container_of(wk, struct imsbr_sipc,
					       initwork);

	pr_debug("sipc %s [%d-%d] workqueue start\n",
		sipc->desc, sipc->dst, sipc->channel);
	imsbr_sipc_query_to_register(sipc);
}

int __init imsbr_sipc_init(void)
{
	INIT_WORK(&imsbr_data.initwork, imsbr_sipc_init_work);
	INIT_WORK(&imsbr_ctrl.initwork, imsbr_sipc_init_work);
	schedule_work(&imsbr_data.initwork);
	schedule_work(&imsbr_ctrl.initwork);

	return 0;
}

void imsbr_sipc_exit(void)
{
	atomic_set(&imsbr_sipc_stopping, 1);
	cancel_work_sync(&imsbr_ctrl.initwork);
	cancel_work_sync(&imsbr_data.initwork);
	imsbr_sipc_destroy(&imsbr_ctrl);
	imsbr_sipc_destroy(&imsbr_data);
}

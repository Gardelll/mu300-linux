/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _IMSBR_SIPC_H
#define _IMSBR_SIPC_H

#include <linux/completion.h>
#include <linux/io.h>
#include <linux/sipc.h>

/*
 * F50: sblock memory is mapped pgprot_noncached by the MU300 sipc-core, i.e. arm64 Device-nGnRnE,
 * where every unaligned access faults. sipc.h's unalign_memcpy() only aligns the head and then
 * calls memcpy(), whose tail does overlapping unaligned loads - the first video RTP packet the CP
 * handed to the AP took the board down with an alignment fault in __pi_memcpy_generic. Every copy
 * in or out of a block therefore goes through memcpy_fromio()/memcpy_toio(), which only make
 * aligned accesses on the I/O side.
 */

#define IMSBR_DATA_BLKSZ	(1500 + 8 + 16) /* MTU + headsize + reserve */
#define IMSBR_DATA_BLKNUM	64

#define IMSBR_CTRL_BLKSZ	128
#define IMSBR_CTRL_BLKNUM	64

#define IMSBR_DESC_NAMESZ	16

struct imsbr_sipc {
	u8		dst;
	u8		channel;
	u16		hdrlen;
	u32		blksize;
	u32		blknum;
	atomic_t	peer_ready;
	struct completion peer_comp;
	char desc[IMSBR_DESC_NAMESZ];
	int (*pre_hook)(struct imsbr_sipc *sipc);
	void (*process)(struct imsbr_sipc *sipc, struct sblock *blk, bool freeit);
	struct task_struct *task;
	struct work_struct initwork;
	bool		created;	/* F50: we made the sblock (vs. spool at boot) */
};

extern struct imsbr_sipc imsbr_data;
extern struct imsbr_sipc imsbr_ctrl;

int imsbr_notify_ltevideo_apsk(void);
int imsbr_build_cmd(const char *cmd, struct sblock *blk,
		    void *payload, int paylen);

int imsbr_sblock_receive(struct imsbr_sipc *sipc, struct sblock *blk);
int imsbr_sblock_get(struct imsbr_sipc *sipc, struct sblock *blk, int size);
int imsbr_sblock_send(struct imsbr_sipc *sipc, struct sblock *blk, int size);
void imsbr_sblock_release(struct imsbr_sipc *sipc, struct sblock *blk);
void imsbr_sblock_put(struct imsbr_sipc *sipc, struct sblock *blk);

int imsbr_sipc_init(void);
void imsbr_sipc_exit(void);

void imsbr_transit(struct imsbr_sipc *sipc, struct sblock *blk,
		   bool freeit);

#endif

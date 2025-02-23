/* TETRA LLC Layer */

/* (C) 2011 by Harald Welte <laforge@gnumonks.org>
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <osmocom/core/msgb.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/bits.h>

#include "tetra_llc_pdu.h"
#include "tetra_mle.h"
#include "tuntap.h"

static int tun_fd = -1;

static struct tllc_state g_llcs = {
	.rx.defrag_list = LLIST_HEAD_INIT(g_llcs.rx.defrag_list),
};

static struct tllc_defrag_q_e *
get_dqe_for_ns(struct tllc_state *llcs, uint8_t ns, int alloc_if_missing)
{
	struct tllc_defrag_q_e *dqe;
	llist_for_each_entry(dqe, &llcs->rx.defrag_list, list) {
		if (dqe->ns == ns)
			return dqe;
	}
	if (alloc_if_missing) {
		dqe = talloc_zero(NULL, struct tllc_defrag_q_e);
		dqe->ns = ns;
		dqe->tl_sdu = msgb_alloc(4096, "LLC defrag");
		llist_add(&dqe->list, &llcs->rx.defrag_list);
	} else
		dqe = NULL;

	return dqe;
}

static int tllc_defrag_in(struct tllc_state *llcs,
			  struct tetra_llc_pdu *lpp,
			  struct msgb *msg, unsigned int len)
{
	struct tllc_defrag_q_e *dqe;

	dqe = get_dqe_for_ns(llcs, lpp->ns, 1);

	/* check if this is the first segment, or the next
	 * expected segment */
	if (!dqe->last_ss ||
	    (dqe->last_ss == lpp->ss - 1)) {
		/* FIXME: append */
		printf("<<APPEND:%u>> ", lpp->ss);
		dqe->last_ss = lpp->ss;
		memcpy(msgb_put(dqe->tl_sdu, len), msg->l3h, len);
	} else
		printf("<<MISS:%u-%u>> ", dqe->last_ss, lpp->ss);

	return 0;
}

static int tllc_defrag_out(struct tetra_mac_state *tms, struct tllc_state *llcs, struct tetra_llc_pdu *lpp)
{
	struct tllc_defrag_q_e *dqe;
	struct msgb *msg;

	dqe = get_dqe_for_ns(llcs, lpp->ns, 0);
	msg = dqe->tl_sdu;

	printf("<<REMOVE>> ");
	msg->l3h = msg->data;
	rx_tl_sdu(tms, msg, msgb_l3len(msg));

	if (tun_fd < 0) {
		tun_fd = tun_alloc("tun0");
		fprintf(stderr, "tun_fd=%d\n", tun_fd);
	}
	if (tun_fd >= 0) {
		uint8_t buf[4096];
		int len = osmo_ubit2pbit(buf, msg->l3h+3+4+4+4+4, msgb_l3len(msg)-3-4-4-4-4);
		write(tun_fd, buf, len);
	}

	llist_del(&dqe->list);
	talloc_free(msg);
	talloc_free(dqe);
	return 0;
}

/* Receive TM-SDU (MAC SDU == LLC PDU) */
/* this resembles TMA-UNITDATA.ind (TM-SDU / length) */
int rx_tm_sdu(struct tetra_mac_state *tms, struct msgb *msg, unsigned int len)
{
	if (!len) {
		return -1;
	} else if (len < 4) {
		printf("WARNING rx_tm_sdu: l2len too small: %d\n", len);
		return -1;
	}

	struct tetra_llc_pdu lpp;
	memset(&lpp, 0, sizeof(lpp));
	tetra_llc_pdu_parse(&lpp, msg->l2h, len);
	msg->l3h = lpp.tl_sdu;
	msg->tail = msg->l3h + lpp.tl_sdu_len; // Strips off FCS (if present)

	printf("TM-SDU(%s)", tetra_get_llc_pdut_dec_name(lpp.pdu_type));
	if (lpp.have_fcs) {
		printf(" fcs=%s ", (lpp.have_fcs && lpp.fcs_invalid ? "BAD" : "OK"));
	}
	printf(" l3len=%d", msgb_l3len(msg));
	if (msgb_l3len(msg)) {
		printf(" %s", osmo_ubit_dump(msg->l3h, msgb_l3len(msg)));
	}
	printf("\n");

	if (!lpp.tl_sdu_len) {
		return len;
	}

	switch (lpp.pdu_type) {
	case TLLC_PDUT_DEC_BL_ADATA:
	case TLLC_PDUT_DEC_BL_DATA:
	case TLLC_PDUT_DEC_BL_UDATA:
	case TLLC_PDUT_DEC_BL_ACK:
	case TLLC_PDUT_DEC_AL_SETUP:
	case TLLC_PDUT_DEC_AL_ACK:
	case TLLC_PDUT_DEC_AL_RNR:
	case TLLC_PDUT_DEC_AL_RECONNECT:
	case TLLC_PDUT_DEC_AL_DISC:
		/* directly hand it to MLE */
		rx_tl_sdu(tms, msg, lpp.tl_sdu_len);
		break;
	case TLLC_PDUT_DEC_AL_DATA:
	case TLLC_PDUT_DEC_AL_UDATA:
	case TLLC_PDUT_DEC_ALX_DATA:
	case TLLC_PDUT_DEC_ALX_UDATA:
		/* input into LLC defragmenter */
		tllc_defrag_in(&g_llcs, &lpp, msg, len);
		break;
	case TLLC_PDUT_DEC_AL_FINAL:
	case TLLC_PDUT_DEC_AL_UFINAL:
	case TLLC_PDUT_DEC_ALX_FINAL:
	case TLLC_PDUT_DEC_ALX_UFINAL:
		/* input into LLC defragmenter */
		tllc_defrag_in(&g_llcs, &lpp, msg, len);
		/* check if the fragment is complete and hand it off*/
		tllc_defrag_out(tms, &g_llcs, &lpp);
		break;

	case TLLC_PDUT_DEC_UNKNOWN:
	case TLLC_PDUT_DEC_ALX_ACK:
	case TLLC_PDUT_DEC_ALX_RNR:
		/* fixme: unhandled types */
		break;
	}

	return len;
}

/*
 * WSM host interface (HI) implementation for Silicon Labs WFX mac80211 drivers
 *
 * Copyright (c) 2017, Silicon Laboratories, Inc.
 * Copyright (c) 2010, ST-Ericsson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/skbuff.h>
#include <linux/etherdevice.h>

#include "wsm.h"
#include "wfx_version.h"
#include "wfx.h"
#include "bh.h"
#include "debug.h"
#include "sta.h"
#include "testmode.h"

static int wsm_generic_confirm(struct wfx_dev *wdev, HiMsgHdr_t *hdr, void *buf)
{
	// FIXME: Use interface id from wsm->s.b.IntId
	struct wfx_vif *wvif = wdev_to_wvif(wdev);
	// All confirm messages start with Status
	int status = le32_to_cpu(*((__le32 *) buf));
	int cmd = hdr->s.t.MsgId;
	int len = hdr->MsgLen - 4; // drop header
	int wsm_cmd;
	void *wsm_arg;

	spin_lock(&wdev->wsm_cmd.lock);
	wsm_arg = wdev->wsm_cmd.arg;
	wsm_cmd = wdev->wsm_cmd.cmd;
	spin_unlock(&wdev->wsm_cmd.lock);

	if (cmd != wsm_cmd) {
		dev_warn(wdev->pdev, "Chip response mismatch request: %#.4X vs %#.4X\n", cmd, wsm_cmd);
		return -EINVAL;
	}

	if (status)
		dev_warn(wdev->pdev, "WSM request %s %08x returned error %d\n",
				get_wsm_name(cmd), cmd, status);

	// FIXME: check that if wsm_arg is provided, caller allocated enough bytes
	// FIXME: access to wsm_arg should be inner spinlock. (but mutex from
	// wsm_tx also protect it).
	if (wsm_arg)
		memcpy(wsm_arg, buf, len);

	// Legacy chip have a special management for this case.
	// Is it still necessary?
	WARN_ON(status && wvif->join_status >= WFX_JOIN_STATUS_JOINING);

	spin_lock(&wdev->wsm_cmd.lock);
	wdev->wsm_cmd.ret = status;
	wdev->wsm_cmd.done = 1;
	spin_unlock(&wdev->wsm_cmd.lock);

	wake_up(&wdev->wsm_cmd_wq);

	return status;
}

static int wsm_tx_confirm(struct wfx_dev *wdev, HiMsgHdr_t *hdr, void *buf)
{
	WsmHiTxCnfBody_t *body = buf;

	wfx_tx_confirm_cb(wdev, body);
	return 0;
}

static int wsm_multi_tx_confirm(struct wfx_dev *wdev, HiMsgHdr_t *hdr, void *buf)
{
	WsmHiMultiTransmitCnfBody_t *body = buf;
	WsmHiTxCnfBody_t *buf_loc = (WsmHiTxCnfBody_t *) &body->TxConfPayload;
	int count = body->NumTxConfs;
	int ret = 0;
	int i;

	if (count <= 0) {
		wfx_err(
			"Number of transmit confirmation message payload error %d\n",
			count);
		return -EINVAL;
	}

	if (count > 1) {
		ret = wsm_release_tx_buffer(wdev, count - 1);
		if (ret < 0) {
			wfx_err("Can not release transmit buffer");
			return ret;
		} else if (ret > 0) {
			wfx_bh_wakeup(wdev);
	}
	}

	wfx_debug_txed_multi(wdev, count);
	for (i = 0; i < count; ++i) {
		wfx_tx_confirm_cb(wdev, buf_loc);
		buf_loc++;
	}
	return ret;
}

int wfx_unmap_link(struct wfx_vif *wvif, int sta_id)
{
	WsmHiMapLinkReqBody_t maplink = {
		.PeerStaId	= sta_id,
		.Flags		= true,
	};

	if (sta_id)
		ether_addr_copy(&maplink.MacAddr[0], wvif->link_id_db[sta_id - 1].old_mac);

	return wsm_map_link(wvif->wdev, &maplink);
}

int wsm_set_probe_responder(struct wfx_vif *wvif, bool enable)
{
	wvif->rx_filter.probeResponder = enable;
	return wsm_set_rx_filter(wvif->wdev, &wvif->rx_filter);
}


static int wsm_startup_indication(struct wfx_dev *wdev, HiMsgHdr_t *hdr, void *buf)
{
	HiStartupIndBody_t *body = buf;

	memcpy(&wdev->wsm_caps, body, sizeof(HiStartupIndBody_t));
	if (body->Status || body->FirmwareType > 4) {
		dev_err(wdev->pdev, "Received invalid startup indication");
		return -EINVAL;
	}

	dev_info(wdev->pdev, "Firmware \"%s\" started. API: %.2x caps: %#.8X\n",
		 body->FirmwareLabel, body->ApiVersion,
		 *((uint32_t *) &body->Capabilities));

	/* Disable unsupported frequency bands */
/*    if (!(wdev->wsm_caps.FirmwareCap & 0x1)) */
/*        wdev->hw->wiphy->bands[NL80211_BAND_2GHZ] = NULL; */
/*    if (!(wdev->wsm_caps.FirmwareCap & 0x2)) */
	wdev->hw->wiphy->bands[NL80211_BAND_5GHZ] = NULL;

	wdev->firmware_ready = 1;
	wake_up(&wdev->wsm_startup_done);
	return 0;
}

static int wsm_receive_indication(struct wfx_dev	*wdev,
				  struct wsm_buf *buf,
				  struct sk_buff **skb_p)
{
	WsmHiRxIndBody_t rx;
	// FIXME: Get interface id from wsm_buf or if_id
	struct wfx_vif *wvif = wdev_to_wvif(wdev);
	struct ieee80211_hdr *hdr;
	size_t hdr_len;
	__le16 fctl;
	int sta_id;

	memcpy(&rx, &((WsmHiRxInd_t *)buf->begin)->Body,
	       sizeof(WsmHiRxIndBody_t));

	/* hdr_len
	 * size of RX indication:
	 * size of the frame_ctl and Msginfo
	 * Size of the Frame
	 */
	hdr_len = (sizeof(WsmHiRxIndBody_t) + sizeof(uint32_t)) -
		  (sizeof(((WsmHiRxIndBody_t *)0)->Frame));

	skb_pull(*skb_p, hdr_len);


	hdr = (struct ieee80211_hdr *)(*skb_p)->data;

	if (!rx.RcpiRssi &&
	    (ieee80211_is_probe_resp(hdr->frame_control) ||
	     ieee80211_is_beacon(hdr->frame_control)))
		return 0;

	if (!wvif->cqm_use_rssi)
		rx.RcpiRssi = rx.RcpiRssi / 2 - 110;

	fctl = hdr->frame_control;
	pr_debug("[WSM] \t\t rx_flags=0x%.8X, frame_ctrl=0x%.4X\n",
		 *((uint32_t *)&rx.RxFlags), le16_to_cpu(fctl));


	sta_id = rx.RxFlags.PeerStaId;

	wfx_rx_cb(wvif, &rx, sta_id, skb_p);
	if (*skb_p)
		skb_push(*skb_p, hdr_len);

	return 0;
}

static int wsm_event_indication(struct wfx_dev *wdev, struct wsm_buf *buf)
{
	int first;
	struct wfx_wsm_event *event;
	// FIXME: Get interface id from wsm_buf
	struct wfx_vif *wvif = wdev_to_wvif(wdev);

	if (wvif->mode == NL80211_IFTYPE_UNSPECIFIED)
		/* STA is stopped. */
		return 0;

	event = kzalloc(sizeof(*event), GFP_KERNEL);
	if (!event)
		return -ENOMEM;

	memcpy(&event->evt, &((WsmHiEventInd_t *)buf->begin)->Body,
	       sizeof(WsmHiEventIndBody_t));

	pr_debug("[WSM] Event: %d(%d)\n",
		 event->evt.EventId, *((uint32_t *)&event->evt.EventData));

	spin_lock(&wvif->event_queue_lock);
	first = list_empty(&wvif->event_queue);
	list_add_tail(&event->link, &wvif->event_queue);
	spin_unlock(&wvif->event_queue_lock);

	if (first)
		queue_work(wdev->workqueue, &wvif->event_handler);

	return 0;
}

static int wsm_channel_switch_indication(struct wfx_dev *wdev, HiMsgHdr_t *hdr, void *buf)
{
	WsmHiSwitchChannelCnfBody_t *body = buf;

	if (body->Status) {
		wfx_err("Failed to receive: indication during channel switch");
		return -EINVAL;
	}
	wdev->channel_switch_in_progress = 0;
	wake_up(&wdev->channel_switch_done);
	wsm_unlock_tx(wdev);

	return 0;
}

static int wsm_set_pm_indication(struct wfx_dev *wdev, HiMsgHdr_t *hdr, void *buf)
{
	if (wdev->ps_mode_switch_in_progress) {
		wdev->ps_mode_switch_in_progress = 0;
		wake_up(&wdev->ps_mode_switch_done);
	}
	return 0;
}

static int wsm_scan_complete_indication(struct wfx_dev *wdev, HiMsgHdr_t *hdr, void *buf)
{
	WsmHiScanCmplIndBody_t *body = buf;

	wfx_scan_complete_cb(wdev, body);

	return 0;
}

static int wsm_join_complete_indication(struct wfx_dev *wdev, HiMsgHdr_t *hdr, void *buf)
{
	// FIXME: Get interface id from hdr
	struct wfx_vif *wvif = wdev_to_wvif(wdev);
	WsmHiJoinCompleteIndBody_t *body = buf;

	pr_debug("[WSM] Join complete indication, status: %d\n", body->Status);
	wfx_join_complete_cb(wvif, body);

	return 0;
}

static int wsm_dbg_info_indication(struct wfx_dev *wdev, HiMsgHdr_t *hdr, void *buf)
{
	WsmHiDebugIndBody_t *body = buf;
	int msgLen = hdr->MsgLen;

	switch (body->DbgId) {
	case 6:
		wfx_err("dbg msg CPU profiling : cpu_load=%d\n",
			body->DbgData.EptaRtStats.MsgStartIdentifier);
		break;
	case 7:
		wfx_testmode_bs_buffer_add((uint8_t *) &body->DbgData, msgLen - 8);
		break;
	default:
		break;
	}

	return 0;
}

static int wsm_ba_timeout_indication(struct wfx_dev *wdev, HiMsgHdr_t *hdr, void *buf)
{
	WsmHiBaTimeoutIndBody_t *body = buf;

	wfx_info("BlockACK timeout, tid %d, addr %pM\n", body->TID, body->TransmitAddress);

	return 0;
}

static int wsm_suspend_resume_indication(struct wfx_dev *wdev,
					 struct wsm_buf *buf)
{
	WsmHiSuspendResumeTxIndBody_t arg;

	memcpy(&arg, &((WsmHiSuspendResumeTxInd_t *)buf->begin)->Body,
	       sizeof(WsmHiSuspendResumeTxIndBody_t));

	wfx_suspend_resume(wdev, &arg);

	return 0;
}

static int wsm_error_indication(struct wfx_dev *wdev, struct wsm_buf *buf)
{
	HiErrorIndBody_t *Body = &((HiErrorInd_t *)buf->begin)->Body;

	wfx_err(" : type 0x%x\n", Body->Type);
	return 0;
}

static int wsm_generic_indication(struct wfx_dev *wdev,
				  struct wsm_buf *buf, __le16 msgLen)
{
	HiGenericIndBody_t *Body = &((HiGenericInd_t *)buf->begin)->Body;
	HiRxStats_t *rx_stats;

	switch (Body->IndicationId) {
	case  HI_GENERIC_INDICATION_ID_RAW:
		/* Not used yet */
		break;
	case HI_GENERIC_INDICATION_ID_STRING:
		/* Display received message */
		wfx_info("%s", (char *)Body->IndicationData.RawData);
		break;
	case HI_GENERIC_INDICATION_ID_RX_STATS:
		rx_stats = &Body->IndicationData.RxStats;
		wfx_info(
			"Receiving new Rx statistics t = %dus:\n"
			"NbFrame %d, PERx10000 %d , throughput %dKbps/s\n"
			"NbFrame by rate:\n"
			"\t1Mbps %d, 2Mpbs %d, 5.5Mbps %d, 11Mpbs %d\n"
			"\t6Mbps %d, 9Mpbs %d, 12Mbps %d, 18Mpbs %d, 24Mbps %d, 36Mbps %d, 48Mbps %d, 54Mbps %d\n"
			"\tMCS0 %d, MCS1 %d, MCS2 %d, MCS3 %d, MCS4 %d, MCS5 %d, MCS6 %d, MCS7 %d\n"
			"PERx10000 by rate:\n"
			"\t1Mbps %d, 2Mpbs %d, 5.5Mbps %d, 11Mpbs %d\n"
			"\t6Mbps %d, 9Mpbs %d, 12Mbps %d, 18Mpbs %d, 24Mbps %d, 36Mbps %d, 48Mbps %d, 54Mbps %d\n"
			"\tMCS0 %d, MCS1 %d, MCS2 %d, MCS3 %d, MCS4 %d, MCS5 %d, MCS6 %d, MCS7 %d\n",
			rx_stats->Date,
			rx_stats->NbRxFrame, rx_stats->PerTotal, rx_stats->Throughput,
			rx_stats->NbRxByRate[0], rx_stats->NbRxByRate[1],
			rx_stats->NbRxByRate[2], rx_stats->NbRxByRate[3],
			rx_stats->NbRxByRate[6], rx_stats->NbRxByRate[7],
			rx_stats->NbRxByRate[8], rx_stats->NbRxByRate[9],
			rx_stats->NbRxByRate[10], rx_stats->NbRxByRate[11],
			rx_stats->NbRxByRate[12], rx_stats->NbRxByRate[13],
			rx_stats->NbRxByRate[14], rx_stats->NbRxByRate[15],
			rx_stats->NbRxByRate[16], rx_stats->NbRxByRate[17],
			rx_stats->NbRxByRate[18], rx_stats->NbRxByRate[19],
			rx_stats->NbRxByRate[20], rx_stats->NbRxByRate[21],
			rx_stats->Per[0], rx_stats->Per[1], rx_stats->Per[2],
			rx_stats->Per[3],
			rx_stats->Per[6], rx_stats->Per[7], rx_stats->Per[8],
			rx_stats->Per[9], rx_stats->Per[10], rx_stats->Per[11],
			rx_stats->Per[12], rx_stats->Per[13],
			rx_stats->Per[14], rx_stats->Per[15], rx_stats->Per[16],
			rx_stats->Per[17], rx_stats->Per[18], rx_stats->Per[19],
			rx_stats->Per[20], rx_stats->Per[21]);
		wfx_info(
			"Receiving new Rx statistics part2:\n"
			"RSSI(dB) by rate:\n"
			"\t1Mbps %d, 2Mpbs %d, 5.5Mbps %d, 11Mpbs %d\n"
			"\t6Mbps %d, 9Mpbs %d, 12Mbps %d, 18Mpbs %d, 24Mbps %d, 36Mbps %d, 48Mbps %d, 54Mbps %d\n"
			"\tMCS0 %d, MCS1 %d, MCS2 %d, MCS3 %d, MCS4 %d, MCS5 %d, MCS6 %d, MCS7 %d\n"
			"SNR (dB) by rate:\n"
			"\t1Mbps %d, 2Mpbs %d, 5.5Mbps %d, 11Mpbs %d\n"
			"\t6Mbps %d, 9Mpbs %d, 12Mbps %d, 18Mpbs %d, 24Mbps %d, 36Mbps %d, 48Mbps %d, 54Mbps %d\n"
			"\tMCS0 %d, MCS1 %d, MCS2 %d, MCS3 %d, MCS4 %d, MCS5 %d, MCS6 %d, MCS7 %d\n"
			"CFO by rate (in kHz):\n"
			"\t1Mbps %d, 2Mpbs %d, 5.5Mbps %d, 11Mpbs %d\n"
			"\t6Mbps %d, 9Mpbs %d, 12Mbps %d, 18Mpbs %d, 24Mbps %d, 36Mbps %d, 48Mbps %d, 54Mbps %d\n"
			"\tMCS0 %d, MCS1 %d, MCS2 %d, MCS3 %d, MCS4 %d, MCS5 %d, MCS6 %d, MCS7 %d\n",
			(s16)rx_stats->Rssi[0] / 100, (s16)rx_stats->Rssi[1] / 100,
			(s16)rx_stats->Rssi[2] / 100, (s16)rx_stats->Rssi[3] / 100,
			(s16)rx_stats->Rssi[6] / 100, (s16)rx_stats->Rssi[7] / 100,
			(s16)rx_stats->Rssi[8] / 100, (s16)rx_stats->Rssi[9] / 100,
			(s16)rx_stats->Rssi[10] / 100, (s16)rx_stats->Rssi[11] / 100,
			(s16)rx_stats->Rssi[12] / 100, (s16)rx_stats->Rssi[13] / 100,
			(s16)rx_stats->Rssi[14] / 100, (s16)rx_stats->Rssi[15] / 100,
			(s16)rx_stats->Rssi[16] / 100, (s16)rx_stats->Rssi[17] / 100,
			(s16)rx_stats->Rssi[18] / 100, (s16)rx_stats->Rssi[19] / 100,
			(s16)rx_stats->Rssi[20] / 100, (s16)rx_stats->Rssi[21] / 100,
			(s16)rx_stats->Snr[0] / 100, (s16)rx_stats->Snr[1] / 100,
			(s16)rx_stats->Snr[2] / 100, (s16)rx_stats->Snr[3] / 100,
			(s16)rx_stats->Snr[6] / 100, (s16)rx_stats->Snr[7] / 100,
			(s16)rx_stats->Snr[8] / 100, (s16)rx_stats->Snr[9] / 100,
			(s16)rx_stats->Snr[10] / 100, (s16)rx_stats->Snr[11] / 100,
			(s16)rx_stats->Snr[12] / 100, (s16)rx_stats->Snr[13] / 100,
			(s16)rx_stats->Snr[14] / 100, (s16)rx_stats->Snr[15] / 100,
			(s16)rx_stats->Snr[16] / 100, (s16)rx_stats->Snr[17] / 100,
			(s16)rx_stats->Snr[18] / 100, (s16)rx_stats->Snr[19] / 100,
			(s16)rx_stats->Snr[20] / 100, (s16)rx_stats->Snr[21] / 100,
			(s16)rx_stats->Cfo[0], (s16)rx_stats->Cfo[1],
			(s16)rx_stats->Cfo[2], (s16)rx_stats->Cfo[3],
			(s16)rx_stats->Cfo[6], (s16)rx_stats->Cfo[7],
			(s16)rx_stats->Cfo[8], (s16)rx_stats->Cfo[9],
			(s16)rx_stats->Cfo[10], (s16)rx_stats->Cfo[11],
			(s16)rx_stats->Cfo[12], (s16)rx_stats->Cfo[13],
			(s16)rx_stats->Cfo[14], (s16)rx_stats->Cfo[15],
			(s16)rx_stats->Cfo[16], (s16)rx_stats->Cfo[17],
			(s16)rx_stats->Cfo[18], (s16)rx_stats->Cfo[19],
			(s16)rx_stats->Cfo[20], (s16)rx_stats->Cfo[21]);
		wfx_info(
			"External power clock %u, frequency %u:\n",
			rx_stats->IsExtPwrClk,
			rx_stats->PwrClkFreq);
		break;
	default:
		wfx_err("wrong type in generic_ind\n");
		return -1;
}

	return 0;
}

void wsm_lock_tx(struct wfx_dev *wdev)
{
	wsm_cmd_lock(wdev);
	if (atomic_add_return(1, &wdev->tx_lock) == 1)
		if (wsm_flush_tx(wdev))
			pr_debug("[WSM] TX is locked.\n");
	wsm_cmd_unlock(wdev);
}

void wsm_lock_tx_async(struct wfx_dev *wdev)
{
	if (atomic_add_return(1, &wdev->tx_lock) == 1)
		pr_debug("[WSM] TX is locked (async).\n");
}

bool wsm_flush_tx(struct wfx_dev *wdev)
{
	unsigned long timestamp = jiffies;
	long timeout;

	/* Flush must be called with TX lock held. */
	BUG_ON(!atomic_read(&wdev->tx_lock));
	/* First check if we really need to do something.
	 * It is safe to use unprotected access, as hw_bufs_used
	 * can only decrements.
	 */
	if (!wdev->hw_bufs_used)
		return true;

	if (wdev->bh_error) {
		/* In case of failure do not wait for magic. */
		wfx_err("[WSM] Fatal error occurred, will not flush TX.\n");
		return false;
	} else {
		bool pending = false;
		int i;

		/* Get a timestamp of "oldest" frame */
		for (i = 0; i < 4; ++i)
			pending |= wfx_queue_get_xmit_timestamp(
				&wdev->tx_queue[i],
					&timestamp, 0xffffffff);
		/* If there's nothing pending, we're good */
		if (!pending)
			return true;

		timeout = timestamp + WSM_CMD_LAST_CHANCE_TIMEOUT - jiffies;
		if (timeout < 0 || wait_event_timeout(wdev->bh_evt_wq,
						      !wdev->hw_bufs_used,
						      timeout) <= 0) {
			/* Hmmm... Not good. Frame had stuck in firmware. */
			wdev->bh_error = 1;
			wiphy_err(wdev->hw->wiphy,
				  "[WSM] TX Frames (%d) stuck in firmware, killing BH\n",
				  wdev->hw_bufs_used);
			wake_up(&wdev->bh_wq);
			return false;
		}
		/* Ok, everything is flushed. */
		return true;
	}
}

void wsm_unlock_tx(struct wfx_dev *wdev)
{
	int tx_lock;

	tx_lock = atomic_sub_return(1, &wdev->tx_lock);
	BUG_ON(tx_lock < 0);

	if (tx_lock == 0) {
		if (!wdev->bh_error)
			wfx_bh_wakeup(wdev);
		pr_debug("[WSM] TX is unlocked.\n");
	}
}

int wsm_handle_exception(struct wfx_dev *wdev, void *data, size_t len)
{
	HiExceptionIndBody_t *body = data;
	static const char * const reason_str[] = {
		"undefined instruction",
		"prefetch abort",
		"data abort",
		"unknown error",
	};

	if (len < sizeof(HiExceptionIndBody_t)) {
		dev_err(wdev->pdev, "Firmware exception.\n");
		print_hex_dump_bytes("Exception: ", DUMP_PREFIX_NONE, data, len);
		return -EINVAL;
	}

	if (body->Reason < 4) {
		dev_err(wdev->pdev, "Firmware exception: %s\n",
			reason_str[body->Reason]);
		return 0;
	}

	dev_err(wdev->pdev, "Firmware assert: id %d, error code %X\n",
		body->Reserved_1, body->Reserved_2);

	return 0;
}

int wsm_handle_rx(struct wfx_dev *wdev, HiMsgHdr_t *wsm,
		  struct sk_buff **skb_p)
{
	int ret = 0;
	u8 wsm_id = wsm->s.t.MsgId;
	struct wsm_buf wsm_buf;

	wsm_buf.begin = (u8 *)&wsm[0];
	wsm_buf.data = (u8 *)&wsm[1];
	wsm_buf.end = &wsm_buf.begin[__le16_to_cpu(wsm->MsgLen)];

	if (wsm_id == WSM_HI_TX_CNF_ID) {
		ret = wsm_tx_confirm(wdev, &wsm[0], &wsm[1]);
	} else if (wsm_id == WSM_HI_MULTI_TRANSMIT_CNF_ID) {
		ret = wsm_multi_tx_confirm(wdev, &wsm[0], &wsm[1]);
	} else if (!(wsm_id & HI_MSG_TYPE_MASK)) {
		ret = wsm_generic_confirm(wdev, &wsm[0], &wsm[1]);
	} else {
		switch (wsm_id) {
		case HI_STARTUP_IND_ID:
			ret = wsm_startup_indication(wdev, &wsm[0], &wsm[1]);
			break;
		case WSM_HI_RX_IND_ID:
			ret = wsm_receive_indication(wdev,
						     &wsm_buf, skb_p);
			break;
		case WSM_HI_EVENT_IND_ID:
			ret = wsm_event_indication(wdev, &wsm_buf);
			break;
		case WSM_HI_SCAN_CMPL_IND_ID:
			ret = wsm_scan_complete_indication(wdev, &wsm[0], &wsm[1]);
			break;
		case WSM_HI_BA_TIMEOUT_IND_ID:
			ret = wsm_ba_timeout_indication(wdev, &wsm[0], &wsm[1]);
			break;
		case WSM_HI_SET_PM_MODE_CMPL_IND_ID:
			ret = wsm_set_pm_indication(wdev, &wsm[0], &wsm[1]);
			break;
		case WSM_HI_SWITCH_CHANNEL_IND_ID:
			ret = wsm_channel_switch_indication(wdev, &wsm[0], &wsm[1]);
			break;
		case WSM_HI_SUSPEND_RESUME_TX_IND_ID:
			ret = wsm_suspend_resume_indication(wdev,
					&wsm_buf);
			break;
		case WSM_HI_DEBUG_IND_ID:
			ret = wsm_dbg_info_indication(wdev, &wsm[0], &wsm[1]);
			break;
		case WSM_HI_JOIN_COMPLETE_IND_ID:
			ret = wsm_join_complete_indication(wdev, &wsm[0], &wsm[1]);
			break;
		case HI_ERROR_IND_ID:
			ret = wsm_error_indication(wdev, &wsm_buf);
			break;
		case HI_GENERIC_IND_ID:
			ret = wsm_generic_indication(wdev, &wsm_buf,
						       wsm->MsgLen);
			break;
		default:
			wfx_err("Unrecognised WSM ID %02x\n", wsm_id);
		}
	}

	return ret;
}

static bool wsm_handle_tx_data(struct wfx_vif		*wvif,
			       WsmHiTxReq_t			*wsm,
			       const struct ieee80211_tx_info *tx_info,
			       const struct wfx_txpriv *txpriv,
			       struct wfx_queue *queue)
{
	bool handled = false;
	const struct ieee80211_hdr *frame =
		(struct ieee80211_hdr *)&((u8 *)wsm)[txpriv->offset];
	__le16 fctl = frame->frame_control;

	enum {
		do_probe,
		do_drop,
		do_wep,
		do_tx,
	} action = do_tx;

	switch (wvif->mode) {
	case NL80211_IFTYPE_STATION:
		if (wvif->join_status == WFX_JOIN_STATUS_MONITOR)
			action = do_tx;
		else if (wvif->join_status < WFX_JOIN_STATUS_PRE_STA)
			action = do_drop;
		break;
	case NL80211_IFTYPE_AP:
		if (!wvif->join_status) {
			action = do_drop;
		} else if (!(BIT(txpriv->raw_link_id) &
		      (BIT(0) | wvif->link_id_map))) {
			wiphy_warn(wvif->wdev->hw->wiphy,
				   "A frame with expired link id is dropped.\n");
			action = do_drop;
		}
		if (wfx_queue_get_generation(wsm->Body.PacketId) >
				WFX_MAX_REQUEUE_ATTEMPTS) {
			wiphy_warn(wvif->wdev->hw->wiphy,
				   "Too many attempts to requeue a frame; dropped.\n");
			action = do_drop;
		}
		break;
	case NL80211_IFTYPE_ADHOC:
		if (wvif->join_status != WFX_JOIN_STATUS_IBSS)
			action = do_drop;
		break;
	case NL80211_IFTYPE_MESH_POINT:
		action = do_tx;
		break;
	case NL80211_IFTYPE_MONITOR:
	default:
		action = do_drop;
		break;
	}

	if (action == do_tx) {
		if (ieee80211_is_nullfunc(fctl)) {
			mutex_lock(&wvif->bss_loss_lock);
			if (wvif->bss_loss_state) {
				wvif->bss_loss_confirm_id = wsm->Body.PacketId;
				wsm->Body.QueueId.QueueId = WSM_QUEUE_ID_VOICE;
			}
			mutex_unlock(&wvif->bss_loss_lock);
		} else if (ieee80211_is_probe_req(fctl)) {
			action = do_probe;
		} else if (ieee80211_has_protected(fctl) &&
			   tx_info->control.hw_key &&
		    tx_info->control.hw_key->keyidx !=
		    wvif->wep_default_key_id &&
		    (tx_info->control.hw_key->cipher ==
		     WLAN_CIPHER_SUITE_WEP40 ||
		     tx_info->control.hw_key->cipher ==
		     WLAN_CIPHER_SUITE_WEP104)) {
			action = do_wep;
		}
	}

	switch (action) {
	case do_probe:
		pr_debug("[WSM] Convert probe request to scan.\n");
		wsm_lock_tx_async(wvif->wdev);
		wvif->wdev->pending_frame_id = wsm->Body.PacketId;
		if (queue_delayed_work(wvif->wdev->workqueue,
				       &wvif->wdev->scan.probe_work, 0) <= 0)
			wsm_unlock_tx(wvif->wdev);
		handled = true;
		break;
	case do_drop:
		pr_debug("[WSM] Drop frame (0x%.4X).\n", fctl);
		BUG_ON(wfx_queue_remove(queue, wsm->Body.PacketId));
		handled = true;
		break;
	case do_wep:
		pr_debug("[WSM] Issue set_default_wep_key.\n");
		wsm_lock_tx_async(wvif->wdev);
		wvif->wep_default_key_id = tx_info->control.hw_key->keyidx;
		wvif->wdev->pending_frame_id = wsm->Body.PacketId;
		if (queue_work(wvif->wdev->workqueue, &wvif->wep_key_work) <= 0)
			wsm_unlock_tx(wvif->wdev);
		handled = true;
		break;
	case do_tx:
		pr_debug("[WSM] Transmit frame.\n");
		break;
	default:
		/* Do nothing */
		break;
	}
	return handled;
}

static int wfx_get_prio_queue(struct wfx_vif *wvif,
				 u32 link_id_map, int *total)
{
	static const int urgent = BIT(WFX_LINK_ID_AFTER_DTIM) |
		BIT(WFX_LINK_ID_UAPSD);
	WsmHiEdcaParamsReqBody_t *edca;
	unsigned score, best = -1;
	int winner = -1;
	int i;

	edca = &wvif->edca.params;
	for (i = 0; i < 4; ++i) {
		int queued;
		queued = wfx_queue_get_num_queued(&wvif->wdev->tx_queue[i],
				link_id_map);
		if (!queued)
			continue;
		*total += queued;
		score = ((edca->AIFSN[i] + edca->CwMin[i]) << 16) +
			((edca->CwMax[i] - edca->CwMin[i]) *
			 (get_random_int() & 0xFFFF));
		if (score < best && (winner < 0 || i != 3)) {
			best = score;
			winner = i;
		}
	}

	/* override winner if bursting */
	if (winner >= 0 && wvif->wdev->tx_burst_idx >= 0 &&
	    winner != wvif->wdev->tx_burst_idx &&
	    !wfx_queue_get_num_queued(
		    &wvif->wdev->tx_queue[winner],
		    link_id_map & urgent) &&
	    wfx_queue_get_num_queued(
		    &wvif->wdev->tx_queue[wvif->wdev->tx_burst_idx],
		    link_id_map))
		winner = wvif->wdev->tx_burst_idx;

	return winner;
}

static int wsm_get_tx_queue_and_mask(struct wfx_dev	*wdev,
				     struct wfx_queue **queue_p,
				     u32 *tx_allowed_mask_p,
				     bool *more)
{
	int idx;
	u32 tx_allowed_mask;
	int total = 0;
	// FIXME: Get interface id from wsm_buf or if_id
	struct wfx_vif *wvif = wdev_to_wvif(wdev);

	/* Search for a queue with multicast frames buffered */
	if (wvif->tx_multicast) {
		tx_allowed_mask = BIT(WFX_LINK_ID_AFTER_DTIM);
		idx = wfx_get_prio_queue(wvif,
				tx_allowed_mask, &total);
		if (idx >= 0) {
			*more = total > 1;
			goto found;
		}
	}

	/* Search for unicast traffic */
	tx_allowed_mask = ~wvif->sta_asleep_mask;
	tx_allowed_mask |= BIT(WFX_LINK_ID_UAPSD);
	if (wvif->sta_asleep_mask) {
		tx_allowed_mask |= wvif->pspoll_mask;
		tx_allowed_mask &= ~BIT(WFX_LINK_ID_AFTER_DTIM);
	} else {
		tx_allowed_mask |= BIT(WFX_LINK_ID_AFTER_DTIM);
	}
	idx = wfx_get_prio_queue(wvif,
			tx_allowed_mask, &total);
	if (idx < 0)
		return -ENOENT;

found:
	*queue_p = &wdev->tx_queue[idx];
	*tx_allowed_mask_p = tx_allowed_mask;
	return 0;
}

/**
 * It returns 1 if Tx data are found else 0.
 * data, tx_len and burst are only set if 1 is returned.
 * burst is the number of pending messages (including the current reported one then burst>=1)
 *   that are allowed to be sent in the same TxOp than the current reported message.
 *   But it does not guaranty that we have the time to send them all in the duration of the TxOp.
 */
int wsm_get_tx(struct wfx_dev *wdev, u8 **data,
	       size_t *tx_len, int *burst)
{
	WsmHiTxReq_t *wsm = NULL;
	struct ieee80211_tx_info *tx_info;
	struct wfx_queue *queue = NULL;
	u32 tx_allowed_mask = 0;
	const struct wfx_txpriv *txpriv = NULL;
	int count = 0;
	// FIXME: Get interface id from wsm_buf
	struct wfx_vif *wvif = wdev_to_wvif(wdev);

	/* More is used only for broadcasts. */
	bool more = false;

	if (wdev->wsm_cmd.ptr) { /* CMD request */
		spin_lock(&wdev->wsm_cmd.lock);
		BUG_ON(!wdev->wsm_cmd.ptr);
		*data = wdev->wsm_cmd.ptr;
		*tx_len = wdev->wsm_cmd.len;
		*burst = 1;
		spin_unlock(&wdev->wsm_cmd.lock);
		return 1;
	}
	if (!wvif) {
		// May happen during unregister
		dev_dbg(wdev->pdev, "%s: non-existant vif", __func__);
		return 0;
	}
	for (;;) {
		int ret;
		int queue_num;
		struct ieee80211_hdr *hdr;

		if (atomic_add_return(0, &wdev->tx_lock))
			break;

		spin_lock_bh(&wvif->ps_state_lock);

		ret = wsm_get_tx_queue_and_mask(wdev, &queue, &tx_allowed_mask, &more);
		queue_num = queue - wdev->tx_queue;

		if (wvif->buffered_multicasts && (ret || !more) &&
		    (wvif->tx_multicast || !wvif->sta_asleep_mask)) {
			wvif->buffered_multicasts = false;
			if (wvif->tx_multicast) {
				wvif->tx_multicast = false;
				queue_work(wdev->workqueue, &wvif->multicast_stop_work);
			}
		}

		spin_unlock_bh(&wvif->ps_state_lock);

		if (ret)
			break;

		if (wfx_queue_get(queue, tx_allowed_mask, &wsm, &tx_info, &txpriv))
			continue;

		if (wsm_handle_tx_data(wvif, wsm, tx_info, txpriv, queue))
			continue;  /* Handled by WSM */

		wsm->Header.s.b.IntId = 0;
		wvif->pspoll_mask &= ~BIT(txpriv->raw_link_id);

		*data = (u8 *)wsm;
		*tx_len = le16_to_cpu(wsm->Header.MsgLen);

		/* allow bursting if txop is set */
		if (wvif->edca.params.TxOpLimit[queue_num])
			*burst = (int)wfx_queue_get_num_queued(queue, tx_allowed_mask) + 1;
		else
			*burst = 1;

		/* store index of bursting queue */
		if (*burst > 1)
			wdev->tx_burst_idx = queue_num;
		else
			wdev->tx_burst_idx = -1;

		hdr = (struct ieee80211_hdr *) &((u8 *) wsm)[txpriv->offset];
		if (more)
			hdr->frame_control |= cpu_to_le16(IEEE80211_FCTL_MOREDATA);
		pr_debug("[WSM] Tx sta_id=%d >>> frame_ctrl=0x%.4x  tx_len=%zu, %p %c\n",
			txpriv->link_id, hdr->frame_control, *tx_len, *data,
			wsm->Body.More ? 'M' : ' ');
		++count;
		break;
	}

	return count;
}

void wsm_txed(struct wfx_dev *wdev, u8 *data)
{
	if (data == wdev->wsm_cmd.ptr) {
		spin_lock(&wdev->wsm_cmd.lock);
		wdev->wsm_cmd.ptr = NULL;
		spin_unlock(&wdev->wsm_cmd.lock);
	}
}




// SPDX-License-Identifier: GPL-2.0
/*
 * Bluetooth HCI driver for the MT6765 CONSYS Bluetooth function.
 *
 * The controller sits behind the WMT/STP core: WMT powers the BT function
 * on and has its RAM patch loaded, and STP carries H:4 framed HCI packets
 * on its BT channel over BTIF. This is the data path of MediaTek's
 * stp_chrdev_bt (/dev/stpbt, "MTK Bluetooth mode") with an hci_dev in
 * place of the character device, so that the kernel Bluetooth stack and
 * the HCI user channel used by the AOSP Bluetooth HAL can drive it.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "wmt_exp.h"
#include "stp_exp.h"

#define VERSION "1.0"

/* STP carries at most 2048 bytes of BT payload per frame (stp_chrdev_bt) */
#define BTMTK_WMT_MAX_FRAME	2048
#define BTMTK_WMT_RX_CHUNK	2048
/* STP returns 0 while its TX window is full; the stock driver retried 3x */
#define BTMTK_WMT_TX_RETRIES	10
#define BTMTK_WMT_TX_RETRY_MS	20

#define MTK_VENDOR_SET_BDADDR	0xfc1a

enum {
	BTMTK_WMT_ON,		/* BT function on, STP callbacks registered */
	BTMTK_WMT_RESETTING,	/* WMT whole-chip reset in progress */
};

struct btmtk_wmt {
	struct hci_dev *hdev;
	unsigned long flags;

	struct workqueue_struct *wq;
	struct work_struct rx_work;
	struct work_struct tx_work;
	struct sk_buff_head txq;

	struct sk_buff *rx_skb;		/* H:4 packet being reassembled */
	u8 rx_buf[BTMTK_WMT_RX_CHUNK];

	struct wakeup_source *ws;
};

/* the STP and WMT callbacks carry no context pointer */
static struct btmtk_wmt *btmtk_wmt_dev;

static void btmtk_wmt_rx_event(void)
{
	struct btmtk_wmt *bdev = READ_ONCE(btmtk_wmt_dev);

	if (!bdev || !test_bit(BTMTK_WMT_ON, &bdev->flags))
		return;

	/*
	 * Keep the system up until the frame has been handed to the stack,
	 * as stp_chrdev_bt does, or an event can sit in the STP queue over
	 * suspend and the command that waits for it times out.
	 */
	__pm_wakeup_event(bdev->ws, 100);
	queue_work(bdev->wq, &bdev->rx_work);
}

static void btmtk_wmt_tx_event(void)
{
	struct btmtk_wmt *bdev = READ_ONCE(btmtk_wmt_dev);

	if (bdev && test_bit(BTMTK_WMT_ON, &bdev->flags) &&
	    !skb_queue_empty(&bdev->txq))
		queue_work(bdev->wq, &bdev->tx_work);
}

static unsigned int btmtk_wmt_hdr_len(u8 type)
{
	switch (type) {
	case HCI_EVENT_PKT:
		return HCI_EVENT_HDR_SIZE;
	case HCI_ACLDATA_PKT:
		return HCI_ACL_HDR_SIZE;
	case HCI_SCODATA_PKT:
		return HCI_SCO_HDR_SIZE;
	default:
		return 0;
	}
}

static unsigned int btmtk_wmt_payload_len(struct sk_buff *skb)
{
	switch (hci_skb_pkt_type(skb)) {
	case HCI_EVENT_PKT:
		return hci_event_hdr(skb)->plen;
	case HCI_ACLDATA_PKT:
		return __le16_to_cpu(hci_acl_hdr(skb)->dlen);
	case HCI_SCODATA_PKT:
		return hci_sco_hdr(skb)->dlen;
	default:
		return 0;
	}
}

static void btmtk_wmt_recv_frame(struct btmtk_wmt *bdev, struct sk_buff *skb)
{
	struct hci_dev *hdev = bdev->hdev;

	switch (hci_skb_pkt_type(skb)) {
	case HCI_EVENT_PKT:
		hdev->stat.evt_rx++;
		break;
	case HCI_ACLDATA_PKT:
		hdev->stat.acl_rx++;
		break;
	case HCI_SCODATA_PKT:
		hdev->stat.sco_rx++;
		break;
	}
	hdev->stat.byte_rx += skb->len;
	hci_recv_frame(hdev, skb);
}

/* reassemble H:4 packets from the byte stream of the STP BT queue */
static void btmtk_wmt_h4_recv(struct btmtk_wmt *bdev, const u8 *data,
			      int count)
{
	struct hci_dev *hdev = bdev->hdev;
	struct sk_buff *skb = bdev->rx_skb;
	unsigned int hlen, plen, len;

	while (count > 0) {
		if (!skb) {
			u8 type = *data++;

			count--;
			hlen = btmtk_wmt_hdr_len(type);
			if (!hlen) {
				hdev->stat.err_rx++;
				bt_dev_err_ratelimited(hdev,
					"unknown H:4 packet type 0x%02x, resyncing",
					type);
				continue;
			}

			skb = bt_skb_alloc(HCI_MAX_FRAME_SIZE, GFP_KERNEL);
			if (!skb) {
				hdev->stat.err_rx++;
				bt_dev_err(hdev, "out of memory, dropping %d bytes",
					   count);
				break;
			}
			hci_skb_pkt_type(skb) = type;
			hci_skb_expect(skb) = hlen;
		}

		len = min_t(unsigned int, hci_skb_expect(skb) - skb->len,
			    count);
		skb_put_data(skb, data, len);
		data += len;
		count -= len;

		if (skb->len < hci_skb_expect(skb))
			continue;

		hlen = btmtk_wmt_hdr_len(hci_skb_pkt_type(skb));
		if (skb->len == hlen && hci_skb_expect(skb) == hlen) {
			/* header complete */
			plen = btmtk_wmt_payload_len(skb);
			if (plen > skb_tailroom(skb)) {
				hdev->stat.err_rx++;
				bt_dev_err(hdev,
					   "packet type 0x%02x too long (%u)",
					   hci_skb_pkt_type(skb), plen);
				kfree_skb(skb);
				skb = NULL;
				continue;
			}
			if (plen) {
				hci_skb_expect(skb) += plen;
				continue;
			}
		}

		btmtk_wmt_recv_frame(bdev, skb);
		skb = NULL;
	}

	bdev->rx_skb = skb;
}

static void btmtk_wmt_rx_work(struct work_struct *work)
{
	struct btmtk_wmt *bdev = container_of(work, struct btmtk_wmt,
					      rx_work);
	int len;

	while (test_bit(BTMTK_WMT_ON, &bdev->flags)) {
		len = mtk_wcn_stp_receive_data(bdev->rx_buf,
					       sizeof(bdev->rx_buf),
					       BT_TASK_INDX);
		if (len <= 0) {
			if (len < 0)
				bt_dev_err(bdev->hdev,
					   "STP receive failed (%d)", len);
			break;
		}
		btmtk_wmt_h4_recv(bdev, bdev->rx_buf, len);
	}
}

static void btmtk_wmt_tx_work(struct work_struct *work)
{
	struct btmtk_wmt *bdev = container_of(work, struct btmtk_wmt,
					      tx_work);
	struct hci_dev *hdev = bdev->hdev;
	struct sk_buff *skb;
	int ret, retry;

	while ((skb = skb_dequeue(&bdev->txq))) {
		if (!test_bit(BTMTK_WMT_ON, &bdev->flags) ||
		    test_bit(BTMTK_WMT_RESETTING, &bdev->flags)) {
			kfree_skb(skb);
			continue;
		}

		for (retry = 0; ; retry++) {
			ret = mtk_wcn_stp_send_data(skb->data, skb->len,
						    BT_TASK_INDX);
			if (ret != 0 || retry >= BTMTK_WMT_TX_RETRIES ||
			    !test_bit(BTMTK_WMT_ON, &bdev->flags))
				break;
			/* STP window full, wait for the controller's acks */
			msleep(BTMTK_WMT_TX_RETRY_MS);
		}

		if (ret <= 0) {
			hdev->stat.err_tx++;
			bt_dev_err_ratelimited(hdev,
				"STP send of %u bytes failed (%d)",
				skb->len, ret);
			kfree_skb(skb);
			continue;
		}

		switch (hci_skb_pkt_type(skb)) {
		case HCI_COMMAND_PKT:
			hdev->stat.cmd_tx++;
			break;
		case HCI_ACLDATA_PKT:
			hdev->stat.acl_tx++;
			break;
		case HCI_SCODATA_PKT:
			hdev->stat.sco_tx++;
			break;
		}
		hdev->stat.byte_tx += skb->len;
		kfree_skb(skb);
	}
}

static void btmtk_wmt_rst_cb(ENUM_WMTDRV_TYPE_T src, ENUM_WMTDRV_TYPE_T dst,
			     ENUM_WMTMSG_TYPE_T type, PVOID buf, UINT32 sz)
{
	struct btmtk_wmt *bdev = READ_ONCE(btmtk_wmt_dev);
	ENUM_WMTRSTMSG_TYPE_T msg = WMTRSTMSG_RESET_INVALID;

	if (!bdev || src != WMTDRV_TYPE_WMT || dst != WMTDRV_TYPE_BT ||
	    type != WMTMSG_TYPE_RESET || !buf || sz > sizeof(msg))
		return;

	memcpy(&msg, buf, sz);

	switch (msg) {
	case WMTRSTMSG_RESET_START:
		bt_dev_warn(bdev->hdev, "CONSYS whole-chip reset started");
		set_bit(BTMTK_WMT_RESETTING, &bdev->flags);
		break;
	case WMTRSTMSG_RESET_END:
	case WMTRSTMSG_RESET_END_FAIL:
		bt_dev_warn(bdev->hdev, "CONSYS whole-chip reset %s",
			    msg == WMTRSTMSG_RESET_END ? "done" : "failed");
		clear_bit(BTMTK_WMT_RESETTING, &bdev->flags);
		/*
		 * The controller lost its state: report a hardware error,
		 * the core then closes and reopens the device, which turns
		 * the BT function off and on again through WMT.
		 */
		if (test_bit(BTMTK_WMT_ON, &bdev->flags))
			hci_reset_dev(bdev->hdev);
		break;
	default:
		break;
	}
}

static void btmtk_wmt_drain_rx(struct btmtk_wmt *bdev)
{
	while (mtk_wcn_stp_receive_data(bdev->rx_buf, sizeof(bdev->rx_buf),
					BT_TASK_INDX) > 0)
		;
	kfree_skb(bdev->rx_skb);
	bdev->rx_skb = NULL;
}

static int btmtk_wmt_open(struct hci_dev *hdev)
{
	struct btmtk_wmt *bdev = hci_get_drvdata(hdev);

	if (mtk_wcn_wmt_func_on(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE) {
		bt_dev_err(hdev, "WMT failed to turn on the BT function");
		return -EIO;
	}

	if (mtk_wcn_stp_is_ready() == MTK_WCN_BOOL_FALSE) {
		bt_dev_err(hdev, "STP is not ready");
		mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT);
		return -EIO;
	}

	/* H:4 frames through the STP BT RX queue, not the hci_stp path */
	mtk_wcn_stp_set_bluez(MTK_WCN_BOOL_FALSE);

	btmtk_wmt_drain_rx(bdev);
	clear_bit(BTMTK_WMT_RESETTING, &bdev->flags);
	set_bit(BTMTK_WMT_ON, &bdev->flags);

	mtk_wcn_wmt_msgcb_reg(WMTDRV_TYPE_BT, btmtk_wmt_rst_cb);
	/* STP drops BT frames until this is registered */
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, btmtk_wmt_rx_event);
	mtk_wcn_stp_register_tx_event_cb(BT_TASK_INDX, btmtk_wmt_tx_event);

	bt_dev_info(hdev, "BT function on (chip 0x%04x hw 0x%04x fw 0x%04x)",
		    mtk_wcn_wmt_ic_info_get(WMTCHIN_CHIPID),
		    mtk_wcn_wmt_ic_info_get(WMTCHIN_HWVER),
		    mtk_wcn_wmt_ic_info_get(WMTCHIN_FWVER));

	return 0;
}

static int btmtk_wmt_close(struct hci_dev *hdev)
{
	struct btmtk_wmt *bdev = hci_get_drvdata(hdev);

	clear_bit(BTMTK_WMT_ON, &bdev->flags);

	mtk_wcn_stp_register_tx_event_cb(BT_TASK_INDX, NULL);
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, NULL);
	mtk_wcn_wmt_msgcb_unreg(WMTDRV_TYPE_BT);

	cancel_work_sync(&bdev->rx_work);
	cancel_work_sync(&bdev->tx_work);
	skb_queue_purge(&bdev->txq);
	btmtk_wmt_drain_rx(bdev);

	if (mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE) {
		bt_dev_err(hdev, "WMT failed to turn off the BT function");
		return -EIO;
	}

	return 0;
}

static int btmtk_wmt_flush(struct hci_dev *hdev)
{
	struct btmtk_wmt *bdev = hci_get_drvdata(hdev);

	skb_queue_purge(&bdev->txq);

	return 0;
}

static int btmtk_wmt_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btmtk_wmt *bdev = hci_get_drvdata(hdev);

	if (skb->len + 1 > BTMTK_WMT_MAX_FRAME) {
		bt_dev_err(hdev, "frame of %u bytes is too long for STP",
			   skb->len);
		kfree_skb(skb);
		return -EINVAL;
	}

	/* prepend the H:4 packet type */
	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);
	skb_queue_tail(&bdev->txq, skb);
	queue_work(bdev->wq, &bdev->tx_work);

	return 0;
}

static int btmtk_wmt_setup(struct hci_dev *hdev)
{
	struct hci_rp_read_bd_addr *rp;
	struct sk_buff *skb;

	skb = __hci_cmd_sync(hdev, HCI_OP_READ_BD_ADDR, 0, NULL,
			     HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "reading the device address failed (%ld)",
			   PTR_ERR(skb));
		return PTR_ERR(skb);
	}

	if (skb->len != sizeof(*rp)) {
		bt_dev_err(hdev, "device address event length mismatch");
		kfree_skb(skb);
		return -EIO;
	}

	rp = (struct hci_rp_read_bd_addr *)skb->data;
	bt_dev_info(hdev, "controller address %pMR", &rp->bdaddr);

	/*
	 * The stock HAL programs the address from NVRAM on every power on.
	 * Without it, leave the controller unconfigured so that userspace
	 * (btmgmt public-addr, the mainline set_bdaddr service) sets one.
	 */
	if (!bacmp(&rp->bdaddr, BDADDR_ANY)) {
		bt_dev_warn(hdev, "no device address programmed");
		hci_set_quirk(hdev, HCI_QUIRK_INVALID_BDADDR);
	}
	kfree_skb(skb);

	return 0;
}

static int btmtk_wmt_set_bdaddr(struct hci_dev *hdev, const bdaddr_t *bdaddr)
{
	struct sk_buff *skb;

	skb = __hci_cmd_sync(hdev, MTK_VENDOR_SET_BDADDR, sizeof(*bdaddr),
			     bdaddr, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "setting the device address failed (%ld)",
			   PTR_ERR(skb));
		return PTR_ERR(skb);
	}
	kfree_skb(skb);

	return 0;
}

static int __init btmtk_wmt_init(void)
{
	struct btmtk_wmt *bdev;
	struct hci_dev *hdev;
	int err;

	bdev = kzalloc_obj(*bdev);
	if (!bdev)
		return -ENOMEM;

	skb_queue_head_init(&bdev->txq);
	INIT_WORK(&bdev->rx_work, btmtk_wmt_rx_work);
	INIT_WORK(&bdev->tx_work, btmtk_wmt_tx_work);

	bdev->wq = alloc_workqueue("btmtk_wmt", WQ_HIGHPRI | WQ_MEM_RECLAIM,
				   0);
	if (!bdev->wq) {
		err = -ENOMEM;
		goto err_free;
	}

	bdev->ws = wakeup_source_register(NULL, "btmtk_wmt");
	if (!bdev->ws) {
		err = -ENOMEM;
		goto err_wq;
	}

	hdev = hci_alloc_dev();
	if (!hdev) {
		err = -ENOMEM;
		goto err_ws;
	}

	bdev->hdev = hdev;
	hci_set_drvdata(hdev, bdev);

	/* BTIF is a UART-like block between the AP and the CONSYS MCU */
	hdev->bus = HCI_UART;
	hdev->manufacturer = 70;	/* MediaTek */
	hdev->open = btmtk_wmt_open;
	hdev->close = btmtk_wmt_close;
	hdev->flush = btmtk_wmt_flush;
	hdev->send = btmtk_wmt_send_frame;
	hdev->setup = btmtk_wmt_setup;
	hdev->set_bdaddr = btmtk_wmt_set_bdaddr;
	/* Page 2 advertises a sync train the firmware rejects (0x0c77, unknown command), failing init. */
	hci_set_quirk(hdev, HCI_QUIRK_BROKEN_LOCAL_EXT_FEATURES_PAGE_2);

	WRITE_ONCE(btmtk_wmt_dev, bdev);

	err = hci_register_dev(hdev);
	if (err < 0) {
		pr_err("btmtk_wmt: can't register HCI device (%d)\n", err);
		goto err_hdev;
	}

	return 0;

err_hdev:
	WRITE_ONCE(btmtk_wmt_dev, NULL);
	hci_free_dev(hdev);
err_ws:
	wakeup_source_unregister(bdev->ws);
err_wq:
	destroy_workqueue(bdev->wq);
err_free:
	kfree(bdev);
	return err;
}

static void __exit btmtk_wmt_exit(void)
{
	struct btmtk_wmt *bdev = btmtk_wmt_dev;

	/* closes the device, which unregisters the STP callbacks */
	hci_unregister_dev(bdev->hdev);
	WRITE_ONCE(btmtk_wmt_dev, NULL);
	hci_free_dev(bdev->hdev);

	destroy_workqueue(bdev->wq);
	wakeup_source_unregister(bdev->ws);
	kfree(bdev);
}

module_init(btmtk_wmt_init);
module_exit(btmtk_wmt_exit);

MODULE_DESCRIPTION("Bluetooth HCI driver for the MT6765 CONSYS over WMT/STP " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTE3600 Unified Linux Kernel Driver
 * Target Silicon: FocalTech FT9362 / FW9369 Fingerprint Sensor
 * Target Platform: Dynabook Satellite Pro C40-J (PYS46G-06J02D)
 *
 * Stage D9: Core Driver Refactoring
 * - Full Session Management, 64-bit Generation Tokens & 5.0 s Watchdog Workqueue
 * - Controller Domain Table & Conservative Identity Verification (PCI 0x8086:0xa0ab)
 * - Watchdog Cancellation Deadlock Remediation (Sync cancellation prior to data->lock)
 * - Transparent 32-bit Compat IOCTL via compat_ptr()
 * - Fail-Closed State Machine & Strict Capture-Gating
 *
 * Safety Gate: Physical hardware access strictly prohibited; offline compilation only.
 */

#include <linux/acpi.h>
#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/lockdep.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "fte3600.h"

#define FTE3600_DRIVER_NAME "fte3600"

/* Controller Domain Management */
enum fte3600_domain_state {
	FTE3600_DOMAIN_IDLE = 0,
	FTE3600_DOMAIN_RESERVED,
	FTE3600_DOMAIN_BUSY,
	FTE3600_DOMAIN_QUARANTINED,
	FTE3600_DOMAIN_REMOVING,
};

struct fte3600_controller_domain {
	u16 pci_vendor;
	u16 pci_device;
	char pci_bdf[32];
	enum fte3600_domain_state state;
	bool quarantined;
	u32 generation;
	spinlock_t lock;
};

static struct fte3600_controller_domain fte3600_domains[FTE3600_MAX_DOMAINS];
static DEFINE_MUTEX(fte3600_domain_table_lock);

/* Global Workqueue for Driver Asynchronous Tasks (Watchdog, Quarantined Workers) */
static struct workqueue_struct *fte3600_wq;

/* Calibration and Operational Session Data */
struct fte3600_session_data {
	bool active;
	u64 token;
	u32 generation;
	struct file *owner_filp;
	struct delayed_work watchdog_work;
	u8 channel_mode;
	u8 stored_dac;
	bool dac_valid;
	u16 fdt_minima[4];
	u16 fdt_thresholds[4];
	bool thresholds_programmed;
};

/* FocalTech Userspace Protocol Framing (/dev/focal_moh_spi) */
#define FOCAL_SPI_READ_ONLY     0x5A
#define FOCAL_SPI_READ_WRITE    0xA5
#define FOCAL_SPI_BACK_DATA     0xB9
#define FOCAL_MAX_BUFF_SIZE     (32 * 1024)

#define FOCAL_WAKE_EVENT_NONE    0
#define FOCAL_WAKE_EVENT_ENABLE  1
#define FOCAL_WAKE_EVENT_INT     2
#define FOCAL_WAKE_EVENT_RESUME  3
#define FOCAL_WAKE_EVENT_SUSPEND 4
#define FOCAL_WAKE_EVENT_DISABLE 5

#define FF_IOC_RESET_DEV         0x8086
#define FF_IOC_POWER_OFF         0x8087
#define FF_IOC_POWER_ON          0x8088
#define FF_IOC_IRQ_EN            0x8089
#define FF_IOC_LOG_EN            0x808A
#define FF_IOC_RELEASE_POLL      0x808B
#define FF_IOC_CSn               0x808C

struct focal_spi_read_buff {
	u8 type;
	u16 tx_len;
	u16 rx_len;
	u8 txbuff[0];
} __packed;

/* Device Private Structure */
struct fte3600_data {
	struct spi_device *spi;
	struct gpio_desc *reset_gpiod;
	struct cdev cdev;
	struct device dev;             /* Embedded device providing kobject refcounting */
	struct mutex lock;             /* Serializes reset, transport, and shutdown */
	wait_queue_head_t event_wq;    /* Waitqueue for pollers; awakened on device unbind */
	enum fte3600_state state;
	atomic_t is_open;              /* Driver-owned single-client exclusive lease */
	atomic_t offline;              /* Set to 1 on unbind/shutdown */
	u32 last_error;
	u8 foundry_code;               /* Silicon foundry fabrication code (e.g. 0xAA for SMIC) */
	u16 line_period_us;            /* Configured line timing period (150 us or 200 us) */
	struct fte3600_controller_domain *domain;
	struct fte3600_session_data session;
	/* FocalTech libfprint compatibility bridge (/dev/focal_moh_spi) */
	struct miscdevice focal_miscdev;
	u8 *focal_wr_buf;
	u8 *focal_rd_buf;
	u8 *sensor_init_data;
	int focal_work_flag;
	wait_queue_head_t focal_poll_wq;
	u8 tx_buf[64] ____cacheline_aligned;
	u8 rx_buf[64] ____cacheline_aligned;
};

/* Global Device Numbers and Class */
static dev_t fte3600_devt;
static struct class *fte3600_class;

/* ACPI Device ID Match Table */
static const struct acpi_device_id fte3600_acpi_ids[] = {
	{ "FTE3600", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, fte3600_acpi_ids);

/* ACPI Driver GPIO Translation Mapping */
static const struct acpi_gpio_params fte3600_reset_gpios = { 0, 0, true };
static const struct acpi_gpio_params fte3600_irq_gpios   = { 1, 0, false };

static const struct acpi_gpio_mapping fte3600_acpi_gpios[] = {
	{ "reset-gpios", &fte3600_reset_gpios, 1 },
	{ "irq-gpios",   &fte3600_irq_gpios,   1 },
	{ },
};

/*
 * Hardware CRC-16 Wire Calculation (CCITT MSB-first, Poly 0x1021, Seed 0xFFFF)
 */
static u16 fte3600_crc16_wire(const u8 *bytes, size_t len)
{
	u16 crc = 0xFFFF;
	size_t i;
	unsigned int bit;

	if (!bytes || len == 0)
		return 0;

	for (i = 0; i < len; ++i) {
		crc = (u16)(crc ^ ((u16)bytes[i] << 8));
		for (bit = 0; bit < 8; ++bit) {
			crc = (u16)(((u16)crc << 1) ^
				    ((crc & 0x8000) ? 0x1021u : 0u));
		}
	}
	return crc;
}

/* Device Release Callback: Invoked when the last reference to data->dev drops to zero */
static void fte3600_device_release(struct device *dev)
{
	struct fte3600_data *data = container_of(dev, struct fte3600_data, dev);

	kfree(data->focal_wr_buf);
	kfree(data->focal_rd_buf);
	kfree(data->sensor_init_data);
	mutex_destroy(&data->lock);
	kfree(data);
}

/*
 * Domain Reservation & Controller Identity Verification
 *
 * Verifies host SPI controller identity strictly against PCI Vendor 0x8086,
 * Device 0xa0ab (Intel LPSS SPI2 controller on Dynabook C40-J).
 * Allocates or re-acquires a domain entry from the static domain table.
 */
static struct fte3600_controller_domain *fte3600_find_or_alloc_domain(struct spi_device *spi)
{
	struct device *d;
	struct pci_dev *pci_dev = NULL;
	const char *bdf;
	int i, free_idx = -1;

	/* Walk up device hierarchy to locate parent PCI host controller */
	d = &spi->controller->dev;
	while (d) {
		if (dev_is_pci(d)) {
			pci_dev = to_pci_dev(d);
			break;
		}
		d = d->parent;
	}

	if (!pci_dev) {
		dev_err(&spi->dev, "Host SPI controller is not a PCI device\n");
		return ERR_PTR(-ENODEV);
	}

	bdf = dev_name(&pci_dev->dev);

	/* Check known host controller: Intel LPSS SPI2 (Vendor 0x8086, Device 0xa0ab) */
	if (pci_dev->vendor != 0x8086 || pci_dev->device != 0xa0ab) {
		dev_err(&spi->dev, "Unsupported host SPI controller (Vendor: 0x%04x, Device: 0x%04x)\n",
			pci_dev->vendor, pci_dev->device);
		return ERR_PTR(-ENODEV);
	}

	mutex_lock(&fte3600_domain_table_lock);
	for (i = 0; i < FTE3600_MAX_DOMAINS; i++) {
		if (fte3600_domains[i].pci_vendor == pci_dev->vendor &&
		    fte3600_domains[i].pci_device == pci_dev->device &&
		    strncmp(fte3600_domains[i].pci_bdf, bdf, sizeof(fte3600_domains[i].pci_bdf)) == 0) {
			if (fte3600_domains[i].quarantined) {
				dev_err(&spi->dev, "Domain %s is QUARANTINED (fatal DMA latch)\n", bdf);
				mutex_unlock(&fte3600_domain_table_lock);
				return ERR_PTR(-ESHUTDOWN);
			}
			mutex_unlock(&fte3600_domain_table_lock);
			return &fte3600_domains[i];
		}
		if (free_idx < 0 && fte3600_domains[i].pci_vendor == 0)
			free_idx = i;
	}

	if (free_idx < 0) {
		dev_err(&spi->dev, "Domain table exhausted (limit %d)\n", FTE3600_MAX_DOMAINS);
		mutex_unlock(&fte3600_domain_table_lock);
		return ERR_PTR(-ENOSPC);
	}

	fte3600_domains[free_idx].pci_vendor = pci_dev->vendor;
	fte3600_domains[free_idx].pci_device = pci_dev->device;
	strscpy(fte3600_domains[free_idx].pci_bdf, bdf, sizeof(fte3600_domains[free_idx].pci_bdf));
	fte3600_domains[free_idx].state = FTE3600_DOMAIN_IDLE;
	fte3600_domains[free_idx].quarantined = false;
	fte3600_domains[free_idx].generation = 1;
	spin_lock_init(&fte3600_domains[free_idx].lock);

	mutex_unlock(&fte3600_domain_table_lock);
	return &fte3600_domains[free_idx];
}

/*
 * Watchdog Delayed Work Function
 *
 * Invoked when 5000 ms of session inactivity elapses.
 * Invalidates the active session and fails closed.
 */
static void fte3600_watchdog_work_fn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct fte3600_data *data = container_of(dwork, struct fte3600_data, session.watchdog_work);

	mutex_lock(&data->lock);
	if (data->session.active) {
		if (data->spi)
			dev_warn(&data->spi->dev,
				 "Session watchdog expired (5000 ms inactivity). Invalidating session.\n");
		data->session.active = false;
		data->session.owner_filp = NULL;
		data->session.generation++;
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = ETIMEDOUT;
	}
	mutex_unlock(&data->lock);
}

/*
 * Session Validation Helper
 *
 * Verifies session activity, OFD ownership, and token match.
 * Automatically re-arms the watchdog timer on success.
 */
static int fte3600_validate_session(struct fte3600_data *data, struct file *filp, u64 token)
{
	lockdep_assert_held(&data->lock);

	if (!data->session.active)
		return -EACCES;

	if (data->session.owner_filp != filp)
		return -EACCES;

	if (data->session.token != token)
		return -EACCES;

	/* Re-arm session watchdog timer (5000 ms) */
	mod_delayed_work(fte3600_wq, &data->session.watchdog_work,
			 msecs_to_jiffies(FTE3600_WATCHDOG_DEFAULT_MS));
	return 0;
}

/* Active-Low Reset Sequence Helper */
static int fte3600_hw_reset_pulse(struct fte3600_data *data)
{
	lockdep_assert_held(&data->lock);

	if (atomic_read(&data->offline) || !data->reset_gpiod)
		return -ENODEV;

	data->state = FTE3600_STATE_RESETTING;

	/* Pre-reset settle interval (Logical 0 = physical HIGH / inactive) */
	gpiod_set_value_cansleep(data->reset_gpiod, 0);
	usleep_range(10000, 11000);

	/* Assert reset: Logical 1 = physical LOW / asserted */
	gpiod_set_value_cansleep(data->reset_gpiod, 1);
	msleep(20);

	/* Deassert reset: Logical 0 = physical HIGH / inactive */
	gpiod_set_value_cansleep(data->reset_gpiod, 0);
	usleep_range(10000, 11000);

	data->state = FTE3600_STATE_RESET_SEQUENCE_ISSUED;
	return 0;
}

/*
 * Asynchronous SPI Transaction Object & Two-Reference Lifetime
 *
 * Reference Accounting:
 * Initial refcount = 2:
 * - Reference 1: Held by submitting thread (waiting on axfer->done).
 * - Reference 2: Held by SPI core / host controller hardware completion path.
 *
 * Worker & Callback Refcount Invariant:
 * Neither completion callbacks nor workqueue workers call module_put(THIS_MODULE).
 * Worker module refcount contribution is strictly ZERO.
 */
struct fte3600_async_xfer {
	struct kref ref;
	struct completion done;
	struct spi_message msg;
	struct spi_transfer xfer;
	struct fte3600_data *data;
	u8 *tx_buf;
	u8 *rx_buf;
	size_t len;
	bool timed_out;
	bool completed_ok;
	int status;
};

/*
 * Transaction Release Callback
 *
 * Invoked when axfer->ref drops to 0.
 *
 * Conservative DMA Retention Invariant:
 * If timed_out is true, Intel LPSS idma64 cannot prove physical silicon bus master
 * cessation. Therefore, memory is PERMANENTLY RETAINED in DRAM (FREED(t) == 0).
 */
static void fte3600_xfer_release(struct kref *kref)
{
	struct fte3600_async_xfer *axfer = container_of(kref, struct fte3600_async_xfer, ref);

	if (axfer->timed_out) {
		pr_err("fte3600: Quarantining stranded xfer %p (DRAM retained, FREED(t)=0)\n",
		       axfer);
		return;
	}

	kfree(axfer->tx_buf);
	kfree(axfer->rx_buf);
	kfree(axfer);
}

/*
 * SPI Hardware Completion Callback
 *
 * Runs in SPI host controller interrupt / callback context.
 * Signals completion and drops Reference 2.
 */
static void fte3600_spi_complete_cb(void *context)
{
	struct fte3600_async_xfer *axfer = context;

	axfer->status = axfer->msg.status;
	axfer->completed_ok = (axfer->msg.status == 0 &&
			       axfer->msg.actual_length == axfer->len);

	complete(&axfer->done);

	/* Drop Reference 2 (held by SPI hardware completion path) */
	kref_put(&axfer->ref, fte3600_xfer_release);
}

/*
 * Asynchronous SPI Transfer Engine with 250 ms Timeout & Global Quarantine Latching
 */
static int fte3600_spi_async_transfer(struct fte3600_data *data,
				      const u8 *tx, u8 *rx, size_t len)
{
	struct fte3600_async_xfer *axfer;
	unsigned long timeout_jiffies;
	long time_left;
	int ret;

	lockdep_assert_held(&data->lock);

	if (!data->spi || atomic_read(&data->offline))
		return -ENODEV;

	if (data->domain && data->domain->quarantined)
		return -ESHUTDOWN;

	if (!tx && !rx)
		return -EINVAL;

	if (len == 0 || len > FTE3600_MAX_SPI_TRANSFER_LEN)
		return -EINVAL;

	axfer = kzalloc(sizeof(*axfer), GFP_KERNEL);
	if (!axfer)
		return -ENOMEM;

	if (tx) {
		axfer->tx_buf = kmemdup(tx, len, GFP_KERNEL);
		if (!axfer->tx_buf) {
			kfree(axfer);
			return -ENOMEM;
		}
	}

	if (rx) {
		axfer->rx_buf = kzalloc(len, GFP_KERNEL);
		if (!axfer->rx_buf) {
			kfree(axfer->tx_buf);
			kfree(axfer);
			return -ENOMEM;
		}
	}

	axfer->data = data;
	axfer->len = len;
	axfer->timed_out = false;
	axfer->completed_ok = false;
	axfer->status = -EINPROGRESS;
	init_completion(&axfer->done);

	/* Initialize kref to 2: Ref 1 for Submitting Thread, Ref 2 for Callback */
	kref_init(&axfer->ref);
	kref_get(&axfer->ref);

	memset(&axfer->xfer, 0, sizeof(axfer->xfer));
	axfer->xfer.tx_buf = axfer->tx_buf;
	axfer->xfer.rx_buf = axfer->rx_buf;
	axfer->xfer.len = len;
	axfer->xfer.cs_change = 0;

	spi_message_init(&axfer->msg);
	axfer->msg.complete = fte3600_spi_complete_cb;
	axfer->msg.context = axfer;
	spi_message_add_tail(&axfer->xfer, &axfer->msg);

	ret = spi_async(data->spi, &axfer->msg);
	if (ret) {
		dev_err(&data->spi->dev, "spi_async submission failed: %d\n", ret);
		/* Submission failure: callback will never run. Drop both references */
		kref_put(&axfer->ref, fte3600_xfer_release);
		kref_put(&axfer->ref, fte3600_xfer_release);
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = (u32)(-ret);
		return ret;
	}

	/* Wait with 250 ms timeout */
	timeout_jiffies = msecs_to_jiffies(250);
	time_left = wait_for_completion_timeout(&axfer->done, timeout_jiffies);

	if (time_left == 0) {
		/* TIMEOUT OCCURRED! */
		dev_err(&data->spi->dev,
			"SPI async transfer timed out (250 ms expired). Latching quarantine.\n");
		axfer->timed_out = true;

		/* Latch controller domain into permanent quarantine */
		if (data->domain) {
			spin_lock_bh(&data->domain->lock);
			data->domain->quarantined = true;
			data->domain->state = FTE3600_DOMAIN_QUARANTINED;
			spin_unlock_bh(&data->domain->lock);
		}

		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = ETIMEDOUT;

		/* Drop Ref 1 (Submitting thread). Ref 2 remains with SPI hardware. */
		kref_put(&axfer->ref, fte3600_xfer_release);
		return -ETIMEDOUT;
	}

	/* Completed within 250 ms */
	if (!axfer->completed_ok || axfer->status != 0) {
		ret = axfer->status ? axfer->status : -EIO;
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = (u32)(-ret);
		kref_put(&axfer->ref, fte3600_xfer_release);
		return ret;
	}

	/* Copy RX data back to caller */
	if (rx && axfer->rx_buf)
		memcpy(rx, axfer->rx_buf, len);

	/* Normal completion: Drop Ref 1 (Submitting thread). Ref 2 was already dropped by callback. */
	kref_put(&axfer->ref, fte3600_xfer_release);
	return 0;
}

/* In-Kernel SPI Transfer Engine (Dispatches through Asynchronous Engine) */
static int fte3600_spi_sync_transfer(struct fte3600_data *data,
				     const u8 *tx, u8 *rx, size_t len)
{
	return fte3600_spi_async_transfer(data, tx, rx, len);
}

/* Protocol Framing Helpers */
static int fte3600_spi_cmd_strobe(struct fte3600_data *data, u8 cmd, u8 complement)
{
	data->tx_buf[0] = cmd;
	data->tx_buf[1] = complement;
	data->tx_buf[2] = 0x00;
	return fte3600_spi_sync_transfer(data, data->tx_buf, NULL, 3);
}

static int fte3600_spi_write_sfr(struct fte3600_data *data, u8 addr, u8 val)
{
	data->tx_buf[0] = 0x09;
	data->tx_buf[1] = 0xF6;
	data->tx_buf[2] = addr;
	data->tx_buf[3] = val;
	return fte3600_spi_sync_transfer(data, data->tx_buf, NULL, 4);
}

static int fte3600_spi_read_sfr(struct fte3600_data *data, u8 addr, u8 *val)
{
	int ret;

	memset(data->rx_buf, 0, 5);
	data->tx_buf[0] = 0x08;
	data->tx_buf[1] = 0xF7;
	data->tx_buf[2] = addr;
	data->tx_buf[3] = 0x00;
	data->tx_buf[4] = 0x00;
	ret = fte3600_spi_sync_transfer(data, data->tx_buf, data->rx_buf, 5);
	if (ret)
		return ret;
	if (val)
		*val = data->rx_buf[4];
	return 0;
}

static int fte3600_spi_write_reg16(struct fte3600_data *data, u16 addr, u16 val)
{
	data->tx_buf[0] = 0x05;
	data->tx_buf[1] = 0xFA;
	data->tx_buf[2] = 0x80 | (u8)(addr >> 8);
	data->tx_buf[3] = (u8)(addr & 0xFF);
	data->tx_buf[4] = 0x00;
	data->tx_buf[5] = 0x01;
	data->tx_buf[6] = (u8)(val >> 8);
	data->tx_buf[7] = (u8)(val & 0xFF);
	return fte3600_spi_sync_transfer(data, data->tx_buf, NULL, 8);
}

static int fte3600_spi_read_reg16(struct fte3600_data *data, u16 addr, u16 *val, u16 *wire_crc)
{
	int ret;
	u16 reg_val;

	memset(data->rx_buf, 0, 10);
	data->tx_buf[0] = 0x04;
	data->tx_buf[1] = 0xFB;
	data->tx_buf[2] = 0x80 | (u8)(addr >> 8);
	data->tx_buf[3] = (u8)(addr & 0xFF);
	data->tx_buf[4] = 0x00;
	data->tx_buf[5] = 0x01;
	memset(&data->tx_buf[6], 0, 4);

	ret = fte3600_spi_sync_transfer(data, data->tx_buf, data->rx_buf, 10);
	if (ret)
		return ret;

	reg_val = ((u16)data->rx_buf[6] << 8) | data->rx_buf[7];
	if (val)
		*val = reg_val;
	if (wire_crc)
		*wire_crc = 0;
	return 0;
}

static int fte3600_spi_rmw_reg16(struct fte3600_data *data, u16 addr, u16 mask, u16 val)
{
	u16 orig = 0;
	int ret;

	ret = fte3600_spi_read_reg16(data, addr, &orig, NULL);
	if (ret)
		return ret;

	return fte3600_spi_write_reg16(data, addr, (orig & mask) | val);
}

/* Minimal FT9362 Silicon Identification Sequence */
static int fte3600_hw_identify(struct fte3600_data *data)
{
	u16 chip_id, crc_rx, crc_calc;
	int ret;

	lockdep_assert_held(&data->lock);

	if (atomic_read(&data->offline) || !data->spi)
		return -ENODEV;

	if (data->state != FTE3600_STATE_RESET_SEQUENCE_ISSUED)
		return -EPERM;

	data->state = FTE3600_STATE_INITIALIZING;

	/* Step A: Wake write 0xC6 = 0x01 (4 bytes: opcode 0x09, ~0xF6, addr 0xC6, val 0x01) */
	data->tx_buf[0] = 0x09;
	data->tx_buf[1] = 0xF6;
	data->tx_buf[2] = 0xC6;
	data->tx_buf[3] = 0x01;
	ret = fte3600_spi_sync_transfer(data, data->tx_buf, NULL, 4);
	if (ret) {
		dev_err(&data->spi->dev, "Wake write failed: %d\n", ret);
		return ret;
	}

	/* Step B: Transceiver stabilization wait (4 ms with CS deasserted HIGH) */
	usleep_range(4000, 5000);

	/* Step C: Wake readback verification 0xC6 (5 bytes: opcode 0x08, ~0xF7, addr 0xC6, 2 dummy) */
	memset(data->rx_buf, 0, 5);
	data->tx_buf[0] = 0x08;
	data->tx_buf[1] = 0xF7;
	data->tx_buf[2] = 0xC6;
	data->tx_buf[3] = 0x00;
	data->tx_buf[4] = 0x00;
	ret = fte3600_spi_sync_transfer(data, data->tx_buf, data->rx_buf, 5);
	if (ret) {
		dev_err(&data->spi->dev, "Wake readback transfer failed: %d\n", ret);
		return ret;
	}

	if (data->rx_buf[4] != 0x01) {
		dev_err(&data->spi->dev,
			"Wake verification mismatch: got 0x%02x, expected 0x01\n",
			data->rx_buf[4]);
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = EIO;
		return -EIO;
	}

	/* Step D: Read Chip ID register 0x1A8B (10 bytes: opcode 0x04, ~0xFB, addr 0x9A 0x8B, 6 dummy) */
	memset(data->rx_buf, 0, 10);
	data->tx_buf[0] = 0x04;
	data->tx_buf[1] = 0xFB;
	data->tx_buf[2] = 0x9A;
	data->tx_buf[3] = 0x8B;
	memset(&data->tx_buf[4], 0, 6);
	ret = fte3600_spi_sync_transfer(data, data->tx_buf, data->rx_buf, 10);
	if (ret) {
		dev_err(&data->spi->dev, "Chip ID transfer failed: %d\n", ret);
		return ret;
	}

	chip_id = ((u16)data->rx_buf[6] << 8) | data->rx_buf[7];
	crc_rx  = ((u16)data->rx_buf[8] << 8) | data->rx_buf[9];

	/* Step E: Wire CRC-16 calculation over rx[6..7] */
	crc_calc = fte3600_crc16_wire(&data->rx_buf[6], 2);

	/* Step F: Variant gate & fault evaluation */
	if (chip_id == 0x0000 || chip_id == 0xFFFF) {
		dev_err(&data->spi->dev,
			"Chip ID bus fault: 0x%04x (bus grounded or floating)\n",
			chip_id);
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = ENODEV;
		return -ENODEV;
	}

	if (crc_rx != crc_calc) {
		dev_err(&data->spi->dev,
			"Wire CRC mismatch: rx 0x%04x != computed 0x%04x\n",
			crc_rx, crc_calc);
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = EBADMSG;
		return -EBADMSG;
	}

	if (chip_id != FTE3600_EXPECTED_CHIP_ID) {
		dev_err(&data->spi->dev,
			"Unsupported chip ID 0x%04x (expected FT9362 0x%04x)\n",
			chip_id, FTE3600_EXPECTED_CHIP_ID);
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = ENODEV;
		return -ENODEV;
	}

	data->state = FTE3600_STATE_IDENTIFIED;
	data->last_error = 0;
	dev_info(&data->spi->dev,
		 "FocalTech FT9362 identified (ID 0x%04x, CRC 0x%04x, state IDENTIFIED)\n",
		 chip_id, crc_rx);
	return 0;
}

/* Volatile Hardware Startup Prefix Sequence */
static int fte3600_hw_startup(struct fte3600_data *data)
{
	u8 status_80, fab_byte, dummy;
	int ret, i;

	lockdep_assert_held(&data->lock);

	if (atomic_read(&data->offline) || !data->spi)
		return -ENODEV;

	if (data->state != FTE3600_STATE_IDENTIFIED)
		return -EPERM;

	data->state = FTE3600_STATE_INITIALIZING;

	/* Step 1: Wake strobe 1 { 0x5A, 0xA5, 0x00 } */
	ret = fte3600_spi_cmd_strobe(data, 0x5A, 0xA5);
	if (ret) {
		dev_err(&data->spi->dev, "Startup wake strobe 1 failed: %d\n", ret);
		goto err;
	}

	/* Step 2: Stabilization wait 1 ms */
	usleep_range(1000, 1500);

	/* Step 3: Verify HW_STATUS (0x80) enters idle state 0x50 */
	ret = fte3600_spi_read_sfr(data, 0x80, &status_80);
	if (ret) {
		dev_err(&data->spi->dev, "Startup status 0x80 read failed: %d\n", ret);
		goto err;
	}

	if (status_80 != 0x50) {
		dev_warn(&data->spi->dev,
			 "Status 0x80 not idle (0x%02x), issuing abort strobe\n",
			 status_80);
		ret = fte3600_spi_cmd_strobe(data, 0xC0, 0x3F);
		if (ret)
			goto err;
		usleep_range(1000, 1500);
		ret = fte3600_spi_read_sfr(data, 0x80, &status_80);
		if (ret || status_80 != 0x50) {
			dev_err(&data->spi->dev,
				"Controller failed to reach idle state: 0x%02x\n",
				status_80);
			ret = -EIO;
			goto err;
		}
	}

	/* Step 4: Wake strobe 2 { 0xA5, 0x5A, 0x00 } */
	ret = fte3600_spi_cmd_strobe(data, 0xA5, 0x5A);
	if (ret) {
		dev_err(&data->spi->dev, "Startup wake strobe 2 failed: %d\n", ret);
		goto err;
	}

	/* Step 5: Foundry Interrogation (index 0x03 and 0x13) */
	for (i = 0; i < 2; i++) {
		u8 idx = (i == 0) ? 0x03 : 0x13;

		ret = fte3600_spi_write_sfr(data, 0xF1, idx);
		if (ret)
			goto err;
		ret = fte3600_spi_write_sfr(data, 0xF4, 0xC0);
		if (ret)
			goto err;
		ret = fte3600_spi_write_sfr(data, 0xF4, 0xC1);
		if (ret)
			goto err;
		ret = fte3600_spi_write_sfr(data, 0xF4, 0xC0);
		if (ret)
			goto err;
		ret = fte3600_spi_write_sfr(data, 0xF4, 0xC0);
		if (ret)
			goto err;
		ret = fte3600_spi_read_sfr(data, 0xF3, &dummy);
		if (ret)
			goto err;
	}

	/* Step 6: Read foundry identification code from register 0x9B */
	data->foundry_code = 0;
	for (i = 0; i < 10; i++) {
		ret = fte3600_spi_read_sfr(data, 0x9B, &fab_byte);
		if (ret)
			goto err;

		if ((fab_byte >> 2) == 0x13) {
			data->foundry_code = 0xAA; /* SMIC Fab AA */
			break;
		}
		if ((fab_byte >> 2) == 0)
			break;
	}

	data->line_period_us = (data->foundry_code == 0xAA) ? 150 : 200;
	data->state = FTE3600_STATE_INITIALIZED_UNCALIBRATED;
	data->last_error = 0;
	dev_info(&data->spi->dev,
		 "FocalTech FT9362 startup prefix complete (foundry 0x%02x, line period %u us, state INITIALIZED_UNCALIBRATED)\n",
		 data->foundry_code, data->line_period_us);
	return 0;

err:
	data->state = FTE3600_STATE_UNKNOWN;
	data->last_error = (u32)(-ret);
	return ret;
}

/*
 * Hardware Analog Front-End (AFE) and FDT Touch Sense Configuration
 *
 * Programs the analog front-end matrix, timings, integration multipliers,
 * and detection circuits on the FocalTech FT9362 silicon according to
 * the hardware specification.
 */
static int fte3600_hw_fdt_configure(struct fte3600_data *data, u8 dac, bool calibrating)
{
	u8 status_80;
	u16 period_val, val_1805, timing;
	int ret;

	lockdep_assert_held(&data->lock);

	/* 1. Ensure hardware is in idle state 0x50 */
	ret = fte3600_spi_cmd_strobe(data, 0x5A, 0xA5);
	if (ret)
		return ret;
	usleep_range(1000, 1500);

	ret = fte3600_spi_read_sfr(data, 0x80, &status_80);
	if (ret)
		return ret;

	if (status_80 != 0x50) {
		ret = fte3600_spi_cmd_strobe(data, 0xC0, 0x3F);
		if (ret)
			return ret;
		usleep_range(1000, 1500);
	}

	ret = fte3600_spi_cmd_strobe(data, 0xA5, 0x5A);
	if (ret)
		return ret;

	/* 2. Program DAC and Matrix Enable (0x1800 = 0x4FFE) */
	ret = fte3600_spi_write_reg16(data, 0x1801, 0xFC80 | (dac & 0x7F));
	if (ret)
		return ret;

	ret = fte3600_spi_write_reg16(data, 0x1800, 0x4FFE);
	if (ret)
		return ret;

	/* 3. Base Image Timing and Matrix Configuration */
	period_val = ((u16)(data->line_period_us - 1) << 5) | 1;
	ret = fte3600_spi_write_reg16(data, 0x1804, 0x27CA); /* shift = 2 */
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x1806, 0xC07F, 0x0480);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x180A, 0xC000, 0x0483);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x180B, 0xC000, 0x0208);
	if (ret)
		return ret;

	ret = fte3600_spi_write_reg16(data, 0x1807, period_val);
	if (ret)
		return ret;

	ret = fte3600_spi_write_reg16(data, 0x1887, 0x0002);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x1805, 0xFF0F, 0x0000);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x1811, 0xFC00, 0x01FE);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x1A83, 0xFFFF, 0x0020);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x1A83, 0xFFFF, 0x0060);
	if (ret)
		return ret;

	ret = fte3600_spi_write_sfr(data, 0x8E, 0xF4);
	if (ret)
		return ret;

	ret = fte3600_spi_write_sfr(data, 0x90, 0x00);
	if (ret)
		return ret;

	ret = fte3600_spi_write_sfr(data, 0x91, 0x07);
	if (ret)
		return ret;

	ret = fte3600_spi_write_sfr(data, 0x92, 0xD0);
	if (ret)
		return ret;

	ret = fte3600_spi_write_sfr(data, 0x90, 0x01);
	if (ret)
		return ret;

	/* 4. FDT Analog Circuitry Configuration */
	ret = fte3600_spi_write_reg16(data, 0x1801, 0xFC80 | (dac & 0x7F));
	if (ret)
		return ret;

	ret = fte3600_spi_write_reg16(data, 0x180C, calibrating ? 0x0000 : 0x0600);
	if (ret)
		return ret;

	/* Timing for rate 60 Hz, repeat 4: ((1000/60 - 1) << 8) | ((4 - 1) << 2) = 0x0F0C */
	timing = ((1000u / 60 - 1) << 8) | (3 << 2);
	ret = fte3600_spi_write_reg16(data, 0x1881, timing);
	if (ret)
		return ret;

	/* Channel mode 1 registers */
	ret = fte3600_spi_write_reg16(data, 0x1800, 0x07FE);
	if (ret)
		return ret;

	ret = fte3600_spi_write_reg16(data, 0x1804, 0x27C8);
	if (ret)
		return ret;

	ret = fte3600_spi_write_reg16(data, 0x1807, 0x1671);
	if (ret)
		return ret;

	ret = fte3600_spi_write_reg16(data, 0x1808, 0x0801);
	if (ret)
		return ret;

	ret = fte3600_spi_write_reg16(data, 0x1887, 0x0005);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x1806, 0xC07F, 0x0980);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x180A, 0xC000, 0x0987);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x180B, 0xC000, 0x0491);
	if (ret)
		return ret;

	/* FDT Analog Banks Setup */
	ret = fte3600_spi_read_reg16(data, 0x1805, &val_1805, NULL);
	if (ret)
		return ret;

	val_1805 &= 0xFFEF;
	ret = fte3600_spi_write_reg16(data, 0x1805, val_1805);
	if (ret)
		return ret;

	ret = fte3600_spi_write_reg16(data, 0x180D, (val_1805 & 0xFC00) | 0x0384);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x1888, 0xFC03, 0x0000);
	if (ret)
		return ret;

	/* Unlock Protected Bank SFR 0x9A = 0x5A */
	ret = fte3600_spi_write_sfr(data, 0x9A, 0x5A);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x00C0, 0xE000, 0x0444);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x00C1, 0xFFC0, 0x0021);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x00C2, 0xFFFF, 0x00C0);
	if (ret)
		return ret;

	/* Lock Protected Bank SFR 0x9A = 0x00 */
	ret = fte3600_spi_write_sfr(data, 0x9A, 0x00);
	if (ret)
		return ret;

	/* Default detection thresholds: low 50, high 45 */
	ret = fte3600_spi_rmw_reg16(data, 0x1880, 0x0000, (45 << 8) | 50);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x1881, 0xFFFE, 0x0082);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x1884, 0xFFF8, 0x0003); /* average 4 */
	if (ret)
		return ret;

	ret = fte3600_spi_write_reg16(data, 0x1A8A, 0x00FF);
	if (ret)
		return ret;

	ret = fte3600_spi_rmw_reg16(data, 0x1A83, 0xFFFF, 0x0008);
	if (ret)
		return ret;

	return 0;
}

/* FDT Touch Sense Analog Front-End Measurement */
static int fte3600_hw_fdt_sample(struct fte3600_data *data, struct fte3600_session_fdt *sfdt)
{
	u16 timing, status_1a82;
	u8 bank_addr;
	int ret, i, iter;
	bool ready = false;

	lockdep_assert_held(&data->lock);

	if (atomic_read(&data->offline) || !data->spi)
		return -ENODEV;

	/* 1. Configure AFE and FDT circuit with requested DAC */
	ret = fte3600_hw_fdt_configure(data, (u8)sfdt->dac_val, true);
	if (ret)
		return ret;

	/* 2. Configure scan mode trigger: verify bit 1 in 0x1881 is set */
	ret = fte3600_spi_read_reg16(data, 0x1881, &timing, NULL);
	if (ret)
		return ret;
	if ((timing & 2) == 0) {
		ret = fte3600_spi_write_reg16(data, 0x1881, timing | 2);
		if (ret)
			return ret;
	}

	/* 3. Send command strobe {0xC2, 0x3D, 0x00} */
	ret = fte3600_spi_cmd_strobe(data, 0xC2, 0x3D);
	if (ret)
		return ret;

	/* 4. Stabilization wait 1 ms */
	usleep_range(1000, 1500);

	/* 5. Trigger measurement: write 0x1885 = 0x0001 */
	ret = fte3600_spi_write_reg16(data, 0x1885, 0x0001);
	if (ret)
		return ret;

	/* 6. Poll status register 0x1A82 for scan completion (bit 3 == 1) */
	for (iter = 0; iter < 10; iter++) {
		ret = fte3600_spi_read_reg16(data, 0x1A82, &status_1a82, NULL);
		if (ret)
			return ret;
		if (status_1a82 & 0x0008) {
			ready = true;
			break;
		}
		usleep_range(1000, 1500);
	}

	if (!ready) {
		dev_err(&data->spi->dev, "FDT scan timeout waiting for 0x1A82 bit 3\n");
		return -ETIMEDOUT;
	}

	/* 7. Read 4 channels ADC bank (14 bytes total: 6 header + 8 data) */
	bank_addr = (data->foundry_code == 0xAA) ? 0xE8 : 0xB8;
	memset(data->rx_buf, 0, 14);
	data->tx_buf[0] = 0x04;
	data->tx_buf[1] = 0xFB;
	data->tx_buf[2] = 0x80;
	data->tx_buf[3] = bank_addr;
	data->tx_buf[4] = 0x00;
	data->tx_buf[5] = 0x04; /* 4 words = 8 bytes */
	memset(&data->tx_buf[6], 0, 8);

	ret = fte3600_spi_sync_transfer(data, data->tx_buf, data->rx_buf, 14);
	if (ret)
		return ret;

	/* 8. Populate results */
	for (i = 0; i < 4; i++) {
		sfdt->adc_samples[i] = ((u16)data->rx_buf[6 + 2 * i] << 8) |
				       data->rx_buf[7 + 2 * i];
	}
	for (i = 4; i < 8; i++)
		sfdt->adc_samples[i] = 0;

	sfdt->wire_crc = 0;
	sfdt->computed_crc = 0;
	sfdt->status_flags = 0x01;

	data->session.stored_dac = (u8)sfdt->dac_val;
	data->session.dac_valid = true;
	return 0;
}

/* Threshold Bank Programming (22-Byte Burst Write) */
static int fte3600_hw_set_thresholds(struct fte3600_data *data,
				     const struct fte3600_session_thresholds *sthresh)
{
	u8 expected_bank_addr;
	int ret, i;

	lockdep_assert_held(&data->lock);

	if (atomic_read(&data->offline) || !data->spi)
		return -ENODEV;

	if (sthresh->bank_len != FTE3600_THRESHOLD_BANK_LEN)
		return -EINVAL;

	expected_bank_addr = (data->foundry_code == 0xAA) ? 0xE0 : 0xB0;

	/* Validate 6-byte header */
	if (sthresh->threshold_bank[0] != 0x05 ||
	    sthresh->threshold_bank[1] != 0xFA ||
	    sthresh->threshold_bank[2] != 0x80 ||
	    sthresh->threshold_bank[3] != expected_bank_addr ||
	    sthresh->threshold_bank[4] != 0x00 ||
	    sthresh->threshold_bank[5] != 0x08) {
		dev_err(&data->spi->dev, "Invalid threshold bank header\n");
		return -EINVAL;
	}

	/* Validate channels 4..7 padding is zero */
	for (i = 14; i < 22; i++) {
		if (sthresh->threshold_bank[i] != 0) {
			dev_err(&data->spi->dev, "Non-zero padding in threshold bank\n");
			return -EINVAL;
		}
	}

	/* Unlock protected SFR 0x9A = 0x5A */
	ret = fte3600_spi_write_sfr(data, 0x9A, 0x5A);
	if (ret)
		return ret;

	/* Burst write 22-byte bank */
	memcpy(data->tx_buf, sthresh->threshold_bank, FTE3600_THRESHOLD_BANK_LEN);
	ret = fte3600_spi_sync_transfer(data, data->tx_buf, NULL, FTE3600_THRESHOLD_BANK_LEN);
	if (ret) {
		fte3600_spi_write_sfr(data, 0x9A, 0x00);
		return ret;
	}

	/* Lock protected SFR 0x9A = 0x00 */
	ret = fte3600_spi_write_sfr(data, 0x9A, 0x00);
	if (ret)
		return ret;

	data->session.thresholds_programmed = true;
	return 0;
}

/* Operational Arming Sequence (Transitions to READY) */
static int fte3600_hw_arm_sensor(struct fte3600_data *data)
{
	int ret;

	lockdep_assert_held(&data->lock);

	if (atomic_read(&data->offline) || !data->spi)
		return -ENODEV;

	/* Step 1: Mode register 0x1881: (val & 0xFF7C) | 0x0081 */
	ret = fte3600_spi_rmw_reg16(data, 0x1881, 0xFF7C, 0x0081);
	if (ret)
		goto err_fail;

	/* Step 2: Detection filter register 0x1A83: (val & 0xFF80) | 0x0016 */
	ret = fte3600_spi_rmw_reg16(data, 0x1A83, 0xFF80, 0x0016);
	if (ret)
		goto err_fail;

	/* Step 3: Clear interrupt latch 0x1A84 = 0x002F */
	ret = fte3600_spi_write_reg16(data, 0x1A84, 0x002F);
	if (ret)
		goto err_fail;

	/* Step 4: Dispatch Arm strobe { 0xC2, 0x3D, 0x00 } */
	ret = fte3600_spi_cmd_strobe(data, 0xC2, 0x3D);
	if (ret)
		goto err_fail;

	/* Step 5: Stabilization wait 1 ms */
	usleep_range(1000, 1500);

	data->state = FTE3600_STATE_READY;
	data->last_error = 0;
	return 0;

err_fail:
	data->state = FTE3600_STATE_UNKNOWN;
	data->last_error = (u32)(-ret);
	return ret;
}

/* FIFO Burst Frame Capture Pipeline (64x80 matrix, 10240 bytes payload) */
static int fte3600_hw_capture_frame(struct fte3600_data *data,
				    void __user *user_buf, size_t buf_len)
{
	u8 status_80, dac;
	int ret, iter;
	bool capture_done = false;
	u16 period_val, val_1800 = 0;

	lockdep_assert_held(&data->lock);

	if (atomic_read(&data->offline) || !data->spi)
		return -ENODEV;

	if (data->state != FTE3600_STATE_READY)
		return -EPERM;

	if (buf_len != FTE3600_IMAGE_BYTES)
		return -EINVAL;

	if (!user_buf)
		return -EFAULT;

	data->state = FTE3600_STATE_CAPTURING;

	/* Step 1: Ensure hardware is in idle state 0x50 */
	ret = fte3600_spi_cmd_strobe(data, 0x5A, 0xA5);
	if (ret)
		goto err;
	usleep_range(1000, 1500);

	ret = fte3600_spi_read_sfr(data, 0x80, &status_80);
	if (ret)
		goto err;

	if (status_80 != 0x50) {
		ret = fte3600_spi_cmd_strobe(data, 0xC0, 0x3F);
		if (ret)
			goto err;
		usleep_range(1000, 1500);
		ret = fte3600_spi_read_sfr(data, 0x80, &status_80);
		if (ret || status_80 != 0x50) {
			ret = -EIO;
			goto err;
		}
	}

	ret = fte3600_spi_cmd_strobe(data, 0xA5, 0x5A);
	if (ret)
		goto err;

	/* Step 2: Configure DAC and enable pixel matrix */
	dac = data->session.dac_valid ? data->session.stored_dac : 54;
	ret = fte3600_spi_write_reg16(data, 0x1801, 0xFC80 | (dac & 0x7F));
	if (ret)
		goto err;

	ret = fte3600_spi_write_reg16(data, 0x1800, 0x4FFE);
	if (ret)
		goto err;

	period_val = ((u16)(data->line_period_us - 1) << 5) | 1;
	ret = fte3600_spi_write_reg16(data, 0x1804, 0x27CA);
	if (ret)
		goto err;

	ret = fte3600_spi_rmw_reg16(data, 0x1806, 0xC07F, 0x0480);
	if (ret)
		goto err;

	ret = fte3600_spi_rmw_reg16(data, 0x180A, 0xC000, 0x0483);
	if (ret)
		goto err;

	ret = fte3600_spi_rmw_reg16(data, 0x180B, 0xC000, 0x0208);
	if (ret)
		goto err;

	ret = fte3600_spi_write_reg16(data, 0x1807, period_val);
	if (ret)
		goto err;

	ret = fte3600_spi_write_reg16(data, 0x1887, 0x0002);
	if (ret)
		goto err;

	/* Step 3: Transmit Capture Trigger Strobe { 0xC4, 0x3B, 0x00 } */
	ret = fte3600_spi_cmd_strobe(data, 0xC4, 0x3B);
	if (ret)
		goto err;

	/* Step 4: Poll status 0x80 for capture completion (0x54) */
	for (iter = 0; iter < 20; iter++) {
		usleep_range(1000, 1500);
		ret = fte3600_spi_read_sfr(data, 0x80, &status_80);
		if (ret)
			goto err;
		if (status_80 == 0x54) {
			capture_done = true;
			break;
		}
	}

	if (!capture_done) {
		dev_err(&data->spi->dev, "Capture completion timeout: status 0x80 = 0x%02x\n",
			status_80);
		ret = -ETIMEDOUT;
		goto err;
	}

	/* Read register 0x1800 and set bit 0 to enable FIFO clocking (keeping 0x4FFE matrix asserted) */
	ret = fte3600_spi_read_reg16(data, 0x1800, &val_1800, NULL);
	if (ret)
		goto err;

	ret = fte3600_spi_write_reg16(data, 0x1800, val_1800 | 0x4FFE | 1);
	if (ret)
		goto err;
	usleep_range(1000, 1500);

	/* Step 5: Read 10,246-byte FIFO in a single burst transfer */
	{
		u8 *tx_burst, *rx_burst;

		tx_burst = kzalloc(FTE3600_FIFO_TRANSFER_LEN, GFP_KERNEL);
		if (!tx_burst) {
			ret = -ENOMEM;
			goto err;
		}
		rx_burst = kzalloc(FTE3600_FIFO_TRANSFER_LEN, GFP_KERNEL);
		if (!rx_burst) {
			kfree(tx_burst);
			ret = -ENOMEM;
			goto err;
		}

		/* 6-byte burst read header: Opcode 0x06 (inverted 0xF9), Reg 0x1A05 (0x9A, 0x05), 5120 words (0x14, 0x00) */
		tx_burst[0] = 0x06;
		tx_burst[1] = 0xF9;
		tx_burst[2] = 0x9A;
		tx_burst[3] = 0x05;
		tx_burst[4] = 0x14;
		tx_burst[5] = 0x00;

		ret = fte3600_spi_sync_transfer(data, tx_burst, rx_burst, FTE3600_FIFO_TRANSFER_LEN);
		if (ret) {
			kfree(tx_burst);
			kfree(rx_burst);
			goto err;
		}

		dev_info(&data->spi->dev, "FIFO burst (10246 bytes) rx[0..15] = %*ph\n",
			 16, rx_burst);

		/* Step 6: Acknowledge capture interrupt (0x1A84 = 0x0020) AFTER reading FIFO */
		ret = fte3600_spi_write_reg16(data, 0x1A84, 0x0020);
		if (ret) {
			kfree(tx_burst);
			kfree(rx_burst);
			goto err;
		}

		/* Step 6b: Return to idle strobe { 0x5A, 0xA5, 0x00 } (Event 199 in events_0) */
		fte3600_spi_cmd_strobe(data, 0x5A, 0xA5);

		/* Step 7: Copy 10,240-byte pixel payload to userspace */
		if (copy_to_user(user_buf, rx_burst + FTE3600_FIFO_PREFIX_LEN, FTE3600_IMAGE_BYTES)) {
			kfree(tx_burst);
			kfree(rx_burst);
			ret = -EFAULT;
			goto err;
		}

		kfree(tx_burst);
		kfree(rx_burst);
	}

	data->state = FTE3600_STATE_READY;
	data->last_error = 0;
	return 0;

err:
	data->state = FTE3600_STATE_UNKNOWN;
	data->last_error = (u32)(-ret);
	return ret;
}

/* Character Device: Open with Single-Client Exclusive Lease */
static int fte3600_cdev_open(struct inode *inode, struct file *filp)
{
	struct fte3600_data *data = container_of(inode->i_cdev, struct fte3600_data, cdev);

	if (atomic_read(&data->offline))
		return -ENODEV;

	if (atomic_cmpxchg(&data->is_open, 0, 1) != 0)
		return -EBUSY;

	if (atomic_read(&data->offline)) {
		atomic_set(&data->is_open, 0);
		return -ENODEV;
	}

	get_device(&data->dev);
	filp->private_data = data;
	return 0;
}

/* Character Device: Release */
static int fte3600_cdev_release(struct inode *inode, struct file *filp)
{
	struct fte3600_data *data = filp->private_data;

	/*
	 * DEADLOCK REMEDIATION INVARIANT:
	 * cancel_delayed_work_sync() MUST be called BEFORE acquiring data->lock.
	 * fte3600_watchdog_work_fn() acquires data->lock; cancelling while holding
	 * data->lock causes an immediate kernel deadlock.
	 */
	cancel_delayed_work_sync(&data->session.watchdog_work);

	mutex_lock(&data->lock);
	if (data->session.active && data->session.owner_filp == filp) {
		data->session.active = false;
		data->session.owner_filp = NULL;
		data->session.generation++;
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = ECANCELED;
	}
	mutex_unlock(&data->lock);

	atomic_set(&data->is_open, 0);
	put_device(&data->dev);
	return 0;
}

/* Character Device: Common IOCTL Handler */
static long fte3600_ioctl_common(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct fte3600_data *data = filp->private_data;
	int ret = 0;

	if (_IOC_TYPE(cmd) != FTE3600_IOC_MAGIC)
		return -ENOTTY;

	/* Fast-path check: if offline, reject all commands except GET_STATE */
	if (atomic_read(&data->offline) && cmd != FTE3600_IOC_GET_STATE)
		return -ENODEV;

	switch (cmd) {
	case FTE3600_IOC_GET_STATE: {
		struct fte3600_state_snapshot snap;

		if (mutex_lock_interruptible(&data->lock))
			return -ERESTARTSYS;

		memset(&snap, 0, sizeof(snap));
		snap.version = FTE3600_UAPI_VERSION;
		snap.state = fte3600_state_to_uapi(data->state);
		if (atomic_read(&data->offline) || data->state == FTE3600_STATE_DEVICE_REMOVAL)
			snap.flags |= FTE3600_FLAG_OFFLINE;
		if (data->state == FTE3600_STATE_RESET_SEQUENCE_ISSUED)
			snap.flags |= FTE3600_FLAG_RESET_SEQUENCE_ISSUED;
		if (data->state == FTE3600_STATE_INITIALIZED_UNCALIBRATED)
			snap.flags |= FTE3600_FLAG_STARTUP_PREFIX_COMPLETED | FTE3600_FLAG_CALIBRATION_PENDING;
		if (data->session.active)
			snap.flags |= FTE3600_FLAG_SESSION_ACTIVE;
		snap.last_error = data->last_error;

		mutex_unlock(&data->lock);

		if (copy_to_user((void __user *)arg, &snap, sizeof(snap)))
			return -EFAULT;
		break;
	}
	case FTE3600_IOC_RESET: {
		/*
		 * Cancel session watchdog synchronously outside data->lock
		 * to prevent potential deadlock.
		 */
		cancel_delayed_work_sync(&data->session.watchdog_work);

		if (mutex_lock_interruptible(&data->lock))
			return -ERESTARTSYS;

		if (atomic_read(&data->offline) || !data->reset_gpiod) {
			mutex_unlock(&data->lock);
			return -ENODEV;
		}

		/* Invalidate any active session upon electrical reset */
		if (data->session.active) {
			data->session.active = false;
			data->session.owner_filp = NULL;
			data->session.generation++;
		}

		ret = fte3600_hw_reset_pulse(data);
		if (ret)
			data->last_error = (u32)(-ret);

		mutex_unlock(&data->lock);
		break;
	}
	case FTE3600_IOC_INIT: {
		struct fte3600_init_params params;
		int i;

		if (copy_from_user(&params, (void __user *)arg, sizeof(params)))
			return -EFAULT;

		if (params.version != FTE3600_UAPI_VERSION)
			return -EINVAL;

		if (params.flags & ~FTE3600_INIT_FLAGS_MASK)
			return -EINVAL;

		for (i = 0; i < 6; i++) {
			if (params.reserved[i] != 0)
				return -EINVAL;
		}

		if (mutex_lock_interruptible(&data->lock))
			return -ERESTARTSYS;

		if (atomic_read(&data->offline) || !data->spi) {
			mutex_unlock(&data->lock);
			return -ENODEV;
		}

		if (data->state == FTE3600_STATE_RESET_SEQUENCE_ISSUED) {
			ret = fte3600_hw_identify(data);
			if (ret == 0 && (params.flags & FTE3600_INIT_FLAG_FULL_STARTUP))
				ret = fte3600_hw_startup(data);
		} else if (data->state == FTE3600_STATE_IDENTIFIED) {
			ret = fte3600_hw_startup(data);
		} else {
			ret = -EPERM;
		}

		if (ret)
			data->last_error = (u32)(-ret);

		mutex_unlock(&data->lock);
		break;
	}
	case FTE3600_IOC_CAPTURE: {
		struct fte3600_frame_buffer fb;
		int i;

		if (copy_from_user(&fb, (void __user *)arg, sizeof(fb)))
			return -EFAULT;

		if (fb.version != FTE3600_UAPI_VERSION)
			return -EINVAL;

		if (fb.length != FTE3600_IMAGE_BYTES)
			return -EINVAL;

		for (i = 0; i < 4; i++) {
			if (fb.reserved[i] != 0)
				return -EINVAL;
		}

		if (mutex_lock_interruptible(&data->lock))
			return -ERESTARTSYS;

		if (atomic_read(&data->offline) || !data->spi) {
			mutex_unlock(&data->lock);
			return -ENODEV;
		}

		/* Strict Capture-Gating Enforcement against non-READY states */
		if (data->state != FTE3600_STATE_READY) {
			mutex_unlock(&data->lock);
			return -EPERM;
		}

		ret = fte3600_hw_capture_frame(data, (void __user *)fb.buffer_ptr, fb.length);
		mutex_unlock(&data->lock);
		return ret;
	}
	case FTE3600_IOC_SESSION_BEGIN: {
		struct fte3600_session_begin sb;
		u32 rand_val;
		int i;

		if (copy_from_user(&sb, (void __user *)arg, sizeof(sb)))
			return -EFAULT;

		if (sb.version != FTE3600_UAPI_VERSION || sb.flags != 0)
			return -EINVAL;

		for (i = 0; i < 3; i++) {
			if (sb.reserved[i] != 0)
				return -EINVAL;
		}

		if (mutex_lock_interruptible(&data->lock))
			return -ERESTARTSYS;

		if (atomic_read(&data->offline) || !data->spi) {
			mutex_unlock(&data->lock);
			return -ENODEV;
		}

		/* Must be at least INITIALIZED_UNCALIBRATED or READY to begin calibration session */
		if (data->state != FTE3600_STATE_INITIALIZED_UNCALIBRATED &&
		    data->state != FTE3600_STATE_READY) {
			mutex_unlock(&data->lock);
			return -EPERM;
		}

		/* Check exclusive OFD lease */
		if (data->session.active && data->session.owner_filp != filp) {
			mutex_unlock(&data->lock);
			return -EBUSY;
		}

		/* Generate generation-keyed 64-bit token */
		get_random_bytes(&rand_val, sizeof(rand_val));
		data->session.token = ((u64)data->session.generation << 32) | (u64)rand_val;
		data->session.active = true;
		data->session.owner_filp = filp;
		data->session.channel_mode = 1;
		data->session.stored_dac = 27;
		data->session.dac_valid = false;
		data->session.thresholds_programmed = false;

		/* Arm 5.0 s watchdog timer */
		mod_delayed_work(fte3600_wq, &data->session.watchdog_work,
				 msecs_to_jiffies(FTE3600_WATCHDOG_DEFAULT_MS));

		sb.session_token = data->session.token;
		sb.watchdog_ms = FTE3600_WATCHDOG_DEFAULT_MS;

		mutex_unlock(&data->lock);

		if (copy_to_user((void __user *)arg, &sb, sizeof(sb)))
			return -EFAULT;
		break;
	}
	case FTE3600_IOC_SESSION_FDT_SAMPLE: {
		struct fte3600_session_fdt sfdt;
		int i;

		if (copy_from_user(&sfdt, (void __user *)arg, sizeof(sfdt)))
			return -EFAULT;

		if (sfdt.version != FTE3600_UAPI_VERSION)
			return -EINVAL;

		for (i = 0; i < 6; i++) {
			if (sfdt.reserved[i] != 0)
				return -EINVAL;
		}

		if (sfdt.channel_mode != 1)
			return -EINVAL;

		if (sfdt.dac_val < 1 || sfdt.dac_val > 125)
			return -EINVAL;

		if (mutex_lock_interruptible(&data->lock))
			return -ERESTARTSYS;

		ret = fte3600_validate_session(data, filp, sfdt.session_token);
		if (ret) {
			mutex_unlock(&data->lock);
			return ret;
		}

		ret = fte3600_hw_fdt_sample(data, &sfdt);
		if (ret) {
			data->last_error = (u32)(-ret);
			mutex_unlock(&data->lock);
			return ret;
		}

		mutex_unlock(&data->lock);

		if (copy_to_user((void __user *)arg, &sfdt, sizeof(sfdt)))
			return -EFAULT;
		break;
	}
	case FTE3600_IOC_SESSION_READ_FRAME: {
		struct fte3600_session_frame sframe;

		if (copy_from_user(&sframe, (void __user *)arg, sizeof(sframe)))
			return -EFAULT;

		if (sframe.version != FTE3600_UAPI_VERSION || sframe.length != FTE3600_IMAGE_BYTES ||
		    sframe.reserved != 0)
			return -EINVAL;

		if (mutex_lock_interruptible(&data->lock))
			return -ERESTARTSYS;

		ret = fte3600_validate_session(data, filp, sframe.session_token);
		if (ret) {
			mutex_unlock(&data->lock);
			return ret;
		}

		/*
		 * If session is in INITIALIZED_UNCALIBRATED during baseline acquisition,
		 * temporarily permit frame capture and restore uncalibrated state.
		 */
		if (data->state == FTE3600_STATE_INITIALIZED_UNCALIBRATED) {
			data->state = FTE3600_STATE_READY;
			ret = fte3600_hw_capture_frame(data, (void __user *)sframe.buffer_ptr, sframe.length);
			if (ret == 0)
				data->state = FTE3600_STATE_INITIALIZED_UNCALIBRATED;
		} else {
			ret = fte3600_hw_capture_frame(data, (void __user *)sframe.buffer_ptr, sframe.length);
		}

		mutex_unlock(&data->lock);
		return ret;
	}
	case FTE3600_IOC_SESSION_SET_THRESHOLDS: {
		struct fte3600_session_thresholds sthresh;
		int i;

		if (copy_from_user(&sthresh, (void __user *)arg, sizeof(sthresh)))
			return -EFAULT;

		if (sthresh.version != FTE3600_UAPI_VERSION || sthresh.bank_len != FTE3600_THRESHOLD_BANK_LEN)
			return -EINVAL;

		for (i = 0; i < 2; i++) {
			if (sthresh.reserved[i] != 0)
				return -EINVAL;
		}

		if (mutex_lock_interruptible(&data->lock))
			return -ERESTARTSYS;

		ret = fte3600_validate_session(data, filp, sthresh.session_token);
		if (ret) {
			mutex_unlock(&data->lock);
			return ret;
		}

		ret = fte3600_hw_set_thresholds(data, &sthresh);
		if (ret) {
			data->last_error = (u32)(-ret);
			mutex_unlock(&data->lock);
			return ret;
		}

		mutex_unlock(&data->lock);
		break;
	}
	case FTE3600_IOC_SESSION_COMMIT: {
		struct fte3600_session_commit scommit;
		int i;

		if (copy_from_user(&scommit, (void __user *)arg, sizeof(scommit)))
			return -EFAULT;

		if (scommit.version != FTE3600_UAPI_VERSION || scommit.flags != 0)
			return -EINVAL;

		for (i = 0; i < 3; i++) {
			if (scommit.reserved[i] != 0)
				return -EINVAL;
		}

		/* Cancel watchdog outside data->lock */
		cancel_delayed_work_sync(&data->session.watchdog_work);

		if (mutex_lock_interruptible(&data->lock))
			return -ERESTARTSYS;

		ret = fte3600_validate_session(data, filp, scommit.session_token);
		if (ret) {
			mutex_unlock(&data->lock);
			return ret;
		}

		if (!data->session.thresholds_programmed) {
			mutex_unlock(&data->lock);
			return -EPERM;
		}

		/* Arm touch sensor and transition to READY */
		ret = fte3600_hw_arm_sensor(data);
		if (ret) {
			mutex_unlock(&data->lock);
			return ret;
		}

		data->session.active = false;
		data->session.owner_filp = NULL;
		data->session.generation++;
		scommit.final_state = (u32)FTE3600_STATE_READY;
		mutex_unlock(&data->lock);

		if (copy_to_user((void __user *)arg, &scommit, sizeof(scommit)))
			return -EFAULT;
		break;
	}
	case FTE3600_IOC_SESSION_ABORT: {
		struct fte3600_session_abort sabort;
		int i;

		if (copy_from_user(&sabort, (void __user *)arg, sizeof(sabort)))
			return -EFAULT;

		if (sabort.version != FTE3600_UAPI_VERSION)
			return -EINVAL;

		for (i = 0; i < 4; i++) {
			if (sabort.reserved[i] != 0)
				return -EINVAL;
		}

		/* Cancel watchdog outside data->lock */
		cancel_delayed_work_sync(&data->session.watchdog_work);

		if (mutex_lock_interruptible(&data->lock))
			return -ERESTARTSYS;

		ret = fte3600_validate_session(data, filp, sabort.session_token);
		if (ret) {
			mutex_unlock(&data->lock);
			return ret;
		}

		data->session.active = false;
		data->session.owner_filp = NULL;
		data->session.generation++;
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = ECANCELED;

		mutex_unlock(&data->lock);
		break;
	}
	default:
		return -ENOTTY;
	}

	return ret;
}

static long fte3600_cdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return fte3600_ioctl_common(filp, cmd, arg);
}

#ifdef CONFIG_COMPAT
static long fte3600_cdev_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return fte3600_ioctl_common(filp, cmd, (unsigned long)compat_ptr(arg));
}
#endif

/* Character Device: Poll Handler */
static __poll_t fte3600_cdev_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct fte3600_data *data = filp->private_data;

	if (atomic_read(&data->offline))
		return EPOLLERR | EPOLLHUP;

	poll_wait(filp, &data->event_wq, wait);

	if (atomic_read(&data->offline))
		return EPOLLERR | EPOLLHUP;

	return 0;
}

/* File Operations Specification */
static const struct file_operations fte3600_fops = {
	.owner          = THIS_MODULE,
	.open           = fte3600_cdev_open,
	.release        = fte3600_cdev_release,
	.unlocked_ioctl = fte3600_cdev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = fte3600_cdev_compat_ioctl,
#endif
	.poll           = fte3600_cdev_poll,
	.llseek         = noop_llseek,
};

/*
 * FocalTech Match-on-Host SPI Interface (/dev/focal_moh_spi)
 *
 * Provides compatibility bridge for userspace libfprint-2.so (FocalTech vendor stack).
 */
static struct fte3600_data *g_fte3600_data;

static int focal_moh_open(struct inode *inode, struct file *filp)
{
	struct fte3600_data *data = g_fte3600_data;

	if (!data || atomic_read(&data->offline))
		return -ENODEV;

	filp->private_data = data;
	return 0;
}

static int focal_moh_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static ssize_t focal_moh_read(struct file *filp, char __user *pUserBuf, size_t count, loff_t *f_pos)
{
	struct fte3600_data *data = filp->private_data;
	struct focal_spi_read_buff *spi_buf;
	u16 tx_len = 0, rx_len = 0;
	int ret;

	if (!data || !data->spi || atomic_read(&data->offline))
		return -ENODEV;

	if (count > FOCAL_MAX_BUFF_SIZE || count < sizeof(struct focal_spi_read_buff))
		return 0;

	if (mutex_lock_interruptible(&data->lock))
		return -ERESTARTSYS;

	if (copy_from_user(data->focal_wr_buf, pUserBuf, count)) {
		mutex_unlock(&data->lock);
		return 0;
	}

	spi_buf = (struct focal_spi_read_buff *)data->focal_wr_buf;

	if (spi_buf->type == FOCAL_SPI_BACK_DATA) {
		if (copy_to_user(pUserBuf, data->sensor_init_data, count)) {
			mutex_unlock(&data->lock);
			return 0;
		}
		mutex_unlock(&data->lock);
		return count;
	}

	if (spi_buf->type == FOCAL_SPI_READ_WRITE) {
		tx_len = spi_buf->tx_len;
		rx_len = spi_buf->rx_len;
	} else if (spi_buf->type == FOCAL_SPI_READ_ONLY) {
		rx_len = spi_buf->rx_len;
		if (spi_buf->tx_len != 0) {
			mutex_unlock(&data->lock);
			return 0;
		}
	} else {
		mutex_unlock(&data->lock);
		return 0;
	}

	if (tx_len > FOCAL_MAX_BUFF_SIZE || rx_len > FOCAL_MAX_BUFF_SIZE || rx_len == 0 ||
	    tx_len + rx_len > FOCAL_MAX_BUFF_SIZE) {
		mutex_unlock(&data->lock);
		return 0;
	}

	if (tx_len > 0)
		ret = spi_write_then_read(data->spi, spi_buf->txbuff, tx_len, data->focal_rd_buf, rx_len);
	else
		ret = spi_read(data->spi, data->focal_rd_buf, rx_len);

	if (ret < 0) {
		mutex_unlock(&data->lock);
		return ret;
	}

	if (copy_to_user(pUserBuf, data->focal_rd_buf, rx_len)) {
		mutex_unlock(&data->lock);
		return 0;
	}

	mutex_unlock(&data->lock);
	return count;
}

static ssize_t focal_moh_write(struct file *filp, const char __user *pUserBuf, size_t count, loff_t *f_pos)
{
	struct fte3600_data *data = filp->private_data;
	struct focal_spi_read_buff *spi_buf;
	int ret;

	if (!data || !data->spi || atomic_read(&data->offline))
		return -ENODEV;

	if (count > FOCAL_MAX_BUFF_SIZE || count < sizeof(struct focal_spi_read_buff))
		return 0;

	if (mutex_lock_interruptible(&data->lock))
		return -ERESTARTSYS;

	if (copy_from_user(data->focal_wr_buf, pUserBuf, count)) {
		mutex_unlock(&data->lock);
		return 0;
	}

	spi_buf = (struct focal_spi_read_buff *)data->focal_wr_buf;
	if (spi_buf->type == FOCAL_SPI_BACK_DATA) {
		memcpy(data->sensor_init_data, data->focal_wr_buf, count);
		mutex_unlock(&data->lock);
		return count;
	}

	ret = spi_write(data->spi, data->focal_wr_buf + sizeof(struct focal_spi_read_buff),
			count - sizeof(struct focal_spi_read_buff));
	mutex_unlock(&data->lock);
	if (ret < 0)
		return ret;
	return count;
}

static long focal_moh_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct fte3600_data *data = filp->private_data;
	int ret = 0;

	if (!data || atomic_read(&data->offline))
		return -ENODEV;

	switch (cmd) {
	case FF_IOC_RESET_DEV:
		if (mutex_lock_interruptible(&data->lock))
			return -ERESTARTSYS;
		ret = fte3600_hw_reset_pulse(data);
		mutex_unlock(&data->lock);
		break;
	case FF_IOC_POWER_OFF:
		if (data->reset_gpiod)
			gpiod_set_value_cansleep(data->reset_gpiod, 1);
		break;
	case FF_IOC_POWER_ON:
		if (data->reset_gpiod)
			gpiod_set_value_cansleep(data->reset_gpiod, 0);
		break;
	case FF_IOC_IRQ_EN:
		break;
	case FF_IOC_LOG_EN:
		break;
	case FF_IOC_RELEASE_POLL:
		data->focal_work_flag = (int)arg;
		if (arg)
			wake_up_interruptible(&data->focal_poll_wq);
		break;
	case FF_IOC_CSn:
		break;
	default:
		break;
	}
	return ret;
}

static __poll_t focal_moh_poll(struct file *file, poll_table *wait)
{
	struct fte3600_data *data = file->private_data;
	__poll_t mask = 0;

	if (!data || atomic_read(&data->offline))
		return EPOLLERR | EPOLLHUP;

	poll_wait(file, &data->focal_poll_wq, wait);

	if (data->focal_work_flag > 0)
		mask = EPOLLIN | EPOLLRDNORM;

	data->focal_work_flag = 0;
	return mask;
}

static const struct file_operations focal_moh_fops = {
	.owner          = THIS_MODULE,
	.open           = focal_moh_open,
	.release        = focal_moh_release,
	.read           = focal_moh_read,
	.write          = focal_moh_write,
	.unlocked_ioctl = focal_moh_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = focal_moh_ioctl,
#endif
	.poll           = focal_moh_poll,
};

/* Passive SPI Driver Probe */
static int fte3600_probe(struct spi_device *spi)
{
	struct fte3600_data *data;
	struct fte3600_controller_domain *domain;
	int ret;

	if (!ACPI_COMPANION(&spi->dev))
		return -ENODEV;

	/* Verify host SPI controller identity and allocate domain */
	domain = fte3600_find_or_alloc_domain(spi);
	if (IS_ERR(domain))
		return PTR_ERR(domain);

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->spi = spi;
	data->domain = domain;
	spi_set_drvdata(spi, data);

	mutex_init(&data->lock);
	init_waitqueue_head(&data->event_wq);
	atomic_set(&data->is_open, 0);
	atomic_set(&data->offline, 0);
	data->state = FTE3600_STATE_UNINITIALIZED;
	data->foundry_code = 0;
	data->line_period_us = 200;

	/* Initialize session management */
	data->session.active = false;
	data->session.generation = 1;
	data->session.token = 0;
	data->session.owner_filp = NULL;
	INIT_DELAYED_WORK(&data->session.watchdog_work, fte3600_watchdog_work_fn);

	/* Initialize embedded struct device */
	device_initialize(&data->dev);
	data->dev.devt = fte3600_devt;
	data->dev.class = fte3600_class;
	data->dev.parent = &spi->dev;
	data->dev.release = fte3600_device_release;
	ret = dev_set_name(&data->dev, FTE3600_DRIVER_NAME);
	if (ret) {
		put_device(&data->dev);
		return ret;
	}

	/* Register driver GPIO mapping table */
	ret = devm_acpi_dev_add_driver_gpios(&spi->dev, fte3600_acpi_gpios);
	if (ret) {
		dev_err_probe(&spi->dev, ret, "Failed to add ACPI driver GPIOs\n");
		put_device(&data->dev);
		return ret;
	}

	/* Acquire reset GPIO descriptor */
	data->reset_gpiod = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(data->reset_gpiod)) {
		ret = dev_err_probe(&spi->dev, PTR_ERR(data->reset_gpiod),
				      "Failed to acquire reset GPIO\n");
		put_device(&data->dev);
		return ret;
	}

	/* Configure SPI bus parameters: Mode 0, 8-bit, 1 MHz */
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	spi->max_speed_hz = 1000000;
	ret = spi_setup(spi);
	if (ret) {
		dev_err_probe(&spi->dev, ret, "Failed to configure SPI bus\n");
		put_device(&data->dev);
		return ret;
	}

	/* Register character device node */
	cdev_init(&data->cdev, &fte3600_fops);
	data->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&data->cdev, &data->dev);
	if (ret) {
		dev_err_probe(&spi->dev, ret, "Failed to add cdev device\n");
		put_device(&data->dev);
		return ret;
	}

	/* Register /dev/focal_moh_spi miscdevice for libfprint compatibility */
	data->focal_wr_buf = kzalloc(FOCAL_MAX_BUFF_SIZE, GFP_KERNEL);
	data->focal_rd_buf = kzalloc(FOCAL_MAX_BUFF_SIZE, GFP_KERNEL);
	data->sensor_init_data = kzalloc(FOCAL_MAX_BUFF_SIZE, GFP_KERNEL);
	init_waitqueue_head(&data->focal_poll_wq);
	data->focal_work_flag = FOCAL_WAKE_EVENT_NONE;

	data->focal_miscdev.minor = MISC_DYNAMIC_MINOR;
	data->focal_miscdev.name = "focal_moh_spi";
	data->focal_miscdev.fops = &focal_moh_fops;
	data->focal_miscdev.mode = 0666;
	g_fte3600_data = data;

	ret = misc_register(&data->focal_miscdev);
	if (ret)
		dev_warn(&spi->dev, "Could not register /dev/focal_moh_spi: %d\n", ret);
	else
		dev_info(&spi->dev, "Created /dev/focal_moh_spi compatibility bridge\n");

	dev_info(&spi->dev, "FTE3600 driver bound (passive probe, state UNINITIALIZED, domain %s)\n",
		 domain->pci_bdf);
	return 0;
}

/* SPI Driver Removal */
static void fte3600_remove(struct spi_device *spi)
{
	struct fte3600_data *data = spi_get_drvdata(spi);

	if (!data)
		return;

	/*
	 * DEADLOCK REMEDIATION INVARIANT:
	 * cancel_delayed_work_sync() MUST execute before acquiring data->lock.
	 */
	cancel_delayed_work_sync(&data->session.watchdog_work);

	if (data->focal_miscdev.this_device)
		misc_deregister(&data->focal_miscdev);
	g_fte3600_data = NULL;

	cdev_device_del(&data->cdev, &data->dev);

	mutex_lock(&data->lock);
	atomic_set(&data->offline, 1);
	data->state = FTE3600_STATE_DEVICE_REMOVAL;
	data->session.active = false;
	data->session.owner_filp = NULL;
	data->session.generation++;
	data->reset_gpiod = NULL;
	data->spi = NULL;
	mutex_unlock(&data->lock);

	wake_up_all(&data->event_wq);
	spi_set_drvdata(spi, NULL);
	put_device(&data->dev);
	dev_info(&spi->dev, "FTE3600 driver unbound\n");
}

#ifdef CONFIG_PM_SLEEP
static int fte3600_pm_suspend(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct fte3600_data *data = spi_get_drvdata(spi);

	if (!data)
		return 0;

	mutex_lock(&data->lock);
	data->focal_work_flag = FOCAL_WAKE_EVENT_SUSPEND;
	wake_up_interruptible(&data->focal_poll_wq);
	mutex_unlock(&data->lock);

	return 0;
}

static int fte3600_pm_resume(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct fte3600_data *data = spi_get_drvdata(spi);

	if (!data)
		return 0;

	mutex_lock(&data->lock);
	if (data->session.active) {
		data->session.active = false;
		data->session.owner_filp = NULL;
		data->session.generation++;
	}

	/* Pulse Pad 14 GPIO reset to re-synchronize silicon from low-power sleep */
	fte3600_hw_reset_pulse(data);
	msleep(15);
	data->focal_work_flag = FOCAL_WAKE_EVENT_RESUME;
	wake_up_interruptible(&data->focal_poll_wq);
	mutex_unlock(&data->lock);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(fte3600_pm_ops, fte3600_pm_suspend, fte3600_pm_resume);
#define FTE3600_PM_OPS (&fte3600_pm_ops)
#else
#define FTE3600_PM_OPS NULL
#endif

/* Native SPI Driver Definition */
static struct spi_driver fte3600_spi_driver = {
	.driver = {
		.name             = FTE3600_DRIVER_NAME,
		.acpi_match_table = ACPI_PTR(fte3600_acpi_ids),
		.pm               = FTE3600_PM_OPS,
	},
	.probe  = fte3600_probe,
	.remove = fte3600_remove,
};

/* Module Initialization and Teardown */
static int __init fte3600_module_init(void)
{
	int ret;

	fte3600_wq = alloc_workqueue("fte3600_wq", WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!fte3600_wq)
		return -ENOMEM;

	ret = alloc_chrdev_region(&fte3600_devt, 0, 1, FTE3600_DRIVER_NAME);
	if (ret) {
		destroy_workqueue(fte3600_wq);
		return ret;
	}

	fte3600_class = class_create(FTE3600_DRIVER_NAME);
	if (IS_ERR(fte3600_class)) {
		ret = PTR_ERR(fte3600_class);
		unregister_chrdev_region(fte3600_devt, 1);
		destroy_workqueue(fte3600_wq);
		return ret;
	}

	ret = spi_register_driver(&fte3600_spi_driver);
	if (ret) {
		class_destroy(fte3600_class);
		unregister_chrdev_region(fte3600_devt, 1);
		destroy_workqueue(fte3600_wq);
		return ret;
	}

	return 0;
}

static void __exit fte3600_module_exit(void)
{
	spi_unregister_driver(&fte3600_spi_driver);
	class_destroy(fte3600_class);
	unregister_chrdev_region(fte3600_devt, 1);
	destroy_workqueue(fte3600_wq);
}

module_init(fte3600_module_init);
module_exit(fte3600_module_exit);

MODULE_DESCRIPTION("Unified Linux Driver for FocalTech FT9362 / FW9369 Fingerprint Sensor");
MODULE_AUTHOR("Antigravity Project");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");

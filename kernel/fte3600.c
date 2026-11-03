// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTE3600 Unified Linux Kernel Driver
 * Target Silicon: FocalTech FT9362 / FW9369 Fingerprint Sensor
 * Target Platform: Dynabook Satellite Pro C40-J (PYS46G-06J02D)
 *
 * Architecture Stage 2E.4.2: SPI Completion Verification and Identification-State Corrections
 * Safety Gate: Physical hardware access strictly prohibited; offline compilation only.
 */

#include <linux/acpi.h>
#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "fte3600.h"

#define FTE3600_DRIVER_NAME "fte3600"

/* Device Private Structure */
struct fte3600_data {
	struct spi_device *spi;
	struct gpio_desc *reset_gpiod;
	struct cdev cdev;
	struct device dev;             /* Embedded device providing kobject refcounting */
	struct mutex lock;             /* Serializes reset, transport, and shutdown */
	wait_queue_head_t event_wq;
	enum fte3600_state state;
	atomic_t is_open;              /* Driver-owned single-client exclusive lease */
	atomic_t offline;              /* Set to 1 on unbind/shutdown */
	u32 last_error;
	u8 tx_buf[32] ____cacheline_aligned;
	u8 rx_buf[32] ____cacheline_aligned;
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
 *
 * This helper calculates the 16-bit CRC transmitted by the FT9362 silicon over
 * the SPI wire. Polynomial: 0x1021 (x^16 + x^12 + x^5 + 1), Initial Seed: 0xFFFF,
 * Input: MSB-first byte stream.
 *
 * Test Vector:
 * Byte sequence [0x93, 0x62] (Chip ID 0x9362) -> CRC 0x1C53.
 *
 * Active Driver Integration (Stage 2E.4):
 * Actively called by fte3600_hw_identify() to validate wire CRC received from
 * Chip ID register 0x1A8B during FTE3600_IOC_INIT execution.
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

	mutex_destroy(&data->lock);
	kfree(data);
}

/*
 * Active-Low Reset Sequence Helper (Policy A: Triggered Exclusively via IOCTL)
 *
 * Locking Context: Must be called with data->lock held.
 *
 * Important GPIO API Limitation:
 * gpiod_set_value_cansleep() returns void. Therefore, successful completion of
 * this sequence only proves that register writes were issued to the Intel PCH
 * LPSS GPIO controller. It does NOT verify physical pin voltage on Pad 14 or
 * that the sensor silicon accepted the reset or entered a known state.
 *
 * State Semantics:
 * State transitions to FTE3600_STATE_RESET_SEQUENCE_ISSUED (compatibility alias
 * FTE3600_STATE_RESET_COMPLETE). This indicates strictly that the electrical
 * reset timing pulse was dispatched to the GPIO controller. It does NOT indicate
 * that the sensor silicon is initialized, identified, calibrated, or ready.
 * Physical sensor recovery and responsiveness are only proven if subsequent SPI
 * chip ID and CRC interrogation succeeds.
 */
static int fte3600_hw_reset_pulse(struct fte3600_data *data)
{
	lockdep_assert_held(&data->lock);

	if (atomic_read(&data->offline) || !data->reset_gpiod)
		return -ENODEV;

	data->state = FTE3600_STATE_RESETTING;

	/* Pre-reset settle interval (Logical 0 = physical HIGH / inactive) */
	gpiod_set_value_cansleep(data->reset_gpiod, 0);
	msleep(10);

	/* Assert reset: Logical 1 = physical LOW / asserted */
	gpiod_set_value_cansleep(data->reset_gpiod, 1);
	msleep(20); /* Reference hold duration (recovery.c:19) */

	/* Deassert reset: Logical 0 = physical HIGH / inactive */
	gpiod_set_value_cansleep(data->reset_gpiod, 0);
	msleep(10); /* Reference settle latency (factory_startup.c:341) */

	/*
	 * Electrical reset pulse sequence completed on GPIO controller.
	 * State is RESET_SEQUENCE_ISSUED. Sensor silicon responsiveness is unverified.
	 */
	data->state = FTE3600_STATE_RESET_SEQUENCE_ISSUED;
	return 0;
}

/*
 * In-Kernel Synchronous SPI Transfer Engine
 *
 * Locking Context:
 * Caller MUST hold data->lock. Enforced via lockdep_assert_held(&data->lock).
 *
 * Buffer Lifetime:
 * Both tx and rx buffers must remain allocated and valid for the entire duration
 * of the synchronous spi_sync() call. Stack DMA buffers and GFP_DMA are prohibited.
 *
 * Return: 0 on success, negative errno on failure. On error, data->state is
 * transitioned to FTE3600_STATE_UNKNOWN to fail closed.
 */
static int fte3600_spi_sync_transfer(struct fte3600_data *data,
				     const u8 *tx, u8 *rx, size_t len)
{
	struct spi_transfer t;
	struct spi_message m;
	int ret;

	lockdep_assert_held(&data->lock);

	/* Check device validity and shutdown state */
	if (!data->spi || atomic_read(&data->offline))
		return -ENODEV;

	/* Validate buffer pointers: at least one must be non-NULL */
	if (!tx && !rx)
		return -EINVAL;

	/* Validate transfer length: reject zero-length and oversized transfers */
	if (len == 0 || len > FTE3600_MAX_SPI_TRANSFER_LEN)
		return -EINVAL;

	memset(&t, 0, sizeof(t));
	t.tx_buf = tx;
	t.rx_buf = rx;
	t.len = len;
	t.cs_change = 0;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);

	ret = spi_sync(data->spi, &m);
	if (ret) {
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = (u32)(-ret);
		return ret;
	}

	if (m.status != 0) {
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = (u32)(-m.status);
		return m.status;
	}

	/* Verify full transfer completion: short transfers are treated as failed */
	if (m.actual_length != len) {
		data->state = FTE3600_STATE_UNKNOWN;
		data->last_error = EIO;
		return -EIO;
	}

	return 0;
}

/*
 * Minimal FT9362 Silicon Identification Sequence
 *
 * Implements the minimal, read-only hardware identification sequence verified
 * in Stage 2A.
 *
 * Preconditions:
 * 1. Caller MUST hold data->lock. Enforced via lockdep_assert_held(&data->lock).
 * 2. Device must be online with valid data->spi.
 * 3. Driver state MUST be FTE3600_STATE_RESET_SEQUENCE_ISSUED (electrical reset pulse issued).
 *
 * Sequence:
 * Step A: Write volatile register 0xC6 = 0x01 (ungate SPI transceiver clock, 4 bytes)
 * Step B: Transceiver stabilization delay 4 ms (CS deasserted HIGH)
 * Step C: Read back register 0xC6 (verify rx[4] == 0x01, 5 bytes)
 * Step D: Interrogate Chip ID register 0x1A8B (read 10 bytes)
 * Step E: Wire CRC-16 verification (CCITT poly 0x1021, seed 0xFFFF over rx[6..7])
 * Step F: Variant gate & fault evaluation (require ID 0x9362, CRC 0x1C53)
 *
 * State Transitions:
 * Upon starting: state = FTE3600_STATE_INITIALIZING
 * Upon success:  state = FTE3600_STATE_IDENTIFIED, last_error = 0, return 0
 * Upon error:    state = FTE3600_STATE_UNKNOWN, last_error = (u32)(-ret), return ret
 *
 * Fail-Closed Invariant:
 * On any transport failure, verification mismatch, or invalid ID/CRC, the driver
 * transitions to FTE3600_STATE_UNKNOWN. No automatic retries or undocumented cleanup
 * writes are issued. Recovery strictly requires a subsequent userspace FTE3600_IOC_RESET.
 *
 * Semantic Boundary:
 * Transitions state to FTE3600_STATE_IDENTIFIED, NOT FTE3600_STATE_READY. Chip
 * identification verifies bus responsiveness and silicon identity; full operational
 * capture readiness (AFE calibration, baseline, FIFO arming) is deferred to future stages.
 */
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

	/* Identification verified: transition state to IDENTIFIED (NOT READY) */
	data->state = FTE3600_STATE_IDENTIFIED;
	data->last_error = 0;
	dev_info(&data->spi->dev,
		 "FocalTech FT9362 identified (ID 0x%04x, CRC 0x%04x, state IDENTIFIED)\n",
		 chip_id, crc_rx);
	return 0;
}

/* Character Device: Open with Single-Client Exclusive Lease */
static int fte3600_cdev_open(struct inode *inode, struct file *filp)
{
	struct fte3600_data *data = container_of(inode->i_cdev, struct fte3600_data, cdev);

	/* Reject immediately if device was already unbound */
	if (atomic_read(&data->offline))
		return -ENODEV;

	/* Enforce single-client exclusive lease */
	if (atomic_cmpxchg(&data->is_open, 0, 1) != 0)
		return -EBUSY;

	/* Re-check offline after acquiring lease in case remove ran concurrently */
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

	atomic_set(&data->is_open, 0);
	put_device(&data->dev);
	return 0;
}

/* Character Device: IOCTL Handler */
static long fte3600_ioctl_common(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct fte3600_data *data = filp->private_data;
	int ret = 0;

	if (_IOC_TYPE(cmd) != FTE3600_IOC_MAGIC)
		return -ENOTTY;

	/* Fast-path check: if already offline, reject hardware commands immediately */
	if (atomic_read(&data->offline) && cmd != FTE3600_IOC_GET_STATE)
		return -ENODEV;

	switch (cmd) {
	case FTE3600_IOC_GET_STATE: {
		struct fte3600_state_snapshot snap;

		if (mutex_lock_interruptible(&data->lock))
			return -ERESTARTSYS;

		memset(&snap, 0, sizeof(snap));
		snap.version = FTE3600_UAPI_VERSION;
		snap.state = (u32)data->state;
		if (atomic_read(&data->offline))
			snap.flags |= FTE3600_FLAG_OFFLINE;
		if (data->state == FTE3600_STATE_RESET_SEQUENCE_ISSUED)
			snap.flags |= FTE3600_FLAG_RESET_SEQUENCE_ISSUED;
		snap.last_error = data->last_error;

		mutex_unlock(&data->lock);

		if (copy_to_user((void __user *)arg, &snap, sizeof(snap)))
			return -EFAULT;
		break;
	}
	case FTE3600_IOC_RESET: {
		if (mutex_lock_interruptible(&data->lock))
			return -ERESTARTSYS;

		/*
		 * Hardware-Resource Shutdown Protocol:
		 * Re-check offline status and GPIO descriptor validity under data->lock!
		 * If fte3600_remove() executed while we waited for the lock, offline
		 * is now 1 and reset_gpiod is NULL. We abort without touching hardware.
		 */
		if (atomic_read(&data->offline) || !data->reset_gpiod) {
			mutex_unlock(&data->lock);
			return -ENODEV;
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

		for (i = 0; i < 6; i++) {
			if (params.reserved[i] != 0)
				return -EINVAL;
		}

		if (mutex_lock_interruptible(&data->lock))
			return -ERESTARTSYS;

		/* Re-check offline status and SPI bus pointer under data->lock */
		if (atomic_read(&data->offline) || !data->spi) {
			mutex_unlock(&data->lock);
			return -ENODEV;
		}

		ret = fte3600_hw_identify(data);

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

		/* Physical FIFO capture deferred in Stage 2E.2 software milestone */
		return -EOPNOTSUPP;
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
	return fte3600_ioctl_common(filp, cmd, arg);
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

/* Passive SPI Driver Probe */
static int fte3600_probe(struct spi_device *spi)
{
	struct fte3600_data *data;
	int ret;

	if (!ACPI_COMPANION(&spi->dev))
		return -ENODEV;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->spi = spi;
	spi_set_drvdata(spi, data);

	mutex_init(&data->lock);
	init_waitqueue_head(&data->event_wq);
	atomic_set(&data->is_open, 0);
	atomic_set(&data->offline, 0);
	data->state = FTE3600_STATE_UNINITIALIZED;

	/*
	 * Initialize embedded struct device and configure metadata.
	 * device_initialize() sets data->dev reference count to 1.
	 */
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

	/* Register driver GPIO mapping table against &spi->dev before lookup */
	ret = devm_acpi_dev_add_driver_gpios(&spi->dev, fte3600_acpi_gpios);
	if (ret) {
		dev_err_probe(&spi->dev, ret, "Failed to add ACPI driver GPIOs\n");
		put_device(&data->dev);
		return ret;
	}

	/* Acquire reset GPIO descriptor with initial logical 0 (physical HIGH / inactive) */
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

	/* Register character device node and bind to device model using cdev_device_add */
	cdev_init(&data->cdev, &fte3600_fops);
	data->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&data->cdev, &data->dev);
	if (ret) {
		dev_err_probe(&spi->dev, ret, "Failed to add cdev device\n");
		put_device(&data->dev);
		return ret;
	}

	/* Initial state remains strictly UNINITIALIZED; zero physical pulses/transactions issued */
	dev_info(&spi->dev, "FTE3600 driver bound (passive probe, state UNINITIALIZED)\n");
	return 0;
}

/* SPI Driver Removal */
static void fte3600_remove(struct spi_device *spi)
{
	struct fte3600_data *data = spi_get_drvdata(spi);

	if (!data)
		return;

	/*
	 * Step 1: Unregister character device and sysfs node.
	 * cdev_device_del() deletes the cdev from chrdev lookup and unlinks
	 * the device from sysfs, ensuring no new opens can reach fte3600_cdev_open.
	 */
	cdev_device_del(&data->cdev, &data->dev);

	/*
	 * Step 2: Hardware-Resource Shutdown Protocol.
	 * Acquire data->lock to serialize with any in-flight or waiting hardware operations.
	 */
	mutex_lock(&data->lock);

	/*
	 * Step 3: Invalidate hardware access under lock.
	 * Mark offline and nullify pointers to devres-managed resources before remove() returns.
	 */
	atomic_set(&data->offline, 1);
	data->reset_gpiod = NULL;
	data->spi = NULL;

	/*
	 * Step 4: Release data->lock.
	 * Any operation that was blocked waiting on data->lock will now wake up,
	 * observe atomic_read(&data->offline) == 1 and data->reset_gpiod == NULL,
	 * and abort with -ENODEV without accessing released devres memory.
	 */
	mutex_unlock(&data->lock);

	/*
	 * Step 5: Wake any sleeping pollers. They will observe offline and return EPOLLERR | EPOLLHUP.
	 */
	wake_up_all(&data->event_wq);

	spi_set_drvdata(spi, NULL);

	/*
	 * Step 6: Drop the driver probe reference.
	 * Context memory will be freed in fte3600_device_release() when all open
	 * file descriptors are closed by userspace.
	 */
	put_device(&data->dev);
	dev_info(&spi->dev, "FTE3600 driver unbound\n");
}

/* Native SPI Driver Definition */
static struct spi_driver fte3600_spi_driver = {
	.driver = {
		.name            = FTE3600_DRIVER_NAME,
		.acpi_match_table = ACPI_PTR(fte3600_acpi_ids),
	},
	.probe  = fte3600_probe,
	.remove = fte3600_remove,
};

/* Module Initialization and Teardown */
static int __init fte3600_module_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&fte3600_devt, 0, 1, FTE3600_DRIVER_NAME);
	if (ret)
		return ret;

	fte3600_class = class_create(FTE3600_DRIVER_NAME);
	if (IS_ERR(fte3600_class)) {
		ret = PTR_ERR(fte3600_class);
		unregister_chrdev_region(fte3600_devt, 1);
		return ret;
	}

	ret = spi_register_driver(&fte3600_spi_driver);
	if (ret) {
		class_destroy(fte3600_class);
		unregister_chrdev_region(fte3600_devt, 1);
		return ret;
	}

	return 0;
}

static void __exit fte3600_module_exit(void)
{
	spi_unregister_driver(&fte3600_spi_driver);
	class_destroy(fte3600_class);
	unregister_chrdev_region(fte3600_devt, 1);
}

module_init(fte3600_module_init);
module_exit(fte3600_module_exit);

MODULE_DESCRIPTION("Unified Linux Driver for FocalTech FT9362 / FW9369 Fingerprint Sensor");
MODULE_AUTHOR("Antigravity Project");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");

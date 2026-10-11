// SPDX-License-Identifier: GPL-2.0-only
/*
 * InvenSense ICM-42607/ICM-42670 6-axis IMU, SPI interface
 *
 * The ICM-42607-P and ICM-42670-P share a register map that differs from the
 * ICM-426xx parts handled by inv_icm42600. This driver keeps to the basics:
 * direct reads of the accelerometer, gyroscope and die temperature in
 * low-noise mode, plus a triggered buffer fed by an external trigger (for
 * example an IIO hrtimer trigger). The interrupt pin, FIFO and APEX motion
 * features are not used.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#define ICM42607_REG_DEVICE_CONFIG		0x01
#define ICM42607_DEVICE_CONFIG_SPI_AP_4WIRE	BIT(2)
#define ICM42607_REG_SIGNAL_PATH_RESET		0x02
#define ICM42607_SIGNAL_PATH_RESET_SOFT		BIT(4)
#define ICM42607_REG_TEMP_DATA1			0x09
#define ICM42607_REG_ACCEL_DATA_X1		0x0b
#define ICM42607_REG_GYRO_DATA_X1		0x11
#define ICM42607_REG_PWR_MGMT0			0x1f
#define ICM42607_PWR_MGMT0_GYRO_MODE		GENMASK(3, 2)
#define ICM42607_PWR_MGMT0_ACCEL_MODE		GENMASK(1, 0)
#define ICM42607_MODE_LOW_NOISE			0x3
#define ICM42607_REG_GYRO_CONFIG0		0x20
#define ICM42607_REG_ACCEL_CONFIG0		0x21
#define ICM42607_CONFIG0_FS_SEL			GENMASK(6, 5)
#define ICM42607_CONFIG0_ODR			GENMASK(3, 0)
#define ICM42607_REG_INT_STATUS			0x3a
#define ICM42607_REG_INTF_CONFIG1		0x36
#define ICM42607_INTF_CONFIG1_CLKSEL		GENMASK(1, 0)
#define ICM42607_CLKSEL_PLL			0x1
#define ICM42607_REG_WHOAMI			0x75
#define ICM42607_REG_MAX			0x7f

#define ICM42607_WHOAMI_ICM42607P		0x60
#define ICM42607_WHOAMI_ICM42670P		0x67

/* Data registers from TEMP_DATA1 to GYRO_DATA_Z0, big endian. */
#define ICM42607_DATA_WORDS			7

#define ICM42607_SENSOR_ACCEL			BIT(0)
#define ICM42607_SENSOR_GYRO			BIT(1)

/* Settle times after a sensor is switched on, from the datasheet tables. */
#define ICM42607_ACCEL_STARTUP_MS		20
#define ICM42607_GYRO_STARTUP_MS		45
#define ICM42607_SUSPEND_DELAY_MS		2000

enum icm42607_scan {
	ICM42607_SCAN_ACCEL_X,
	ICM42607_SCAN_ACCEL_Y,
	ICM42607_SCAN_ACCEL_Z,
	ICM42607_SCAN_GYRO_X,
	ICM42607_SCAN_GYRO_Y,
	ICM42607_SCAN_GYRO_Z,
	ICM42607_SCAN_TEMP,
	ICM42607_SCAN_TIMESTAMP,
};

/* Position of each scan element in the TEMP_DATA1..GYRO_DATA_Z0 block. */
static const u8 icm42607_scan_word[] = {
	[ICM42607_SCAN_ACCEL_X] = 1,
	[ICM42607_SCAN_ACCEL_Y] = 2,
	[ICM42607_SCAN_ACCEL_Z] = 3,
	[ICM42607_SCAN_GYRO_X] = 4,
	[ICM42607_SCAN_GYRO_Y] = 5,
	[ICM42607_SCAN_GYRO_Z] = 6,
	[ICM42607_SCAN_TEMP] = 0,
};

struct icm42607_odr {
	int val;
	int val2;
	u8 code;
};

/* Low-noise mode rates shared by the accelerometer and the gyroscope. */
static const struct icm42607_odr icm42607_odrs[] = {
	{ 12, 500000, 0xc },
	{ 25, 0, 0xb },
	{ 50, 0, 0xa },
	{ 100, 0, 0x9 },
	{ 200, 0, 0x8 },
	{ 400, 0, 0x7 },
	{ 800, 0, 0x6 },
	{ 1600, 0, 0x5 },
};

/* Scales in nano units per LSB, indexed by FS_SEL. */
static const int icm42607_accel_scales[][2] = {
	{ 0, 4788403 },		/* +-16 g: 9.80665 / 2048 m/s^2 */
	{ 0, 2394202 },		/* +-8 g */
	{ 0, 1197101 },		/* +-4 g */
	{ 0, 598550 },		/* +-2 g */
};

static const int icm42607_gyro_scales[][2] = {
	{ 0, 1064225 },		/* +-2000 dps: pi / (180 * 16.4) rad/s */
	{ 0, 532113 },		/* +-1000 dps */
	{ 0, 266462 },		/* +-500 dps: pi / (180 * 65.5) */
	{ 0, 133231 },		/* +-250 dps: pi / (180 * 131) */
};

#define ICM42607_ACCEL_FS_DEFAULT	1	/* +-8 g */
#define ICM42607_GYRO_FS_DEFAULT	0	/* +-2000 dps */
#define ICM42607_ODR_DEFAULT		2	/* 50 Hz */

struct icm42607_state {
	struct regmap *map;
	struct device *dev;
	/* Serialises register access and the cached configuration below. */
	struct mutex lock;
	struct iio_mount_matrix orientation;
	unsigned int powered;		/* sensors currently switched on */
	unsigned int buffer_sensors;	/* sensors needed by the buffer */
	unsigned int odr;		/* index into icm42607_odrs */
	unsigned int accel_fs;
	unsigned int gyro_fs;
	struct {
		__be16 data[ICM42607_DATA_WORDS];
		aligned_s64 timestamp;
	} scan;
	__be16 raw[ICM42607_DATA_WORDS];
};

static const struct iio_mount_matrix *
icm42607_get_mount_matrix(const struct iio_dev *indio_dev,
			  const struct iio_chan_spec *chan)
{
	const struct icm42607_state *st = iio_priv(indio_dev);

	return &st->orientation;
}

static const struct iio_chan_spec_ext_info icm42607_ext_info[] = {
	IIO_MOUNT_MATRIX(IIO_SHARED_BY_TYPE, icm42607_get_mount_matrix),
	{ }
};

#define ICM42607_CHAN(_type, _axis, _index) {				\
	.type = _type,							\
	.modified = 1,							\
	.channel2 = IIO_MOD_##_axis,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
	.info_mask_shared_by_type_available = BIT(IIO_CHAN_INFO_SCALE),	\
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
	.info_mask_shared_by_all_available =				\
		BIT(IIO_CHAN_INFO_SAMP_FREQ),				\
	.scan_index = _index,						\
	.scan_type = {							\
		.sign = 's',						\
		.realbits = 16,						\
		.storagebits = 16,					\
		.endianness = IIO_BE,					\
	},								\
	.ext_info = icm42607_ext_info,					\
}

static const struct iio_chan_spec icm42607_channels[] = {
	ICM42607_CHAN(IIO_ACCEL, X, ICM42607_SCAN_ACCEL_X),
	ICM42607_CHAN(IIO_ACCEL, Y, ICM42607_SCAN_ACCEL_Y),
	ICM42607_CHAN(IIO_ACCEL, Z, ICM42607_SCAN_ACCEL_Z),
	ICM42607_CHAN(IIO_ANGL_VEL, X, ICM42607_SCAN_GYRO_X),
	ICM42607_CHAN(IIO_ANGL_VEL, Y, ICM42607_SCAN_GYRO_Y),
	ICM42607_CHAN(IIO_ANGL_VEL, Z, ICM42607_SCAN_GYRO_Z),
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET),
		.scan_index = ICM42607_SCAN_TEMP,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_BE,
		},
	},
	IIO_CHAN_SOFT_TIMESTAMP(ICM42607_SCAN_TIMESTAMP),
};

static unsigned int icm42607_chan_sensors(const struct iio_chan_spec *chan)
{
	switch (chan->type) {
	case IIO_ACCEL:
		return ICM42607_SENSOR_ACCEL;
	case IIO_ANGL_VEL:
		return ICM42607_SENSOR_GYRO;
	default:
		/* The temperature sensor runs while either sensor is on. */
		return ICM42607_SENSOR_ACCEL;
	}
}

static int icm42607_write_config0(struct icm42607_state *st, unsigned int reg,
				  unsigned int fs, unsigned int odr)
{
	return regmap_write(st->map, reg,
			    FIELD_PREP(ICM42607_CONFIG0_FS_SEL, fs) |
			    FIELD_PREP(ICM42607_CONFIG0_ODR,
				       icm42607_odrs[odr].code));
}

/* Switch the given sensors on (and the others off) and wait for their data. */
static int icm42607_set_power(struct icm42607_state *st, unsigned int sensors)
{
	unsigned int started = sensors & ~st->powered;
	unsigned int val = 0, wait_ms = 0;
	const struct icm42607_odr *odr = &icm42607_odrs[st->odr];
	int ret;

	if (sensors == st->powered)
		return 0;

	if (sensors & ICM42607_SENSOR_ACCEL)
		val |= FIELD_PREP(ICM42607_PWR_MGMT0_ACCEL_MODE,
				  ICM42607_MODE_LOW_NOISE);
	if (sensors & ICM42607_SENSOR_GYRO)
		val |= FIELD_PREP(ICM42607_PWR_MGMT0_GYRO_MODE,
				  ICM42607_MODE_LOW_NOISE);

	ret = regmap_write(st->map, ICM42607_REG_PWR_MGMT0, val);
	if (ret)
		return ret;
	st->powered = sensors;

	/* No register write is allowed for 200 us after a mode change. */
	fsleep(200);

	if (started & ICM42607_SENSOR_GYRO)
		wait_ms = ICM42607_GYRO_STARTUP_MS;
	else if (started & ICM42607_SENSOR_ACCEL)
		wait_ms = ICM42607_ACCEL_STARTUP_MS;
	if (wait_ms) {
		/* And one output period for the first sample. */
		wait_ms += DIV_ROUND_UP(1000000, odr->val * 1000 + odr->val2 / 1000);
		msleep(wait_ms);
	}

	return 0;
}

static int icm42607_read_word(struct icm42607_state *st,
			      const struct iio_chan_spec *chan, int *val)
{
	unsigned int reg;
	int ret;

	switch (chan->type) {
	case IIO_ACCEL:
		reg = ICM42607_REG_ACCEL_DATA_X1;
		break;
	case IIO_ANGL_VEL:
		reg = ICM42607_REG_GYRO_DATA_X1;
		break;
	case IIO_TEMP:
		reg = ICM42607_REG_TEMP_DATA1;
		break;
	default:
		return -EINVAL;
	}
	if (chan->modified)
		reg += 2 * (chan->channel2 - IIO_MOD_X);

	ret = pm_runtime_resume_and_get(st->dev);
	if (ret)
		return ret;

	mutex_lock(&st->lock);
	ret = icm42607_set_power(st, st->powered | icm42607_chan_sensors(chan));
	if (!ret)
		ret = regmap_bulk_read(st->map, reg, st->raw, sizeof(st->raw[0]));
	if (!ret)
		*val = (s16)be16_to_cpu(st->raw[0]);
	mutex_unlock(&st->lock);

	pm_runtime_put_autosuspend(st->dev);

	return ret;
}

static int icm42607_read_raw(struct iio_dev *indio_dev,
			     const struct iio_chan_spec *chan,
			     int *val, int *val2, long mask)
{
	struct icm42607_state *st = iio_priv(indio_dev);
	const int (*scale)[2];
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = icm42607_read_word(st, chan, val);
		iio_device_release_direct(indio_dev);
		return ret ? ret : IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ACCEL:
			scale = &icm42607_accel_scales[st->accel_fs];
			break;
		case IIO_ANGL_VEL:
			scale = &icm42607_gyro_scales[st->gyro_fs];
			break;
		case IIO_TEMP:
			/* T = raw / 128 + 25 degrees C, in milli degrees C. */
			*val = 7;
			*val2 = 812500;
			return IIO_VAL_INT_PLUS_MICRO;
		default:
			return -EINVAL;
		}
		*val = (*scale)[0];
		*val2 = (*scale)[1];
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_OFFSET:
		if (chan->type != IIO_TEMP)
			return -EINVAL;
		*val = 25 * 128;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = icm42607_odrs[st->odr].val;
		*val2 = icm42607_odrs[st->odr].val2;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static const int icm42607_odr_avail[] = {
	12, 500000, 25, 0, 50, 0, 100, 0, 200, 0, 400, 0, 800, 0, 1600, 0,
};

static int icm42607_read_avail(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       const int **vals, int *type, int *length,
			       long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		*type = IIO_VAL_INT_PLUS_NANO;
		if (chan->type == IIO_ACCEL) {
			*vals = (const int *)icm42607_accel_scales;
			*length = ARRAY_SIZE(icm42607_accel_scales) * 2;
		} else if (chan->type == IIO_ANGL_VEL) {
			*vals = (const int *)icm42607_gyro_scales;
			*length = ARRAY_SIZE(icm42607_gyro_scales) * 2;
		} else {
			return -EINVAL;
		}
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*type = IIO_VAL_INT_PLUS_MICRO;
		*vals = icm42607_odr_avail;
		*length = ARRAY_SIZE(icm42607_odr_avail);
		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}
}

static int icm42607_write_scale(struct icm42607_state *st,
				const struct iio_chan_spec *chan,
				int val, int val2)
{
	const int (*scales)[2];
	unsigned int i, n, reg, *fs;

	if (chan->type == IIO_ACCEL) {
		scales = icm42607_accel_scales;
		n = ARRAY_SIZE(icm42607_accel_scales);
		reg = ICM42607_REG_ACCEL_CONFIG0;
		fs = &st->accel_fs;
	} else if (chan->type == IIO_ANGL_VEL) {
		scales = icm42607_gyro_scales;
		n = ARRAY_SIZE(icm42607_gyro_scales);
		reg = ICM42607_REG_GYRO_CONFIG0;
		fs = &st->gyro_fs;
	} else {
		return -EINVAL;
	}

	for (i = 0; i < n; i++)
		if (scales[i][0] == val && scales[i][1] == val2)
			break;
	if (i == n)
		return -EINVAL;

	guard(mutex)(&st->lock);
	*fs = i;
	return icm42607_write_config0(st, reg, i, st->odr);
}

static int icm42607_write_odr(struct icm42607_state *st, int val, int val2)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(icm42607_odrs); i++)
		if (icm42607_odrs[i].val == val && icm42607_odrs[i].val2 == val2)
			break;
	if (i == ARRAY_SIZE(icm42607_odrs))
		return -EINVAL;

	guard(mutex)(&st->lock);
	ret = icm42607_write_config0(st, ICM42607_REG_ACCEL_CONFIG0,
				     st->accel_fs, i);
	if (ret)
		return ret;
	ret = icm42607_write_config0(st, ICM42607_REG_GYRO_CONFIG0,
				     st->gyro_fs, i);
	if (ret)
		return ret;
	st->odr = i;

	return 0;
}

static int icm42607_write_raw(struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan,
			      int val, int val2, long mask)
{
	struct icm42607_state *st = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = icm42607_write_scale(st, chan, val, val2);
		iio_device_release_direct(indio_dev);
		return ret;
	case IIO_CHAN_INFO_SAMP_FREQ:
		return icm42607_write_odr(st, val, val2);
	default:
		return -EINVAL;
	}
}

static int icm42607_write_raw_get_fmt(struct iio_dev *indio_dev,
				      const struct iio_chan_spec *chan,
				      long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int icm42607_update_scan_mode(struct iio_dev *indio_dev,
				     const unsigned long *scan_mask)
{
	struct icm42607_state *st = iio_priv(indio_dev);
	unsigned int sensors = 0;

	if (*scan_mask & (BIT(ICM42607_SCAN_ACCEL_X) |
			  BIT(ICM42607_SCAN_ACCEL_Y) |
			  BIT(ICM42607_SCAN_ACCEL_Z) |
			  BIT(ICM42607_SCAN_TEMP)))
		sensors |= ICM42607_SENSOR_ACCEL;
	if (*scan_mask & (BIT(ICM42607_SCAN_GYRO_X) |
			  BIT(ICM42607_SCAN_GYRO_Y) |
			  BIT(ICM42607_SCAN_GYRO_Z)))
		sensors |= ICM42607_SENSOR_GYRO;

	guard(mutex)(&st->lock);
	st->buffer_sensors = sensors;
	return icm42607_set_power(st, st->powered | sensors);
}

static const struct iio_info icm42607_info = {
	.read_raw = icm42607_read_raw,
	.read_avail = icm42607_read_avail,
	.write_raw = icm42607_write_raw,
	.write_raw_get_fmt = icm42607_write_raw_get_fmt,
	.update_scan_mode = icm42607_update_scan_mode,
};

static int icm42607_buffer_preenable(struct iio_dev *indio_dev)
{
	struct icm42607_state *st = iio_priv(indio_dev);

	return pm_runtime_resume_and_get(st->dev);
}

static int icm42607_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct icm42607_state *st = iio_priv(indio_dev);

	mutex_lock(&st->lock);
	st->buffer_sensors = 0;
	mutex_unlock(&st->lock);

	pm_runtime_put_autosuspend(st->dev);

	return 0;
}

static const struct iio_buffer_setup_ops icm42607_buffer_ops = {
	.preenable = icm42607_buffer_preenable,
	.postdisable = icm42607_buffer_postdisable,
};

static irqreturn_t icm42607_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct icm42607_state *st = iio_priv(indio_dev);
	unsigned int bit, i = 0;
	int ret;

	mutex_lock(&st->lock);
	ret = regmap_bulk_read(st->map, ICM42607_REG_TEMP_DATA1, st->raw,
			       sizeof(st->raw));
	if (!ret) {
		memset(&st->scan, 0, sizeof(st->scan));
		iio_for_each_active_channel(indio_dev, bit)
			st->scan.data[i++] = st->raw[icm42607_scan_word[bit]];
		iio_push_to_buffers_with_ts(indio_dev, &st->scan,
					    sizeof(st->scan), pf->timestamp);
	}
	mutex_unlock(&st->lock);

	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static int icm42607_chip_init(struct icm42607_state *st)
{
	unsigned int whoami, val;
	int ret;

	/* The AP interface may come up in 3-wire mode; SDO is wired here. */
	ret = regmap_write(st->map, ICM42607_REG_DEVICE_CONFIG,
			   ICM42607_DEVICE_CONFIG_SPI_AP_4WIRE);
	if (ret)
		return ret;

	ret = regmap_write(st->map, ICM42607_REG_SIGNAL_PATH_RESET,
			   ICM42607_SIGNAL_PATH_RESET_SOFT);
	if (ret)
		return ret;
	fsleep(2000);

	/* The soft reset also resets the serial interface configuration. */
	ret = regmap_write(st->map, ICM42607_REG_DEVICE_CONFIG,
			   ICM42607_DEVICE_CONFIG_SPI_AP_4WIRE);
	if (ret)
		return ret;

	ret = regmap_read(st->map, ICM42607_REG_WHOAMI, &whoami);
	if (ret)
		return ret;
	switch (whoami) {
	case ICM42607_WHOAMI_ICM42607P:
	case ICM42607_WHOAMI_ICM42670P:
		break;
	case 0x00:
	case 0xff:
		return dev_err_probe(st->dev, -ENODEV,
				     "no device (WHO_AM_I 0x%02x)\n", whoami);
	default:
		dev_warn(st->dev, "unknown WHO_AM_I 0x%02x, continuing\n",
			 whoami);
		break;
	}

	/* Clear the reset-done status. */
	ret = regmap_read(st->map, ICM42607_REG_INT_STATUS, &val);
	if (ret)
		return ret;

	ret = regmap_update_bits(st->map, ICM42607_REG_INTF_CONFIG1,
				 ICM42607_INTF_CONFIG1_CLKSEL,
				 FIELD_PREP(ICM42607_INTF_CONFIG1_CLKSEL,
					    ICM42607_CLKSEL_PLL));
	if (ret)
		return ret;

	ret = icm42607_write_config0(st, ICM42607_REG_ACCEL_CONFIG0,
				     st->accel_fs, st->odr);
	if (ret)
		return ret;
	ret = icm42607_write_config0(st, ICM42607_REG_GYRO_CONFIG0,
				     st->gyro_fs, st->odr);
	if (ret)
		return ret;

	ret = regmap_write(st->map, ICM42607_REG_PWR_MGMT0, 0);
	if (ret)
		return ret;
	st->powered = 0;

	dev_dbg(st->dev, "WHO_AM_I 0x%02x\n", whoami);

	return 0;
}

static void icm42607_power_off(void *data)
{
	struct icm42607_state *st = data;

	regmap_write(st->map, ICM42607_REG_PWR_MGMT0, 0);
}

static const struct regmap_config icm42607_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = ICM42607_REG_MAX,
	.read_flag_mask = BIT(7),
};

static int icm42607_probe(struct spi_device *spi)
{
	static const char * const supplies[] = { "vdd", "vddio" };
	struct device *dev = &spi->dev;
	struct icm42607_state *st;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->dev = dev;
	st->odr = ICM42607_ODR_DEFAULT;
	st->accel_fs = ICM42607_ACCEL_FS_DEFAULT;
	st->gyro_fs = ICM42607_GYRO_FS_DEFAULT;
	ret = devm_mutex_init(dev, &st->lock);
	if (ret)
		return ret;

	st->map = devm_regmap_init_spi(spi, &icm42607_regmap_config);
	if (IS_ERR(st->map))
		return dev_err_probe(dev, PTR_ERR(st->map),
				     "failed to init regmap\n");

	ret = devm_regulator_bulk_get_enable(dev, ARRAY_SIZE(supplies),
					     supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable supplies\n");
	/* Power-up time before the first register access. */
	fsleep(3000);

	ret = iio_read_mount_matrix(dev, &st->orientation);
	if (ret)
		return ret;

	ret = icm42607_chip_init(st);
	if (ret)
		return dev_err_probe(dev, ret, "chip initialisation failed\n");

	ret = devm_add_action_or_reset(dev, icm42607_power_off, st);
	if (ret)
		return ret;

	indio_dev->name = "icm42607";
	indio_dev->info = &icm42607_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = icm42607_channels;
	indio_dev->num_channels = ARRAY_SIZE(icm42607_channels);

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      iio_pollfunc_store_time,
					      icm42607_trigger_handler,
					      &icm42607_buffer_ops);
	if (ret)
		return ret;

	spi_set_drvdata(spi, indio_dev);

	ret = devm_pm_runtime_set_active_enabled(dev);
	if (ret)
		return ret;
	pm_runtime_set_autosuspend_delay(dev, ICM42607_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);

	return devm_iio_device_register(dev, indio_dev);
}

static int icm42607_runtime_suspend(struct device *dev)
{
	struct icm42607_state *st = iio_priv(dev_get_drvdata(dev));

	guard(mutex)(&st->lock);
	return icm42607_set_power(st, 0);
}

static int icm42607_runtime_resume(struct device *dev)
{
	struct icm42607_state *st = iio_priv(dev_get_drvdata(dev));

	/* Bring the buffer's sensors back after a system suspend. */
	guard(mutex)(&st->lock);
	return icm42607_set_power(st, st->buffer_sensors);
}

static DEFINE_RUNTIME_DEV_PM_OPS(icm42607_pm_ops, icm42607_runtime_suspend,
				 icm42607_runtime_resume, NULL);

static const struct of_device_id icm42607_of_match[] = {
	{ .compatible = "invensense,icm42607p" },
	{ .compatible = "invensense,icm42670p" },
	{ }
};
MODULE_DEVICE_TABLE(of, icm42607_of_match);

static const struct spi_device_id icm42607_spi_id[] = {
	{ "icm42607p" },
	{ "icm42670p" },
	{ }
};
MODULE_DEVICE_TABLE(spi, icm42607_spi_id);

static struct spi_driver icm42607_driver = {
	.driver = {
		.name = "inv-icm42607",
		.of_match_table = icm42607_of_match,
		.pm = pm_ptr(&icm42607_pm_ops),
	},
	.probe = icm42607_probe,
	.id_table = icm42607_spi_id,
};
module_spi_driver(icm42607_driver);

MODULE_DESCRIPTION("InvenSense ICM-42607/ICM-42670 IMU SPI driver");
MODULE_LICENSE("GPL");

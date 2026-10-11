// SPDX-License-Identifier: GPL-2.0-only
/*
 * Orient Chip OCH1970 3D Hall sensor used as a magnetic scroll wheel
 *
 * The sensor sits next to a diametrically magnetised wheel. Turning the wheel
 * rotates the field in the sensor's X/Z plane, so the wheel angle is
 * atan2(X, Z). Like the vendor driver, the sensor is polled and every step of
 * at least OCH1970_STEP_DEG reports one KEY_UP or KEY_DOWN press.
 */

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>

#define OCH1970_REG_WIA			0x00
#define OCH1970_REG_ST_X_Y_Z		0x17	/* ST[15:0], X, Y, Z: 8 bytes */
#define OCH1970_REG_CNTL2		0x21

#define OCH1970_CNTL2_KEEP		GENMASK(7, 6)
#define OCH1970_CNTL2_SMR_HIGH_RANGE	BIT(5)
#define OCH1970_CNTL2_SDR_LOW_POWER	BIT(4)
#define OCH1970_CNTL2_MODE_STANDBY	0x00
#define OCH1970_CNTL2_MODE_CONT_100HZ	0x0c

#define OCH1970_POLL_MS			50
#define OCH1970_STEP_DEG		30
#define OCH1970_REBASE_MS		3000

struct och1970 {
	struct i2c_client *client;
	struct input_dev *input;
	u8 cntl2;		/* standby value with the measurement settings */
	bool have_base;
	int base_deg;
	unsigned long base_time;
};

/* tan(0..45 degrees) * 65536 */
static const u32 och1970_tan_q16[] = {
	0, 1144, 2289, 3435, 4583, 5734, 6888, 8047,
	9210, 10380, 11556, 12739, 13930, 15130, 16340, 17560,
	18792, 20036, 21294, 22566, 23853, 25157, 26478, 27818,
	29179, 30560, 31964, 33392, 34846, 36327, 37837, 39378,
	40951, 42560, 44205, 45889, 47615, 49385, 51202, 53070,
	54991, 56970, 59009, 61113, 63287, 65536,
};

/* atan(small / big) in whole degrees, 0..45, for 0 <= small <= big, big > 0 */
static int och1970_atan_octant(u32 small, u32 big)
{
	u32 ratio = div_u64((u64)small << 16, big);
	int d;

	for (d = 1; d < ARRAY_SIZE(och1970_tan_q16); d++) {
		if (ratio <= och1970_tan_q16[d])
			return ratio - och1970_tan_q16[d - 1] <
			       och1970_tan_q16[d] - ratio ? d - 1 : d;
	}

	return 45;
}

/* atan2(s, c) in degrees, 0..359, or -1 when there is no field */
static int och1970_angle(int s, int c)
{
	u32 as = abs(s), ac = abs(c);
	int a;

	if (!as && !ac)
		return -1;

	if (as <= ac)
		a = och1970_atan_octant(as, ac);
	else
		a = 90 - och1970_atan_octant(ac, as);

	if (c < 0)
		a = 180 - a;
	if (s < 0)
		a = 360 - a;

	return a % 360;
}

static int och1970_set_mode(struct och1970 *och, u8 mode)
{
	int error;

	/* Mode and range changes go through standby. */
	error = i2c_smbus_write_byte_data(och->client, OCH1970_REG_CNTL2,
					  och->cntl2 | OCH1970_CNTL2_MODE_STANDBY);
	if (error || mode == OCH1970_CNTL2_MODE_STANDBY)
		return error;

	return i2c_smbus_write_byte_data(och->client, OCH1970_REG_CNTL2,
					 och->cntl2 | mode);
}

static void och1970_poll(struct input_dev *input)
{
	struct och1970 *och = input_get_drvdata(input);
	unsigned long now = jiffies;
	int x, z, deg, delta;
	u8 buf[8];
	int ret;

	ret = i2c_smbus_read_i2c_block_data(och->client, OCH1970_REG_ST_X_Y_Z,
					    sizeof(buf), buf);
	if (ret != sizeof(buf)) {
		dev_err_ratelimited(&och->client->dev,
				    "failed to read field: %d\n", ret);
		return;
	}

	x = (s16)get_unaligned_be16(&buf[2]);
	z = (s16)get_unaligned_be16(&buf[6]);
	deg = och1970_angle(x, z);

	dev_dbg(&och->client->dev, "x %d y %d z %d: %d deg\n", x,
		(s16)get_unaligned_be16(&buf[4]), z, deg);

	if (deg < 0)
		return;

	if (!och->have_base ||
	    time_after_eq(now, och->base_time +
				msecs_to_jiffies(OCH1970_REBASE_MS))) {
		/* Follow slow drift while the wheel is idle. */
		och->have_base = true;
		och->base_deg = deg;
		och->base_time = now;
		return;
	}

	delta = deg - och->base_deg;
	if (delta > 180)
		delta -= 360;
	else if (delta <= -180)
		delta += 360;

	if (abs(delta) < OCH1970_STEP_DEG)
		return;

	/* Vendor convention: increasing angle is KEY_DOWN. */
	input_report_key(input, delta > 0 ? KEY_DOWN : KEY_UP, 1);
	input_sync(input);
	input_report_key(input, delta > 0 ? KEY_DOWN : KEY_UP, 0);
	input_sync(input);

	och->base_deg = deg;
	och->base_time = now;
}

static int och1970_open(struct input_dev *input)
{
	struct och1970 *och = input_get_drvdata(input);

	och->have_base = false;

	return och1970_set_mode(och, OCH1970_CNTL2_MODE_CONT_100HZ);
}

static void och1970_close(struct input_dev *input)
{
	struct och1970 *och = input_get_drvdata(input);

	och1970_set_mode(och, OCH1970_CNTL2_MODE_STANDBY);
}

static int och1970_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gpio_desc *reset;
	struct input_dev *input;
	struct och1970 *och;
	int ret;

	och = devm_kzalloc(dev, sizeof(*och), GFP_KERNEL);
	if (!och)
		return -ENOMEM;

	och->client = client;

	ret = devm_regulator_get_enable_optional(dev, "vdd");
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "failed to enable vdd\n");

	/* Hold reset low for 10 ms like the vendor driver, then release it. */
	reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "failed to get reset\n");
	if (reset) {
		fsleep(10000);
		gpiod_set_value_cansleep(reset, 0);
		fsleep(5000);
	}

	ret = i2c_smbus_read_byte_data(client, OCH1970_REG_WIA);
	if (ret < 0)
		return dev_err_probe(dev, ret, "no response\n");
	dev_dbg(dev, "WIA %#04x\n", ret);

	ret = i2c_smbus_read_byte_data(client, OCH1970_REG_CNTL2);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to read CNTL2\n");

	/* Vendor settings: low-noise drive, high measurement range. */
	och->cntl2 = (ret & OCH1970_CNTL2_KEEP) | OCH1970_CNTL2_SMR_HIGH_RANGE;
	ret = och1970_set_mode(och, OCH1970_CNTL2_MODE_STANDBY);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enter standby\n");

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	och->input = input;
	input->name = "och1970-wheel";
	input->id.bustype = BUS_I2C;
	input->open = och1970_open;
	input->close = och1970_close;
	input_set_drvdata(input, och);

	input_set_capability(input, EV_KEY, KEY_UP);
	input_set_capability(input, EV_KEY, KEY_DOWN);

	ret = input_setup_polling(input, och1970_poll);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set up polling\n");
	input_set_poll_interval(input, OCH1970_POLL_MS);

	i2c_set_clientdata(client, och);

	ret = input_register_device(input);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register input\n");

	return 0;
}

static int och1970_suspend(struct device *dev)
{
	struct och1970 *och = i2c_get_clientdata(to_i2c_client(dev));
	int ret = 0;

	mutex_lock(&och->input->mutex);
	if (input_device_enabled(och->input))
		ret = och1970_set_mode(och, OCH1970_CNTL2_MODE_STANDBY);
	mutex_unlock(&och->input->mutex);

	return ret;
}

static int och1970_resume(struct device *dev)
{
	struct och1970 *och = i2c_get_clientdata(to_i2c_client(dev));
	int ret = 0;

	mutex_lock(&och->input->mutex);
	if (input_device_enabled(och->input)) {
		och->have_base = false;
		ret = och1970_set_mode(och, OCH1970_CNTL2_MODE_CONT_100HZ);
	}
	mutex_unlock(&och->input->mutex);

	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(och1970_pm_ops, och1970_suspend,
				och1970_resume);

static const struct i2c_device_id och1970_id[] = {
	{ "och1970" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, och1970_id);

static const struct of_device_id och1970_of_match[] = {
	{ .compatible = "ocs,och1970" },
	{ }
};
MODULE_DEVICE_TABLE(of, och1970_of_match);

static struct i2c_driver och1970_driver = {
	.driver = {
		.name = "och1970",
		.of_match_table = och1970_of_match,
		.pm = pm_sleep_ptr(&och1970_pm_ops),
	},
	.probe = och1970_probe,
	.id_table = och1970_id,
};
module_i2c_driver(och1970_driver);

MODULE_DESCRIPTION("Orient Chip OCH1970 Hall sensor scroll wheel driver");
MODULE_LICENSE("GPL");

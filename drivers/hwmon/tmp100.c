#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/gpio.h>
#include <linux/hwmon.h>

#define TMP100_REG_TEMP	    0x00
#define TMP100_REG_CFG	    0x01
#define TMP100_REG_CFG_SD	BIT(0)
#define TMP100_REG_CFG_TM	BIT(1)
#define TMP100_REG_CFG_POL	BIT(2)
#define TMP100_REG_CFG_F0	BIT(3)
#define TMP100_REG_CFG_F1	BIT(4)
#define TMP100_REG_CFG_R0	BIT(5)
#define TMP100_REG_CFG_R1	BIT(6)
#define TMP100_REG_CFG_ALERT	BIT(7)
#define TMP100_REG_TLOW	    0x02
#define TMP100_REG_THIGH    0x03

struct tmp100_data {
	struct i2c_client *client;
};

static inline int tmp100_reg_to_mc(int val)
{
	return val * 1000 / 256;
}

static inline int tmp100_mc_to_reg(int val)
{
	return val * 256 / 1000;
}

static umode_t tmp100_is_visible(const void *data, enum hwmon_sensor_types type,
				 u32 attr, int channel)
{
	if (type != hwmon_temp || channel != 0)
		return -EINVAL;

	switch (attr) {
	case hwmon_temp_input:
	case hwmon_temp_max_alarm:
		/* read only for all */
		return S_IRUGO;
	case hwmon_temp_max:
		/* allow read/write */
		return S_IWUSR | S_IRUGO;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int tmp100_read(struct device *hwmon, enum hwmon_sensor_types type,
		       u32 attr, int channel, long *value)
{
	struct tmp100_data *tmp100 = dev_get_drvdata(hwmon);
	u8 cfg;

	if (type != hwmon_temp || channel != 0)
		return -EINVAL;

	switch (attr) {
	case hwmon_temp_input:
		*value = i2c_smbus_read_word_swapped(tmp100->client,
						     TMP100_REG_TEMP);
		*value = tmp100_reg_to_mc(*value);
		break;
	case hwmon_temp_max_alarm: /* alert is active ? */
		cfg = i2c_smbus_read_byte_data(tmp100->client, TMP100_REG_CFG);
		*value = ((cfg & TMP100_REG_CFG_ALERT) != 0) ^
			 ((cfg & TMP100_REG_CFG_POL) == 0);
		break;
	case hwmon_temp_max:
		/* return t-high */
		*value = i2c_smbus_read_word_swapped(tmp100->client, 0x03);
		*value = tmp100_reg_to_mc(*value);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int tmp100_write(struct device *hwmon, enum hwmon_sensor_types type,
			u32 attr, int channel, long value)
{
	struct tmp100_data *tmp100 = dev_get_drvdata(hwmon);

	if (type != hwmon_temp || channel != 0)
		return -EINVAL;

	switch (attr) {
	case hwmon_temp_max:
		value = tmp100_mc_to_reg(value);
		i2c_smbus_write_word_swapped(tmp100->client, 0x03, value);
		i2c_smbus_write_word_swapped(tmp100->client, 0x02, value);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static const struct hwmon_channel_info *tmp100_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MAX_ALARM),
	NULL /* null terminated */
};

static const struct hwmon_ops tmp100_hwmon_ops = {
	.is_visible = tmp100_is_visible,
	.read = tmp100_read,
	.write = tmp100_write,
};

static const struct hwmon_chip_info tmp100_chip_info = {
	.ops = &tmp100_hwmon_ops,
	.info = tmp100_info,
};

static int tmp100_probe(struct i2c_client *client,
			const struct i2c_device_id *tmp100_id)
{
	struct device *dev = &client->dev;
	struct tmp100_data *tmp100;
	struct device *hwmon;

	dev_info(dev, "New tmp100 detected, address = 0x%02x\n", client->addr);

	/* Allocate a new tmp100 instance */
	tmp100 = devm_kzalloc(dev, sizeof(*tmp100), GFP_KERNEL);
	if (!tmp100)
		return -ENOMEM;

	/* Link the i2c-device/tmp100 objects */
	dev_set_drvdata(dev, tmp100);
	tmp100->client = client;

	/* Register to the hwmon subsystem/class */
	hwmon = devm_hwmon_device_register_with_info(dev, client->name, tmp100,
						     &tmp100_chip_info, NULL);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);

	return 0;
}

static int tmp100_remove(struct i2c_client *client)
{
	dev_info(&client->dev, "Remove tmp100, address = 0x%02x\n",
		 client->addr);
	return 0;
}

static const struct of_device_id tmp100_of_match[] = {
	{ .compatible = "ti,tmp100", },
	{},
};
MODULE_DEVICE_TABLE(of, tmp100_of_match);

static struct i2c_driver tmp100_driver = {
    	.driver = {
            	.name   = "tmp100",
            	.of_match_table = of_match_ptr(tmp100_of_match),
    	},
    	.probe      	= tmp100_probe,
    	.remove     	= tmp100_remove,
};
module_i2c_driver(tmp100_driver);

MODULE_DESCRIPTION("TI TMP100 temperature sensor driver");
MODULE_LICENSE("GPL");

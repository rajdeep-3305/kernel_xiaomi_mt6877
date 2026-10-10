// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/led-class-flash.h>
#include <linux/led-class-multicolor.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <media/v4l2-flash-led-class.h>

#if IS_ENABLED(CONFIG_MTK_FLASHLIGHT)
#include <linux/power_supply.h>

#include "flashlight-core.h"
#endif

enum {
	MT6360_LED_ISNK1 = 0,
	MT6360_LED_ISNK2,
	MT6360_LED_ISNK3,
	MT6360_LED_ISNKML,
	MT6360_LED_FLASH1,
	MT6360_LED_FLASH2,
	MT6360_MAX_LEDS
};

#define MT6360_REG_RGBEN		0x380
#define MT6360_REG_ISNK(_led_no)	(0x381 + (_led_no))
#define MT6360_ISNK_ENMASK(_led_no)	BIT(7 - (_led_no))
#define MT6360_ISNK_MASK		GENMASK(4, 0)
#define MT6360_CHRINDSEL_MASK		BIT(3)

/* Virtual definition for multicolor */
#define MT6360_VIRTUAL_MULTICOLOR	(MT6360_MAX_LEDS + 1)
#define MULTICOLOR_NUM_CHANNELS		3

#define MT6360_REG_FLEDEN		0x37E
#define MT6360_REG_STRBTO		0x373
#define MT6360_REG_FLEDBASE(_id)	(0x372 + 4 * (_id - MT6360_LED_FLASH1))
#define MT6360_REG_FLEDISTRB(_id)	(MT6360_REG_FLEDBASE(_id) + 2)
#define MT6360_REG_FLEDITOR(_id)	(MT6360_REG_FLEDBASE(_id) + 3)
#define MT6360_REG_CHGSTAT2		0x3E1
#define MT6360_REG_FLEDSTAT1		0x3E9
#define MT6360_ITORCH_MASK		GENMASK(4, 0)
#define MT6360_ISTROBE_MASK		GENMASK(6, 0)
#define MT6360_STRBTO_MASK		GENMASK(6, 0)
#define MT6360_TORCHEN_MASK		BIT(3)
#define MT6360_STROBEN_MASK		BIT(2)
#define MT6360_FLCSEN_MASK(_id)		BIT(MT6360_LED_FLASH2 - _id)
#define MT6360_FLEDCHGVINOVP_MASK	BIT(3)
#define MT6360_FLED1STRBTO_MASK		BIT(11)
#define MT6360_FLED2STRBTO_MASK		BIT(10)
#define MT6360_FLED1STRB_MASK		BIT(9)
#define MT6360_FLED2STRB_MASK		BIT(8)
#define MT6360_FLED1SHORT_MASK		BIT(7)
#define MT6360_FLED2SHORT_MASK		BIT(6)
#define MT6360_FLEDLVF_MASK		BIT(3)

#define MT6360_ISNKRGB_STEPUA		2000
#define MT6360_ISNKRGB_MAXUA		24000
#define MT6360_ISNKML_STEPUA		5000
#define MT6360_ISNKML_MAXUA		150000

#define MT6360_ITORCH_MINUA		25000
#define MT6360_ITORCH_STEPUA		12500
#define MT6360_ITORCH_MAXUA		400000
#define MT6360_ISTRB_MINUA		50000
#define MT6360_ISTRB_STEPUA		12500
#define MT6360_ISTRB_MAXUA		1500000
#define MT6360_STRBTO_MINUS		64000
#define MT6360_STRBTO_STEPUS		32000
#define MT6360_STRBTO_MAXUS		2432000

#if IS_ENABLED(CONFIG_MTK_FLASHLIGHT)
#define MT6360_FLASHLIGHT_NAME		"flashlights-mt6360"
#define MT6360_FLASHLIGHT_LEVELS		32
#define MT6360_FLASHLIGHT_TORCH_LEVELS	16

static const unsigned int mt6360_flashlight_current_ma[] = {
	25, 50, 75, 100, 125, 150, 175, 200,
	225, 250, 275, 300, 325, 350, 375, 400,
	450, 500, 550, 600, 650, 700, 750, 800,
	850, 900, 950, 1000, 1050, 1100, 1150, 1200,
};
#endif

struct mt6360_led {
	union {
		struct led_classdev isnk;
		struct led_classdev_mc mc;
		struct led_classdev_flash flash;
	};
	struct v4l2_flash *v4l2_flash;
	struct mt6360_priv *priv;
	u32 led_no;
	enum led_default_state default_state;
#if IS_ENABLED(CONFIG_MTK_FLASHLIGHT)
	struct flashlight_device_id flashlight_id;
	int flashlight_level;
	bool flashlight_enabled;
	bool flashlight_strobe;
	bool flashlight_registered;
#endif
};

struct mt6360_priv {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock;
	unsigned int fled_strobe_used;
	unsigned int fled_torch_used;
	unsigned int leds_active;
	unsigned int leds_count;
	struct mt6360_led leds[];
};

#if IS_ENABLED(CONFIG_MTK_FLASHLIGHT)
static DEFINE_MUTEX(mt6360_flashlight_lock);
static struct mt6360_led *mt6360_flashlight_leds[2];
static unsigned int mt6360_flashlight_users;
static bool mt6360_voltage_reduced;
#endif

static int mt6360_mc_brightness_set(struct led_classdev *lcdev,
				    enum led_brightness level)
{
	struct led_classdev_mc *mccdev = lcdev_to_mccdev(lcdev);
	struct mt6360_led *led = container_of(mccdev, struct mt6360_led, mc);
	struct mt6360_priv *priv = led->priv;
	u32 real_bright, enable_mask = 0, enable = 0;
	int i, ret;

	mutex_lock(&priv->lock);

	led_mc_calc_color_components(mccdev, level);

	for (i = 0; i < mccdev->num_colors; i++) {
		struct mc_subled *subled = mccdev->subled_info + i;

		real_bright = min(lcdev->max_brightness, subled->brightness);
		ret = regmap_update_bits(priv->regmap, MT6360_REG_ISNK(i),
					 MT6360_ISNK_MASK, real_bright);
		if (ret)
			goto out;

		enable_mask |= MT6360_ISNK_ENMASK(subled->channel);
		if (real_bright)
			enable |= MT6360_ISNK_ENMASK(subled->channel);
	}

	ret = regmap_update_bits(priv->regmap, MT6360_REG_RGBEN, enable_mask,
				 enable);

out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int mt6360_isnk_brightness_set(struct led_classdev *lcdev,
				      enum led_brightness level)
{
	struct mt6360_led *led = container_of(lcdev, struct mt6360_led, isnk);
	struct mt6360_priv *priv = led->priv;
	u32 enable_mask = MT6360_ISNK_ENMASK(led->led_no);
	u32 val = level ? MT6360_ISNK_ENMASK(led->led_no) : 0;
	int ret;

	mutex_lock(&priv->lock);

	ret = regmap_update_bits(priv->regmap, MT6360_REG_ISNK(led->led_no),
				 MT6360_ISNK_MASK, level);
	if (ret)
		goto out;

	ret = regmap_update_bits(priv->regmap, MT6360_REG_RGBEN, enable_mask,
				 val);

out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int mt6360_torch_brightness_set(struct led_classdev *lcdev,
				       enum led_brightness level)
{
	struct mt6360_led *led =
		container_of(lcdev, struct mt6360_led, flash.led_cdev);
	struct mt6360_priv *priv = led->priv;
	u32 enable_mask = MT6360_TORCHEN_MASK | MT6360_FLCSEN_MASK(led->led_no);
	u32 val = level ? MT6360_FLCSEN_MASK(led->led_no) : 0;
	u32 prev = priv->fled_torch_used, curr;
	int ret;

	mutex_lock(&priv->lock);

	/*
	 * Only one set of flash control logic, use the flag to avoid strobe is
	 * currently used.
	 */
	if (priv->fled_strobe_used) {
		dev_warn(lcdev->dev, "Please disable strobe first [%d]\n",
			 priv->fled_strobe_used);
		ret = -EBUSY;
		goto unlock;
	}

	if (level)
		curr = prev | BIT(led->led_no);
	else
		curr = prev & ~BIT(led->led_no);

	if (curr)
		val |= MT6360_TORCHEN_MASK;

	if (level) {
		ret = regmap_update_bits(priv->regmap,
					 MT6360_REG_FLEDITOR(led->led_no),
					 MT6360_ITORCH_MASK, level - 1);
		if (ret)
			goto unlock;
	}

	ret = regmap_update_bits(priv->regmap, MT6360_REG_FLEDEN, enable_mask,
				 val);
	if (ret)
		goto unlock;

	priv->fled_torch_used = curr;

unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

static int mt6360_flash_brightness_set(struct led_classdev_flash *fl_cdev,
				       u32 brightness)
{
	/*
	 * Due to the current spike when turning on flash, let brightness to be
	 * kept by framework.
	 * This empty function is used to prevent led_classdev_flash register
	 * ops check failure.
	 */
	return 0;
}

static int _mt6360_flash_brightness_set(struct led_classdev_flash *fl_cdev,
					u32 brightness)
{
	struct mt6360_led *led =
		container_of(fl_cdev, struct mt6360_led, flash);
	struct mt6360_priv *priv = led->priv;
	struct led_flash_setting *s = &fl_cdev->brightness;
	u32 val = (brightness - s->min) / s->step;

	return regmap_update_bits(priv->regmap,
				  MT6360_REG_FLEDISTRB(led->led_no),
				  MT6360_ISTROBE_MASK, val);
}

static int mt6360_strobe_set(struct led_classdev_flash *fl_cdev, bool state)
{
	struct mt6360_led *led =
		container_of(fl_cdev, struct mt6360_led, flash);
	struct mt6360_priv *priv = led->priv;
	struct led_classdev *lcdev = &fl_cdev->led_cdev;
	struct led_flash_setting *s = &fl_cdev->brightness;
	u32 enable_mask = MT6360_STROBEN_MASK | MT6360_FLCSEN_MASK(led->led_no);
	u32 val = state ? MT6360_FLCSEN_MASK(led->led_no) : 0;
	u32 prev = priv->fled_strobe_used, curr;
	int ret;

	mutex_lock(&priv->lock);

	/*
	 * Only one set of flash control logic, use the flag to avoid torch is
	 * currently used
	 */
	if (priv->fled_torch_used) {
		dev_warn(lcdev->dev, "Please disable torch first [0x%x]\n",
			 priv->fled_torch_used);
		ret = -EBUSY;
		goto unlock;
	}

	if (state)
		curr = prev | BIT(led->led_no);
	else
		curr = prev & ~BIT(led->led_no);

	if (curr)
		val |= MT6360_STROBEN_MASK;

	ret = regmap_update_bits(priv->regmap, MT6360_REG_FLEDEN, enable_mask,
				 val);
	if (ret) {
		dev_err(lcdev->dev, "[%d] control current source %d fail\n",
			led->led_no, state);
		goto unlock;
	}

	/*
	 * If the flash need to be on, config the flash current ramping up to
	 * the setting value.
	 * Else, always recover back to the minimum one
	 */
	ret = _mt6360_flash_brightness_set(fl_cdev, state ? s->val : s->min);
	if (ret)
		goto unlock;

	/*
	 * For the flash turn on/off, HW rampping up/down time is 5ms/500us,
	 * respectively.
	 */
	if (!prev && curr)
		usleep_range(5000, 6000);
	else if (prev && !curr)
		udelay(500);

	priv->fled_strobe_used = curr;

unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

static int mt6360_strobe_get(struct led_classdev_flash *fl_cdev, bool *state)
{
	struct mt6360_led *led =
		container_of(fl_cdev, struct mt6360_led, flash);
	struct mt6360_priv *priv = led->priv;

	mutex_lock(&priv->lock);
	*state = !!(priv->fled_strobe_used & BIT(led->led_no));
	mutex_unlock(&priv->lock);

	return 0;
}

static int mt6360_timeout_set(struct led_classdev_flash *fl_cdev, u32 timeout)
{
	struct mt6360_led *led =
		container_of(fl_cdev, struct mt6360_led, flash);
	struct mt6360_priv *priv = led->priv;
	struct led_flash_setting *s = &fl_cdev->timeout;
	u32 val = (timeout - s->min) / s->step;
	int ret;

	mutex_lock(&priv->lock);
	ret = regmap_update_bits(priv->regmap, MT6360_REG_STRBTO,
				 MT6360_STRBTO_MASK, val);
	mutex_unlock(&priv->lock);

	return ret;
}

static int mt6360_fault_get(struct led_classdev_flash *fl_cdev, u32 *fault)
{
	struct mt6360_led *led =
		container_of(fl_cdev, struct mt6360_led, flash);
	struct mt6360_priv *priv = led->priv;
	u16 fled_stat;
	unsigned int chg_stat, strobe_timeout_mask, fled_short_mask;
	u32 rfault = 0;
	int ret;

	mutex_lock(&priv->lock);
	ret = regmap_read(priv->regmap, MT6360_REG_CHGSTAT2, &chg_stat);
	if (ret)
		goto unlock;

	ret = regmap_raw_read(priv->regmap, MT6360_REG_FLEDSTAT1, &fled_stat,
			      sizeof(fled_stat));
	if (ret)
		goto unlock;

	if (led->led_no == MT6360_LED_FLASH1) {
		strobe_timeout_mask = MT6360_FLED1STRBTO_MASK;
		fled_short_mask = MT6360_FLED1SHORT_MASK;
	} else {
		strobe_timeout_mask = MT6360_FLED2STRBTO_MASK;
		fled_short_mask = MT6360_FLED2SHORT_MASK;
	}

	if (chg_stat & MT6360_FLEDCHGVINOVP_MASK)
		rfault |= LED_FAULT_INPUT_VOLTAGE;

	if (fled_stat & strobe_timeout_mask)
		rfault |= LED_FAULT_TIMEOUT;

	if (fled_stat & fled_short_mask)
		rfault |= LED_FAULT_SHORT_CIRCUIT;

	if (fled_stat & MT6360_FLEDLVF_MASK)
		rfault |= LED_FAULT_UNDER_VOLTAGE;

	*fault = rfault;
unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

static const struct led_flash_ops mt6360_flash_ops = {
	.flash_brightness_set = mt6360_flash_brightness_set,
	.strobe_set = mt6360_strobe_set,
	.strobe_get = mt6360_strobe_get,
	.timeout_set = mt6360_timeout_set,
	.fault_get = mt6360_fault_get,
};

#if IS_ENABLED(CONFIG_MTK_FLASHLIGHT)
static int mt6360_flashlight_set_high_voltage(bool enable)
{
	union power_supply_propval value = { .intval = enable };
	struct power_supply *charger;
	int ret;

	charger = power_supply_get_by_name("mtk-master-charger");
	if (!charger)
		return -ENODEV;

	ret = power_supply_set_property(charger,
					POWER_SUPPLY_PROP_VOLTAGE_MAX, &value);
	if (!ret)
		power_supply_changed(charger);
	power_supply_put(charger);

	return ret;
}

static void mt6360_flashlight_set_scenario_locked(int scenario)
{
	int ret;

	if (scenario & FLASHLIGHT_SCENARIO_CAMERA_MASK) {
		if (mt6360_voltage_reduced)
			return;

		ret = mt6360_flashlight_set_high_voltage(false);
		if (!ret)
			mt6360_voltage_reduced = true;
	} else {
		if (!mt6360_voltage_reduced)
			return;

		ret = mt6360_flashlight_set_high_voltage(true);
		if (!ret)
			mt6360_voltage_reduced = false;
	}
}

static int mt6360_flashlight_disable(struct mt6360_led *led)
{
	int ret;

	if (!led->flashlight_enabled)
		return 0;

	if (led->flashlight_strobe)
		ret = mt6360_strobe_set(&led->flash, false);
	else
		ret = mt6360_torch_brightness_set(&led->flash.led_cdev,
						    LED_OFF);
	if (!ret)
		led->flashlight_enabled = false;

	return ret;
}

static int mt6360_flashlight_enable(struct mt6360_led *led)
{
	struct led_classdev_flash *flash = &led->flash;
	struct led_classdev *lcdev = &flash->led_cdev;
	u32 current_ua, brightness;
	int ret;

	if (led->flashlight_enabled)
		return 0;

	current_ua = mt6360_flashlight_current_ma[led->flashlight_level] * 1000;
	if (led->flashlight_level < MT6360_FLASHLIGHT_TORCH_LEVELS) {
		brightness = (current_ua - MT6360_ITORCH_MINUA) /
			     MT6360_ITORCH_STEPUA + 1;
		brightness = min(brightness, lcdev->max_brightness);
		ret = mt6360_torch_brightness_set(lcdev, brightness);
		if (!ret)
			led->flashlight_strobe = false;
	} else {
		flash->brightness.val = clamp_val(current_ua,
						  flash->brightness.min,
						  flash->brightness.max);
		ret = mt6360_strobe_set(flash, true);
		if (!ret)
			led->flashlight_strobe = true;
	}

	if (!ret)
		led->flashlight_enabled = true;

	return ret;
}

static int mt6360_flashlight_open(void)
{
	mutex_lock(&mt6360_flashlight_lock);
	mt6360_flashlight_users++;
	mutex_unlock(&mt6360_flashlight_lock);

	return 0;
}

static int mt6360_flashlight_release(void)
{
	mutex_lock(&mt6360_flashlight_lock);
	if (mt6360_flashlight_users)
		mt6360_flashlight_users--;
	if (!mt6360_flashlight_users && mt6360_voltage_reduced)
		mt6360_flashlight_set_scenario_locked(
			FLASHLIGHT_SCENARIO_FLASHLIGHT);
	mutex_unlock(&mt6360_flashlight_lock);

	return 0;
}

static int mt6360_flashlight_ioctl(unsigned int cmd, unsigned long arg)
{
	struct flashlight_dev_arg *flash_arg = (void *)arg;
	struct mt6360_led *led;
	u64 timeout_us;
	u32 fault;
	int level, ret = 0;
	bool enabled;

	if (!flash_arg || flash_arg->channel < 0 ||
	    flash_arg->channel >= ARRAY_SIZE(mt6360_flashlight_leds))
		return -EINVAL;

	mutex_lock(&mt6360_flashlight_lock);
	led = mt6360_flashlight_leds[flash_arg->channel];
	if (!led) {
		ret = -ENODEV;
		goto out_unlock;
	}

	switch (cmd) {
	case FLASH_IOC_SET_TIME_OUT_TIME_MS:
		if (flash_arg->arg <= 0)
			break;
		timeout_us = (u64)flash_arg->arg * USEC_PER_MSEC;
		timeout_us = clamp_t(u64, timeout_us,
				     led->flash.timeout.min,
				     led->flash.timeout.max);
		led->flash.timeout.val = timeout_us;
		ret = mt6360_timeout_set(&led->flash, timeout_us);
		break;
	case FLASH_IOC_SET_DUTY:
		level = clamp(flash_arg->arg, 0,
			      MT6360_FLASHLIGHT_LEVELS - 1);
		enabled = led->flashlight_enabled;
		if (enabled) {
			ret = mt6360_flashlight_disable(led);
			if (ret)
				break;
		}
		led->flashlight_level = level;
		if (enabled)
			ret = mt6360_flashlight_enable(led);
		break;
	case FLASH_IOC_SET_SCENARIO:
		mt6360_flashlight_set_scenario_locked(flash_arg->arg);
		break;
	case FLASH_IOC_SET_ONOFF:
		if (flash_arg->arg)
			ret = mt6360_flashlight_enable(led);
		else
			ret = mt6360_flashlight_disable(led);
		break;
	case FLASH_IOC_IS_CHARGER_READY:
	case FLASH_IOC_IS_HARDWARE_READY:
		flash_arg->arg = FLASHLIGHT_CHARGER_READY;
		break;
	case FLASH_IOC_GET_DUTY_NUMBER:
		flash_arg->arg = MT6360_FLASHLIGHT_LEVELS;
		break;
	case FLASH_IOC_GET_MAX_TORCH_DUTY:
		flash_arg->arg = MT6360_FLASHLIGHT_TORCH_LEVELS - 1;
		break;
	case FLASH_IOC_GET_DUTY_CURRENT:
		level = clamp(flash_arg->arg, 0,
			      MT6360_FLASHLIGHT_LEVELS - 1);
		flash_arg->arg = mt6360_flashlight_current_ma[level];
		break;
	case FLASH_IOC_GET_HW_TIMEOUT:
		flash_arg->arg = led->flash.timeout.val / USEC_PER_MSEC;
		break;
	case FLASH_IOC_GET_HW_FAULT:
	case FLASH_IOC_GET_HW_FAULT2:
		ret = mt6360_fault_get(&led->flash, &fault);
		if (!ret)
			flash_arg->arg = fault;
		break;
	default:
		ret = -ENOTTY;
		break;
	}

out_unlock:
	mutex_unlock(&mt6360_flashlight_lock);
	return ret;
}

static ssize_t mt6360_flashlight_strobe_store(struct flashlight_arg arg)
{
	struct mt6360_led *led;
	int ret;

	if (arg.channel < 0 ||
	    arg.channel >= ARRAY_SIZE(mt6360_flashlight_leds))
		return -EINVAL;

	mutex_lock(&mt6360_flashlight_lock);
	led = mt6360_flashlight_leds[arg.channel];
	if (!led) {
		ret = -ENODEV;
		goto out_unlock;
	}

	ret = mt6360_flashlight_disable(led);
	if (ret)
		goto out_unlock;
	led->flashlight_level = clamp(arg.level, 0,
				      MT6360_FLASHLIGHT_LEVELS - 1);
	ret = mt6360_flashlight_enable(led);

out_unlock:
	mutex_unlock(&mt6360_flashlight_lock);
	if (ret)
		return ret;

	if (arg.dur > 0)
		msleep(arg.dur);

	mutex_lock(&mt6360_flashlight_lock);
	ret = mt6360_flashlight_disable(led);
	mutex_unlock(&mt6360_flashlight_lock);

	return ret;
}

static int mt6360_flashlight_set_driver(int set)
{
	return 0;
}

static struct flashlight_operations mt6360_flashlight_ops = {
	.flashlight_open = mt6360_flashlight_open,
	.flashlight_release = mt6360_flashlight_release,
	.flashlight_ioctl = mt6360_flashlight_ioctl,
	.flashlight_strobe_store = mt6360_flashlight_strobe_store,
	.flashlight_set_driver = mt6360_flashlight_set_driver,
};

static int mt6360_flashlight_register(struct mt6360_led *led)
{
	int channel = led->led_no - MT6360_LED_FLASH1;
	int ret;

	if (channel < 0 || channel >= ARRAY_SIZE(mt6360_flashlight_leds))
		return -EINVAL;

	led->flashlight_id.type = 0;
	led->flashlight_id.ct = channel;
	led->flashlight_id.part = 0;
	led->flashlight_id.channel = channel;
	led->flashlight_id.decouple = 0;
	strscpy(led->flashlight_id.name, MT6360_FLASHLIGHT_NAME,
		sizeof(led->flashlight_id.name));

	mutex_lock(&mt6360_flashlight_lock);
	if (mt6360_flashlight_leds[channel]) {
		mutex_unlock(&mt6360_flashlight_lock);
		return -EBUSY;
	}
	mt6360_flashlight_leds[channel] = led;
	mutex_unlock(&mt6360_flashlight_lock);

	ret = flashlight_dev_register_by_device_id(&led->flashlight_id,
						    &mt6360_flashlight_ops);
	if (ret) {
		mutex_lock(&mt6360_flashlight_lock);
		mt6360_flashlight_leds[channel] = NULL;
		mutex_unlock(&mt6360_flashlight_lock);
		return ret;
	}

	led->flashlight_registered = true;
	return 0;
}

static void mt6360_flashlight_unregister(struct mt6360_led *led)
{
	int channel = led->led_no - MT6360_LED_FLASH1;

	if (!led->flashlight_registered)
		return;

	flashlight_dev_unregister_by_device_id(&led->flashlight_id);

	mutex_lock(&mt6360_flashlight_lock);
	mt6360_flashlight_disable(led);
	if (channel >= 0 && channel < ARRAY_SIZE(mt6360_flashlight_leds) &&
	    mt6360_flashlight_leds[channel] == led)
		mt6360_flashlight_leds[channel] = NULL;
	led->flashlight_registered = false;
	mutex_unlock(&mt6360_flashlight_lock);
}
#else
static int mt6360_flashlight_register(struct mt6360_led *led)
{
	return 0;
}

static void mt6360_flashlight_unregister(struct mt6360_led *led)
{
}
#endif

static int mt6360_isnk_init_default_state(struct mt6360_led *led)
{
	struct mt6360_priv *priv = led->priv;
	unsigned int regval;
	u32 level;
	int ret;

	ret = regmap_read(priv->regmap, MT6360_REG_ISNK(led->led_no), &regval);
	if (ret)
		return ret;
	level = regval & MT6360_ISNK_MASK;

	ret = regmap_read(priv->regmap, MT6360_REG_RGBEN, &regval);
	if (ret)
		return ret;

	if (!(regval & MT6360_ISNK_ENMASK(led->led_no)))
		level = LED_OFF;

	switch (led->default_state) {
	case LEDS_DEFSTATE_ON:
		led->isnk.brightness = led->isnk.max_brightness;
		break;
	case LEDS_DEFSTATE_KEEP:
		led->isnk.brightness = min(level, led->isnk.max_brightness);
		break;
	default:
		led->isnk.brightness = LED_OFF;
	}

	return mt6360_isnk_brightness_set(&led->isnk, led->isnk.brightness);
}

static int mt6360_flash_init_default_state(struct mt6360_led *led)
{
	struct led_classdev_flash *flash = &led->flash;
	struct mt6360_priv *priv = led->priv;
	u32 enable_mask = MT6360_TORCHEN_MASK | MT6360_FLCSEN_MASK(led->led_no);
	u32 level;
	unsigned int regval;
	int ret;

	ret = regmap_read(priv->regmap, MT6360_REG_FLEDITOR(led->led_no),
			  &regval);
	if (ret)
		return ret;
	level = regval & MT6360_ITORCH_MASK;

	ret = regmap_read(priv->regmap, MT6360_REG_FLEDEN, &regval);
	if (ret)
		return ret;

	if ((regval & enable_mask) == enable_mask)
		level += 1;
	else
		level = LED_OFF;

	switch (led->default_state) {
	case LEDS_DEFSTATE_ON:
		flash->led_cdev.brightness = flash->led_cdev.max_brightness;
		break;
	case LEDS_DEFSTATE_KEEP:
		flash->led_cdev.brightness =
			min(level, flash->led_cdev.max_brightness);
		break;
	default:
		flash->led_cdev.brightness = LED_OFF;
	}

	return mt6360_torch_brightness_set(&flash->led_cdev,
					   flash->led_cdev.brightness);
}

#if IS_ENABLED(CONFIG_V4L2_FLASH_LED_CLASS)
static int mt6360_flash_external_strobe_set(struct v4l2_flash *v4l2_flash,
					    bool enable)
{
	struct led_classdev_flash *flash = v4l2_flash->fled_cdev;
	struct mt6360_led *led = container_of(flash, struct mt6360_led, flash);
	struct mt6360_priv *priv = led->priv;
	u32 mask = MT6360_FLCSEN_MASK(led->led_no);
	u32 val = enable ? mask : 0;
	int ret;

	mutex_lock(&priv->lock);

	ret = regmap_update_bits(priv->regmap, MT6360_REG_FLEDEN, mask, val);
	if (ret)
		goto unlock;

	if (enable)
		priv->fled_strobe_used |= BIT(led->led_no);
	else
		priv->fled_strobe_used &= ~BIT(led->led_no);

unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

static const struct v4l2_flash_ops v4l2_flash_ops = {
	.external_strobe_set = mt6360_flash_external_strobe_set,
};

static void mt6360_init_v4l2_flash_config(struct mt6360_led *led,
					  struct v4l2_flash_config *config)
{
	struct led_classdev *lcdev;
	struct led_flash_setting *s = &config->intensity;

	lcdev = &led->flash.led_cdev;

	s->min = MT6360_ITORCH_MINUA;
	s->step = MT6360_ITORCH_STEPUA;
	s->val = s->max = s->min + (lcdev->max_brightness - 1) * s->step;

	config->has_external_strobe = 1;
	strscpy(config->dev_name, lcdev->dev->kobj.name,
		sizeof(config->dev_name));

	config->flash_faults = LED_FAULT_SHORT_CIRCUIT | LED_FAULT_TIMEOUT |
			       LED_FAULT_INPUT_VOLTAGE |
			       LED_FAULT_UNDER_VOLTAGE;
}
#else
static const struct v4l2_flash_ops v4l2_flash_ops;
static void mt6360_init_v4l2_flash_config(struct mt6360_led *led,
					  struct v4l2_flash_config *config)
{
}
#endif

static int mt6360_led_register(struct device *parent, struct mt6360_led *led,
				struct led_init_data *init_data)
{
	struct mt6360_priv *priv = led->priv;
	struct v4l2_flash_config v4l2_config = {0};
	int ret;

	if ((led->led_no == MT6360_LED_ISNK1 ||
	     led->led_no == MT6360_VIRTUAL_MULTICOLOR) &&
	     (priv->leds_active & BIT(MT6360_LED_ISNK1))) {
		/*
		 * Change isink1 to SW control mode, disconnect it with
		 * charger state
		 */
		ret = regmap_update_bits(priv->regmap, MT6360_REG_RGBEN,
					 MT6360_CHRINDSEL_MASK,
					 MT6360_CHRINDSEL_MASK);
		if (ret) {
			dev_err(parent, "Failed to config ISNK1 to SW mode\n");
			return ret;
		}
	}

	switch (led->led_no) {
	case MT6360_VIRTUAL_MULTICOLOR:
		ret = mt6360_mc_brightness_set(&led->mc.led_cdev, LED_OFF);
		if (ret) {
			dev_err(parent,
				"Failed to init multicolor brightness\n");
			return ret;
		}

		ret = devm_led_classdev_multicolor_register_ext(parent,
							   &led->mc, init_data);
		if (ret) {
			dev_err(parent, "Couldn't register multicolor\n");
			return ret;
		}
		break;
	case MT6360_LED_ISNK1 ... MT6360_LED_ISNKML:
		ret = mt6360_isnk_init_default_state(led);
		if (ret) {
			dev_err(parent, "Failed to init %d isnk state\n",
				led->led_no);
			return ret;
		}

		ret = devm_led_classdev_register_ext(parent, &led->isnk,
						     init_data);
		if (ret) {
			dev_err(parent, "Couldn't register isink %d\n",
				led->led_no);
			return ret;
		}
		break;
	default:
		ret = mt6360_flash_init_default_state(led);
		if (ret) {
			dev_err(parent, "Failed to init %d flash state\n",
				led->led_no);
			return ret;
		}

		ret = devm_led_classdev_flash_register_ext(parent, &led->flash,
							   init_data);
		if (ret) {
			dev_err(parent, "Couldn't register flash %d\n",
				led->led_no);
			return ret;
		}

		mt6360_init_v4l2_flash_config(led, &v4l2_config);
		led->v4l2_flash = v4l2_flash_init(parent, init_data->fwnode,
						  &led->flash,
						  &v4l2_flash_ops,
						  &v4l2_config);
		if (IS_ERR(led->v4l2_flash)) {
			dev_err(parent, "Failed to register %d v4l2 sd\n",
				led->led_no);
			return PTR_ERR(led->v4l2_flash);
		}

		ret = mt6360_flashlight_register(led);
		if (ret) {
			dev_err(parent,
				"Failed to register %d with flashlight core\n",
				led->led_no);
			return ret;
		}
	}

	return 0;
}

static u32 clamp_align(u32 val, u32 min, u32 max, u32 step)
{
	u32 retval;

	retval = clamp_val(val, min, max);
	if (step > 1)
		retval = rounddown(retval - min, step) + min;

	return retval;
}

static int mt6360_init_isnk_properties(struct mt6360_led *led,
				       struct led_init_data *init_data)
{
	struct led_classdev *lcdev;
	struct mt6360_priv *priv = led->priv;
	struct fwnode_handle *child;
	u32 step_uA = MT6360_ISNKRGB_STEPUA, max_uA = MT6360_ISNKRGB_MAXUA;
	u32 val;
	int num_color = 0, ret;

	if (led->led_no == MT6360_VIRTUAL_MULTICOLOR) {
		struct mc_subled *sub_led;

		sub_led = devm_kzalloc(priv->dev,
			sizeof(*sub_led) * MULTICOLOR_NUM_CHANNELS, GFP_KERNEL);
		if (!sub_led)
			return -ENOMEM;

		fwnode_for_each_child_node(init_data->fwnode, child) {
			u32 reg, color;

			ret = fwnode_property_read_u32(child, "reg", &reg);
			if (ret || reg > MT6360_LED_ISNK3 ||
			    priv->leds_active & BIT(reg)) {
				fwnode_handle_put(child);
				return -EINVAL;
			}

			ret = fwnode_property_read_u32(child, "color", &color);
			if (ret) {
				dev_err(priv->dev,
					"led %d, no color specified\n",
					led->led_no);
				fwnode_handle_put(child);
				return ret;
			}

			priv->leds_active |= BIT(reg);
			sub_led[num_color].color_index = color;
			sub_led[num_color].channel = reg;
			num_color++;
		}

		if (num_color < 2) {
			dev_err(priv->dev,
			     "Multicolor must include 2 or more led channel\n");
			return -EINVAL;
		}

		led->mc.num_colors = num_color;
		led->mc.subled_info = sub_led;

		lcdev = &led->mc.led_cdev;
		lcdev->brightness_set_blocking = mt6360_mc_brightness_set;
	} else {
		if (led->led_no == MT6360_LED_ISNKML) {
			step_uA = MT6360_ISNKML_STEPUA;
			max_uA = MT6360_ISNKML_MAXUA;
		}

		lcdev = &led->isnk;
		lcdev->brightness_set_blocking = mt6360_isnk_brightness_set;
	}

	ret = fwnode_property_read_u32(init_data->fwnode, "led-max-microamp",
				       &val);
	if (ret) {
		dev_warn(priv->dev,
		     "Not specified led-max-microamp, config to the minimum\n");
		val = step_uA;
	} else
		val = clamp_align(val, 0, max_uA, step_uA);

	lcdev->max_brightness = val / step_uA;

	fwnode_property_read_string(init_data->fwnode, "linux,default-trigger",
				    &lcdev->default_trigger);

	return 0;
}

static int mt6360_init_flash_properties(struct mt6360_led *led,
					struct led_init_data *init_data)
{
	struct led_classdev_flash *flash = &led->flash;
	struct led_classdev *lcdev = &flash->led_cdev;
	struct mt6360_priv *priv = led->priv;
	struct led_flash_setting *s;
	u32 val;
	int ret;

	ret = fwnode_property_read_u32(init_data->fwnode, "led-max-microamp",
				       &val);
	if (ret) {
		dev_warn(priv->dev,
		     "Not specified led-max-microamp, config to the minimum\n");
		val = MT6360_ITORCH_MINUA;
	} else
		val = clamp_align(val, MT6360_ITORCH_MINUA, MT6360_ITORCH_MAXUA,
				  MT6360_ITORCH_STEPUA);

	lcdev->max_brightness =
		(val - MT6360_ITORCH_MINUA) / MT6360_ITORCH_STEPUA + 1;
	lcdev->brightness_set_blocking = mt6360_torch_brightness_set;
	lcdev->flags |= LED_DEV_CAP_FLASH;

	ret = fwnode_property_read_u32(init_data->fwnode, "flash-max-microamp",
				       &val);
	if (ret) {
		dev_warn(priv->dev,
		   "Not specified flash-max-microamp, config to the minimum\n");
		val = MT6360_ISTRB_MINUA;
	} else
		val = clamp_align(val, MT6360_ISTRB_MINUA, MT6360_ISTRB_MAXUA,
				  MT6360_ISTRB_STEPUA);

	s = &flash->brightness;
	s->min = MT6360_ISTRB_MINUA;
	s->step = MT6360_ISTRB_STEPUA;
	s->val = s->max = val;

	/*
	 * Always configure as min level when off to prevent flash current
	 * spike.
	 */
	ret = _mt6360_flash_brightness_set(flash, s->min);
	if (ret)
		return ret;

	ret = fwnode_property_read_u32(init_data->fwnode,
				       "flash-max-timeout-us", &val);
	if (ret) {
		dev_warn(priv->dev,
		 "Not specified flash-max-timeout-us, config to the minimum\n");
		val = MT6360_STRBTO_MINUS;
	} else
		val = clamp_align(val, MT6360_STRBTO_MINUS, MT6360_STRBTO_MAXUS,
				  MT6360_STRBTO_STEPUS);

	s = &flash->timeout;
	s->min = MT6360_STRBTO_MINUS;
	s->step = MT6360_STRBTO_STEPUS;
	s->val = s->max = val;

	flash->ops = &mt6360_flash_ops;

	return 0;
}

static void mt6360_v4l2_flash_release(struct mt6360_priv *priv)
{
	int i;

	for (i = 0; i < priv->leds_count; i++) {
		struct mt6360_led *led = priv->leds + i;

		mt6360_flashlight_unregister(led);
		if (led->v4l2_flash)
			v4l2_flash_release(led->v4l2_flash);
	}
}

static int mt6360_led_probe(struct platform_device *pdev)
{
	struct mt6360_priv *priv;
	size_t count;
	int i = 0, ret;

	count = device_get_child_node_count(&pdev->dev);
	if (!count || count > MT6360_MAX_LEDS) {
		dev_err(&pdev->dev,
			"No child node or node count over max led number %zu\n",
			count);
		return -EINVAL;
	}

	priv = devm_kzalloc(&pdev->dev,
			    struct_size(priv, leds, count), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->leds_count = count;
	priv->dev = &pdev->dev;
	mutex_init(&priv->lock);

	priv->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!priv->regmap) {
		dev_err(&pdev->dev, "Failed to get parent regmap\n");
		return -ENODEV;
	}

	device_for_each_child_node_scoped(&pdev->dev, child) {
		struct mt6360_led *led = priv->leds + i;
		struct led_init_data init_data = { .fwnode = child, };
		u32 reg, led_color;

		ret = fwnode_property_read_u32(child, "color", &led_color);
		if (ret)
			goto out_flash_release;

		if (led_color == LED_COLOR_ID_RGB ||
		    led_color == LED_COLOR_ID_MULTI)
			reg = MT6360_VIRTUAL_MULTICOLOR;
		else {
			ret = fwnode_property_read_u32(child, "reg", &reg);
			if (ret)
				goto out_flash_release;

			if (reg >= MT6360_MAX_LEDS) {
				ret = -EINVAL;
				goto out_flash_release;
			}
		}

		if (priv->leds_active & BIT(reg)) {
			ret = -EINVAL;
			goto out_flash_release;
		}
		priv->leds_active |= BIT(reg);

		led->led_no = reg;
		led->priv = priv;
		led->default_state = led_init_default_state_get(child);

		if (reg == MT6360_VIRTUAL_MULTICOLOR ||
		    reg <= MT6360_LED_ISNKML)
			ret = mt6360_init_isnk_properties(led, &init_data);
		else
			ret = mt6360_init_flash_properties(led, &init_data);

		if (ret)
			goto out_flash_release;

		ret = mt6360_led_register(&pdev->dev, led, &init_data);
		if (ret)
			goto out_flash_release;

		i++;
	}

	platform_set_drvdata(pdev, priv);
	return 0;

out_flash_release:
	mt6360_v4l2_flash_release(priv);
	return ret;
}

static int mt6360_led_remove(struct platform_device *pdev)
{
	struct mt6360_priv *priv = platform_get_drvdata(pdev);

	mt6360_v4l2_flash_release(priv);
	return 0;
}

static const struct of_device_id __maybe_unused mt6360_led_of_id[] = {
	{ .compatible = "mediatek,mt6360-led", },
	{}
};
MODULE_DEVICE_TABLE(of, mt6360_led_of_id);

static struct platform_driver mt6360_led_driver = {
	.driver = {
		.name = "mt6360-led",
		.of_match_table = mt6360_led_of_id,
	},
	.probe = mt6360_led_probe,
	.remove = mt6360_led_remove,
};
module_platform_driver(mt6360_led_driver);

MODULE_AUTHOR("Gene Chen <gene_chen@richtek.com>");
MODULE_DESCRIPTION("MT6360 LED Driver");
MODULE_LICENSE("GPL v2");

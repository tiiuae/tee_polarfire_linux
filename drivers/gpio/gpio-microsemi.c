// SPDX-License-Identifier: GPL-2.0
/*
 * Microsemi GPIO driver
 *
 * Copyright (C) 2018 Microsemi, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio/driver.h>
#include <linux/clk.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/of_irq.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define MSS_NUM_GPIO		(32)

#define MSS_GPIO_X_CFG_EN_INT       (3)
#define MSS_GPIO_X_CFG_BIT_GPIO_OE  (2)
#define MSS_GPIO_X_CFG_BIT_EN_IN    (1)
#define MSS_GPIO_X_CFG_BIT_EN_OUT   (0)

#define MSS_GPIO_INTR_EDGE_BOTH_MASK     ((4u) << 5)
#define MSS_GPIO_INTR_EDGE_NEGATIVE_MASK ((3u) << 5)
#define MSS_GPIO_INTR_EDGE_POSITIVE_MASK ((2u) << 5)
#define MSS_GPIO_INTR_LEVEL_LOW_MASK     ((1u) << 5)
#define MSS_GPIO_INTR_LEVEL_HIGH_MASK    ((0u) << 5)

#define MSS_GPIO_INDEX_TO_CFG(gpio_cfg_base, gpio_port_index) \
	((gpio_cfg_base) + (gpio_port_index))
#define MSS_GPIO_IRQ_MASK (BIT_MASK(32)-1)

typedef u32 MSS_GPIO_REG_TYPE;
#define MSS_GPIO_IOREAD  ioread32
#define MSS_GPIO_IOWRITE iowrite32

struct microsemi_mss_gpio_chip {
	raw_spinlock_t lock;
	struct gpio_chip gc;
        struct clk		*clk;

	MSS_GPIO_REG_TYPE __iomem *base;
	MSS_GPIO_REG_TYPE __iomem *gpio_cfg_base;
	MSS_GPIO_REG_TYPE __iomem *gpio_irq_base;
	MSS_GPIO_REG_TYPE __iomem *gpio_in_base;
	MSS_GPIO_REG_TYPE __iomem *gpio_out_base;
	unsigned int irq_parent[MSS_NUM_GPIO];
};

/*
 * microsemi_mss_gpio_assign_bit() - local helper function to set/clear bit
 * in register
 * @base_addr: register address
 * @bit_offset: the bit offset index
 * @value: if valid is non zero, set the bit, else clear the bit
 */
static void microsemi_mss_gpio_assign_bit(MSS_GPIO_REG_TYPE *base_addr,
					  int bit_offset, int value)
{
	MSS_GPIO_REG_TYPE output = MSS_GPIO_IOREAD(base_addr);

	if (value)
		output |= BIT(bit_offset);
	else
		output &= ~BIT(bit_offset);

	MSS_GPIO_IOWRITE(output, base_addr);
}

/*
 * microsemi_mss_gpio_direction_input() - set direction of GPIO port to input
 * @gc: GPIO chip pointer
 * @gpio_index: GPIO port index
 *
 * Returns zero (no error)
 */
static int microsemi_mss_gpio_direction_input(struct gpio_chip *gc,
					      unsigned int gpio_index)
{
	struct microsemi_mss_gpio_chip *mss_gpio = gpiochip_get_data(gc);
	MSS_GPIO_REG_TYPE gpio_cfg;
	unsigned long flags;

	if (gpio_index >= gc->ngpio)
		return -EINVAL;

	raw_spin_lock_irqsave(&mss_gpio->lock, flags);

	gpio_cfg = MSS_GPIO_IOREAD(
		MSS_GPIO_INDEX_TO_CFG(mss_gpio->gpio_cfg_base, gpio_index));
	gpio_cfg |= BIT(MSS_GPIO_X_CFG_BIT_EN_IN);
	gpio_cfg &= ~(BIT(MSS_GPIO_X_CFG_BIT_EN_OUT) |
		BIT(MSS_GPIO_X_CFG_BIT_GPIO_OE));
	MSS_GPIO_IOWRITE(gpio_cfg,
		MSS_GPIO_INDEX_TO_CFG(mss_gpio->gpio_cfg_base, gpio_index));

	raw_spin_unlock_irqrestore(&mss_gpio->lock, flags);

	return 0;
}

/*
 * microsemi_mss_gpio_direction_output() - set direction of GPIO port to
 * output, and set value
 * @gc: GPIO chip pointer
 * @gpio_index: GPIO port index
 * @value: The initial value to output on the port
 *
 * Returns zero (no error)
 */
static int microsemi_mss_gpio_direction_output(struct gpio_chip *gc,
				unsigned int gpio_index, int value)
{
	struct microsemi_mss_gpio_chip *mss_gpio = gpiochip_get_data(gc);
	MSS_GPIO_REG_TYPE gpio_cfg;
	unsigned long flags;

	if (gpio_index >= gc->ngpio)
		return -EINVAL;

	raw_spin_lock_irqsave(&mss_gpio->lock, flags);

	//enable GPIO for Output
	//gpio_cfg = MSS_GPIO_IOREAD(MSS_GPIO_INDEX_TO_CFG(mss_gpio->gpio_cfg,
	//	gpio_index));
	gpio_cfg = 0u;
	gpio_cfg |= BIT(MSS_GPIO_X_CFG_BIT_EN_OUT) |
		BIT(MSS_GPIO_X_CFG_BIT_GPIO_OE);
	gpio_cfg &= ~BIT(MSS_GPIO_X_CFG_BIT_EN_IN);
	MSS_GPIO_IOWRITE(gpio_cfg,
		MSS_GPIO_INDEX_TO_CFG(mss_gpio->gpio_cfg_base, gpio_index));

	// write value to output once enabled
	microsemi_mss_gpio_assign_bit(mss_gpio->gpio_out_base, gpio_index,
		value);

	raw_spin_unlock_irqrestore(&mss_gpio->lock, flags);

	return 0;
}

/*
 * microsemi_mss_gpio_get_direction() - get direction of GPIO port
 * @gc: GPIO chip pointer
 * @gpio_index: GPIO port index
 *
 * Returns zero if direction is output, else greater than zero if input
 */
static int microsemi_mss_gpio_get_direction(struct gpio_chip *gc,
					    unsigned int gpio_index)
{
	struct microsemi_mss_gpio_chip *mss_gpio = gpiochip_get_data(gc);
	MSS_GPIO_REG_TYPE gpio_cfg;
	int result;

	if (gpio_index >= gc->ngpio)
		return -EINVAL;

	gpio_cfg =
		MSS_GPIO_IOREAD(MSS_GPIO_INDEX_TO_CFG(mss_gpio->gpio_cfg_base,
			gpio_index));

	if (gpio_cfg & BIT(MSS_GPIO_X_CFG_BIT_EN_IN))
		result = 1;
	else if ((gpio_cfg &
		(BIT(MSS_GPIO_X_CFG_BIT_EN_OUT) |
			BIT(MSS_GPIO_X_CFG_BIT_GPIO_OE)))
		== (BIT(MSS_GPIO_X_CFG_BIT_EN_OUT) |
			BIT(MSS_GPIO_X_CFG_BIT_GPIO_OE)))
		result = 0;
	else
		result = 0;	// sane default

	return result;
}

/*
 * microsemi_mss_gpio_get_value() - get the value being output on a GPIO port
 * @gc: GPIO chip pointer
 * @gpio_index: GPIO port index
 *
 * Returns the value being output on the specified GPIO port, or 0 if the port
 * is not an output
 */
static int microsemi_mss_gpio_get_value(struct gpio_chip *gc,
					unsigned int gpio_index)
{
	struct microsemi_mss_gpio_chip *mss_gpio = gpiochip_get_data(gc);

	if (gpio_index >= gc->ngpio)
		return -EINVAL;

	return !!(MSS_GPIO_IOREAD(mss_gpio->gpio_in_base) &
			BIT(gpio_index));

}

/*
 * microsemi_mss_gpio_set_value() - set the value being output on a GPIO port
 * @gc: GPIO chip pointer
 * @gpio_index: GPIO port index
 * @value: The value to output on the port
 *
 */
static void microsemi_mss_gpio_set_value(struct gpio_chip *gc,
					 unsigned int gpio_index, int value)
{
	struct microsemi_mss_gpio_chip *mss_gpio = gpiochip_get_data(gc);
	unsigned long flags;

	if (gpio_index >= gc->ngpio)
		return;

	raw_spin_lock_irqsave(&mss_gpio->lock, flags);

	microsemi_mss_gpio_assign_bit(mss_gpio->gpio_out_base, gpio_index,
		value);

	raw_spin_unlock_irqrestore(&mss_gpio->lock, flags);
}

/*
 * microsemi_mss_gpio_irq_set_type() - set the type of the IRQ trigger for a
 * GPIO port
 * @d: Pointer to irq_data
 * @type: trigger type (see include/linux/irq.h)
 *
 * Returns 0 (no error)
 */
static int microsemi_mss_gpio_irq_set_type(struct irq_data *d,
						unsigned int type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	int gpio_index = irqd_to_hwirq(d);

	u32 interrupt_type;
	struct microsemi_mss_gpio_chip *mss_gpio = gpiochip_get_data(gc);
	MSS_GPIO_REG_TYPE gpio_cfg;
	unsigned long flags;

	if ((gpio_index < 0) || (gpio_index >= gc->ngpio))
		return -EINVAL;

	switch (type) {
	case IRQ_TYPE_EDGE_BOTH:
		interrupt_type = MSS_GPIO_INTR_EDGE_BOTH_MASK;
		break;

	case IRQ_TYPE_EDGE_FALLING:
		interrupt_type = MSS_GPIO_INTR_EDGE_NEGATIVE_MASK;
		break;

	case IRQ_TYPE_EDGE_RISING:
		interrupt_type = MSS_GPIO_INTR_EDGE_POSITIVE_MASK;
		break;

	case IRQ_TYPE_LEVEL_HIGH:
		interrupt_type = MSS_GPIO_INTR_LEVEL_HIGH_MASK;
		break;

	case IRQ_TYPE_LEVEL_LOW:
	default:
		/* sane default */
		interrupt_type = MSS_GPIO_INTR_LEVEL_LOW_MASK;
		break;
	}

	raw_spin_lock_irqsave(&mss_gpio->lock, flags);

	gpio_cfg = MSS_GPIO_IOREAD(
		MSS_GPIO_INDEX_TO_CFG(mss_gpio->gpio_cfg_base, gpio_index));
	gpio_cfg |= interrupt_type;
	MSS_GPIO_IOWRITE(gpio_cfg,
		MSS_GPIO_INDEX_TO_CFG(mss_gpio->gpio_cfg_base, gpio_index));

	raw_spin_unlock_irqrestore(&mss_gpio->lock, flags);

	return 0;
}

/* chained_irq_{enter,exit} already mask the parent */
static void microsemi_mss_gpio_irq_mask(struct irq_data *d)
{
}

static void microsemi_mss_gpio_irq_unmask(struct irq_data *d)
{
}

/*
 * microsemi_mss_gpio_irq_enable() - enable IRQ handling for GPIO port
 * @d: Pointer to irq_data
 */
static void microsemi_mss_gpio_irq_enable(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct microsemi_mss_gpio_chip *mss_gpio = gpiochip_get_data(gc);
	int gpio_index = irqd_to_hwirq(d) % MSS_NUM_GPIO; // must not fail

	/* Switch to input */
	microsemi_mss_gpio_direction_input(gc, gpio_index);

	/* Clear any sticky pending interrupts */
	microsemi_mss_gpio_assign_bit(mss_gpio->gpio_irq_base, gpio_index, 1);

	/* Enable interrupts */
	microsemi_mss_gpio_assign_bit(
		MSS_GPIO_INDEX_TO_CFG(mss_gpio->gpio_cfg_base, gpio_index),
		MSS_GPIO_X_CFG_EN_INT, 1);
}

/*
 * microsemi_mss_gpio_irq_disable() - disable IRQ handling for GPIO port
 * @d: Pointer to irq_data
 */
static void microsemi_mss_gpio_irq_disable(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct microsemi_mss_gpio_chip *mss_gpio = gpiochip_get_data(gc);
	int gpio_index = irqd_to_hwirq(d) % MSS_NUM_GPIO; // must not fail

	microsemi_mss_gpio_assign_bit(MSS_GPIO_INDEX_TO_CFG
				      (mss_gpio->gpio_cfg_base, gpio_index),
				      MSS_GPIO_X_CFG_EN_INT, 0);
}

static struct irq_chip microsemi_mss_gpio_irqchip = {
	.name = "microsemi_mss_gpio-gpio",
	.irq_set_type = microsemi_mss_gpio_irq_set_type,
	.irq_mask = microsemi_mss_gpio_irq_mask,
	.irq_unmask = microsemi_mss_gpio_irq_unmask,
	.irq_enable = microsemi_mss_gpio_irq_enable,
	.irq_disable = microsemi_mss_gpio_irq_disable,
	.flags = IRQCHIP_MASK_ON_SUSPEND,
};

/*
 * microsemi_mss_gpio_irq_handler() - generic edge/level IRQ handler for GPIO
 * interrupts
 * @desc: Pointer to interrupt descriptor structure
 */
static void microsemi_mss_gpio_irq_handler(struct irq_desc *desc)
{
	struct microsemi_mss_gpio_chip *mss_gpio =
	    gpiochip_get_data(irq_desc_get_handler_data(desc));
	struct irq_chip *irqchip = irq_desc_get_chip(desc);
	MSS_GPIO_REG_TYPE status;
	int offset;

	chained_irq_enter(irqchip, desc);

	status = MSS_GPIO_IOREAD(mss_gpio->gpio_irq_base) & MSS_GPIO_IRQ_MASK;
	for_each_set_bit(offset, (const unsigned long *)&status,
		mss_gpio->gc.ngpio) { 
		generic_handle_irq(irq_find_mapping(mss_gpio->gc.irq.domain,
			offset));
	}

	chained_irq_exit(irqchip, desc);
}

static irqreturn_t microsemi_gpio_irq_handler(int irq, void *mss_gpio_data)
{
	struct microsemi_mss_gpio_chip *mss_gpio = mss_gpio_data;
	MSS_GPIO_REG_TYPE status;
	int offset;
	status = MSS_GPIO_IOREAD(mss_gpio->gpio_irq_base) & MSS_GPIO_IRQ_MASK;
	//MSS_GPIO_IOWRITE(0xFFFFFFFF, mss_gpio->gpio_irq_base);
	for_each_set_bit(offset, (const unsigned long *)&status,
		mss_gpio->gc.ngpio) { 
		microsemi_mss_gpio_assign_bit(mss_gpio->gpio_irq_base, offset, 1);
		generic_handle_irq(irq_find_mapping(mss_gpio->gc.irq.domain,
			offset));
	}
	return IRQ_HANDLED;
}

/*
 * microsemi_mss_gpio_probe() - probe function
 * @pdev Pointer to platform device structure
 */
static int microsemi_mss_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = pdev->dev.of_node;
	struct microsemi_mss_gpio_chip *mss_gpio;
	struct resource *res;
	int gpio_index, irq, ret, ngpio;
	struct gpio_irq_chip *irq_c;
	struct irq_chip *irqc;
	struct clk			*clk;
	int irq_base = 0;

	mss_gpio = devm_kzalloc(dev, sizeof(*mss_gpio), GFP_KERNEL);
	if (!mss_gpio) {
		//dev_err(dev, "out of memory\n");
		return -ENOMEM;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mss_gpio->base = devm_ioremap_resource(dev, res);

	if (IS_ERR(mss_gpio->base)) {
		dev_err(dev, "failed to allocate device memory\n");
		return PTR_ERR(mss_gpio->base);
	}
	clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(clk)) {
		dev_err(&pdev->dev, "failed to get clock\n");
		return PTR_ERR(clk);
	}

	ret = clk_prepare_enable(clk);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable clock\n");
		return -EINVAL;
	}
        mss_gpio->clk = clk;

	mss_gpio->gpio_cfg_base = mss_gpio->base + (0x00u/sizeof(MSS_GPIO_REG_TYPE));
	mss_gpio->gpio_irq_base = mss_gpio->base + (0x80u/sizeof(MSS_GPIO_REG_TYPE));
	mss_gpio->gpio_in_base  = mss_gpio->base + (0x84u/sizeof(MSS_GPIO_REG_TYPE));
	mss_gpio->gpio_out_base = mss_gpio->base + (0x88u/sizeof(MSS_GPIO_REG_TYPE));

	ngpio = of_irq_count(node);
	if (ngpio > MSS_NUM_GPIO) {
		dev_err(dev, "too many interrupts\n");
		return -EINVAL;
	}

	raw_spin_lock_init(&mss_gpio->lock);

	mss_gpio->gc.direction_input = microsemi_mss_gpio_direction_input;
	mss_gpio->gc.direction_output = microsemi_mss_gpio_direction_output;
	mss_gpio->gc.get_direction = microsemi_mss_gpio_get_direction;
	mss_gpio->gc.get = microsemi_mss_gpio_get_value;
	mss_gpio->gc.set = microsemi_mss_gpio_set_value;
	mss_gpio->gc.base = 0;
	mss_gpio->gc.ngpio = ngpio;
	mss_gpio->gc.label = dev_name(dev);
	mss_gpio->gc.parent = dev;
	mss_gpio->gc.owner = THIS_MODULE;

	irq_c = &mss_gpio->gc.irq;
	irq_c->chip = &microsemi_mss_gpio_irqchip;
	irq_c->chip->parent_device = dev;
	irq_c->handler = handle_simple_irq;
	irq_c->default_type = IRQ_TYPE_NONE;
	irq_c->num_parents = 0;

	irq = platform_get_irq(pdev, 0);

	irq_c->parents = irq;

	irq_base = devm_irq_alloc_descs(mss_gpio->gc.parent,
					-1, 0, ngpio, 0);
	if (irq_base < 0) {
		dev_err(mss_gpio->gc.parent, "Couldn't allocate IRQ numbers\n");
		return -ENODEV;
	}

	irq_c->first = irq_base;

	ret = gpiochip_add_data(&mss_gpio->gc, mss_gpio);
	if (ret)
		return ret;

	ret = devm_request_irq(mss_gpio->gc.parent, irq,
			       microsemi_gpio_irq_handler,
			       IRQF_SHARED, pdev->name, mss_gpio);
	if (ret) {
		dev_info(dev, "Microsemi MSS GPIO devm_request_irq failed \n");
	}

	/* Disable all GPIO interrupts before enabling parent interrupts */
	for (gpio_index = 0; gpio_index < ngpio; gpio_index++) {
		MSS_GPIO_REG_TYPE gpio_cfg;
		unsigned long flags;

		raw_spin_lock_irqsave(&mss_gpio->lock, flags);

		gpio_cfg = MSS_GPIO_IOREAD(
			MSS_GPIO_INDEX_TO_CFG(mss_gpio->gpio_cfg_base,
			gpio_index));
	gpio_cfg &= ~(BIT(MSS_GPIO_X_CFG_EN_INT));
		MSS_GPIO_IOWRITE(gpio_cfg,
			MSS_GPIO_INDEX_TO_CFG(mss_gpio->gpio_cfg_base,
			gpio_index));

		raw_spin_unlock_irqrestore(&mss_gpio->lock, flags);
	}

	platform_set_drvdata(pdev, mss_gpio);
	dev_info(dev, "Microsemi MSS GPIO registered %d GPIO%s\n", ngpio, ngpio ? "s":"");

	return 0;
}

static const struct of_device_id microsemi_mss_gpio_match[] = {
	{
		.compatible = "microsemi,ms-pf-mss-gpio",
	},
	{
		.compatible = "microchip,mpfs-gpio",
	},
	{},
};

static struct platform_driver microsemi_mss_gpio_driver = {
	.probe = microsemi_mss_gpio_probe,
	.driver = {
		.name = "microsemi,mss-gpio",
		.of_match_table = of_match_ptr(microsemi_mss_gpio_match),
	},
};

builtin_platform_driver(microsemi_mss_gpio_driver)

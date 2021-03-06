/*
 * Copyright (c) 2017 Intel Corporation
 * Copyright (c) 2021 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT espressif_esp32_i2c

/* Include esp-idf headers first to avoid redefining BIT() macro */
#include <soc/dport_reg.h>
#include <soc/i2c_reg.h>
#include <esp32/rom/gpio.h>
#include <soc/gpio_sig_map.h>

#include <soc.h>
#include <errno.h>
#include <drivers/gpio.h>
#include <drivers/gpio/gpio_esp32.h>
#include <drivers/i2c.h>
#include <drivers/interrupt_controller/intc_esp32.h>
#include <drivers/clock_control.h>
#include <sys/util.h>
#include <string.h>

#define LOG_LEVEL CONFIG_I2C_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(i2c_esp32);

#include "i2c-priv.h"

/* Number of entries in hardware command queue */
#define I2C_ESP32_NUM_CMDS 16
/* Number of bytes in hardware FIFO */
#define I2C_ESP32_BUFFER_SIZE 32

#define I2C_ESP32_TRANSFER_TIMEOUT_MS 100
#define I2C_ESP32_TIMEOUT_USEC  1000

enum i2c_esp32_opcodes {
	I2C_ESP32_OP_RSTART,
	I2C_ESP32_OP_WRITE,
	I2C_ESP32_OP_READ,
	I2C_ESP32_OP_STOP,
	I2C_ESP32_OP_END
};

struct i2c_esp32_cmd {
	uint32_t num_bytes : 8;
	uint32_t ack_en : 1;
	uint32_t ack_exp : 1;
	uint32_t ack_val : 1;
	uint32_t opcode : 3;
	uint32_t reserved : 17;
	uint32_t done : 1;
};

struct i2c_esp32_data {
	uint32_t dev_config;
	uint16_t address;
	uint32_t err_status;

	struct k_sem fifo_sem;
	struct k_sem transfer_sem;

	int irq_line;
};

typedef void (*irq_connect_cb)(void);

struct i2c_esp32_config {
	int index;

	const struct device *clock_dev;

	const struct {
		int sda_out;
		int sda_in;
		int scl_out;
		int scl_in;
	} sig;

	const struct {
		int scl;
		int sda;
	} pins;

	const clock_control_subsys_t peripheral_id;

	const struct {
		bool tx_lsb_first;
		bool rx_lsb_first;
	} mode;

	int irq_source;

	const uint32_t default_config;
	const uint32_t bitrate;
};

static int i2c_esp32_configure_pins(int pin, int matrix_out, int matrix_in)
{
	const int pin_mode = GPIO_OUTPUT_HIGH |
			     GPIO_OPEN_DRAIN  |
			     GPIO_PULL_UP;
	const char *device_name = gpio_esp32_get_gpio_for_pin(pin);
	const struct device *gpio;
	int ret;

	if (!device_name) {
		return -EINVAL;
	}
	gpio = device_get_binding(device_name);
	if (!gpio) {
		return -EINVAL;
	}

	ret = gpio_pin_configure(gpio, pin, pin_mode);
	if (ret < 0) {
		return ret;
	}

	esp_rom_gpio_matrix_out(pin, matrix_out, false, false);
	esp_rom_gpio_matrix_in(pin, matrix_in, false);

	return 0;
}

static int i2c_esp32_configure_speed(const struct device *dev,
				     uint32_t speed)
{
	static const uint32_t speed_to_freq_tbl[] = {
		[I2C_SPEED_STANDARD] = KHZ(100),
		[I2C_SPEED_FAST] = KHZ(400),
		[I2C_SPEED_FAST_PLUS] = MHZ(1),
		[I2C_SPEED_HIGH] = 0,
		[I2C_SPEED_ULTRA] = 0
	};

	const struct i2c_esp32_config *config = dev->config;

	uint32_t sys_clk_freq = 0;
	uint32_t freq_hz = speed_to_freq_tbl[speed];
	uint32_t period;

	if (!freq_hz) {
		return -ENOTSUP;
	}

	if (clock_control_get_rate(config->clock_dev,
				   config->peripheral_id,
				   &sys_clk_freq)) {
		return -EINVAL;
	}

	period = (sys_clk_freq / freq_hz);
	period /= 2U; /* Set hold and setup times to 1/2th of period */

	esp32_set_mask32(period << I2C_SCL_LOW_PERIOD_S,
		   I2C_SCL_LOW_PERIOD_REG(config->index));
	esp32_set_mask32(period << I2C_SCL_HIGH_PERIOD_S,
		   I2C_SCL_HIGH_PERIOD_REG(config->index));

	esp32_set_mask32(period << I2C_SCL_START_HOLD_TIME_S,
		   I2C_SCL_START_HOLD_REG(config->index));
	esp32_set_mask32(period << I2C_SCL_RSTART_SETUP_TIME_S,
		   I2C_SCL_RSTART_SETUP_REG(config->index));
	esp32_set_mask32(period << I2C_SCL_STOP_HOLD_TIME_S,
		   I2C_SCL_STOP_HOLD_REG(config->index));
	esp32_set_mask32(period << I2C_SCL_STOP_SETUP_TIME_S,
		   I2C_SCL_STOP_SETUP_REG(config->index));

	period /= 2U; /* Set sample and hold times to 1/4th of period */
	esp32_set_mask32(period << I2C_SDA_HOLD_TIME_S,
		   I2C_SDA_HOLD_REG(config->index));
	esp32_set_mask32(period << I2C_SDA_SAMPLE_TIME_S,
		   I2C_SDA_SAMPLE_REG(config->index));

	return 0;
}

static int i2c_esp32_configure(const struct device *dev, uint32_t dev_config)
{
	const struct i2c_esp32_config *config = dev->config;
	struct i2c_esp32_data *data = dev->data;
	unsigned int key = irq_lock();
	uint32_t v = 0U;
	int ret;

	ret = i2c_esp32_configure_pins(config->pins.scl,
				       config->sig.scl_out,
				       config->sig.scl_in);
	if (ret < 0) {
		return ret;
	}

	ret = i2c_esp32_configure_pins(config->pins.sda,
				       config->sig.sda_out,
				       config->sig.sda_in);
	if (ret < 0) {
		return ret;
	}

	clock_control_on(config->clock_dev, config->peripheral_id);

	/* MSB or LSB first is configurable for both TX and RX */
	if (config->mode.tx_lsb_first) {
		v |= I2C_TX_LSB_FIRST;
	}

	if (config->mode.rx_lsb_first) {
		v |= I2C_RX_LSB_FIRST;
	}

	if (dev_config & I2C_MODE_MASTER) {
		v |= I2C_MS_MODE;
		sys_write32(0, I2C_SLAVE_ADDR_REG(config->index));
	} else {
		uint32_t addr = (data->address & I2C_SLAVE_ADDR_V);

		if (dev_config & I2C_ADDR_10_BITS) {
			addr |= I2C_ADDR_10BIT_EN;
		}
		sys_write32(addr << I2C_SLAVE_ADDR_S,
			    I2C_SLAVE_ADDR_REG(config->index));

		/* Before setting up FIFO and interrupts, stop transmission */
		sys_clear_bit(I2C_CTR_REG(config->index), I2C_TRANS_START_S);

		/* Byte after address isn't the offset address in slave RAM */
		sys_clear_bit(I2C_FIFO_CONF_REG(config->index),
			      I2C_FIFO_ADDR_CFG_EN_S);
	}

	/* Use open-drain for clock and data pins */
	v |= (I2C_SCL_FORCE_OUT | I2C_SDA_FORCE_OUT);
	v |= I2C_CLK_EN;
	sys_write32(v, I2C_CTR_REG(config->index));

	ret = i2c_esp32_configure_speed(dev, I2C_SPEED_GET(dev_config));
	if (ret < 0) {
		goto out;
	}

	/* Use FIFO to transmit data */
	sys_clear_bit(I2C_FIFO_CONF_REG(config->index), I2C_NONFIFO_EN_S);

	v = CONFIG_I2C_ESP32_TIMEOUT & I2C_TIME_OUT_REG;
	sys_write32(v << I2C_TIME_OUT_REG_S, I2C_TO_REG(config->index));

	/* Enable interrupt types handled by the ISR */
	sys_write32(I2C_ACK_ERR_INT_ENA_M |
		    I2C_TIME_OUT_INT_ENA_M |
		    I2C_TRANS_COMPLETE_INT_ENA_M |
		    I2C_ARBITRATION_LOST_INT_ENA_M,
		    I2C_INT_ENA_REG(config->index));

	irq_enable(data->irq_line);

out:
	irq_unlock(key);

	return ret;
}

static inline void i2c_esp32_reset_fifo(const struct i2c_esp32_config *config)
{
	uint32_t reg = I2C_FIFO_CONF_REG(config->index);

	/* Writing 1 and then 0 to these bits will reset the I2C fifo */
	esp32_set_mask32(I2C_TX_FIFO_RST | I2C_RX_FIFO_RST, reg);
	esp32_clear_mask32(I2C_TX_FIFO_RST | I2C_RX_FIFO_RST, reg);
}

static int i2c_esp32_wait_timeout(uint32_t *counter)
{
	if (*counter == 0) {
		return 1;
	}

	(*counter)--;
	k_busy_wait(1);
	return 0;
}

static int i2c_esp32_wait(const struct device *dev,
			  volatile struct i2c_esp32_cmd *wait_cmd)
{
	const struct i2c_esp32_config *config = dev->config;
	struct i2c_esp32_data *data = dev->data;
	uint32_t timeout = I2C_ESP32_TIMEOUT_USEC;

	if (wait_cmd) {
		while (!wait_cmd->done) {
			if (i2c_esp32_wait_timeout(&timeout)) {
				return -ETIMEDOUT;
			}
		}
	}

	/* Wait for I2C bus to finish its business */
	timeout = I2C_ESP32_TIMEOUT_USEC;
	while (sys_read32(I2C_SR_REG(config->index)) & I2C_BUS_BUSY) {
		if (i2c_esp32_wait_timeout(&timeout)) {
			return -ETIMEDOUT;
		}
	}

	uint32_t status = data->err_status;
	if (status & (I2C_ARBITRATION_LOST_INT_RAW | I2C_ACK_ERR_INT_RAW)) {
		data->err_status = 0;
		return -EIO;
	}
	if (status & I2C_TIME_OUT_INT_RAW) {
		data->err_status = 0;
		return -ETIMEDOUT;
	}

	return 0;
}

static int i2c_esp32_transmit(const struct device *dev, volatile struct i2c_esp32_cmd *wait_cmd)
{
	const struct i2c_esp32_config *config = dev->config;
	struct i2c_esp32_data *data = dev->data;

	/* Start transmission and wait for the ISR to give the semaphore */
	sys_set_bit(I2C_CTR_REG(config->index), I2C_TRANS_START_S);
	if (k_sem_take(&data->fifo_sem, K_MSEC(I2C_ESP32_TRANSFER_TIMEOUT_MS)) < 0) {
		return -ETIMEDOUT;
	}

	return i2c_esp32_wait(dev, wait_cmd);
}

static volatile struct i2c_esp32_cmd *
i2c_esp32_write_addr(const struct device *dev,
		     volatile struct i2c_esp32_cmd *cmd,
		     struct i2c_msg *msg,
		     uint16_t addr)
{
	const struct i2c_esp32_config *config = dev->config;
	struct i2c_esp32_data *data = dev->data;
	uint32_t addr_len = 1U;

	i2c_esp32_reset_fifo(config);

	sys_write32(addr & I2C_FIFO_RDATA, I2C_DATA_APB_REG(config->index));
	if (data->dev_config & I2C_ADDR_10_BITS) {
		sys_write32(I2C_DATA_APB_REG(config->index),
			    (addr >> 8) & I2C_FIFO_RDATA);
		addr_len++;
	}

	if ((msg->flags & I2C_MSG_RW_MASK) != I2C_MSG_WRITE) {
		*cmd++ = (struct i2c_esp32_cmd) {
			.opcode = I2C_ESP32_OP_WRITE,
			.ack_en = true,
			.num_bytes = addr_len,
		};
	} else {
		msg->len += addr_len;
	}

	return cmd;
}

static int i2c_esp32_read_msg(const struct device *dev, uint16_t addr,
			      struct i2c_msg msg)
{
	const struct i2c_esp32_config *config = dev->config;
	volatile struct i2c_esp32_cmd *cmd =
		(void *)I2C_COMD0_REG(config->index);
	uint32_t i;
	int ret;

	/* Set the R/W bit to R */
	addr |= BIT(0);

	*cmd++ = (struct i2c_esp32_cmd) {
		.opcode = I2C_ESP32_OP_RSTART
	};

	cmd = i2c_esp32_write_addr(dev, cmd, &msg, addr);

	for (; msg.len; cmd = (void *)I2C_COMD0_REG(config->index)) {
		volatile struct i2c_esp32_cmd *wait_cmd = NULL;
		uint32_t to_read = MIN(I2C_ESP32_BUFFER_SIZE, msg.len - 1);

		/* Might be the last byte, in which case, `to_read` will
		 * be 0 here.  See comment below.
		 */
		if (to_read) {
			*cmd++ = (struct i2c_esp32_cmd) {
				.opcode = I2C_ESP32_OP_READ,
				.num_bytes = to_read,
			};
		}

		/* I2C master won't acknowledge the last byte read from the
		 * slave device.  Divide the read command in two segments as
		 * recommended by the ESP32 Technical Reference Manual.
		 */
		if (msg.len - to_read <= 1U) {
			/* Read the last byte and explicitly ask for an
			 * acknowledgment.
			 */
			*cmd++ = (struct i2c_esp32_cmd) {
				.opcode = I2C_ESP32_OP_READ,
				.num_bytes = 1,
				.ack_val = true,
			};

			/* Account for the `msg.len - 1` when clamping
			 * transmission length to FIFO buffer size.
			 */
			to_read++;

			if (msg.flags & I2C_MSG_STOP) {
				wait_cmd = cmd;
				*cmd++ = (struct i2c_esp32_cmd) {
					.opcode = I2C_ESP32_OP_STOP
				};
			}
		}
		if (!wait_cmd) {
			*cmd++ = (struct i2c_esp32_cmd) {
				.opcode = I2C_ESP32_OP_END
			};
		}

		ret = i2c_esp32_transmit(dev, wait_cmd);
		if (ret < 0) {
			return ret;
		}

		for (i = 0U; i < to_read; i++) {
			uint32_t v = sys_read32(I2C_DATA_APB_REG(config->index));

			*msg.buf++ = v & I2C_FIFO_RDATA;
		}
		msg.len -= to_read;

		i2c_esp32_reset_fifo(config);
	}

	return 0;
}

static int i2c_esp32_write_msg(const struct device *dev, uint16_t addr,
			       struct i2c_msg msg)
{
	const struct i2c_esp32_config *config = dev->config;
	volatile struct i2c_esp32_cmd *cmd =
		(void *)I2C_COMD0_REG(config->index);

	*cmd++ = (struct i2c_esp32_cmd) {
		.opcode = I2C_ESP32_OP_RSTART
	};

	cmd = i2c_esp32_write_addr(dev, cmd, &msg, addr);

	for (; msg.len; cmd = (void *)I2C_COMD0_REG(config->index)) {
		uint32_t to_send = MIN(I2C_ESP32_BUFFER_SIZE, msg.len);
		uint32_t i;
		int ret;

		/* Copy data to TX fifo */
		for (i = 0U; i < to_send; i++) {
			sys_write32(*msg.buf++,
				    I2C_DATA_APB_REG(config->index));
		}
		*cmd++ = (struct i2c_esp32_cmd) {
			.opcode = I2C_ESP32_OP_WRITE,
			.num_bytes = to_send,
			.ack_en = true,
		};
		msg.len -= to_send;

		if (!msg.len || (msg.flags & I2C_MSG_STOP)) {
			*cmd = (struct i2c_esp32_cmd) {
				.opcode = I2C_ESP32_OP_STOP
			};
		} else {
			*cmd = (struct i2c_esp32_cmd) {
				.opcode = I2C_ESP32_OP_END
			};
		}

		ret = i2c_esp32_transmit(dev, cmd);
		if (ret < 0) {
			return ret;
		}

		i2c_esp32_reset_fifo(config);
	}

	return 0;
}

static int i2c_esp32_transfer(const struct device *dev, struct i2c_msg *msgs,
			      uint8_t num_msgs, uint16_t addr)
{
	struct i2c_esp32_data *data = dev->data;
	int ret = 0;
	uint8_t i;

	k_sem_take(&data->transfer_sem, K_FOREVER);

	/* Mask out unused address bits, and make room for R/W bit */
	addr &= BIT_MASK(data->dev_config & I2C_ADDR_10_BITS ? 10 : 7);
	addr <<= 1;

	for (i = 0U; i < num_msgs; i++) {
		if ((msgs[i].flags & I2C_MSG_RW_MASK) == I2C_MSG_WRITE) {
			ret = i2c_esp32_write_msg(dev, addr, msgs[i]);
		} else {
			ret = i2c_esp32_read_msg(dev, addr, msgs[i]);
		}

		if (ret < 0) {
			break;
		}
	}

	k_sem_give(&data->transfer_sem);

	return ret;
}

static void i2c_esp32_isr(void *arg)
{
	struct device *dev = (struct device *)arg;
	const int fifo_give_mask = I2C_ACK_ERR_INT_ST |
				   I2C_TIME_OUT_INT_ST |
				   I2C_TRANS_COMPLETE_INT_ST |
				   I2C_ARBITRATION_LOST_INT_ST;
	const struct i2c_esp32_config *config = dev->config;
	uint32_t status = sys_read32(I2C_INT_STATUS_REG(config->index));

	if (status & fifo_give_mask) {
		struct i2c_esp32_data *data = dev->data;

		/* Only give the semaphore if a watched interrupt happens.
		 * Error checking is performed at the other side of the
		 * semaphore, by reading the status register.
		 */
		if (status & I2C_ACK_ERR_INT_ST) {
			data->err_status |= I2C_ACK_ERR_INT_ST;
		}
		if (status & I2C_ARBITRATION_LOST_INT_ST) {
			data->err_status |= I2C_ARBITRATION_LOST_INT_ST;
		}
		if (status & I2C_TIME_OUT_INT_ST) {
			data->err_status |= I2C_TIME_OUT_INT_ST;
		}
		k_sem_give(&data->fifo_sem);
	}

	/* Acknowledge all I2C interrupts */
	sys_write32(~0, I2C_INT_CLR_REG(config->index));
}

static int i2c_esp32_init(const struct device *dev);

static const struct i2c_driver_api i2c_esp32_driver_api = {
	.configure = i2c_esp32_configure,
	.transfer = i2c_esp32_transfer,
};

#if DT_NODE_HAS_STATUS(DT_DRV_INST(0), okay)
static const struct i2c_esp32_config i2c_esp32_config_0 = {
	.index = 0,
	.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(0)),
	.peripheral_id = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(0, offset),
	.sig = {
		.sda_out = I2CEXT0_SDA_OUT_IDX,
		.sda_in = I2CEXT0_SDA_IN_IDX,
		.scl_out = I2CEXT0_SCL_OUT_IDX,
		.scl_in = I2CEXT0_SCL_IN_IDX,
	},
	.pins = {
		.scl = DT_INST_PROP(0, scl_pin),
		.sda = DT_INST_PROP(0, sda_pin),
	},
	.mode = {
		.tx_lsb_first =
			IS_ENABLED(CONFIG_I2C_ESP32_0_TX_LSB_FIRST),
		.rx_lsb_first =
			IS_ENABLED(CONFIG_I2C_ESP32_0_RX_LSB_FIRST),
	},
	.irq_source = DT_IRQN(DT_NODELABEL(i2c0)),
	.default_config = I2C_MODE_MASTER, /* FIXME: Zephyr don't support I2C_SLAVE_MODE */
	.bitrate = DT_INST_PROP(0, clock_frequency),
};

static struct i2c_esp32_data i2c_esp32_data_0;

DEVICE_DT_INST_DEFINE(0, &i2c_esp32_init, NULL,
		    &i2c_esp32_data_0, &i2c_esp32_config_0,
		    POST_KERNEL, CONFIG_I2C_INIT_PRIORITY,
		    &i2c_esp32_driver_api);
#endif /* DT_NODE_HAS_STATUS(DT_DRV_INST(0), okay) */

#if DT_NODE_HAS_STATUS(DT_DRV_INST(1), okay)
static const struct i2c_esp32_config i2c_esp32_config_1 = {
	.index = 1,
	.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(1)),
	.peripheral_id = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(1, offset),
	.sig = {
		.sda_out = I2CEXT1_SDA_OUT_IDX,
		.sda_in = I2CEXT1_SDA_IN_IDX,
		.scl_out = I2CEXT1_SCL_OUT_IDX,
		.scl_in = I2CEXT1_SCL_IN_IDX,
	},
	.pins = {
		.scl = DT_INST_PROP(1, scl_pin),
		.sda = DT_INST_PROP(1, sda_pin),
	},
	.mode = {
		.tx_lsb_first =
			IS_ENABLED(CONFIG_I2C_ESP32_1_TX_LSB_FIRST),
		.rx_lsb_first =
			IS_ENABLED(CONFIG_I2C_ESP32_1_RX_LSB_FIRST),
	},
	.irq_source = DT_IRQN(DT_NODELABEL(i2c1)),
	.default_config = I2C_MODE_MASTER, /* FIXME: Zephyr don't support I2C_SLAVE_MODE */
	.bitrate = DT_INST_PROP(1, clock_frequency),
};

static struct i2c_esp32_data i2c_esp32_data_1;

DEVICE_DT_INST_DEFINE(1, &i2c_esp32_init, NULL,
		    &i2c_esp32_data_1, &i2c_esp32_config_1,
		    POST_KERNEL, CONFIG_I2C_INIT_PRIORITY,
		    &i2c_esp32_driver_api);
#endif /* DT_NODE_HAS_STATUS(DT_DRV_INST(1), okay) */

static int i2c_esp32_init(const struct device *dev)
{
	const struct i2c_esp32_config *config = dev->config;
	struct i2c_esp32_data *data = dev->data;
	uint32_t bitrate_cfg = i2c_map_dt_bitrate(config->bitrate);

	unsigned int key = irq_lock();

	k_sem_init(&data->fifo_sem, 1, 1);
	k_sem_init(&data->transfer_sem, 1, 1);

	/* Even if irq_enable() is called on config->irq.line, disable
	 * interrupt sources in the I2C controller.
	 */
	sys_write32(0, I2C_INT_ENA_REG(config->index));
	data->irq_line = esp_intr_alloc(config->irq_source, 0, i2c_esp32_isr, (void *)dev, NULL);
	irq_unlock(key);

	return i2c_esp32_configure(dev, config->default_config | bitrate_cfg);
}

/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <stdio.h>
#include <zephyr/drivers/i2c.h>


const struct device *const mpu9250 = DEVICE_DT_GET_ANY(invensense_mpu9250);


int main(void)
{
    const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

	uint8_t reg = 0x75;
	uint8_t chipId = 0;

	i2c_write_read(i2c_dev, 0x68, &reg, 1, &chipId, 1);

	printk("WHO_AM_I = 0x%02x\n", chipId);

    if (!mpu9250 || !device_is_ready(mpu9250)) {
        printk("Device is not ready or not found\n");
        return 0;
    }

	struct sensor_value accel[3];
    struct sensor_value gyro[3];
    struct sensor_value accel_x;

    while (1) {
		int rc = sensor_sample_fetch(mpu9250);

		if (rc == 0) {
			rc = sensor_channel_get(mpu9250, SENSOR_CHAN_ACCEL_XYZ, accel);
		}
		if (rc == 0) {
			rc = sensor_channel_get(mpu9250, SENSOR_CHAN_GYRO_XYZ, gyro);
		}
		/* Remove temperature read - not supported without MAGN_EN */
		if (rc == 0) {
			rc = sensor_channel_get(mpu9250, SENSOR_CHAN_ACCEL_X, &accel_x);
		}

		if (rc == 0) {
			printf("  accel %f %f %f m/s/s\n"
				"  gyro  %f %f %f rad/s\n",
				sensor_value_to_double(&accel[0]),
				sensor_value_to_double(&accel[1]),
				sensor_value_to_double(&accel[2]),
				sensor_value_to_double(&gyro[0]),
				sensor_value_to_double(&gyro[1]),
				sensor_value_to_double(&gyro[2]));

			printf("  Accel X (raw): %d.%06d\n", accel[0].val1, abs(accel[0].val2));
			printf("  Accel Y (raw): %d.%06d\n", accel[1].val1, abs(accel[1].val2));
			printf("  Accel Z (raw): %d.%06d\n", accel[2].val1, abs(accel[2].val2));
			printf("  Accel X (individual channel): %f m/s/s\n",
				sensor_value_to_double(&accel_x));
		} else {
			printf("sample fetch/get failed: %d\n", rc);
		}

		k_sleep(K_MSEC(1000));
	}   
}
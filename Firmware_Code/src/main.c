/*
 * Copyright (c) 2021 Bosch Sensortec GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>

LOG_MODULE_REGISTER(imu_test, LOG_LEVEL_INF);

/* ===== Thermistor Configuration ===== */
#define VREF_MV       3000.0
#define R_FIXED_OHM   10000.0
#define R0_OHM        10000.0
#define BETA          3950.0
#define T0_K          298.15
#define TEMP_CUTOFF   45.0

/* ===== PWM Peltier Configuration ===== */
#define PWM_PERIOD_NS   PWM_MSEC(10)   /* 10ms period */
#define PELTIER_OFF_NS  0
#define PELTIER_ON_NS   5000000        /* 50% duty cycle */

/* ===== LRA Vibration Motor Configuration ===== */
#define LRA_OFF_NS      0
#define LRA_ON_NS       5000000        /* 50% duty cycle */

/* ===== Global Variables ===== */
static int16_t adc_buf;
static struct adc_sequence adc_seq = {
    .buffer = &adc_buf,
    .buffer_size = sizeof(adc_buf),
};
static const struct adc_dt_spec adc_channel = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));

/* ===== Function Declarations ===== */
static int read_thermistor_mv(int *out_mv);
static double thermistor_temp_c_from_mv(int vout_mv);
static int set_peltier_pwm(uint32_t pulse_ns);
static int set_lra_pwm(uint32_t pulse_ns);

/* ===== Thermistor Reading Function ===== */
static int read_thermistor_mv(int *out_mv)
{
    int err = adc_read(adc_channel.dev, &adc_seq);
    if (err < 0) {
        LOG_ERR("adc_read failed (%d)", err);
        return err;
    }
    int mv = (int)adc_buf;
    err = adc_raw_to_millivolts_dt(&adc_channel, &mv);
    if (err < 0) {
        LOG_WRN("adc_raw_to_millivolts not supported; raw=%d", (int)adc_buf);
        return -ENOTSUP;
    }
    *out_mv = mv;
    return 0;
}

/* ===== Temperature Calculation Function ===== */
static double thermistor_temp_c_from_mv(int vout_mv)
{
    if (vout_mv <= 1) vout_mv = 1;
    if (vout_mv >= (int)VREF_MV - 1) vout_mv = (int)VREF_MV - 1;

    double v = (double)vout_mv;
    double r_therm = R_FIXED_OHM * v / (VREF_MV - v); 
    double inv_T = (1.0 / T0_K) + (1.0 / BETA) * log(r_therm / R0_OHM);
    double T_k = 1.0 / inv_T;
    return T_k - 273.15;
}

/* ===== PWM Control Function ===== */
static int set_peltier_pwm(uint32_t pulse_ns)
{
    /* First Peltier: Use pwm_led0 alias */
    const struct pwm_dt_spec pwm_peltier1 = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0));
    /* Second Peltier: Use pwm_led2 alias (P1.12) */
    const struct pwm_dt_spec pwm_peltier2 = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led2));
    
    int err1 = pwm_set_dt(&pwm_peltier1, PWM_PERIOD_NS, pulse_ns);
    int err2 = pwm_set_dt(&pwm_peltier2, PWM_PERIOD_NS, pulse_ns);
    
    if (err1 < 0) {
        LOG_ERR("Peltier1 PWM set failed (%d)", err1);
        return err1;
    }
    if (err2 < 0) {
        LOG_ERR("Peltier2 PWM set failed (%d)", err2);
        return err2;
    }
    
    return 0;
}

/* ===== LRA Vibration Motor Control Function ===== */
static int set_lra_pwm(uint32_t pulse_ns)
{
    const struct pwm_dt_spec pwm_lra1 = PWM_DT_SPEC_GET(DT_NODELABEL(lra_vib1));
    const struct pwm_dt_spec pwm_lra2 = PWM_DT_SPEC_GET(DT_NODELABEL(lra_vib2));
    
    int err1 = pwm_set_dt(&pwm_lra1, PWM_PERIOD_NS, pulse_ns);
    int err2 = pwm_set_dt(&pwm_lra2, PWM_PERIOD_NS, pulse_ns);
    
    if (err1 < 0) {
        LOG_ERR("LRA1 PWM set failed (%d)", err1);
        return err1;
    }
    if (err2 < 0) {
        LOG_ERR("LRA2 PWM set failed (%d)", err2);
        return err2;
    }
    
    return 0;
}

int main(void)
{
        const struct device *const dev = DEVICE_DT_GET_ONE(bosch_bmi270);
        struct sensor_value acc[3], gyr[3];
        struct sensor_value full_scale, sampling_freq, oversampling;
        
        /* LED GPIO device and pins */
        const struct device *gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
        
        /* Check if GPIO device is ready */
        if (!device_is_ready(gpio_dev)) {
                LOG_ERR("GPIO device not ready");
                return 0;
        }
        
        /* Configure LED pins as outputs */
        gpio_pin_configure(gpio_dev, 22, GPIO_OUTPUT_ACTIVE | GPIO_ACTIVE_HIGH);
        gpio_pin_configure(gpio_dev, 23, GPIO_OUTPUT_ACTIVE | GPIO_ACTIVE_HIGH);
        
        LOG_INF("LEDs configured on P0.22 and P0.23");
        
        /* Initialize ADC for thermistor (P0.04) */
        if (!adc_is_ready_dt(&adc_channel)) {
                LOG_ERR("ADC device not ready");
                return 0;
        }
        
        int ret = adc_channel_setup_dt(&adc_channel);
        if (ret < 0) {
                LOG_ERR("adc_channel_setup failed (%d)", ret);
                return 0;
        }
        
        ret = adc_sequence_init_dt(&adc_channel, &adc_seq);
        if (ret < 0) {
                LOG_ERR("adc_sequence_init failed (%d)", ret);
                return 0;
        }
        
        LOG_INF("ADC configured for thermistor on P0.04, channel %d", adc_channel.channel_id);
        LOG_INF("ADC device: %s, resolution: %d bits", adc_channel.dev->name, adc_channel.resolution);
        
        /* Test ADC reading */
        int test_mv = 0;
        ret = read_thermistor_mv(&test_mv);
        if (ret == 0) {
                LOG_INF("Initial ADC test: raw=%d, mV=%d", (int)adc_buf, test_mv);
        } else {
                LOG_ERR("Initial ADC test failed: %d", ret);
        }
        
        /* Initialize PWM for Peltiers */
        const struct pwm_dt_spec pwm_peltier1 = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0));
        const struct pwm_dt_spec pwm_peltier2 = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led2));
        if (!pwm_is_ready_dt(&pwm_peltier1)) {
                LOG_ERR("Peltier1 PWM device not ready");
                return 0;
        }
        if (!pwm_is_ready_dt(&pwm_peltier2)) {
                LOG_ERR("Peltier2 PWM device not ready");
                return 0;
        }
        
        /* Disable Peltiers initially */
        set_peltier_pwm(PELTIER_OFF_NS);
        LOG_INF("PWM configured for Peltiers: P1.09 (Peltier1) and P1.12 (Peltier2)");
 
        if (!device_is_ready(dev)) {
                LOG_ERR("Device %s is not ready", dev->name);
                return 0;
        }

        LOG_INF("Device %p name is %s", dev, dev->name);
 
         /* Setting scale in G, due to loss of precision if the SI unit m/s^2
          * is used
          */
         full_scale.val1 = 2;            /* G */
         full_scale.val2 = 0;
         sampling_freq.val1 = 100;       /* Hz. Performance mode */
         sampling_freq.val2 = 0;
         oversampling.val1 = 1;          /* Normal mode */
         oversampling.val2 = 0;
 
        int rc;
        rc = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_FULL_SCALE, &full_scale);
        if (rc) { LOG_ERR("Accel FULL_SCALE set failed (%d)", rc); }
        rc = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_OVERSAMPLING, &oversampling);
        if (rc) { LOG_ERR("Accel OVERSAMPLING set failed (%d)", rc); }
         /* Set sampling frequency last as this also sets the appropriate
          * power mode. If already sampling, change to 0.0Hz before changing
          * other attributes
          */
        rc = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &sampling_freq);
        if (rc) { LOG_ERR("Accel SAMPLING_FREQUENCY set failed (%d)", rc); }
 
 
         /* Setting scale in degrees/s to match the sensor scale */
         full_scale.val1 = 500;          /* dps */
         full_scale.val2 = 0;
         sampling_freq.val1 = 100;       /* Hz. Performance mode */
         sampling_freq.val2 = 0;
         oversampling.val1 = 1;          /* Normal mode */
         oversampling.val2 = 0;
 
        rc = sensor_attr_set(dev, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_FULL_SCALE, &full_scale);
        if (rc) { LOG_ERR("Gyro FULL_SCALE set failed (%d)", rc); }
        rc = sensor_attr_set(dev, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_OVERSAMPLING, &oversampling);
        if (rc) { LOG_ERR("Gyro OVERSAMPLING set failed (%d)", rc); }
         /* Set sampling frequency last as this also sets the appropriate
          * power mode. If already sampling, change sampling frequency to
          * 0.0Hz before changing other attributes
          */
        rc = sensor_attr_set(dev, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &sampling_freq);
        if (rc) { LOG_ERR("Gyro SAMPLING_FREQUENCY set failed (%d)", rc); }
 
        while (1) {
                /* 100ms period for better temperature monitoring */
                k_sleep(K_MSEC(100));

                rc = sensor_sample_fetch(dev);
                if (rc) {
                        LOG_ERR("sensor_sample_fetch failed (%d)", rc);
                        continue;
                }

                rc = sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, acc);
                if (rc) { LOG_ERR("ACCEL_XYZ read failed (%d)", rc); }
                rc = sensor_channel_get(dev, SENSOR_CHAN_GYRO_XYZ, gyr);
                if (rc) { LOG_ERR("GYRO_XYZ read failed (%d)", rc); }

                /* Convert X acceleration to m/s^2 for comparison */
                double ax_ms2 = sensor_value_to_double(&acc[0]);
                
                /* Read temperature from thermistor */
                int mv = 0;
                double temp_c = NAN;
                ret = read_thermistor_mv(&mv);
                if (ret == 0) {
                        temp_c = thermistor_temp_c_from_mv(mv);
                }
                
                /* Control LEDs, Peltier, and LRA based on X acceleration and temperature */
                bool led_on = false;
                uint32_t peltier_ns = PELTIER_OFF_NS;
                uint32_t lra_ns = LRA_OFF_NS;
                
                if (ax_ms2 < 5.0) {
                        /* X acceleration < 5 m/s^2: turn on LEDs and LRA */
                        led_on = true;
                        gpio_pin_set(gpio_dev, 22, 1);
                        gpio_pin_set(gpio_dev, 23, 1);
                        lra_ns = LRA_ON_NS;  /* LRA vibration motor ON */
                        
                        /* Check temperature protection for Peltier */
                        if (!isnan(temp_c) && temp_c > TEMP_CUTOFF) {
                                peltier_ns = PELTIER_OFF_NS;
                                LOG_WRN("Temperature protection: %.1f°C >= %.1f°C -> Peltier OFF", 
                                        temp_c, TEMP_CUTOFF);
                        } else {
                                peltier_ns = PELTIER_ON_NS;
                                if (!isnan(temp_c)) {
                                        LOG_INF("Peltier ON (Temp=%.1f°C)", temp_c);
                                } else {
                                        LOG_INF("Peltier ON (Temp=N/A)");
                                }
                        }
                } else {
                        /* X acceleration >= 5 m/s^2: turn off LEDs, Peltier, and LRA */
                        led_on = false;
                        peltier_ns = PELTIER_OFF_NS;
                        lra_ns = LRA_OFF_NS;  /* LRA vibration motor OFF */
                        gpio_pin_set(gpio_dev, 22, 0);
                        gpio_pin_set(gpio_dev, 23, 0);
                }
                
                /* Set Peltier PWM */
                set_peltier_pwm(peltier_ns);
                
                /* Set LRA PWM */
                set_lra_pwm(lra_ns);

                /* printf output like reference code */
                printf("AX: %d.%06d AY: %d.%06d AZ: %d.%06d  "
                       "GX: %d.%06d GY: %d.%06d GZ: %d.%06d  ",
                       acc[0].val1, acc[0].val2, acc[1].val1, acc[1].val2, acc[2].val1, acc[2].val2,
                       gyr[0].val1, gyr[0].val2, gyr[1].val1, gyr[1].val2, gyr[2].val1, gyr[2].val2);

                if (ret == 0)      printf("[Therm=%dmV, %.1fC] ", mv, temp_c);
                printf("[LED=%s, Peltier=%s, LRA=%s]\n",
                       led_on ? "ON" : "OFF",
                       (peltier_ns == PELTIER_OFF_NS) ? "OFF" : "ON(50%)",
                       (lra_ns == LRA_OFF_NS) ? "OFF" : "ON(50%)");
        }
         return 0;
 }

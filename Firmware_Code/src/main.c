/*
 * Copyright (c) 2021 Bosch Sensortec GmbH
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <stdio.h>
#include <math.h>

/* LED配置 - 使用led1别名 */
#define LED0_NODE DT_ALIAS(led1)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* 按钮配置 - 使用sw1别名 (Button 1) */
#define SW1_NODE DT_ALIAS(sw1)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(SW1_NODE, gpios, {0});

/* 按钮中断回调结构 */
static struct gpio_callback button_cb_data;

/* 全局变量 */
static double reference_x = 0.0;           /* 参考X轴值 */
static bool reference_set = false;         /* 是否已设置参考值 */
static volatile bool should_set_reference = false;  /* 中断标志 */
static const double MAX_CHANGE = 3.0;      /* 最大变化值 */

/* 按钮按下中断回调函数 */
void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    /* 在中断中设置标志，让主循环处理参考值设置 */
    should_set_reference = true;
}

int main(void)
{
    const struct device *const dev = DEVICE_DT_GET_ONE(bosch_bmi270);
    struct sensor_value acc[3], gyr[3];
    struct sensor_value full_scale, sampling_freq, oversampling;
    int ret;
    
    /* 检查IMU设备是否就绪 */
    if (!device_is_ready(dev)) {
        printf("Device %s is not ready\n", dev->name);
        return 0;
    }
    
    /* 检查LED GPIO是否就绪 */
    if (!gpio_is_ready_dt(&led)) {
        printf("LED GPIO is not ready\n");
        return 0;
    }
    
    /* 检查按钮GPIO是否就绪 */
    if (!gpio_is_ready_dt(&button)) {
        printf("Button GPIO is not ready\n");
        return 0;
    }
    
    /* 配置LED为输出模式 */
    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        printf("Error configuring LED GPIO\n");
        return 0;
    }
    
    /* 配置按钮为输入模式 */
    ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
    if (ret < 0) {
        printf("Error configuring button GPIO\n");
        return 0;
    }
    
    /* 配置按钮中断 */
    ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) {
        printf("Error configuring button interrupt\n");
        return 0;
    }
    
    /* 初始化并添加回调 */
    gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);
    
    printf("Device %p name is %s\n", dev, dev->name);
    
    /* 配置加速度计 */
    full_scale.val1 = 2; /* G */
    full_scale.val2 = 0;
    sampling_freq.val1 = 100; /* Hz. Performance mode */
    sampling_freq.val2 = 0;
    oversampling.val1 = 1; /* Normal mode */
    oversampling.val2 = 0;
    
    sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_FULL_SCALE,
                    &full_scale);
    sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_OVERSAMPLING,
                    &oversampling);
    sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ,
                    SENSOR_ATTR_SAMPLING_FREQUENCY,
                    &sampling_freq);
    
    /* 配置陀螺仪 */
    full_scale.val1 = 500; /* dps */
    full_scale.val2 = 0;
    sampling_freq.val1 = 100; /* Hz. Performance mode */
    sampling_freq.val2 = 0;
    oversampling.val1 = 1; /* Normal mode */
    oversampling.val2 = 0;
    
    sensor_attr_set(dev, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_FULL_SCALE,
                    &full_scale);
    sensor_attr_set(dev, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_OVERSAMPLING,
                    &oversampling);
    sensor_attr_set(dev, SENSOR_CHAN_GYRO_XYZ,
                    SENSOR_ATTR_SAMPLING_FREQUENCY,
                    &sampling_freq);
    
    printf("Press Button 1 to set reference X-axis value\n");
    printf("Max change threshold: %.1f\n", MAX_CHANGE);
    
    while (1) {
        /* 100ms周期，与100Hz采样频率匹配 */
        k_sleep(K_MSEC(100));
        
        sensor_sample_fetch(dev);
        sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, acc);
        sensor_channel_get(dev, SENSOR_CHAN_GYRO_XYZ, gyr);
        
        /* 将加速度值转换为浮点数进行比较 */
        double acc_x = (double)acc[0].val1 + (double)acc[0].val2 / 1000000.0;
        
        /* 检查是否需要设置参考值（中断触发） */
        if (should_set_reference) {
            reference_x = acc_x;
            reference_set = true;
            should_set_reference = false;  /* 清除标志 */
            printf("Reference X set to: %.6f\n", reference_x);
        }
        
        /* LED控制逻辑 */
        if (reference_set) {
            /* 计算当前X轴与参考值的差值（只关心减少的情况） */
            double change = reference_x - acc_x;  /* 正值表示X轴减少了 */
            
            /* 只有当X轴值比参考值小超过MAX_CHANGE时才点亮LED */
            if (change > MAX_CHANGE) {
                gpio_pin_set_dt(&led, 1);  /* 点亮LED */
                printf("LED ON (decrease: %.3f) - ", change);
            } else {
                gpio_pin_set_dt(&led, 0);  /* 熄灭LED */
                if (change > 0) {
                    printf("LED OFF (decrease: %.3f, not enough) - ", change);
                } else {
                    printf("LED OFF (increase: %.3f) - ", -change);
                }
            }
        } else {
            /* 如果还没有设置参考值，LED保持熄灭 */
            gpio_pin_set_dt(&led, 0);
            printf("No reference set - ");
        }
        
        printf("AX: %d.%06d; AY: %d.%06d; AZ: %d.%06d; "
               "GX: %d.%06d; GY: %d.%06d; GZ: %d.%06d;",
               acc[0].val1, acc[0].val2,
               acc[1].val1, acc[1].val2,
               acc[2].val1, acc[2].val2,
               gyr[0].val1, gyr[0].val2,
               gyr[1].val1, gyr[1].val2,
               gyr[2].val1, gyr[2].val2);
        
        if (reference_set) {
            printf(" [Ref: %.3f]", reference_x);
        }
        printf("\n");
    }
    
    return 0;
}
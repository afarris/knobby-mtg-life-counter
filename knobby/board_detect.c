#ifndef SIMULATOR  /* whole file is ESP32 hardware — excluded from sim/web builds */

#include "board_detect.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

#define CST816S_ADDR    0x15
#define I2C_PROBE_PORT  I2C_NUM_0

/* ---- Board configurations ----
 * To add a new board:
 *   1. Add a board_pins_t struct below with all pin assignments and mirror flags
 *      (get these from the manufacturer's pinconfig.h / example code)
 *   2. Add an extern for it in board_detect.h
 *   3. Add a pointer to it in the candidates[] array below
 * Detection probes CST816S touch (addr 0x15) on each candidate's I2C pins.
 * First board whose touch responds wins, so put the most common board first. */

/* board_k518 / board_k718 pin tables live in board_pins.c (shared with
 * the simulator build). */

/* Candidates to probe, in order. First match wins. */
static const board_pins_t *candidates[] = {
    &board_k518,
    &board_k718,
};
#define NUM_CANDIDATES (sizeof(candidates) / sizeof(candidates[0]))

const board_pins_t *board = NULL;

/* ---- I2C probe ---- */

static bool probe_touch(const board_pins_t *candidate)
{
    int scl = candidate->touch_scl;
    int sda = candidate->touch_sda;
    int rst = candidate->touch_rst;
    bool found = false;

    /* Toggle reset to wake CST816S — it won't respond to I2C without this */
    gpio_set_direction((gpio_num_t)rst, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)rst, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Set up temporary I2C master for probe */
    i2c_config_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = sda;
    conf.scl_io_num = scl;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 100000;

    if (i2c_param_config(I2C_PROBE_PORT, &conf) == ESP_OK &&
        i2c_driver_install(I2C_PROBE_PORT, I2C_MODE_MASTER, 0, 0, 0) == ESP_OK) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (CST816S_ADDR << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        found = (i2c_master_cmd_begin(I2C_PROBE_PORT, cmd, pdMS_TO_TICKS(100)) == ESP_OK);
        i2c_cmd_link_delete(cmd);
        i2c_driver_delete(I2C_PROBE_PORT);
    }

    /* Reset probed pins so they don't interfere with later init */
    gpio_reset_pin((gpio_num_t)scl);
    gpio_reset_pin((gpio_num_t)sda);
    gpio_reset_pin((gpio_num_t)rst);

    return found;
}

void board_detect(void)
{
    for (size_t i = 0; i < NUM_CANDIDATES; i++) {
        printf("Probing %s... ", candidates[i]->name);
        if (probe_touch(candidates[i])) {
            printf("found!\n");
            board = candidates[i];
            return;
        }
        printf("no\n");
    }

    /* Fallback to first candidate */
    printf("No board detected, defaulting to %s\n", candidates[0]->name);
    board = candidates[0];
}

#endif /* SIMULATOR */

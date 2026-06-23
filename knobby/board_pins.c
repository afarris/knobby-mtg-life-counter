/* Per-board pin tables, compiled into BOTH the firmware and the
 * simulator so the data cannot drift between them. The I2C probe that
 * selects a table at boot lives in board_detect.c (firmware only);
 * the simulator stubs the probe in sim/sim_stubs.c. */

#include "board_detect.h"

const board_pins_t board_k518 = {
    .name       = "JC3636K518",
    .tft_blk    = 47,  .tft_rst  = 21,  .tft_cs  = 14,  .tft_sck = 13,
    .tft_sda0   = 15,  .tft_sda1 = 16,  .tft_sda2 = 17, .tft_sda3 = 18,
    .touch_scl  = 12,  .touch_sda = 11, .touch_int = 9,  .touch_rst = 10,
    .enc_a      = 8,   .enc_b    = 7,
    .bat_adc    = 1,
    .btn        = 0,
    .mirror_x   = false, .mirror_y = false,
};

const board_pins_t board_k718 = {
    .name       = "JC3636K718",
    .tft_blk    = 21,  .tft_rst  = 17,  .tft_cs  = 12,  .tft_sck = 11,
    .tft_sda0   = 13,  .tft_sda1 = 14,  .tft_sda2 = 15, .tft_sda3 = 16,
    .touch_scl  = 10,  .touch_sda = 9,  .touch_int = 7,  .touch_rst = 8,
    .enc_a      = 1,   .enc_b    = 2,
    .bat_adc    = 6,
    .btn        = 0,
    .mirror_x   = true,  .mirror_y = true,
};

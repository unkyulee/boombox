/* Copyright 2015-2017 Jack Humbert
 * Updated 2020 mixedfeelings
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include QMK_KEYBOARD_H

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {

    [0] = LAYOUT(
        MS_BTN1, MS_BTN2, MS_BTN3),

    [1] = LAYOUT(
        MS_BTN1, MS_BTN2, MS_BTN3),

    [2] = LAYOUT(
        MS_BTN1, MS_BTN2, MS_BTN3),

    [3] = LAYOUT(
        MS_BTN1, MS_BTN2, MS_BTN3)};

#if defined(ENCODER_MAP_ENABLE)

const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][2] = {
    [0] = {ENCODER_CCW_CW(MS_LEFT, MS_RGHT), ENCODER_CCW_CW(MS_UP, MS_DOWN), ENCODER_CCW_CW(MS_WHLU, MS_WHLD)},
    [1] = {ENCODER_CCW_CW(MS_LEFT, MS_RGHT), ENCODER_CCW_CW(MS_UP, MS_DOWN), ENCODER_CCW_CW(MS_WHLU, MS_WHLD)},
    [2] = {ENCODER_CCW_CW(MS_LEFT, MS_RGHT), ENCODER_CCW_CW(MS_UP, MS_DOWN), ENCODER_CCW_CW(MS_WHLU, MS_WHLD)},
    [3] = {ENCODER_CCW_CW(MS_LEFT, MS_RGHT), ENCODER_CCW_CW(MS_UP, MS_DOWN), ENCODER_CCW_CW(MS_WHLU, MS_WHLD)},
};

#endif

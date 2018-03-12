/*
 * Copyright (c) 2017, Silicon Laboratories, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */


#ifndef _PDS_PARSER_DEFS_H
#define _PDS_PARSER_DEFS_H

enum PDS_DICT_CONV_VALUE {
    PDS_DICT_ENTRY_VALUE_STR,
    PDS_DICT_ENTRY_VALUE_INT
};

#define PDS_ENUM_CHAR_TO_STR(ENUM_VAL) (const char[]){PDS_DICT_ENTRY_VALUE_STR,(char)ENUM_VAL,0}
#define PDS_ENUM_INT_TO_STR(ENUM_VAL) (const char[]){PDS_DICT_ENTRY_VALUE_INT,(char)ENUM_VAL}

/* MISC ENTRIES */
enum PDS_DICT_ENABLED_DISABLED_E
{
    PDS_DISABLED = '0',
    PDS_ENABLED = '1'
};

/* PINS CONFIGURATIONS */
enum PDS_DICT_PIN_PULL_UP_DOWN_E{
    PDS_PIN_NO_PULL = '0',
    PDS_PIN_PULL_UP = '3',
    PDS_PIN_PULL_DOWN = '2',
    PDS_PIN_PULL_MAINTAIN = '4'
};

enum PDS_DICT_PIN_MODE_E {
    PDS_PIN_MODE_TRISTATE = '0',
    PDS_PIN_MODE_FUNC = '1',
    PDS_PIN_MODE_GPIO = '2',
    PDS_PIN_MODE_DEBUG = '3',
    PDS_PIN_MODE_CLOCK = '7'
};

/* TEST FEATURE */
enum PDSF_DICT_TESTF_TX_CW_MODE_E {
    PDS_TX_CW_MODE_SINGLE = '0',
    PDS_TX_CW_MODE_DUAL = '1'
};

enum PDS_DICT_TESTF_PACKET_HT_E {
    PDS_TX_PACKET_HT_PARAM_MM = '0',
    PDS_TX_PACKET_HT_PARAM_GF = '1'
};

enum PDS_DICT_TESTF_MODE_E {
    PDS_TEST_TX_CW = '0',
    PDS_TEST_TX_PACKET = '1',
    PDS_TEST_RX = '2'
};

enum PDS_DICT_TESTF_RATE_E {
    PDS_RATE_B_1Mbps   = 0,
    PDS_RATE_B_2Mbps   = 1,
    PDS_RATE_B_5_5Mbps = 2,
    PDS_RATE_B_11Mbps = 3,
    PDS_RATE_G_6Mbps  = 6,
    PDS_RATE_G_9Mbps  = 7,
    PDS_RATE_G_12Mbps = 8,
    PDS_RATE_G_18Mbps = 9,
    PDS_RATE_G_24Mbps = 10,
    PDS_RATE_G_36Mbps = 11,
    PDS_RATE_G_48Mbps = 12,
    PDS_RATE_G_54Mbps = 13,
    PDS_RATE_N_MCS0 = 14,
    PDS_RATE_N_MCS1 = 15,
    PDS_RATE_N_MCS2 = 16,
    PDS_RATE_N_MCS3 = 17,
    PDS_RATE_N_MCS4 = 18,
    PDS_RATE_N_MCS5 = 19,
    PDS_RATE_N_MCS6 = 20,
    PDS_RATE_N_MCS7 = 21
};

/* ANTENNA SELECTION */
enum PDS_DICT_ANTENNA_SELECTION_E {
    PDS_ATNA_SEL_TX1_RX1 = '0',
    PDS_ATNA_SEL_TX2_RX2 = '1',
    PDS_ATNA_SEL_TX2_RX1 = '2',
    PDS_ATNA_SEL_TX1_RX2 = '3',
    PDS_ATNA_SEL_TX12_RX12 = '4'
};

enum PDS_DICT_ANTENNA_DIV_MODE_E {
    PDS_ATNA_DIV_MODE_INTERNAL = '1',
    PDS_ATNA_DIV_MODE_EXTERNAL = '2',
    PDS_ATNA_DIV_DISABLED = PDS_DISABLED
};

/* DEBUG CONFIGURATION */
enum PDS_DICT_DEBUG_DIG_SEL_E {
    PDS_DBG_DIG_DISABLED = '0',
    PDS_DBG_DIG_MUX = '1',
    PDS_DBG_DIG_RX_IQ_OUT = '2',
    PDS_DBG_DIG_TX_IQ_IN = '3',
    PDS_DBG_DIG_RX_IQ_IN = '4',
    PDS_DBG_DIG_TX_IQ_OUT = '5'
};

enum PDS_DICT_DEBUG_JTAG_MODE_E {
    PDS_DBG_JTAG_MODE_DCHAINED = '0',
    PDS_DBG_JTAG_MODE_ARM9_ONLY = '2',
    PDS_DBG_JTAG_MODE_ARM0_ONLY = '3'
};

enum PDS_DICT_DEBUG_ANALOG_MUX_E {
    PDS_DBG_ANALOG_DISABALED = PDS_DISABLED,
    PDS_DBG_ANALOG_DIAG0 = '1',
    PDS_DBG_ANALOG_DIAG1 = '2'
};

#endif // _PDS_PARSER_DEFS_H

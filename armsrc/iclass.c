//-----------------------------------------------------------------------------
// Copyright (C) Gerhard de Koning Gans - May 2008
// Contribution made during a security research at Radboud University Nijmegen
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// Routines to support iClass.
//-----------------------------------------------------------------------------
#include "iclass.h"

#include "proxmark3_arm.h"
#include "cmd.h"
// Needed for CRC in emulation mode;
// same construction as in ISO 14443;
// different initial value (CRC_ICLASS)
#include "crc16.h"
#include "optimized_cipher.h"

#include "appmain.h"
#include "BigBuf.h"
#include "fpgaloader.h"
#include "string.h"
#include "util.h"
#include "dbprint.h"
#include "protocols.h"
#include "ticks.h"
#include "iso15693.h"
#include "iclass_cmd.h"              // iclass_card_select_t struct
#include "i2c.h"                     // i2c defines (SIM module access)
#include "printf.h"

uint8_t get_pagemap(const picopass_hdr_t *hdr) {
    return (hdr->conf.fuses & (FUSE_CRYPT0 | FUSE_CRYPT1)) >> 3;
}


#ifndef ICLASS_16KS_SIZE
#define ICLASS_16KS_SIZE       0x100 * 8
#endif

/*
* CARD TO READER
* in ISO15693-2 mode -  Manchester
* in ISO 14443b - BPSK coding
*
* Timings:
*  ISO 15693-2
*           Tout = 330 µs, Tprog 1 = 4 to 15 ms, Tslot = 330 µs + (number of slots x 160 µs)
*  ISO 14443a
*           Tout = 100 µs, Tprog = 4 to 15 ms, Tslot = 100 µs+ (number of slots x 80 µs)
*  ISO 14443b
            Tout = 76 µs, Tprog = 4 to 15 ms, Tslot = 119 µs+ (number of slots x 150 µs)
*
*
*  So for current implementation in ISO15693, its 330 µs from end of reader, to start of card.
*/

//=============================================================================
// a `sniffer' for iClass communication
// Both sides of communication!
//=============================================================================
void SniffIClass(uint8_t jam_search_len, uint8_t *jam_search_string) {
    SniffIso15693(jam_search_len, jam_search_string, true);
}

static void rotateCSN(const uint8_t *original_csn, uint8_t *rotated_csn) {
    for (uint8_t i = 0; i < 8; i++) {
        rotated_csn[i] = (original_csn[i] >> 3) | (original_csn[(i + 1) % 8] << 5);
    }
}

// Encode SOF only
static void CodeIClassTagSOF(void) {
    tosend_reset();
    tosend_t *ts = get_tosend();
    ts->buf[++ts->max] = 0x1D;
    ts->max++;
}

/*
 * SOF comprises 3 parts;
 * * An unmodulated time of 56.64 us
 * * 24 pulses of 423.75 kHz (fc/32)
 * * A logic 1, which starts with an unmodulated time of 18.88us
 *   followed by 8 pulses of 423.75kHz (fc/32)
 *
 *
 * EOF comprises 3 parts:
 * - A logic 0 (which starts with 8 pulses of fc/32 followed by an unmodulated
 *   time of 18.88us.
 * - 24 pulses of fc/32
 * - An unmodulated time of 56.64 us
 *
 *
 * A logic 0 starts with 8 pulses of fc/32
 * followed by an unmodulated time of 256/fc (~18,88us).
 *
 * A logic 0 starts with unmodulated time of 256/fc (~18,88us) followed by
 * 8 pulses of fc/32 (also 18.88us)
 *
 * The mode FPGA_HF_SIMULATOR_MODULATE_424K_8BIT which we use to simulate tag,
 * works like this.
 * - A 1-bit input to the FPGA becomes 8 pulses on 423.5kHz (fc/32) (18.88us).
 * - A 0-bit input to the FPGA becomes an unmodulated time of 18.88us
 *
 * In this mode
 * SOF can be written as 00011101 = 0x1D
 * EOF can be written as 10111000 = 0xb8
 * logic 1 be written as 01 = 0x1
 * logic 0 be written as 10 = 0x2
 *
 *
 */

/**
 * @brief SimulateIClass simulates an iClass card.
 * @param arg0 type of simulation
 *          - 0 uses the first 8 bytes in usb data as CSN
 *          - 2 "dismantling iclass"-attack. This mode iterates through all CSN's specified
 *          in the usb data. This mode collects MAC from the reader, in order to do an offline
 *          attack on the keys. For more info, see "dismantling iclass" and proxclone.com.
 *          - Other : Uses the default CSN (031fec8af7ff12e0)
 * @param arg1 - number of CSN's contained in datain (applicable for mode 2 only)
 * @param arg2
 * @param datain
 */
// turn off afterwards
void SimulateIClass(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint8_t *datain) {
    iclass_simulate(arg0, arg1, arg2, datain, NULL, NULL);
}

void iclass_simulate(uint8_t sim_type, uint8_t num_csns, bool send_reply, uint8_t *datain, uint8_t *dataout, uint16_t *dataoutlen) {

    LEDsoff();

    Iso15693InitTag();

    clear_trace();

    // only logg if we are called from the client.
    set_tracing(send_reply);

    //Use the emulator memory for SIM
    uint8_t *emulator = BigBuf_get_EM_addr();
    uint8_t mac_responses[PM3_CMD_DATA_SIZE] = { 0 };

    if (sim_type == ICLASS_SIM_MODE_CSN) {
        // Use the CSN from commandline
        memcpy(emulator, datain, 8);
        do_iclass_simulation(ICLASS_SIM_MODE_CSN, NULL);

    } else if (sim_type == ICLASS_SIM_MODE_CSN_DEFAULT) {
        //Default CSN
        uint8_t csn[] = { 0x03, 0x1f, 0xec, 0x8a, 0xf7, 0xff, 0x12, 0xe0 };
        // Use the CSN from commandline
        memcpy(emulator, csn, 8);
        do_iclass_simulation(ICLASS_SIM_MODE_CSN, NULL);

    } else if (sim_type == ICLASS_SIM_MODE_READER_ATTACK) {

        Dbprintf("going into attack mode, %d CSNS sent", num_csns);
        // In this mode, a number of csns are within datain. We'll simulate each one, one at a time
        // in order to collect MAC's from the reader. This can later be used in an offlne-attack
        // in order to obtain the keys, as in the "dismantling iclass"-paper.
#define EPURSE_MAC_SIZE 16
        int i = 0;
        for (; i < num_csns && i * EPURSE_MAC_SIZE + 8 < PM3_CMD_DATA_SIZE; i++) {

            memcpy(emulator, datain + (i * 8), 8);

            if (do_iclass_simulation(ICLASS_SIM_MODE_EXIT_AFTER_MAC, mac_responses + i * EPURSE_MAC_SIZE)) {

                if (dataoutlen)
                    *dataoutlen = i * EPURSE_MAC_SIZE;

                // Button pressed
                if (send_reply)
                    reply_old(CMD_ACK, CMD_HF_ICLASS_SIMULATE, i, 0, mac_responses, i * EPURSE_MAC_SIZE);
                goto out;
            }
        }
        if (dataoutlen)
            *dataoutlen = i * EPURSE_MAC_SIZE;

        if (send_reply)
            reply_old(CMD_ACK, CMD_HF_ICLASS_SIMULATE, i, 0, mac_responses, i * EPURSE_MAC_SIZE);

    } else if (sim_type == ICLASS_SIM_MODE_FULL || sim_type == ICLASS_SIM_MODE_FULL_GLITCH || sim_type == ICLASS_SIM_MODE_FULL_GLITCH_KEY) {

        //This is 'full sim' mode, where we use the emulator storage for data.
        //ie:  BigBuf_get_EM_addr should be previously filled with data from the "eload" command
        picopass_hdr_t *hdr = (picopass_hdr_t *)BigBuf_get_EM_addr();
        uint8_t pagemap = get_pagemap(hdr);
        if (pagemap == PICOPASS_NON_SECURE_PAGEMODE) {
            do_iclass_simulation_nonsec();
        } else {
            do_iclass_simulation(sim_type, NULL);
        }

        if (send_reply) {
            reply_mix(CMD_ACK, CMD_HF_ICLASS_SIMULATE, 0, 0, NULL, 0);
        }

    } else if (sim_type == ICLASS_SIM_MODE_READER_ATTACK_KEYROLL) {

        // This is the KEYROLL version of sim 2.
        // the collected data (mac_response) is doubled out since we are trying to collect both keys in the keyroll process.
        // Keyroll iceman  9 csns * 8 * 2 = 144
        // keyroll CARL55  15csns * 8 * 2 = 15 * 8 * 2 = 240
        Dbprintf("going into attack keyroll mode, %d CSNS sent", num_csns);
        // In this mode, a number of csns are within datain. We'll simulate each one, one at a time
        // in order to collect MAC's from the reader. This can later be used in an offlne-attack
        // in order to obtain the keys, as in the "dismantling iclass"-paper.

        // keyroll mode,   reader swaps between old key and new key alternatively when fail a authentication.
        // attack below is same as SIM 2, but we run the CSN twice to collected the mac for both keys.
        int i = 0;
        // The usb data is 512 bytes, fitting 65 8-byte CSNs in there.  iceman fork uses 9 CSNS
        for (; i < num_csns && i * EPURSE_MAC_SIZE + 8 < PM3_CMD_DATA_SIZE; i++) {

            memcpy(emulator, datain + (i * 8), 8);

            // keyroll 1
            if (do_iclass_simulation(ICLASS_SIM_MODE_EXIT_AFTER_MAC, mac_responses + i * EPURSE_MAC_SIZE)) {

                if (dataoutlen)
                    *dataoutlen = i * EPURSE_MAC_SIZE * 2;

                if (send_reply)
                    reply_old(CMD_ACK, CMD_HF_ICLASS_SIMULATE, i * 2, 0, mac_responses, i * EPURSE_MAC_SIZE * 2);

                // Button pressed
                goto out;
            }

            // keyroll 2
            if (do_iclass_simulation(ICLASS_SIM_MODE_EXIT_AFTER_MAC, mac_responses + (i + num_csns) * EPURSE_MAC_SIZE)) {

                if (dataoutlen)
                    *dataoutlen = i * EPURSE_MAC_SIZE * 2;

                if (send_reply)
                    reply_old(CMD_ACK, CMD_HF_ICLASS_SIMULATE, i * 2, 0, mac_responses, i * EPURSE_MAC_SIZE * 2);

                // Button pressed
                goto out;
            }
        }

        if (dataoutlen)
            *dataoutlen = i * EPURSE_MAC_SIZE * 2;

        // double the amount of collected data.
        if (send_reply)
            reply_old(CMD_ACK, CMD_HF_ICLASS_SIMULATE, i * 2, 0, mac_responses, i * EPURSE_MAC_SIZE * 2);

    } else {
        // We may want a mode here where we hardcode the csns to use (from proxclone).
        // That will speed things up a little, but not required just yet.
        DbpString("the mode is not implemented, reserved for future use");
    }

out:
    if (dataout && dataoutlen)
        memcpy(dataout, mac_responses, *dataoutlen);

    switch_off();
    BigBuf_free_keep_EM();
}

/**
 * Simulation assumes a SECURE PAGE simulation with authentication and application areas.
 *
 *
 * @brief Does the actual simulation
 * @param csn - csn to use
 * @param breakAfterMacReceived if true, returns after reader MAC has been received.
 */
int do_iclass_simulation(int simulationMode, uint8_t *reader_mac_buf) {

    // free eventually allocated BigBuf memory
    BigBuf_free_keep_EM();

    uint16_t page_size = 32 * 8;
    uint8_t current_page = 0;

    // maintain cipher states for both credit and debit key for each page
    State_t cipher_state_KD[8];
    State_t cipher_state_KC[8];
    State_t *cipher_state = &cipher_state_KD[0];

    uint8_t *emulator = BigBuf_get_EM_addr();
    uint8_t *csn = emulator;

    // CSN followed by two CRC bytes
    uint8_t anticoll_data[10] = { 0 };
    uint8_t csn_data[10] = { 0 };
    memcpy(csn_data, csn, sizeof(csn_data));

    // Construct anticollision-CSN
    rotateCSN(csn_data, anticoll_data);

    // Compute CRC on both CSNs
    AddCrc(anticoll_data, 8);
    AddCrc(csn_data, 8);

    uint8_t diversified_kd[8] = { 0 };
    uint8_t diversified_kc[8] = { 0 };
    uint8_t *diversified_key = diversified_kd;

    // configuration block
    uint8_t conf_block[10] = {0x12, 0xFF, 0xFF, 0xFF, 0x7F, 0x1F, 0xFF, 0x3C, 0x00, 0x00};

    // e-Purse
    uint8_t card_challenge_data[8] = { 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    // AIA
    uint8_t aia_data[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};

    if (simulationMode == ICLASS_SIM_MODE_FULL || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH_KEY) {

        memcpy(conf_block, emulator + (8 * 1), 8);            // blk 1
        memcpy(card_challenge_data, emulator + (8 * 2), 8); // e-purse, blk 2
        memcpy(diversified_kd, emulator + (8 * 3), 8);      // Kd, blk 3
        memcpy(diversified_kc, emulator + (8 * 4), 8);      // Kc, blk 4

        // (iceman) this only works for 2KS / 16KS tags.
        // Use application data from block 5
        memcpy(aia_data, emulator + (8 * 5), 8);
    }

    AddCrc(conf_block, 8);
    AddCrc(aia_data, 8);

    // set epurse of sim2,4 attack
    if (reader_mac_buf != NULL) {
        memcpy(reader_mac_buf, card_challenge_data, 8);
    }

    if ((conf_block[5] & 0x80) == 0x80) {
        page_size = 256 * 8;
    }

    // From PicoPass DS:
    // When the page is in personalization mode this bit is equal to 1.
    // Once the application issuer has personalized and coded its dedicated areas, this bit must be set to 0:
    // the page is then "in application mode".
    bool personalization_mode = conf_block[7] & 0x80;

    uint8_t block_wr_lock = conf_block[3];

    // chip memory may be divided in 8 pages
    uint8_t max_page = ((conf_block[4] & 0x10) == 0x10) ? 0 : 7;

    // pre-calculate the cipher states, feeding it the CC
    cipher_state_KD[0] = opt_doTagMAC_1(card_challenge_data, diversified_kd);
    cipher_state_KC[0] = opt_doTagMAC_1(card_challenge_data, diversified_kc);

    if (simulationMode == ICLASS_SIM_MODE_FULL || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH_KEY) {

        for (int i = 1; i < max_page; i++) {

            uint8_t *epurse = emulator + (i * page_size) + (8 * 2);
            uint8_t *kd = emulator + (i * page_size) + (8 * 3);
            uint8_t *kc = emulator + (i * page_size) + (8 * 4);

            cipher_state_KD[i] = opt_doTagMAC_1(epurse, kd);
            cipher_state_KC[i] = opt_doTagMAC_1(epurse, kc);
        }
    }

    bool glitch_key_read = false;

    // Anti-collision process:
    // Reader 0a
    // Tag    0f
    // Reader 0c
    // Tag    anticoll. CSN
    // Reader 81 anticoll. CSN
    // Tag    CSN

    uint8_t *modulated_response = NULL;
    int modulated_response_size;
    uint8_t *trace_data = NULL;
    int trace_data_size;

    // Respond SOF -- takes 1 bytes
    uint8_t resp_sof[2] = {0};
    int resp_sof_len;

    // Anticollision CSN (rotated CSN)
    // 22: Takes 2 bytes for SOF/EOF and 10 * 2 = 20 bytes (2 bytes/byte)
    uint8_t *resp_anticoll = BigBuf_calloc(22);
    int resp_anticoll_len;

    // CSN (block 0)
    // 22: Takes 2 bytes for SOF/EOF and 10 * 2 = 20 bytes (2 bytes/byte)
    uint8_t *resp_csn = BigBuf_calloc(22);
    int resp_csn_len;

    // configuration (blk 1) PICOPASS 2ks
    uint8_t *resp_conf = BigBuf_calloc(22);
    int resp_conf_len;

    // e-Purse (blk 2)
    // 18: Takes 2 bytes for SOF/EOF and 8 * 2 = 16 bytes (2 bytes/bit)
    uint8_t *resp_cc = BigBuf_calloc(18);
    int resp_cc_len;

    // Kd, Kc (blocks 3 and 4). Cannot be read. Always respond with 0xff bytes only
    uint8_t *resp_ff = BigBuf_calloc(22);
    int resp_ff_len;
    uint8_t ff_data[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
    AddCrc(ff_data, 8);

    // Application Issuer Area  (blk 5)
    uint8_t *resp_aia = BigBuf_calloc(22);
    int resp_aia_len;

    // receive command
    uint8_t *receivedCmd = BigBuf_calloc(MAX_FRAME_SIZE);

    // Prepare card messages
    tosend_t *ts = get_tosend();

    // First card answer: SOF
    CodeIClassTagSOF();
    memcpy(resp_sof, ts->buf, ts->max);
    resp_sof_len = ts->max;

    // Anticollision CSN
    CodeIso15693AsTag(anticoll_data, sizeof(anticoll_data));
    memcpy(resp_anticoll, ts->buf, ts->max);
    resp_anticoll_len = ts->max;

    // CSN (block 0)
    CodeIso15693AsTag(csn_data, sizeof(csn_data));
    memcpy(resp_csn, ts->buf, ts->max);
    resp_csn_len = ts->max;

    // Configuration (block 1)
    CodeIso15693AsTag(conf_block, sizeof(conf_block));
    memcpy(resp_conf, ts->buf, ts->max);
    resp_conf_len = ts->max;

    // e-Purse (block 2)
    CodeIso15693AsTag(card_challenge_data, sizeof(card_challenge_data));
    memcpy(resp_cc, ts->buf, ts->max);
    resp_cc_len = ts->max;

    // Kd, Kc (blocks 3 and 4)
    CodeIso15693AsTag(ff_data, sizeof(ff_data));
    memcpy(resp_ff, ts->buf, ts->max);
    resp_ff_len = ts->max;

    // Application Issuer Area (block 5)
    CodeIso15693AsTag(aia_data, sizeof(aia_data));
    memcpy(resp_aia, ts->buf, ts->max);
    resp_aia_len = ts->max;

    //This is used for responding to READ-block commands or other data which is dynamically generated
    //First the 'trace'-data, not encoded for FPGA
    uint8_t *data_generic_trace = BigBuf_calloc(34); // 32 bytes data + 2byte CRC is max tag answer

    //Then storage for the modulated data
    //Each bit is doubled when modulated for FPGA, and we also have SOF and EOF (2 bytes)
    uint8_t *data_response = BigBuf_calloc((34 * 2) + 3);

    enum { IDLE, ACTIVATED, SELECTED, HALTED } chip_state = IDLE;

    bool button_pressed = false;
    uint8_t cmd, options, block;
    int len, kc_attempt = 0;
    bool exit_loop = false;
    bool using_kc = false;

    while (exit_loop == false) {
        WDT_HIT();

        // Now look at the reader command and provide appropriate responses
        // default is no response:
        modulated_response = NULL;
        modulated_response_size = 0;
        trace_data = NULL;
        trace_data_size = 0;

        uint32_t reader_eof_time = 0;
        len = GetIso15693CommandFromReader(receivedCmd, MAX_FRAME_SIZE, &reader_eof_time);
        if (len < 0) {
            button_pressed = true;
            exit_loop = true;
            continue;
        }

        // extra response data
        cmd = receivedCmd[0] & 0xF;
        options = (receivedCmd[0] >> 4) & 0xFF;
        block = receivedCmd[1];

        if (cmd == ICLASS_CMD_ACTALL && len == 1) {   // 0x0A
            // Reader in anti collision phase
            modulated_response = resp_sof;
            modulated_response_size = resp_sof_len;
            chip_state = ACTIVATED;
            goto send;

        } else if (cmd == ICLASS_CMD_READ_OR_IDENTIFY && len == 1) { // 0x0C
            // Reader asks for anti collision CSN
            if (chip_state == SELECTED || chip_state == ACTIVATED) {
                modulated_response = resp_anticoll;
                modulated_response_size = resp_anticoll_len;
                trace_data = anticoll_data;
                trace_data_size = sizeof(anticoll_data);
            }
            goto send;

        } else if (cmd == ICLASS_CMD_SELECT && len == 9) {
            // Reader selects anticollision CSN.
            // Tag sends the corresponding real CSN
            if (chip_state == ACTIVATED || chip_state == SELECTED) {
                if (!memcmp(receivedCmd + 1, anticoll_data, 8)) {
                    modulated_response = resp_csn;
                    modulated_response_size = resp_csn_len;
                    trace_data = csn_data;
                    trace_data_size = sizeof(csn_data);
                    chip_state = SELECTED;
                } else {
                    chip_state = IDLE;
                }
            } else if (chip_state == HALTED || chip_state == IDLE) {
                // RESELECT with CSN
                if (!memcmp(receivedCmd + 1, csn_data, 8)) {
                    modulated_response = resp_csn;
                    modulated_response_size = resp_csn_len;
                    trace_data = csn_data;
                    trace_data_size = sizeof(csn_data);
                    chip_state = SELECTED;
                }
            }
            goto send;


        } else if (cmd == ICLASS_CMD_READ_OR_IDENTIFY && len == 4) { // 0x0C

            if (chip_state != SELECTED) {
                goto send;
            }
            if (simulationMode == ICLASS_SIM_MODE_EXIT_AFTER_MAC) {
                // provide defaults for blocks 0 ... 5

                // block0,1,2,5 is always readable.
                switch (block) {
                    case 0: { // csn (0c 00)
                        modulated_response = resp_csn;
                        modulated_response_size = resp_csn_len;
                        trace_data = csn_data;
                        trace_data_size = sizeof(csn_data);
                        goto send;
                    }
                    case 1: { // configuration (0c 01)
                        modulated_response = resp_conf;
                        modulated_response_size = resp_conf_len;
                        trace_data = conf_block;
                        trace_data_size = sizeof(conf_block);
                        goto send;
                    }
                    case 2: {// e-purse (0c 02)
                        modulated_response = resp_cc;
                        modulated_response_size = resp_cc_len;
                        trace_data = card_challenge_data;
                        trace_data_size = sizeof(card_challenge_data);
                        // set epurse of sim2,4 attack
                        if (reader_mac_buf != NULL) {
                            memcpy(reader_mac_buf, card_challenge_data, 8);
                        }
                        goto send;
                    }
                    case 3:
                    case 4: { // Kd, Kc, always respond with 0xff bytes
                        modulated_response = resp_ff;
                        modulated_response_size = resp_ff_len;
                        trace_data = ff_data;
                        trace_data_size = sizeof(ff_data);
                        goto send;
                    }
                    case 5: { // Application Issuer Area (0c 05)
                        modulated_response = resp_aia;
                        modulated_response_size = resp_aia_len;
                        trace_data = aia_data;
                        trace_data_size = sizeof(aia_data);
                        goto send;
                    }
                } // switch
            } else if (simulationMode == ICLASS_SIM_MODE_FULL || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH_KEY) {
                if (block == 3 || block == 4) { // Kd, Kc, always respond with 0xff bytes
                    modulated_response = resp_ff;
                    modulated_response_size = resp_ff_len;
                    trace_data = ff_data;
                    trace_data_size = sizeof(ff_data);
                } else { // use data from emulator memory
                    if (simulationMode == ICLASS_SIM_MODE_FULL_GLITCH) {
                        //Jam the read based on the last SIO block
                        if (memcmp(emulator + (current_page * page_size) + (5 * 8), ff_data, PICOPASS_BLOCK_SIZE) == 0) { //SR card
                            if (block == 16) { //SR cards use a standard legth SIO
                                goto send;
                            }
                        } else { //For SE cards we have to account for different SIO lengths depending if a standard or custom key is used
                            uint8_t *sio = emulator + (current_page * page_size) + (6 * 8);
                            if (block == (5 + ((sio[1] + 12) / 8))) {
                                goto send;
                            }
                        }
                    }

                    memcpy(data_generic_trace, emulator + (current_page * page_size) + (block * 8), 8);
                    AddCrc(data_generic_trace, 8);
                    trace_data = data_generic_trace;
                    trace_data_size = 10;
                    CodeIso15693AsTag(trace_data, trace_data_size);
                    memcpy(data_response, ts->buf, ts->max);
                    modulated_response = data_response;
                    modulated_response_size = ts->max;
                }
                goto send;
            }

        } else if (cmd == ICLASS_CMD_READCHECK && block == 0x02 && len == 2) {  // 0x88
            // Read e-purse KD (88 02)  KC  (18 02)
            if (chip_state != SELECTED) {
                goto send;
            }

            // debit key
            if (receivedCmd[0] == 0x88) {
                cipher_state = &cipher_state_KD[current_page];
                diversified_key = diversified_kd;
                using_kc = false;
            } else {
                cipher_state = &cipher_state_KC[current_page];
                diversified_key = diversified_kc;
                using_kc = true;
            }

            modulated_response = resp_cc;
            modulated_response_size = resp_cc_len;
            trace_data = card_challenge_data;
            trace_data_size = sizeof(card_challenge_data);
            goto send;

        } else if (cmd == ICLASS_CMD_CHECK && len == 9) { // 0x05

            // Reader random and reader MAC!!!
            if (chip_state != SELECTED) {
                goto send;
            }

            if (simulationMode == ICLASS_SIM_MODE_FULL || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH_KEY) {

                if (glitch_key_read) {
                    goto send;
                }

                // NR, from reader, is in receivedCmd +1
                opt_doTagMAC_2(*cipher_state, receivedCmd + 1, data_generic_trace, diversified_key);

                /*
                uint8_t _mac[4] = {0};
                opt_doReaderMAC_2(*cipher_state, receivedCmd + 1, _mac,  diversified_key);

                if (_mac[0] != receivedCmd[5] || _mac[1] != receivedCmd[6] || _mac[2] != receivedCmd[7] || _mac[3] != receivedCmd[8]) {
                    Dbprintf("reader auth " _RED_("failed"));
                    Dbprintf("hf iclass lookup --csn %02x%02x%02x%02x%02x%02x%02x%02x --epurse %02x%02x%02x%02x%02x%02x%02x%02x --macs %02x%02x%02x%02x%02x%02x%02x%02x f iclass_default_keys.dic",
                             csn_data[0], csn_data[1], csn_data[2], csn_data[3], csn_data[4], csn_data[5], csn_data[6], csn_data[7],
                             card_challenge_data[0], card_challenge_data[1], card_challenge_data[2], card_challenge_data[3],
                             card_challenge_data[4], card_challenge_data[5], card_challenge_data[6], card_challenge_data[7],
                             receivedCmd[1], receivedCmd[2], receivedCmd[3], receivedCmd[4],
                             receivedCmd[5], receivedCmd[6], receivedCmd[7], receivedCmd[8]
                            );

                    goto send;
                }
                */

                trace_data = data_generic_trace;
                trace_data_size = 4;
                CodeIso15693AsTag(trace_data, trace_data_size);
                memcpy(data_response, ts->buf, ts->max);
                modulated_response = data_response;
                modulated_response_size = ts->max;

                if (using_kc)
                    kc_attempt++;

            } else {
                // Not fullsim, we don't respond
                chip_state = HALTED;

                if (simulationMode == ICLASS_SIM_MODE_EXIT_AFTER_MAC) {

                    if (g_dbglevel ==  DBG_EXTENDED) {
                        Dbprintf("CSN: %02x %02x %02x %02x %02x %02x %02x %02x", csn[0], csn[1], csn[2], csn[3], csn[4], csn[5], csn[6], csn[7]);
                        Dbprintf("RDR:  (len=%02d): %02x %02x %02x %02x %02x %02x %02x %02x %02x", len,
                                 receivedCmd[0], receivedCmd[1], receivedCmd[2],
                                 receivedCmd[3], receivedCmd[4], receivedCmd[5],
                                 receivedCmd[6], receivedCmd[7], receivedCmd[8]);
                    } else {
                        Dbprintf("CSN: %02x .... %02x OK", csn[0], csn[7]);
                    }
                    if (reader_mac_buf != NULL) {
                        // save NR and MAC for sim 2,4
                        memcpy(reader_mac_buf + 8, receivedCmd + 1, 8);
                    }
                    exit_loop = true;
                }
            }
            goto send;

        } else if (cmd == ICLASS_CMD_HALT && options == 0 && len == 1) {

            if (chip_state != SELECTED) {
                goto send;
            }
            // Reader ends the session
            modulated_response = resp_sof;
            modulated_response_size = resp_sof_len;
            chip_state = HALTED;
            goto send;

        } else if ((simulationMode == ICLASS_SIM_MODE_FULL || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH_KEY) && cmd == ICLASS_CMD_READ4 && len == 4) { // 0x06

            if (chip_state != SELECTED) {
                goto send;
            }
            //Read block
            memcpy(data_generic_trace, emulator + (current_page * page_size) + (block * 8), 32);
            AddCrc(data_generic_trace, 32);
            trace_data = data_generic_trace;
            trace_data_size = 34;
            CodeIso15693AsTag(trace_data, trace_data_size);
            memcpy(data_response, ts->buf, ts->max);
            modulated_response = data_response;
            modulated_response_size = ts->max;
            goto send;

        } else if (cmd == ICLASS_CMD_UPDATE  && (len == 12 || len == 14)) {

            // We're expected to respond with the data+crc, exactly what's already in the receivedCmd
            // receivedCmd is now UPDATE 1b | ADDRESS 1b | DATA 8b | Signature 4b or CRC 2b
            if (chip_state != SELECTED) {
                goto send;
            }
            // is chip in ReadOnly (RO)
            if ((block_wr_lock & 0x80) == 0) goto send;

            if (block == 12 && (block_wr_lock & 0x40) == 0) goto send;
            if (block == 11 && (block_wr_lock & 0x20) == 0) goto send;
            if (block == 10 && (block_wr_lock & 0x10) == 0) goto send;
            if (block ==  9 && (block_wr_lock & 0x08) == 0) goto send;
            if (block ==  8 && (block_wr_lock & 0x04) == 0) goto send;
            if (block ==  7 && (block_wr_lock & 0x02) == 0) goto send;
            if (block ==  6 && (block_wr_lock & 0x01) == 0) goto send;

            if (block == 2) { // update e-purse
                memcpy(card_challenge_data, receivedCmd + 2, 8);
                CodeIso15693AsTag(card_challenge_data, sizeof(card_challenge_data));
                memcpy(resp_cc, ts->buf, ts->max);
                resp_cc_len = ts->max;
                cipher_state_KD[current_page] = opt_doTagMAC_1(card_challenge_data, diversified_kd);
                cipher_state_KC[current_page] = opt_doTagMAC_1(card_challenge_data, diversified_kc);
                if (simulationMode == ICLASS_SIM_MODE_FULL || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH_KEY) {
                    memcpy(emulator + (current_page * page_size) + (8 * 2), card_challenge_data, 8);
                }
            } else if (block == 3) { // update Kd
                for (int i = 0; i < 8; i++) {
                    if (personalization_mode || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH_KEY) {
                        diversified_kd[i] = receivedCmd[2 + i];
                    } else {
                        diversified_kd[i] ^= receivedCmd[2 + i];
                    }
                }
                cipher_state_KD[current_page] = opt_doTagMAC_1(card_challenge_data, diversified_kd);
                if (simulationMode == ICLASS_SIM_MODE_FULL || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH_KEY) {
                    memcpy(emulator + (current_page * page_size) + (8 * 3), diversified_kd, 8);
                    if (simulationMode == ICLASS_SIM_MODE_FULL_GLITCH_KEY) {
                        glitch_key_read = true;
                        goto send;
                    }
                }
            } else if (block == 4) { // update Kc
                for (int i = 0; i < 8; i++) {
                    if (personalization_mode) {
                        diversified_kc[i] = receivedCmd[2 + i];
                    } else {
                        diversified_kc[i] ^= receivedCmd[2 + i];
                    }
                }
                cipher_state_KC[current_page] = opt_doTagMAC_1(card_challenge_data, diversified_kc);
                if (simulationMode == ICLASS_SIM_MODE_FULL || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH_KEY) {
                    memcpy(emulator + (current_page * page_size) + (8 * 4), diversified_kc, 8);
                }
            } else if (simulationMode == ICLASS_SIM_MODE_FULL || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH_KEY) {
                // update emulator memory
                memcpy(emulator + (current_page * page_size) + (8 * block), receivedCmd + 2, 8);
            }

            if (simulationMode == ICLASS_SIM_MODE_FULL_GLITCH) {
                //Jam the read based on the last SIO block
                uint8_t *sr_or_sio = emulator + (current_page * page_size) + (6 * 8);
                if (memcmp(emulator + (current_page * page_size) + (5 * 8), ff_data, PICOPASS_BLOCK_SIZE) == 0) { //SR card
                    if (block == 16) { //SR cards use a standard legth SIO
                        //update block 6 byte 1 from 03 to A3
                        sr_or_sio[0] |= 0xA0;
                        goto send;
                    }
                } else { //For SE cards we have to account for different SIO lengths depending if a standard or custom key is used
                    if (block == (5 + ((sr_or_sio[1] + 12) / 8))) {
                        goto send;
                    }
                }
            }

            memcpy(data_generic_trace, receivedCmd + 2, 8);
            AddCrc(data_generic_trace, 8);
            trace_data = data_generic_trace;
            trace_data_size = 10;
            CodeIso15693AsTag(trace_data, trace_data_size);
            memcpy(data_response, ts->buf, ts->max);
            modulated_response = data_response;
            modulated_response_size = ts->max;
            goto send;

        } else if (cmd == ICLASS_CMD_PAGESEL && len == 4) {  // 0x84
            // Pagesel,
            //  - enables to select a page in the selected chip memory and return its configuration block
            // Chips with a single page will not answer to this command
            // Otherwise, we should answer 8bytes (conf block 1) + 2bytes CRC
            if (chip_state != SELECTED) {
                goto send;
            }

            if ((simulationMode == ICLASS_SIM_MODE_FULL || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH || simulationMode == ICLASS_SIM_MODE_FULL_GLITCH_KEY) && max_page > 0) {

                // if on 2k,  always ignore 3msb,  & 0x1F)
                uint8_t page = receivedCmd[1] & 0x1F;
                if (page > max_page) {
                    goto send;
                }

                current_page = page;

                memcpy(data_generic_trace, emulator + (current_page * page_size) + (8 * 1), 8);
                memcpy(diversified_kd, emulator + (current_page * page_size) + (8 * 3), 8);
                memcpy(diversified_kc, emulator + (current_page * page_size) + (8 * 4), 8);

                cipher_state = &cipher_state_KD[current_page];

                personalization_mode = data_generic_trace[7] & 0x80;
                block_wr_lock = data_generic_trace[3];

                AddCrc(data_generic_trace, 8);

                trace_data = data_generic_trace;
                trace_data_size = 10;

                CodeIso15693AsTag(trace_data, trace_data_size);
                memcpy(data_response, ts->buf, ts->max);
                modulated_response = data_response;
                modulated_response_size = ts->max;
            }
            goto send;

        } else if (cmd == ICLASS_CMD_DETECT) { // 0x0F
            // not supported yet, ignore
//        } else if (cmd == 0x26 && len == 5) {
            // standard ISO15693 INVENTORY command. Ignore.
        } else {
            // Never seen this command before
            if (g_dbglevel >= DBG_EXTENDED)
                print_result("Unhandled command received ", receivedCmd, len);
        }

send:
        /**
        A legit tag has about 330us delay between reader EOT and tag SOF.
        **/
        if (modulated_response_size > 0) {
            uint32_t response_time = reader_eof_time + DELAY_ICLASS_VCD_TO_VICC_SIM;
            TransmitTo15693Reader(modulated_response, modulated_response_size, &response_time, 0, false);
            LogTrace_ISO15693(trace_data, trace_data_size, response_time * 32, (response_time * 32) + (modulated_response_size * 32 * 64), NULL, false);
        }

        if (chip_state == HALTED) {
            uint32_t wait_time = GetCountSspClk() + ICLASS_READER_TIMEOUT_ACTALL;
            while (GetCountSspClk() < wait_time) {};
        }

        // CC attack
        // wait to trigger the reader bug, then wait 1000ms
        if (kc_attempt > 3) {
            uint32_t wait_time = GetCountSspClk() + (16000 * 100);
            while (GetCountSspClk() < wait_time) {};
            kc_attempt = 0;
            exit_loop = true;
        }
    }

    LEDsoff();

    if (button_pressed)
        DbpString("button pressed");

    return button_pressed;
}

int do_iclass_simulation_nonsec(void) {
    // free eventually allocated BigBuf memory
    BigBuf_free_keep_EM();

    uint16_t page_size = 32 * 8;
    uint8_t current_page = 0;

    uint8_t *emulator = BigBuf_get_EM_addr();
    uint8_t *csn = emulator;

    // CSN followed by two CRC bytes
    uint8_t anticoll_data[10] = { 0 };
    uint8_t csn_data[10] = { 0 };
    memcpy(csn_data, csn, sizeof(csn_data));

    // Construct anticollision-CSN
    rotateCSN(csn_data, anticoll_data);

    // Compute CRC on both CSNs
    AddCrc(anticoll_data, 8);
    AddCrc(csn_data, 8);

    // configuration block
    uint8_t conf_block[10] = {0x12, 0xFF, 0xFF, 0xFF, 0x7F, 0x1F, 0xFF, 0x3C, 0x00, 0x00};

    // AIA
    uint8_t aia_data[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};

    memcpy(conf_block, emulator + (8 * 1), 8);
    memcpy(aia_data, emulator + (8 * 2), 8);

    AddCrc(conf_block, 8);
    AddCrc(aia_data, 8);

    if ((conf_block[5] & 0x80) == 0x80) {
        page_size = 256 * 8;
    }

    // chip memory may be divided in 8 pages
    uint8_t max_page = ((conf_block[4] & 0x10) == 0x10) ? 0 : 7;

    // Anti-collision process:
    // Reader 0a
    // Tag    0f
    // Reader 0c
    // Tag    anticoll. CSN
    // Reader 81 anticoll. CSN
    // Tag    CSN

    uint8_t *modulated_response = NULL;
    int modulated_response_size = 0;
    uint8_t *trace_data = NULL;
    int trace_data_size = 0;

    // Respond SOF -- takes 1 bytes
    uint8_t resp_sof[2] = { 0 };
    int resp_sof_len;

    // Anticollision CSN (rotated CSN)
    // 22: Takes 2 bytes for SOF/EOF and 10 * 2 = 20 bytes (2 bytes/byte)
    uint8_t *resp_anticoll = BigBuf_calloc(28);
    int resp_anticoll_len;

    // CSN
    // 22: Takes 2 bytes for SOF/EOF and 10 * 2 = 20 bytes (2 bytes/byte)
    uint8_t *resp_csn = BigBuf_calloc(28);
    int resp_csn_len;

    // configuration (blk 1) PICOPASS 2ks
    uint8_t *resp_conf = BigBuf_calloc(28);
    int resp_conf_len;

    // Application Issuer Area  (blk 5)
    uint8_t *resp_aia = BigBuf_calloc(28);
    int resp_aia_len;

    // receive command
    uint8_t *receivedCmd = BigBuf_calloc(MAX_FRAME_SIZE);

    // Prepare card messages
    tosend_t *ts = get_tosend();
    ts->max = 0;

    // First card answer: SOF
    CodeIClassTagSOF();
    memcpy(resp_sof, ts->buf, ts->max);
    resp_sof_len = ts->max;

    // Anticollision CSN
    CodeIso15693AsTag(anticoll_data, sizeof(anticoll_data));
    memcpy(resp_anticoll, ts->buf, ts->max);
    resp_anticoll_len = ts->max;

    // CSN (block 0)
    CodeIso15693AsTag(csn_data, sizeof(csn_data));
    memcpy(resp_csn, ts->buf, ts->max);
    resp_csn_len = ts->max;

    // Configuration (block 1)
    CodeIso15693AsTag(conf_block, sizeof(conf_block));
    memcpy(resp_conf, ts->buf, ts->max);
    resp_conf_len = ts->max;

    // Application Issuer Area (block 2)
    CodeIso15693AsTag(aia_data, sizeof(aia_data));
    memcpy(resp_aia, ts->buf, ts->max);
    resp_aia_len = ts->max;

    //This is used for responding to READ-block commands or other data which is dynamically generated
    //First the 'trace'-data, not encoded for FPGA
    uint8_t *data_generic_trace = BigBuf_calloc(32 + 2); // 32 bytes data + 2byte CRC is max tag answer

    //Then storage for the modulated data
    //Each bit is doubled when modulated for FPGA, and we also have SOF and EOF (2 bytes)
    uint8_t *data_response = BigBuf_calloc((32 + 2) * 2 + 2);

    enum { IDLE, ACTIVATED, SELECTED, HALTED } chip_state = IDLE;

    bool button_pressed = false;
    uint8_t cmd, options, block;
    int len;

    bool exit_loop = false;
    while (exit_loop == false) {
        WDT_HIT();

        uint32_t reader_eof_time = 0;
        len = GetIso15693CommandFromReader(receivedCmd, MAX_FRAME_SIZE, &reader_eof_time);
        if (len < 0) {
            button_pressed = true;
            exit_loop = true;
            continue;
        }

        // Now look at the reader command and provide appropriate responses
        // default is no response:
        modulated_response = NULL;
        modulated_response_size = 0;
        trace_data = NULL;
        trace_data_size = 0;

        // extra response data
        cmd = receivedCmd[0] & 0xF;
        options = (receivedCmd[0] >> 4) & 0xFF;
        block = receivedCmd[1];

        if (cmd == ICLASS_CMD_ACTALL && len == 1) {   // 0x0A
            // Reader in anti collision phase
            if (chip_state != HALTED) {
                modulated_response = resp_sof;
                modulated_response_size = resp_sof_len;
                chip_state = ACTIVATED;
            }
            goto send;

        } else if (cmd == ICLASS_CMD_READ_OR_IDENTIFY && len == 1) { // 0x0C
            // Reader asks for anti collision CSN
            if (chip_state == SELECTED || chip_state == ACTIVATED) {
                modulated_response = resp_anticoll;
                modulated_response_size = resp_anticoll_len;
                trace_data = anticoll_data;
                trace_data_size = sizeof(anticoll_data);
            }
            goto send;

        } else if (cmd == ICLASS_CMD_SELECT && len == 9) {
            // Reader selects anticollision CSN.
            // Tag sends the corresponding real CSN
            if (chip_state == ACTIVATED || chip_state == SELECTED) {
                if (!memcmp(receivedCmd + 1, anticoll_data, 8)) {
                    modulated_response = resp_csn;
                    modulated_response_size = resp_csn_len;
                    trace_data = csn_data;
                    trace_data_size = sizeof(csn_data);
                    chip_state = SELECTED;
                } else {
                    chip_state = IDLE;
                }
            } else if (chip_state == HALTED) {
                // RESELECT with CSN
                if (!memcmp(receivedCmd + 1, csn_data, 8)) {
                    modulated_response = resp_csn;
                    modulated_response_size = resp_csn_len;
                    trace_data = csn_data;
                    trace_data_size = sizeof(csn_data);
                    chip_state = SELECTED;
                }
            }
            goto send;


        } else if (cmd == ICLASS_CMD_READ_OR_IDENTIFY && len == 4) { // 0x0C

            if (chip_state != SELECTED) {
                goto send;
            }

            switch (block) {
                case 0: { // csn (0c 00)
                    modulated_response = resp_csn;
                    modulated_response_size = resp_csn_len;
                    trace_data = csn_data;
                    trace_data_size = sizeof(csn_data);
                    goto send;
                }
                case 1: { // configuration (0c 01)
                    modulated_response = resp_conf;
                    modulated_response_size = resp_conf_len;
                    trace_data = conf_block;
                    trace_data_size = sizeof(conf_block);
                    goto send;
                }
                case 2: { // Application Issuer Area (0c 02)
                    modulated_response = resp_aia;
                    modulated_response_size = resp_aia_len;
                    trace_data = aia_data;
                    trace_data_size = sizeof(aia_data);
                    goto send;
                }
                default : {
                    memcpy(data_generic_trace, emulator + (block << 3), 8);
                    AddCrc(data_generic_trace, 8);
                    trace_data = data_generic_trace;
                    trace_data_size = 10;
                    CodeIso15693AsTag(trace_data, trace_data_size);
                    memcpy(data_response, ts->buf, ts->max);
                    modulated_response = data_response;
                    modulated_response_size = ts->max;
                    goto send;
                }
            } // swith

        } else if (cmd == ICLASS_CMD_READCHECK) {                 // 0x88
            goto send;

        } else if (cmd == ICLASS_CMD_CHECK && len == 9) {         // 0x05
            goto send;

        } else if (cmd == ICLASS_CMD_HALT && options == 0 && len == 1) {

            if (chip_state != SELECTED) {
                goto send;
            }
            // Reader ends the session
            modulated_response = resp_sof;
            modulated_response_size = resp_sof_len;
            chip_state = HALTED;
            goto send;

        } else if (cmd == ICLASS_CMD_READ4 && len == 4) {         // 0x06

            if (chip_state != SELECTED) {
                goto send;
            }
            //Read block
            memcpy(data_generic_trace, emulator + (current_page * page_size) + (block * 8), 8 * 4);
            AddCrc(data_generic_trace, 8 * 4);
            trace_data = data_generic_trace;
            trace_data_size = 34;
            CodeIso15693AsTag(trace_data, trace_data_size);
            memcpy(data_response, ts->buf, ts->max);
            modulated_response = data_response;
            modulated_response_size = ts->max;
            goto send;

        } else if (cmd == ICLASS_CMD_UPDATE  && (len == 12 || len == 14)) {

            // We're expected to respond with the data+crc, exactly what's already in the receivedCmd
            // receivedCmd is now UPDATE 1b | ADDRESS 1b | DATA 8b | Signature 4b or CRC 2b
            if (chip_state != SELECTED) {
                goto send;
            }

            // update emulator memory
            memcpy(emulator + (current_page * page_size) + (8 * block), receivedCmd + 2, 8);

            memcpy(data_generic_trace, receivedCmd + 2, 8);
            AddCrc(data_generic_trace, 8);
            trace_data = data_generic_trace;
            trace_data_size = 10;
            CodeIso15693AsTag(trace_data, trace_data_size);
            memcpy(data_response, ts->buf, ts->max);
            modulated_response = data_response;
            modulated_response_size = ts->max;
            goto send;

        } else if (cmd == ICLASS_CMD_PAGESEL && len == 4) {  // 0x84
            // Pagesel,
            //  - enables to select a page in the selected chip memory and return its configuration block
            // Chips with a single page will not answer to this command
            // Otherwise, we should answer 8bytes (conf block 1) + 2bytes CRC
            if (chip_state != SELECTED) {
                goto send;
            }

            if (max_page > 0) {

                current_page = receivedCmd[1];

                memcpy(data_generic_trace, emulator + (current_page * page_size) + (8 * 1), 8);
                AddCrc(data_generic_trace, 8);
                trace_data = data_generic_trace;
                trace_data_size = 10;

                CodeIso15693AsTag(trace_data, trace_data_size);
                memcpy(data_response, ts->buf, ts->max);
                modulated_response = data_response;
                modulated_response_size = ts->max;
            }
            goto send;

//            } else if(cmd == ICLASS_CMD_DETECT) {  // 0x0F
//        } else if (cmd == 0x26 && len == 5) {
            // standard ISO15693 INVENTORY command. Ignore.
        } else {
            // Never seen this command before
            if (g_dbglevel >= DBG_EXTENDED)
                print_result("Unhandled command received ", receivedCmd, len);
        }

send:
        /**
        A legit tag has about 330us delay between reader EOT and tag SOF.
        **/
        if (modulated_response_size > 0) {
            uint32_t response_time = reader_eof_time + DELAY_ICLASS_VCD_TO_VICC_SIM;
            TransmitTo15693Reader(modulated_response, modulated_response_size, &response_time, 0, false);
            LogTrace_ISO15693(trace_data, trace_data_size, response_time * 32, (response_time * 32) + (modulated_response_size * 32 * 64), NULL, false);
        }
    }

    LEDsoff();

    if (button_pressed)
        DbpString("button pressed");

    return button_pressed;

}

// THE READER CODE
void iclass_send_as_reader(uint8_t *frame, int len, uint32_t *start_time, uint32_t *end_time, bool shallow_mod) {
    CodeIso15693AsReader(frame, len);
    tosend_t *ts = get_tosend();
    TransmitTo15693Tag(ts->buf, ts->max, start_time, shallow_mod);
    *end_time = *start_time + (32 * ((8 * ts->max) - 4)); // subtract the 4 padding bits after EOF
    LogTrace_ISO15693(frame, len, (*start_time * 4), (*end_time * 4), NULL, true);
}

static bool iclass_send_cmd_with_retries(uint8_t *cmd, size_t cmdsize, uint8_t *resp, size_t max_resp_size,
                                         uint8_t expected_size, uint8_t tries, uint32_t *start_time,
                                         uint16_t timeout, uint32_t *eof_time, bool shallow_mod) {

    uint16_t resp_len = 0;
    while (tries-- > 0) {

        iclass_send_as_reader(cmd, cmdsize, start_time, eof_time, shallow_mod);
        if (resp == NULL) {
            return true;
        }

        int res = GetIso15693AnswerFromTag(resp, max_resp_size, timeout, eof_time, false, true, &resp_len);
        if (res == PM3_SUCCESS && expected_size == resp_len) {
            return true;
        }

        // Timed out waiting for the tag to reply, but perhaps the tag did hear the command and is attempting to reply
        // So wait long enough for the tag to encode it's reply plus required frame delays on each side before retrying
        // And then double it, because in practice it seems to make it much more likely to succeed
        // Response time calculation from expected_size lifted from GetIso15693AnswerFromTag
        *start_time = *eof_time + ((DELAY_ICLASS_VICC_TO_VCD_READER + DELAY_ISO15693_VCD_TO_VICC_READER + (expected_size * 8 * 8 * 16)) * 2);
    }
    return false;
}

/**
 * @brief Talks to an iclass tag, sends the commands to get CSN and CC.
 * @param card_data where the CSN, CONFIG, CC are stored for return
 *        8 bytes csn + 8 bytes config + 8 bytes CC
 * @return false = fail
 *         true = Got all.
 */
static bool select_iclass_tag_ex(picopass_hdr_t *hdr, bool use_credit_key, uint32_t *eof_time, uint8_t *status, bool shallow_mod) {

    static uint8_t act_all[] = { ICLASS_CMD_ACTALL };
    static uint8_t identify[] = { ICLASS_CMD_READ_OR_IDENTIFY, 0x00, 0x73, 0x33 };
    static uint8_t read_conf[] = { ICLASS_CMD_READ_OR_IDENTIFY, 0x01, 0xfa, 0x22 };
    uint8_t select[] = { 0x80 | ICLASS_CMD_SELECT, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t read_aia[] = { ICLASS_CMD_READ_OR_IDENTIFY, 0x05, 0xde, 0x64};
    uint8_t read_check_cc[] = { 0x80 | ICLASS_CMD_READCHECK, 0x02 };
    uint8_t resp[ICLASS_BUFFER_SIZE] = {0};

    // Bit 4: K.If this bit equals to one, the READCHECK will use the Credit Key (Kc); if equals to zero, Debit Key (Kd) will be used
    // bit 7: parity.
    if (use_credit_key)
        read_check_cc[0] = 0x10 | ICLASS_CMD_READCHECK;

    // wakeup
    uint32_t start_time = GetCountSspClk();
    iclass_send_as_reader(act_all, 1, &start_time, eof_time, shallow_mod);
    int res;
    uint16_t resp_len = 0;
    res = GetIso15693AnswerFromTag(resp, sizeof(resp), ICLASS_READER_TIMEOUT_ACTALL, eof_time, false, true, &resp_len);
    if (res != PM3_SUCCESS)
        return false;

    // send Identify
    start_time = *eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
    iclass_send_as_reader(identify, 1, &start_time, eof_time, shallow_mod);

    // expect a 10-byte response here, 8 byte anticollision-CSN and 2 byte CRC
    res = GetIso15693AnswerFromTag(resp, sizeof(resp), ICLASS_READER_TIMEOUT_OTHERS, eof_time, false, true, &resp_len);
    if (res != PM3_SUCCESS || resp_len != 10)
        return false;

    // copy the Anti-collision CSN to our select-packet
    memcpy(&select[1], resp, 8);

    // select the card
    start_time = *eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
    iclass_send_as_reader(select, sizeof(select), &start_time, eof_time, shallow_mod);

    // expect a 10-byte response here, 8 byte CSN and 2 byte CRC
    res = GetIso15693AnswerFromTag(resp, sizeof(resp), ICLASS_READER_TIMEOUT_OTHERS, eof_time, false, true, &resp_len);
    if (res != PM3_SUCCESS || resp_len != 10)
        return false;

    // save CSN
    memcpy(hdr->csn, resp, sizeof(hdr->csn));

    // card selected, now read config (block1) (only 8 bytes no CRC)
    start_time = *eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
    iclass_send_as_reader(read_conf, sizeof(read_conf), &start_time, eof_time, shallow_mod);

    // expect a 8-byte response here
    res = GetIso15693AnswerFromTag(resp, sizeof(resp), ICLASS_READER_TIMEOUT_OTHERS, eof_time, false, true, &resp_len);
    if (res != PM3_SUCCESS || resp_len != 10)
        return false;

    // save CONF
    memcpy((uint8_t *)&hdr->conf, resp, sizeof(hdr->conf));

    if (status)
        *status |= (FLAG_ICLASS_CSN | FLAG_ICLASS_CONF);

    uint8_t pagemap = get_pagemap(hdr);
    if (pagemap != PICOPASS_NON_SECURE_PAGEMODE) {

        // read App Issuer Area block 5
        start_time = *eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
        iclass_send_as_reader(read_aia, sizeof(read_aia), &start_time, eof_time, shallow_mod);

        // expect a 10-byte response here
        res = GetIso15693AnswerFromTag(resp, sizeof(resp), ICLASS_READER_TIMEOUT_OTHERS, eof_time, false, true, &resp_len);
        if (res != PM3_SUCCESS || resp_len != 10)
            return false;

        if (status) {
            *status |= FLAG_ICLASS_AIA;
            memcpy(hdr->app_issuer_area, resp, sizeof(hdr->app_issuer_area));
        }

        // card selected, now read e-purse (cc) (block2) (only 8 bytes no CRC)
        start_time = *eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
        iclass_send_as_reader(read_check_cc, sizeof(read_check_cc), &start_time, eof_time, shallow_mod);

        // expect a 8-byte response here
        res = GetIso15693AnswerFromTag(resp, sizeof(resp), ICLASS_READER_TIMEOUT_OTHERS, eof_time, false, true, &resp_len);
        if (res != PM3_SUCCESS || resp_len != 8)
            return false;

        memcpy(hdr->epurse, resp, sizeof(hdr->epurse));

        if (status)
            *status |= FLAG_ICLASS_CC;

    }  else {

        // on NON_SECURE_PAGEMODE cards, AIA is on block2..

        // read App Issuer Area block 2
        read_aia[1] = 0x02;
        read_aia[2] = 0x61;
        read_aia[3] = 0x10;

        start_time = *eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
        iclass_send_as_reader(read_aia, sizeof(read_aia), &start_time, eof_time, shallow_mod);

        // expect a 10-byte response here
        res = GetIso15693AnswerFromTag(resp, sizeof(resp), ICLASS_READER_TIMEOUT_OTHERS, eof_time, false, true, &resp_len);
        if (res != PM3_SUCCESS || resp_len != 10)
            return false;

        if (status) {
            *status |= FLAG_ICLASS_AIA;
            memcpy(hdr->epurse, resp, sizeof(hdr->epurse));
        }
    }

    return true;
}

bool select_iclass_tag(picopass_hdr_t *hdr, bool use_credit_key, uint32_t *eof_time, bool shallow_mod) {
    uint8_t result = 0;
    return select_iclass_tag_ex(hdr, use_credit_key, eof_time, &result, shallow_mod);
}

// Reader iClass Anticollission
// turn off afterwards
void ReaderIClass(uint8_t flags) {

    // flag to use credit key
    bool use_credit_key = ((flags & FLAG_ICLASS_READER_CREDITKEY) == FLAG_ICLASS_READER_CREDITKEY);
    bool shallow_mod = (flags & FLAG_ICLASS_READER_SHALLOW_MOD);

    if ((flags & FLAG_ICLASS_READER_INIT) == FLAG_ICLASS_READER_INIT) {
        Iso15693InitReader();
    }

    if ((flags & FLAG_ICLASS_READER_CLEARTRACE) == FLAG_ICLASS_READER_CLEARTRACE) {
        clear_trace();
    }


    uint8_t res = 0;
    uint32_t eof_time = 0;
    picopass_hdr_t hdr = {0};

    if (select_iclass_tag_ex(&hdr, use_credit_key, &eof_time, &res, shallow_mod) == false) {
        reply_ng(CMD_HF_ICLASS_READER, PM3_ERFTRANS, NULL, 0);
        goto out;
    }

    // Page mapping for secure mode
    // 0 : CSN
    // 1 : Configuration
    // 2 : e-purse
    // 3 : kd / debit / aa2 (write-only)
    // 4 : kc / credit / aa1 (write-only)
    // 5 : AIA, Application issuer area
    //
    // Page mapping for non secure mode
    // 0 : CSN
    // 1 : Configuration
    // 2 : AIA, Application issuer area

    // Return to client, e 6 * 8 bytes of data.
    // with 0xFF:s in block 3 and 4.

    iclass_card_select_resp_t payload = {
        .status = res
    };
    memcpy(&payload.header.hdr, &hdr, sizeof(picopass_hdr_t));

    reply_ng(CMD_HF_ICLASS_READER, PM3_SUCCESS, (uint8_t *)&payload, sizeof(iclass_card_select_resp_t));

out:
    switch_off();
}

bool authenticate_iclass_tag(iclass_auth_req_t *payload, picopass_hdr_t *hdr, uint32_t *start_time, uint32_t *eof_time, uint8_t *mac_out) {

    uint8_t cmd_check[9] = { ICLASS_CMD_CHECK };
    uint8_t mac[4] = {0};
    uint8_t resp_auth[4] = {0};
    uint8_t ccnr[12] = {0};

    uint8_t *pmac = mac;
    if (mac_out)
        pmac = mac_out;

    memcpy(ccnr, hdr->epurse, sizeof(hdr->epurse));

    if (payload->use_replay) {

        memcpy(pmac, payload->key + 4, 4);
        memcpy(cmd_check + 1, payload->key, 8);

    } else {

        uint8_t div_key[8] = {0};
        if (payload->use_raw)
            memcpy(div_key, payload->key, 8);
        else
            iclass_calc_div_key(hdr->csn, payload->key, div_key, payload->use_elite);

        if (payload->use_credit_key)
            memcpy(hdr->key_c, div_key, sizeof(hdr->key_c));
        else
            memcpy(hdr->key_d, div_key, sizeof(hdr->key_d));

        opt_doReaderMAC(ccnr, div_key, pmac);

        // copy MAC to check command (readersignature)
        cmd_check[5] = pmac[0];
        cmd_check[6] = pmac[1];
        cmd_check[7] = pmac[2];
        cmd_check[8] = pmac[3];
    }
    return iclass_send_cmd_with_retries(cmd_check, sizeof(cmd_check), resp_auth, sizeof(resp_auth), 4, 2, start_time, ICLASS_READER_TIMEOUT_OTHERS, eof_time, payload->shallow_mod);
}


/* this function works on the following assumptions.
* - one select first, to get CSN / CC (e-purse)
* - calculate before diversified keys and precalc mac based on CSN/KEY.
* - data in contains of diversified keys, mac
* - key loop only test one type of authtication key. Ie two calls needed
*   to cover debit and credit key. (AA1/AA2)
*/
void iClass_Authentication_fast(iclass_chk_t *p) {
    // sanitation
    if (p == NULL) {
        reply_ng(CMD_HF_ICLASS_CHKKEYS, PM3_ESOFT, NULL, 0);
        return;
    }

    bool shallow_mod = p->shallow_mod;

    uint8_t check[9] = { ICLASS_CMD_CHECK };
    uint8_t resp[ICLASS_BUFFER_SIZE] = {0};
    uint8_t readcheck_cc[] = { 0x80 | ICLASS_CMD_READCHECK, 0x02 };

    if (p->use_credit_key)
        readcheck_cc[0] = 0x10 | ICLASS_CMD_READCHECK;

    // select card / e-purse
    picopass_hdr_t hdr = {0};
    iclass_premac_t *keys = p->items;

    LED_A_ON();

    // fresh start
    switch_off();
    SpinDelay(20);
    Iso15693InitReader();

    bool isOK = false;

    uint32_t start_time = 0, eof_time = 0;
    if (select_iclass_tag(&hdr, p->use_credit_key, &eof_time, shallow_mod) == false)
        goto out;

    start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;

    // since select_iclass_tag call sends s readcheck,  we start with sending first response.
    uint16_t checked = 0;

    // Keychunk loop
    uint8_t i = 0;
    for (i = 0; i < p->count; i++) {

        // Allow button press / usb cmd to interrupt device
        if (checked == 1000) {
            if (BUTTON_PRESS() || data_available()) goto out;
            checked = 0;
        }
        ++checked;

        WDT_HIT();
        LED_B_ON();

        // copy MAC to check command (readersignature)
        check[5] = keys[i].mac[0];
        check[6] = keys[i].mac[1];
        check[7] = keys[i].mac[2];
        check[8] = keys[i].mac[3];

        // expect 4bytes, 3 retries times..
        isOK = iclass_send_cmd_with_retries(check, sizeof(check), resp, sizeof(resp), 4, 2, &start_time, ICLASS_READER_TIMEOUT_OTHERS, &eof_time, shallow_mod);
        if (isOK)
            goto out;

        start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
        // Auth Sequence MUST begin with reading e-purse. (block2)
        // Card selected, now read e-purse (cc) (block2) (only 8 bytes no CRC)
        iclass_send_as_reader(readcheck_cc, sizeof(readcheck_cc), &start_time, &eof_time, shallow_mod);
        LED_B_OFF();
    }

out:
    // send keyindex.
    reply_ng(CMD_HF_ICLASS_CHKKEYS, (isOK) ? PM3_SUCCESS : PM3_ESOFT, (uint8_t *)&i, sizeof(i));
    switch_off();
}

// Tries to read block.
// retries 3times.
// reply 8 bytes block
bool iclass_read_block(uint16_t blockno, uint8_t *data, uint32_t *start_time, uint32_t *eof_time, bool shallow_mod) {
    uint8_t resp[10];
    uint8_t c[] = {ICLASS_CMD_READ_OR_IDENTIFY, blockno, 0x00, 0x00};
    AddCrc(c + 1, 1);
    bool isOK = iclass_send_cmd_with_retries(c, sizeof(c), resp, sizeof(resp), 10, 2, start_time, ICLASS_READER_TIMEOUT_OTHERS, eof_time, shallow_mod);
    if (isOK) {
        memcpy(data, resp, 8);
    }
    return isOK;
}

// turn off afterwards
// send in authentication needed data,  if to use auth.
// reply 8 bytes block if send_reply  (for client)
void iClass_ReadBlock(uint8_t *msg) {

    iclass_auth_req_t *payload = (iclass_auth_req_t *)msg;
    bool shallow_mod = payload->shallow_mod;

    iclass_readblock_resp_t response = { .isOK = true };
    memset(response.data, 0, sizeof(response.data));

    uint8_t cmd_read[] = {ICLASS_CMD_READ_OR_IDENTIFY, payload->blockno, 0x00, 0x00};
    AddCrc(cmd_read + 1, 1);

    Iso15693InitReader();

    // select tag.
    uint32_t eof_time = 0;
    picopass_hdr_t hdr = {0};
    bool res = select_iclass_tag(&hdr, payload->use_credit_key, &eof_time, shallow_mod);
    if (res == false) {
        if (payload->send_reply) {
            response.isOK = res;
            reply_ng(CMD_HF_ICLASS_READBL, PM3_ETIMEOUT, (uint8_t *)&response, sizeof(response));
        }
        goto out;
    }

    uint32_t start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;

    // authenticate
    if (payload->do_auth) {

        res = authenticate_iclass_tag(payload, &hdr, &start_time, &eof_time, NULL);
        if (res == false) {
            if (payload->send_reply) {
                response.isOK = res;
                reply_ng(CMD_HF_ICLASS_READBL, PM3_ETIMEOUT, (uint8_t *)&response, sizeof(response));
            }
            goto out;
        }
    }

    start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;

    // read data
    uint8_t resp[10];
    res = iclass_send_cmd_with_retries(cmd_read, sizeof(cmd_read), resp, sizeof(resp), 10, 3, &start_time, ICLASS_READER_TIMEOUT_OTHERS, &eof_time, shallow_mod);
    if (res) {
        memcpy(response.data, resp, sizeof(response.data));
        if (payload->send_reply) {
            reply_ng(CMD_HF_ICLASS_READBL, PM3_SUCCESS, (uint8_t *)&response, sizeof(response));
        }
    } else {
        if (payload->send_reply) {
            response.isOK = res;
            reply_ng(CMD_HF_ICLASS_READBL, PM3_ETIMEOUT, (uint8_t *)&response, sizeof(response));
        }
    }

out:
    switch_off();
}

// Dump command seems to dump a block related portion of card memory.
// I suppose it will need to do an authentatication to AA1,  read its blocks by calling this.
// then authenticate AA2, and read those blocks by calling this.
// By the looks at it only 2K cards is supported,  or first page dumps on larger cards.
// turn off afterwards
void iClass_Dump(uint8_t *msg) {

    BigBuf_free();

    iclass_dump_req_t *cmd = (iclass_dump_req_t *)msg;
    iclass_auth_req_t *req = &cmd->req;
    bool shallow_mod = req->shallow_mod;

    uint8_t *dataout = BigBuf_calloc(ICLASS_16KS_SIZE);
    if (dataout == NULL) {
        DbpString("Failed to allocate memory");
        if (req->send_reply) {
            reply_ng(CMD_HF_ICLASS_DUMP, PM3_EMALLOC, NULL, 0);
        }
        switch_off();
        return;
    }
    memset(dataout, 0xFF, ICLASS_16KS_SIZE);

    Iso15693InitReader();

    // select tag.
    uint32_t eof_time = 0;
    picopass_hdr_t hdr = {0};
    memset(&hdr, 0xff, sizeof(picopass_hdr_t));

    bool res = select_iclass_tag(&hdr, req->use_credit_key, &eof_time, shallow_mod);
    if (res == false) {
        if (req->send_reply) {
            reply_ng(CMD_HF_ICLASS_DUMP, PM3_ETIMEOUT, NULL, 0);
        }
        switch_off();
        return;
    }

    uint32_t start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;

    // authenticate
    if (req->do_auth) {
        res = authenticate_iclass_tag(req, &hdr, &start_time, &eof_time, NULL);
        if (res == false) {
            if (req->send_reply) {
                reply_ng(CMD_HF_ICLASS_DUMP, PM3_ETIMEOUT, NULL, 0);
            }
            switch_off();
            return;
        }
    }

    start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;

    bool dumpsuccess = true;

    // main read loop
    uint16_t i;
    for (i = cmd->start_block; i <= cmd->end_block; i++) {

        uint8_t resp[10];
        uint8_t c[] = {ICLASS_CMD_READ_OR_IDENTIFY, i, 0x00, 0x00};
        AddCrc(c + 1, 1);

        res = iclass_send_cmd_with_retries(c, sizeof(c), resp, sizeof(resp), 10, 3, &start_time, ICLASS_READER_TIMEOUT_OTHERS, &eof_time, shallow_mod);
        if (res) {
            memcpy(dataout + (8 * i), resp, 8);
        } else {
            Dbprintf("failed to read block %u ( 0x%02x)", i, i);
            dumpsuccess = false;
        }
    }

    switch_off();

    // copy diversified key back.
    if (req->do_auth) {
        if (req->use_credit_key)
            memcpy(dataout + (8 * 4), hdr.key_c, 8);
        else
            memcpy(dataout + (8 * 3), hdr.key_d, 8);
    }

    if (req->send_reply) {
        struct p {
            bool isOK;
            uint16_t block_cnt;
            uint32_t bb_offset;
        } PACKED response;

        response.isOK = dumpsuccess;
        response.block_cnt = i - cmd->start_block;
        response.bb_offset = dataout - BigBuf_get_addr();
        reply_ng(CMD_HF_ICLASS_DUMP, PM3_SUCCESS, (uint8_t *)&response, sizeof(response));
    }

    BigBuf_free();
}

static bool iclass_writeblock_ext(uint8_t blockno, uint8_t *data, uint8_t *mac, bool use_mac, bool shallow_mod) {

    // write command: cmd, 1 blockno, 8 data, 4 mac
    uint8_t write[14] = { 0x80 | ICLASS_CMD_UPDATE, blockno };
    uint8_t write_len = 14;
    memcpy(write + 2, data, 8);

    if (use_mac) {
        memcpy(write + 10, mac, 4);
    } else {
        AddCrc(write + 1, 9);
        write_len -= 2;
    }

    uint8_t resp[10] = {0};
    uint32_t eof_time = 0, start_time = 0;
    bool isOK = iclass_send_cmd_with_retries(write, write_len, resp, sizeof(resp), 10, 3, &start_time, ICLASS_READER_TIMEOUT_UPDATE, &eof_time, shallow_mod);
    if (isOK == false) {
        return false;
    }

    if (blockno == 2) {
        // check response. e-purse update swaps first and second half
        if (memcmp(data + 4, resp, 4) || memcmp(data, resp + 4, 4)) {
            return false;
        }
    } else if (blockno == 3 || blockno == 4) {
        // check response. Key updates always return 0xffffffffffffffff
        uint8_t all_ff[PICOPASS_BLOCK_SIZE] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        if (memcmp(all_ff, resp, PICOPASS_BLOCK_SIZE)) {
            return false;
        }
    } else {
        // check response. All other updates return unchanged data
        if (memcmp(data, resp, PICOPASS_BLOCK_SIZE)) {
            return false;
        }
    }

    return true;
}

static bool iclass_writeblock_sp(uint8_t blockno, uint8_t *data, uint8_t *mac, bool shallow_mod, uint32_t *start_time, uint32_t *eof_time, bool short_delay) {

    // write command: cmd, 1 blockno, 8 data, 4 mac
    uint8_t write[14] = { 0x80 | ICLASS_CMD_UPDATE, blockno };
    uint8_t write_len = 14;
    memcpy(write + 2, data, 8);
    memcpy(write + 10, mac, 4);

    uint8_t resp[10] = {0};
    bool isOK = false;
    if (short_delay) {
        isOK = iclass_send_cmd_with_retries(write, write_len, resp, sizeof(resp), 10, 3, start_time, ICLASS_READER_TIMEOUT_UPDATE_FAST, eof_time, shallow_mod);
    } else {
        isOK = iclass_send_cmd_with_retries(write, write_len, resp, sizeof(resp), 10, 3, start_time, ICLASS_READER_TIMEOUT_UPDATE, eof_time, shallow_mod);
    }
    if (isOK == false) {
        return false;
    }

    // check response. All other updates return unchanged data
    if (memcmp(data, resp, PICOPASS_BLOCK_SIZE)) {
        return false;
    }

    return true;
}

// turn off afterwards
void iClass_WriteBlock(uint8_t *msg) {

    LED_A_ON();

    iclass_writeblock_req_t *payload = (iclass_writeblock_req_t *)msg;
    bool shallow_mod = payload->req.shallow_mod;

    uint8_t write[14] = { 0x80 | ICLASS_CMD_UPDATE, payload->req.blockno };
    uint8_t write_len = 14;

    Iso15693InitReader();

    // select tag.
    uint32_t eof_time = 0;
    picopass_hdr_t hdr = {0};
    bool res = select_iclass_tag(&hdr, payload->req.use_credit_key, &eof_time, shallow_mod);
    if (res == false) {
        goto out;
    }

    uint32_t start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;

    uint8_t mac[4] = {0};

    // authenticate
    if (payload->req.do_auth) {

        res = authenticate_iclass_tag(&payload->req, &hdr, &start_time, &eof_time, mac);
        if (res == false) {
            goto out;
        }
    }

    // new block data
    memcpy(write + 2, payload->data, PICOPASS_BLOCK_SIZE);

    uint8_t pagemap = get_pagemap(&hdr);
    if (pagemap == PICOPASS_NON_SECURE_PAGEMODE) {
        // Unsecured tags uses CRC16,  but don't include the UPDATE operation code
        // byte0 = update op
        // byte1 = block no
        // byte2..9 = new block data
        AddCrc(write + 1, 9);
        write_len -= 2;
    } else {

        if (payload->req.use_replay) {
            memcpy(write + 10, payload->mac, sizeof(payload->mac));
        } else {
            // Secure tags uses MAC
            uint8_t wb[9];
            wb[0] = payload->req.blockno;
            memcpy(wb + 1, payload->data, PICOPASS_BLOCK_SIZE);

            if (payload->req.use_credit_key)
                doMAC_N(wb, sizeof(wb), hdr.key_c, mac);
            else
                doMAC_N(wb, sizeof(wb), hdr.key_d, mac);

            memcpy(write + 10, mac, sizeof(mac));
        }
    }

    start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;

    uint8_t resp[10] = {0};

    uint8_t tries = 3;
    while (tries-- > 0) {

        iclass_send_as_reader(write, write_len, &start_time, &eof_time, shallow_mod);

        if (tearoff_hook() == PM3_ETEAROFF) { // tearoff occurred
            res = false;
            switch_off();
            if (payload->req.send_reply) {
                reply_ng(CMD_HF_ICLASS_WRITEBL, PM3_ETEAROFF, (uint8_t *)&res, sizeof(bool));
            }
            return;
        } else {

            uint16_t resp_len = 0;
            int res2 = GetIso15693AnswerFromTag(resp, sizeof(resp), ICLASS_READER_TIMEOUT_UPDATE, &eof_time, false, true, &resp_len);
            if (res2 == PM3_SUCCESS && resp_len == 10) {
                res = true;
                break;
            }
        }
    }

    if (tries == 0) {
        res = false;
        goto out;
    }

    // verify write
    if ((pagemap != PICOPASS_NON_SECURE_PAGEMODE) && (payload->req.blockno == 2)) {
        // check response. e-purse update swaps first and second half
        if (memcmp(payload->data + 4, resp, 4) || memcmp(payload->data, resp + 4, 4)) {
            res = false;
            goto out;
        }
    } else if ((pagemap != PICOPASS_NON_SECURE_PAGEMODE) && (payload->req.blockno == 3 || payload->req.blockno == 4)) {
        // check response. Key updates always return 0xffffffffffffffff
        uint8_t all_ff[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        if (memcmp(all_ff, resp, sizeof(all_ff))) {
            res = false;
            goto out;
        }
    } else {
        // check response. All other updates return unchanged data
        if (memcmp(payload->data, resp, 8)) {
            res = false;
            goto out;
        }
    }

out:
    switch_off();

    if (payload->req.send_reply) {
        reply_ng(CMD_HF_ICLASS_WRITEBL, PM3_SUCCESS, (uint8_t *)&res, sizeof(bool));
    }
}

void iclass_credit_epurse(iclass_credit_epurse_t *payload) {

    LED_A_ON();

    bool shallow_mod = payload->req.shallow_mod;

    Iso15693InitReader();

    // select tag.
    uint32_t eof_time = 0;
    picopass_hdr_t hdr = {0};
    uint8_t res = select_iclass_tag(&hdr, payload->req.use_credit_key, &eof_time, shallow_mod);
    if (res == false) {
        goto out;
    }

    uint32_t start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;

    uint8_t mac[4] = {0};

    // authenticate
    if (payload->req.do_auth) {

        res = authenticate_iclass_tag(&payload->req, &hdr, &start_time, &eof_time, mac);
        if (res == false) {
            goto out;
        }
    }

    start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;

    uint8_t cmd_read[] = {ICLASS_CMD_READ_OR_IDENTIFY, payload->req.blockno, 0x00, 0x00};
    AddCrc(cmd_read + 1, 1);

    uint8_t epurse[10];
    res = iclass_send_cmd_with_retries(cmd_read, sizeof(cmd_read), epurse, sizeof(epurse), 10, 3, &start_time, ICLASS_READER_TIMEOUT_OTHERS, &eof_time, shallow_mod);
    if (!res) {
        switch_off();
        if (payload->req.send_reply) {
            reply_ng(CMD_HF_ICLASS_CREDIT_EPURSE, PM3_ETIMEOUT, (uint8_t *)&res, sizeof(uint8_t));
        }
        return;
    }

    uint8_t write[14] = { 0x80 | ICLASS_CMD_UPDATE, payload->req.blockno };
    uint8_t write_len = 14;

    uint8_t epurse_offset = 0;
    const uint8_t empty_epurse[] = {0xff, 0xff, 0xff, 0xff};
    if (memcmp(epurse, empty_epurse, 4) == 0) {
        // epurse data in stage 2
        epurse_offset = 4;
    }

    memcpy(epurse + epurse_offset, payload->epurse, 4);

    // blank out debiting value as per the first step of the crediting procedure
    epurse[epurse_offset + 0] = 0xFF;
    epurse[epurse_offset + 1] = 0xFF;

    // initial epurse write for credit
    memcpy(write + 2, epurse, 8);

    doMAC_N(write + 1, 9, payload->req.use_credit_key ? hdr.key_c : hdr.key_d, mac);
    memcpy(write + 10, mac, sizeof(mac));

    start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;

    uint8_t resp[10] = {0};

    uint8_t tries = 3;
    while (tries-- > 0) {

        iclass_send_as_reader(write, write_len, &start_time, &eof_time, shallow_mod);

        if (tearoff_hook() == PM3_ETEAROFF) { // tearoff occurred
            res = false;
            switch_off();
            if (payload->req.send_reply)
                reply_ng(CMD_HF_ICLASS_CREDIT_EPURSE, PM3_ETEAROFF, (uint8_t *)&res, sizeof(uint8_t));
            return;
        } else {

            uint16_t resp_len = 0;
            int res2 = GetIso15693AnswerFromTag(resp, sizeof(resp), ICLASS_READER_TIMEOUT_UPDATE, &eof_time, false, true, &resp_len);
            if (res2 == PM3_SUCCESS && resp_len == 10) {
                res = true;
                break;
            }
        }
    }

    if (tries == 0) {
        res = false;
        goto out;
    }

    // check response. e-purse update swaps first and second half
    if (memcmp(write + 2 + 4, resp, 4) || memcmp(write + 2, resp + 4, 4)) {
        res = false;
        goto out;
    }

    // new epurse write
    // epurse offset is now flipped after the first write
    epurse_offset ^= 4;
    memcpy(resp + epurse_offset, payload->epurse, 4);
    memcpy(write + 2, resp, 8);

    doMAC_N(write + 1, 9, payload->req.use_credit_key ? hdr.key_c : hdr.key_d, mac);
    memcpy(write + 10, mac, sizeof(mac));

    start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;

    tries = 3;
    while (tries-- > 0) {

        iclass_send_as_reader(write, write_len, &start_time, &eof_time, shallow_mod);

        if (tearoff_hook() == PM3_ETEAROFF) { // tearoff occurred
            res = false;
            switch_off();
            if (payload->req.send_reply)
                reply_ng(CMD_HF_ICLASS_CREDIT_EPURSE, PM3_ETEAROFF, (uint8_t *)&res, sizeof(uint8_t));
            return;
        } else {

            uint16_t resp_len = 0;
            int res2 = GetIso15693AnswerFromTag(resp, sizeof(resp), ICLASS_READER_TIMEOUT_UPDATE, &eof_time, false, true, &resp_len);
            if (res2 == PM3_SUCCESS && resp_len == 10) {
                res = true;
                break;
            }
        }
    }

    if (tries == 0) {
        res = false;
        goto out;
    }

    // check response. e-purse update swaps first and second half
    if (memcmp(write + 2 + 4, resp, 4) || memcmp(write + 2, resp + 4, 4)) {
        res = false;
        goto out;
    }

out:
    switch_off();

    if (payload->req.send_reply)
        reply_ng(CMD_HF_ICLASS_CREDIT_EPURSE, PM3_SUCCESS, (uint8_t *)&res, sizeof(uint8_t));
}

static void iclass_cmp_print(uint8_t *b1, uint8_t *b2, const char *header1, const char *header2) {

    char line1[240] = {0};
    char line2[240] = {0};

    strcat(line1, header1);
    strcat(line2, header2);

    for (uint8_t i = 0; i < PICOPASS_BLOCK_SIZE; i++) {

        int l1 = strlen(line1);
        int l2 = strlen(line2);

        uint8_t hi1 = NIBBLE_HIGH(b1[i]);
        uint8_t low1 = NIBBLE_LOW(b1[i]);

        uint8_t hi2 = NIBBLE_HIGH(b2[i]);
        uint8_t low2 = NIBBLE_LOW(b2[i]);

        if (hi1 != hi2) {
            sprintf(line1 + l1, _RED_("%1X"), hi1);
            sprintf(line2 + l2, _GREEN_("%1X"), hi2);
        } else {
            sprintf(line1 + l1, "%1X", hi1);
            sprintf(line2 + l2, "%1X", hi2);
        }

        l1 = strlen(line1);
        l2 = strlen(line2);

        if (low1 != low2) {
            sprintf(line1 + l1, _RED_("%1X"), low1);
            sprintf(line2 + l2, _GREEN_("%1X"), low2);
        } else {
            sprintf(line1 + l1, "%1X", low1);
            sprintf(line2 + l2, "%1X", low2);
        }
    }
    DbpString(line1);
    DbpString(line2);
}

void iClass_TearBlock(iclass_tearblock_req_t *msg) {

    if (msg == NULL) {
        reply_ng(CMD_HF_ICLASS_TEARBL, PM3_ESOFT, NULL, 0);
        return;
    }

    // local variable copies
    int tear_start = msg->tear_start;
    int tear_end = msg->tear_end;
    int tear_inc = msg->increment;
    int tear_loop = msg->tear_loop;

    int loop_count = 0;

    uint32_t start_time = 0;
    uint32_t eof_time = 0;

    int isok = PM3_SUCCESS;

    uint8_t data[8] = {0};
    memcpy(data, msg->data, sizeof(data));

    uint8_t mac[4] = {0};
    memcpy(mac, msg->mac, sizeof(mac));

    picopass_hdr_t hdr = {0};
    iclass_auth_req_t req = {
        .blockno = msg->req.blockno,
        .do_auth = msg->req.do_auth,
        .send_reply = msg->req.send_reply,
        .shallow_mod = msg->req.shallow_mod,
        .use_credit_key = msg->req.use_credit_key,
        .use_elite = msg->req.use_elite,
        .use_raw = msg->req.use_raw,
        .use_replay = msg->req.use_replay
    };
    memcpy(req.key, msg->req.key, PICOPASS_BLOCK_SIZE);

    LED_A_ON();
    Iso15693InitReader();

    // save old debug log level
    int oldbg = g_dbglevel;

    // no debug logging please
    g_dbglevel = DBG_NONE;

    // select
    bool res = select_iclass_tag(&hdr, req.use_credit_key, &eof_time, req.shallow_mod);
    if (res == false) {
        DbpString(_RED_("Failed to select iClass tag"));
        isok = PM3_ECARDEXCHANGE;
        goto out;
    }

    // authenticate
    start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
    res = authenticate_iclass_tag(&req, &hdr, &start_time, &eof_time, mac);
    if (res == false) {
        DbpString(_RED_("Failed to authenticate with iClass tag"));
        isok = PM3_ECARDEXCHANGE;
        goto out;
    }

    uint8_t data_read_orig[PICOPASS_BLOCK_SIZE] = {0};

    // read block
    start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
    res = iclass_read_block(req.blockno, data_read_orig, &start_time, &eof_time, req.shallow_mod);
    if (res == false) {
        Dbprintf("Failed to read block %u", req.blockno);
        isok = PM3_ECARDEXCHANGE;
        goto out;
    }

    bool erase_phase = false;
    bool read_ok = false;

    // static uint8_t empty[PICOPASS_BLOCK_SIZE] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static uint8_t zeros[PICOPASS_BLOCK_SIZE] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    uint8_t ff_data[PICOPASS_BLOCK_SIZE] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t data_read[PICOPASS_BLOCK_SIZE] = {0};

    // create READ command
    uint8_t cmd_read[] = {ICLASS_CMD_READ_OR_IDENTIFY, req.blockno, 0x00, 0x00};
    AddCrc(cmd_read + 1, 1);

    // create WRITE COMMAND and new block data
    uint8_t cmd_write[14] = { 0x80 | ICLASS_CMD_UPDATE, req.blockno };
    uint8_t cmd_write_len = 14;
    memcpy(cmd_write + 2, data, PICOPASS_BLOCK_SIZE);

    uint8_t pagemap = get_pagemap(&hdr);
    if (pagemap == PICOPASS_NON_SECURE_PAGEMODE) {
        // Unsecured tags uses CRC16,  but don't include the UPDATE operation code
        // byte0 = update op
        // byte1 = block no
        // byte2..9 = new block data
        AddCrc(cmd_write + 1, 9);
        cmd_write_len -= 2;
    } else {

        if (req.use_replay) {
            memcpy(cmd_write + 10, mac, sizeof(mac));
        } else {
            // Secure tags uses MAC
            uint8_t wb[9];
            wb[0] = req.blockno;
            memcpy(wb + 1, data, PICOPASS_BLOCK_SIZE);

            if (req.use_credit_key)
                doMAC_N(wb, sizeof(wb), hdr.key_c, mac);
            else
                doMAC_N(wb, sizeof(wb), hdr.key_d, mac);

            memcpy(cmd_write + 10, mac, sizeof(mac));
        }
    }

    // Main loop
    while ((tear_start <= tear_end) && (read_ok == false)) {

        if (BUTTON_PRESS() || data_available()) {
            isok = PM3_EOPABORTED;
            goto out;
        }

        // set tear off trigger
        g_tearoff_enabled = true;
        g_tearoff_delay_us = (tear_start & 0xFFFF);

        if (tear_loop > 1) {
            DbprintfEx(FLAG_INPLACE, "[" _BLUE_("#") "] Tear off delay " _YELLOW_("%u") " / " _YELLOW_("%u") " us - " _YELLOW_("%3u") " iter", tear_start, tear_end, loop_count + 1);
        } else {
            DbprintfEx(FLAG_INPLACE, "[" _BLUE_("#") "] Tear off delay " _YELLOW_("%u") " / " _YELLOW_("%u") " us", tear_start, tear_end);
        }

        // write block
        start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
        iclass_send_as_reader(cmd_write, cmd_write_len, &start_time, &eof_time, req.shallow_mod);

        tearoff_hook();

        switch_off();

        // start reading block

        // reinit
        Iso15693InitReader();

        // select tag
        res = select_iclass_tag(&hdr, req.use_credit_key, &eof_time, req.shallow_mod);
        if (res == false) {
            continue;
        }

        // skip authentication for config and e-purse blocks (1,2)
        if (req.blockno > 2) {

            // authenticate
            start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
            res = authenticate_iclass_tag(&req, &hdr, &start_time, &eof_time, NULL);
            if (res == false) {
                DbpString("Failed to authenticate after tear");
                continue;
            }
        }

        // read again and keep field on
        start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
        res = iclass_read_block(req.blockno, data_read, &start_time, &eof_time, req.shallow_mod);
        if (res == false) {
            DbpString("Failed to read block after tear");
            continue;
        }

        //
        bool tear_success = true;

        if (memcmp(data_read, data, PICOPASS_BLOCK_SIZE) != 0) {
            tear_success = false;
        }

        if ((tear_success == false) &&
                (memcmp(data_read, zeros, PICOPASS_BLOCK_SIZE) != 0) &&
                (memcmp(data_read, data_read_orig, PICOPASS_BLOCK_SIZE) != 0)) {

            // tearoff succeeded (partially)

            if (memcmp(data_read, ff_data, PICOPASS_BLOCK_SIZE) == 0 &&
                    memcmp(data_read_orig, ff_data, PICOPASS_BLOCK_SIZE) != 0) {

                if (erase_phase == false) {
                    DbpString("");
                    DbpString(_CYAN_("Erase phase hit... ALL ONES"));

                    iclass_cmp_print(data_read_orig, data_read, "Original: ", "Read:     ");
                }
                erase_phase = true;

            } else {

                if (erase_phase) {
                    DbpString("");
                    DbpString(_MAGENTA_("Tearing! Write phase (post erase)"));
                    iclass_cmp_print(data_read_orig, data_read, "Original: ", "Read:     ");
                } else {
                    DbpString("");
                    DbpString(_CYAN_("Tearing! unknown phase"));
                    iclass_cmp_print(data_read_orig, data_read, "Original: ", "Read:     ");
                }
            }

            // shall we exit?   well it depends on some things.
            bool goto_out = false;

            if (req.blockno == 2) {
                if (memcmp(data_read, ff_data, PICOPASS_BLOCK_SIZE) == 0 && memcmp(data_read_orig, ff_data, PICOPASS_BLOCK_SIZE) != 0) {
                    DbpString("");
                    Dbprintf("E-purse has been teared ( %s )", _GREEN_("ok"));
                    isok = PM3_SUCCESS;
                    goto_out = true;
                }
            }

            if (req.blockno == 1) {

                // if more OTP bits set..
                if (data_read[1] > data_read_orig[1] ||
                        data_read[2] > data_read_orig[2]) {


                    // step 4 if bits changed attempt to write the new bits to the tag
                    if (data_read[7] == 0xBC) {
                        data_read[7] = 0xAC;
                    }

                    // prepare WRITE command
                    cmd_write_len = 14;
                    memcpy(cmd_write + 2, data_read, PICOPASS_BLOCK_SIZE);

                    if (pagemap == PICOPASS_NON_SECURE_PAGEMODE) {
                        // Unsecured tags uses CRC16,  but don't include the UPDATE operation code
                        // byte0 = update op
                        // byte1 = block no
                        // byte2..9 = new block data
                        AddCrc(cmd_write + 1, 9);
                        cmd_write_len -= 2;
                    } else {

                        if (req.use_replay) {
                            memcpy(cmd_write + 10, mac, sizeof(mac));
                        } else {
                            // Secure tags uses MAC
                            uint8_t wb[9];
                            wb[0] = req.blockno;
                            memcpy(wb + 1, data_read, PICOPASS_BLOCK_SIZE);

                            if (req.use_credit_key)
                                doMAC_N(wb, sizeof(wb), hdr.key_c, mac);
                            else
                                doMAC_N(wb, sizeof(wb), hdr.key_d, mac);

                            memcpy(cmd_write + 10, mac, sizeof(mac));
                        }
                    }

                    // write block
                    start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
                    iclass_send_as_reader(cmd_write, cmd_write_len, &start_time, &eof_time, req.shallow_mod);

                    uint16_t resp_len = 0;
                    uint8_t resp[ICLASS_BUFFER_SIZE] = {0};
                    res = GetIso15693AnswerFromTag(resp, sizeof(resp), ICLASS_READER_TIMEOUT_UPDATE, &eof_time, false, true, &resp_len);
                    if (res == PM3_SUCCESS && resp_len == 10) {
                        Dbprintf("Wrote to block");
                    }

                    switch_off();

                    DbpString("");
                    DbpString("More OTP bits got set!!!");

                    Iso15693InitReader();

                    // select tag,   during which we read block1
                    res = select_iclass_tag(&hdr, req.use_credit_key, &eof_time, req.shallow_mod);
                    if (res) {

                        if (memcmp(&hdr.conf, cmd_write + 2, PICOPASS_BLOCK_SIZE) == 0) {
                            Dbprintf("Stabilize the bits ( "_GREEN_("ok") " )");
                        } else {
                            Dbprintf("Stabilize the bits ( "_RED_("failed") " )");
                        }
                    }

                    isok = PM3_SUCCESS;
                    goto_out = true;
                }

                if (data_read[0] != data_read_orig[0]) {
                    DbpString("");
                    Dbprintf("Application limit changed, from "_YELLOW_("%u")" to "_YELLOW_("%u"), data_read_orig[0], data_read[0]);
                    isok = PM3_SUCCESS;
                    goto_out = true;
                }

                if (data_read[7] != data_read_orig[7]) {
                    DbpString("");
                    Dbprintf("Fuse changed, from "_YELLOW_("%02x")" to "_YELLOW_("%02x"), data_read_orig[7], data_read[7]);

                    const char *flag_names[8] = {
                        "RA",
                        "Fprod0",
                        "Fprod1",
                        "Crypt0 (*1)",
                        "Crypt1 (*0)",
                        "Coding0",
                        "Coding1",
                        "Fpers  (*1)"
                    };
                    Dbprintf(_YELLOW_("%-10s %-10s %-10s"), "Fuse", "Original", "Changed");
                    Dbprintf("---------------------------------------");
                    for (int i = 7; i >= 0; --i) {
                        int bit1 = (data_read_orig[7] >> i) & 1;
                        int bit2 = (data_read[7] >> i) & 1;
                        Dbprintf("%-11s %-10d %-10d", flag_names[i], bit1, bit2);
                    }

                    isok = PM3_SUCCESS;
                    goto_out = true;
                }
            }

            if (goto_out) {
                goto out;
            }
        }

        // tearoff succeeded with expected values,  which is unlikely
        if (tear_success) {
            read_ok = true;
            tear_success = true;
            DbpString("");
            DbpString("tear success (expected values)!");
        }

        loop_count++;

        // increase tear off delay
        if (loop_count == tear_loop) {
            tear_start += tear_inc;
            loop_count = 0;
        }
    }

out:

    switch_off();

    // reset tear off trigger
    g_tearoff_enabled = false;

    // restore debug message levels
    g_dbglevel = oldbg;

    if (msg->req.send_reply) {
        reply_ng(CMD_HF_ICLASS_TEARBL, isok, NULL, 0);
    }
}

void iClass_Restore(iclass_restore_req_t *msg) {

    // sanitation
    if (msg == NULL) {
        reply_ng(CMD_HF_ICLASS_RESTORE, PM3_ESOFT, NULL, 0);
        return;
    }

    if (msg->item_cnt == 0) {
        if (msg->req.send_reply) {
            reply_ng(CMD_HF_ICLASS_RESTORE, PM3_ESOFT, NULL, 0);
        }
        return;
    }

    bool shallow_mod = msg->req.shallow_mod;

    LED_A_ON();
    Iso15693InitReader();

    uint16_t written = 0;
    uint32_t eof_time = 0;
    picopass_hdr_t hdr = {0};

    // select
    bool res = select_iclass_tag(&hdr, msg->req.use_credit_key, &eof_time, shallow_mod);
    if (res == false) {
        goto out;
    }

    // authenticate
    uint8_t mac[4] = {0};
    uint32_t start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;

    // authenticate
    if (msg->req.do_auth) {
        res = authenticate_iclass_tag(&msg->req, &hdr, &start_time, &eof_time, mac);
        if (res == false) {
            goto out;
        }
    }

    // main loop
    bool use_mac;
    for (uint8_t i = 0; i < msg->item_cnt; i++) {

        iclass_restore_item_t item = msg->blocks[i];

        uint8_t pagemap = get_pagemap(&hdr);
        if (pagemap == PICOPASS_NON_SECURE_PAGEMODE) {
            // Unsecured tags uses CRC16
            use_mac = false;
        } else {
            // Secure tags uses MAC
            use_mac = true;
            uint8_t wb[9] = {0};
            wb[0] = item.blockno;
            memcpy(wb + 1, item.data, 8);

            if (msg->req.use_credit_key)
                doMAC_N(wb, sizeof(wb), hdr.key_c, mac);
            else
                doMAC_N(wb, sizeof(wb), hdr.key_d, mac);
        }

        // data + mac
        if (iclass_writeblock_ext(item.blockno, item.data, mac, use_mac, shallow_mod)) {
            Dbprintf("Write block [%3d/0x%02X] " _GREEN_("successful"), item.blockno, item.blockno);
            written++;
        } else {
            Dbprintf("Write block [%3d/0x%02X] " _RED_("failed"), item.blockno, item.blockno);
        }
    }

out:

    switch_off();
    if (msg->req.send_reply) {
        int isOK = (written == msg->item_cnt) ? PM3_SUCCESS : PM3_ESOFT;
        reply_ng(CMD_HF_ICLASS_RESTORE, isOK, NULL, 0);
    }
}

static void generate_single_key_block_inverted_opt(const uint8_t *startingKey, uint32_t index, uint8_t *keyBlock) {

    uint8_t bits_index = index / 16383;
    uint8_t ending_bits[] = { //all possible 70 combinations of 4x0 and 4x1 as key ending bits
        0x0F, 0x17, 0x1B, 0x1D, 0x1E, 0x27, 0x2B, 0x2D, 0x2E, 0x33,
        0x35, 0x36, 0x39, 0x3A, 0x3C, 0x47, 0x4B, 0x4D, 0x4E, 0x53,
        0x55, 0x56, 0x59, 0x5A, 0x5C, 0x63, 0x65, 0x66, 0x69, 0x6A,
        0x6C, 0x71, 0x72, 0x74, 0x78, 0x87, 0x8B, 0x8D, 0x8E, 0x93,
        0x95, 0x96, 0x99, 0x9A, 0x9C, 0xA3, 0xA5, 0xA6, 0xA9, 0xAA,
        0xAC, 0xB1, 0xB2, 0xB4, 0xB8, 0xC3, 0xC5, 0xC6, 0xC9, 0xCA,
        0xCC, 0xD1, 0xD2, 0xD4, 0xD8, 0xE1, 0xE2, 0xE4, 0xE8, 0xF0
    };

    uint8_t binary_endings[8]; // Array to store binary values for each ending bit
    // Extract each bit from the ending_bits[k] and store it in binary_endings
    uint8_t ending = ending_bits[bits_index];
    for (int i = 7; i >= 0; i--) {
        binary_endings[i] = ending & 1;
        ending >>= 1;
    }

    uint8_t binary_mids[8];    // Array to store the 2-bit chunks of index
    // Iterate over the 16-bit integer and store 2 bits at a time in the result array
    for (int i = 0; i < 8; i++) {
        // Shift and mask to get 2 bits and store them as an 8-bit value
        binary_mids[7 - i] = (index >> (i * 2)) & 0x03; // 0x03 is a mask for 2 bits (binary 11)
    }
    memcpy(keyBlock, startingKey, PICOPASS_BLOCK_SIZE);

    // Start from the second byte, index 1 as we're never gonna touch the first byte
    for (int i = 1; i < PICOPASS_BLOCK_SIZE; i++) {
        // Clear the last three bits of the current byte (AND with 0xF8)
        keyBlock[i] &= 0xF8;
        // Set the last bit to the corresponding value from binary_endings (OR with binary_endings[i])
        keyBlock[i] |= ((binary_mids[i] & 0x03) << 1) | (binary_endings[i] & 0x01);
    }
}

void iClass_Recover(iclass_recover_req_t *msg) {

    bool shallow_mod = false;
    uint8_t zero_key[PICOPASS_BLOCK_SIZE] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t genkeyblock[PICOPASS_BLOCK_SIZE] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t fast_restore_key[PICOPASS_BLOCK_SIZE] = {0};
    uint8_t fast_previous_key[PICOPASS_BLOCK_SIZE] = {0};
    uint8_t fast_current_key[PICOPASS_BLOCK_SIZE] = {0};
    uint32_t index = msg->index;
    bool short_delay = msg->short_delay;
    int bits_found = -1;
    bool recovered = false;
    bool completed = false;
    bool interrupted = false;
    uint8_t div_key2[8] = {0};
    uint32_t eof_time = 0;
    uint32_t start_time = 0;
    uint8_t read_check_cc[] = { 0x10 | ICLASS_CMD_READCHECK, 0x18 }; //block 24 with credit key
    uint8_t read_check_cc2[] = { 0x80 | ICLASS_CMD_READCHECK, 0x02 }; //block 2 -> to check Kd macs

    /*  iclass_mac_table is a series of weak macs, those weak macs correspond to the different combinations of the last 3 bits of each key byte. */

    static uint8_t iclass_mac_table[8][8] = {    //Reference weak macs table
        { 0x00, 0x00, 0x00, 0x00, 0xBF, 0x5D, 0x67, 0x7F }, //Expected mac when last 3 bits of each byte are: 000
        { 0x00, 0x00, 0x00, 0x00, 0x10, 0xED, 0x6F, 0x11 }, //Expected mac when last 3 bits of each byte are: 001
        { 0x00, 0x00, 0x00, 0x00, 0x53, 0x35, 0x42, 0x0F }, //Expected mac when last 3 bits of each byte are: 010
        { 0x00, 0x00, 0x00, 0x00, 0xAB, 0x47, 0x4D, 0xA0 }, //Expected mac when last 3 bits of each byte are: 011
        { 0x00, 0x00, 0x00, 0x00, 0xF6, 0xCF, 0x43, 0x36 }, //Expected mac when last 3 bits of each byte are: 100
        { 0x00, 0x00, 0x00, 0x00, 0x59, 0x7F, 0x4B, 0x58 }, //Expected mac when last 3 bits of each byte are: 101
        { 0x00, 0x00, 0x00, 0x00, 0x1A, 0xA7, 0x66, 0x46 }, //Expected mac when last 3 bits of each byte are: 110
        { 0x00, 0x00, 0x00, 0x00, 0xE2, 0xD5, 0x69, 0xE9 }  //Expected mac when last 3 bits of each byte are: 111
    };

    LED_A_ON();
    DbpString(_RED_("Interrupting this process may render the card unusable!"));
    memcpy(div_key2, msg->nfa, 8);

    //START LOOP
    uint32_t loops = 1;
    bool card_select = false;
    bool card_auth = false;
    bool priv_esc = false;
    int status_message = 0;
    int reinit_tentatives = 0;
    bool res = false;
    picopass_hdr_t hdr = {0};
    uint8_t original_mac[8] = {0};
    uint8_t mac1[4] = {0};

    while (!card_select || !card_auth) {

        Iso15693InitReader(); //has to be at the top as it starts tracing
        if (!msg->debug) {
            set_tracing(false); //disable tracing to prevent crashes - set to true for debugging
        } else {
            if (loops == 1) {
                clear_trace(); //if we're debugging better to clear the trace but do it only on the first loop
            }
        }
        //Step0 Card Select Routine
        eof_time = 0; //reset eof time
        res = select_iclass_tag(&hdr, false, &eof_time, shallow_mod);
        if (res) {
            status_message = 1; //card select successful
            card_select = true;
        }

        //Step 0A - The read_check_cc block has to be in AA2, set it by checking the card configuration
        read_check_cc[1] = ((uint8_t *)&hdr.conf)[0] + 1; //first block of AA2

        //Step1 Authenticate with AA1 using trace
        if (card_select) {
            memcpy(original_mac, msg->req.key, 8);
            start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
            res = authenticate_iclass_tag(&msg->req, &hdr, &start_time, &eof_time, mac1);
            if (res) {
                status_message = 2; //authentication with AA1 macs successful
                card_auth = true;
            }
        }
        if (!card_auth || !card_select) {
            reinit_tentatives++;
            switch_off();
        }
        if (reinit_tentatives == 5) {
            DbpString("");
            DbpString(_RED_("Unable to select or authenticate with card multiple times! Stopping."));
            goto out;
        }
    }

    while (bits_found == -1) {

        reinit_tentatives = 0;
        int res2;
        uint8_t resp[10] = {0};
        uint8_t mac2[4] = {0};
        res = false;
        uint16_t resp_len = 0;

        if (BUTTON_PRESS() || loops > msg->loop) {
            if (loops > msg->loop) {
                completed = true;
            } else {
                interrupted = true;
            }
            if (msg->fast) {
                goto fast_restore;
            }
            goto out;
        }

        if (msg->test) {
            Dbprintf(_YELLOW_("*Cycled Reader*") " TEST Index - Loops: "_YELLOW_("%3d / %3d") " *", loops, msg->loop);
        } else if (msg->debug || (!card_select && !card_auth)) {
            Dbprintf(_YELLOW_("*Cycled Reader*") " Index: "_RED_("%3d")" Loops: "_YELLOW_("%3d / %3d") " *", index, loops, msg->loop);
        } else {
            DbprintfEx(FLAG_INPLACE, "[" _BLUE_("#") "] Index: "_CYAN_("%3d")" Loops: "_YELLOW_("%3d / %3d")" ", index, loops, msg->loop);
        }

        while (!card_select || !card_auth) {

            Iso15693InitReader(); //has to be at the top as it starts tracing
            set_tracing(false); //disable tracing to prevent crashes - set to true for debugging
            //Step0 Card Select Routine
            eof_time = 0; //reset eof time
            res = select_iclass_tag(&hdr, false, &eof_time, shallow_mod);
            if (res) {
                status_message = 1; //card select successful
                card_select = true;
            }

            //Step1 Authenticate with AA1 using trace
            if (card_select) {
                memcpy(original_mac, msg->req.key, 8);
                start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
                res = authenticate_iclass_tag(&msg->req, &hdr, &start_time, &eof_time, mac1);
                if (res) {
                    status_message = 2; //authentication with AA1 macs successful
                    card_auth = true;
                }
            }
            if (!card_auth || !card_select) {
                reinit_tentatives++;
                switch_off();
            }
            if (reinit_tentatives == 5) {
                DbpString("");
                DbpString(_RED_("Unable to select or authenticate with card multiple times! Stopping."));
                goto out;
            }
        }

        //Step2 Privilege Escalation: attempt to read AA2 with credentials for AA1
        int priv_esc_tries = 0;
        while (!priv_esc) {
            //The privilege escalation is done with a readcheck and not just a normal read!
            start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
            iclass_send_as_reader(read_check_cc, sizeof(read_check_cc), &start_time, &eof_time, shallow_mod);
            // expect a 8-byte response here
            res2 = GetIso15693AnswerFromTag(resp, sizeof(resp), ICLASS_READER_TIMEOUT_OTHERS, &eof_time, false, true, &resp_len);
            if (res2 != PM3_SUCCESS || resp_len != 8) {
                priv_esc_tries++;
            } else {
                status_message = 3; //privilege escalation successful
                priv_esc = true;
            }
            if (priv_esc_tries == 5) {
                DbpString("");
                DbpString(_RED_("Unable to complete privilege escalation! Stopping."));
                goto out;
            }
        }
        if (priv_esc && status_message != 3) {
            start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
            iclass_send_as_reader(read_check_cc, sizeof(read_check_cc), &start_time, &eof_time, shallow_mod);
            status_message = 3;
        }

        //Step3 Calculate New Key (Optimised Algo V2)
        generate_single_key_block_inverted_opt(zero_key, index, genkeyblock);
        if (msg->test) {
            memcpy(genkeyblock, zero_key, PICOPASS_BLOCK_SIZE);
        }

        if (msg->fast) { //if we're skipping restoring the original key to gain speed, xor the new index key with the previous index key and update the difference and track restore values differently
            if (index > 0 && loops > 1) {
                generate_single_key_block_inverted_opt(zero_key, index - 1, fast_previous_key);
            } else {
                memcpy(fast_previous_key, zero_key, PICOPASS_BLOCK_SIZE);
            }
            for (int i = 0; i < PICOPASS_BLOCK_SIZE; i++) {
                fast_current_key[i] = genkeyblock[i] ^ fast_previous_key[i];
                fast_restore_key[i] = fast_restore_key[i] ^ fast_current_key[i];
            }
            memcpy(genkeyblock, fast_current_key, PICOPASS_BLOCK_SIZE);
        }

        //Step4 Calculate New Mac

        uint8_t wb[9] = {0};
        uint8_t blockno = 3;
        wb[0] = blockno;
        memcpy(wb + 1, genkeyblock, 8);
        doMAC_N(wb, sizeof(wb), div_key2, mac2);
        bool written = false;
        bool write_error = false;
        while (written == false && write_error == false) {
            //Step5 Perform Write
            if (iclass_writeblock_sp(blockno, genkeyblock, mac2, shallow_mod, &start_time, &eof_time, short_delay)) {
                status_message = 4; //wrote new key on the card - unverified
            }
            if (!msg->fast) { //if we're going slow we check at every write that the write actually happened
                //Reset cypher state
                start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
                iclass_send_as_reader(read_check_cc2, sizeof(read_check_cc2), &start_time, &eof_time, shallow_mod);
                //try to authenticate with the original mac to verify the write happened
                memcpy(msg->req.key, original_mac, 8);
                start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
                res = authenticate_iclass_tag(&msg->req, &hdr, &start_time, &eof_time, mac1);
                if (msg->test) {
                    if (res) {
                        DbpString("");
                        DbpString(_GREEN_("*** CARD EPURSE IS LOUD! OK TO ATTEMPT KEY RETRIEVAL! RUN AGAIN WITH -notest ***"));
                        completed = true;
                        goto out;
                    } else {
                        DbpString("");
                        DbpString(_RED_("*** CARD EPURSE IS SILENT! RISK OF BRICKING! DO NOT EXECUTE KEY UPDATES! SCAN IT ON READER FOR EPURSE UPDATE, COLLECT NEW TRACES AND TRY AGAIN! ***"));
                        goto out;
                    }
                } else {
                    if (res) {
                        write_error = true; //failed to update the key, the card's key is the original one
                    } else {
                        status_message = 5; //verified the card key was updated to the new one
                        written = true;
                    }
                }
            } else { //if we're going fast we can skip the above checks as we're just xorring the key over and over
                status_message = 5;
                written = true;
            }
        }

        if (write_error == false) {
            //Step6 Perform 8 authentication attempts + 1 to verify if we found the weak key
            for (int i = 0; i < 8 ; ++i) {
                start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
                iclass_send_as_reader(read_check_cc2, sizeof(read_check_cc2), &start_time, &eof_time, shallow_mod);
                //need to craft the authentication payload accordingly
                memcpy(msg->req.key, iclass_mac_table[i], 8);
                start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
                res = authenticate_iclass_tag(&msg->req, &hdr, &start_time, &eof_time, mac1); //mac1 here shouldn't matter
                if (res == true) {
                    bits_found = i;
                    recovered = true;
                }
            }

            bool reverted = false;
            uint8_t revert_retries = 0;
            if (msg->fast) { //if we're going fast only restore the original key at the end
                if (recovered) {
                    goto fast_restore;
                }
            } else {
                //if we're NOT going fast, regardless of bits being found, restore the original key and verify it
                while (!reverted) {
                    //Regain privilege escalation with a readcheck
                    start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
                    iclass_send_as_reader(read_check_cc, sizeof(read_check_cc), &start_time, &eof_time, shallow_mod);
                    start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
                    if (iclass_writeblock_sp(blockno, genkeyblock, mac2, shallow_mod, &start_time, &eof_time, short_delay)) {
                        status_message = 6; //restore of original key successful but unverified
                    }
                    //Do a readcheck first to reset the cypher state
                    start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
                    iclass_send_as_reader(read_check_cc2, sizeof(read_check_cc2), &start_time, &eof_time, shallow_mod);
                    //need to craft the authentication payload accordingly
                    memcpy(msg->req.key, original_mac, 8);
                    start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
                    res = authenticate_iclass_tag(&msg->req, &hdr, &start_time, &eof_time, mac1);
                    if (res == true) {
                        status_message = 7; //restore of original key verified - card usable again
                        reverted = true;
                        if (recovered) {
                            goto restore;
                        }
                    }

                    revert_retries++;
                    if (revert_retries >= 7) { //must always be an odd number!
                        DbpString("");
                        DbpString(_CYAN_("Last Written Key: "));
                        Dbhexdump(8, genkeyblock, false);
                        Dbprintf(_RED_("Attempted to restore original key for %3d times and failed. Stopping. Card is likely unusable."), revert_retries);
                        goto out;
                    }
                }
            }


        }

        if (msg->debug) {
            if (status_message >= 1) {
                DbpString("");
                DbpString("Card Select:............."_GREEN_("Ok!"));
            }
            if (status_message >= 2) {
                DbpString("AA1 macs authentication:."_GREEN_("Ok!"));
            }
            if (status_message >= 3) {
                DbpString("Privilege Escalation:...."_GREEN_("Ok!"));
            }
            if (status_message >= 4) {
                DbpString("Wrote key: ");
                Dbhexdump(8, genkeyblock, false);
            }
            if (status_message >= 5) {
                DbpString("Key Update:.............."_GREEN_("Verified!"));
            }
            if (status_message >= 6) {
                DbpString("Original Key Restore:...."_GREEN_("Ok!"));
            }
            if (status_message >= 7) {
                DbpString("Original Key Restore:...."_GREEN_("Verified!"));
            }
        }

        if (write_error && (msg->debug || msg->test)) { //if there was a write error, re-run the loop for the same key index
            DbpString("Loop Error: "_RED_("Repeating Loop!"));
            card_select = false;
            card_auth = false;
            priv_esc = false;
        } else {
            loops++;
            index++;
            status_message = 2;
        }

    }//end while

fast_restore:
    ;//empty statement for compilation
    uint8_t mac2[4] = {0};
    uint8_t wb[9] = {0};
    uint8_t blockno = 3;
    wb[0] = blockno;
    bool reverted = false;
    uint8_t revert_retries = 0;
    while (!reverted) {
        //Regain privilege escalation with a readcheck
        start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
        iclass_send_as_reader(read_check_cc, sizeof(read_check_cc), &start_time, &eof_time, shallow_mod);
        memcpy(wb + 1, fast_restore_key, 8);
        doMAC_N(wb, sizeof(wb), div_key2, mac2);
        start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
        if (iclass_writeblock_sp(blockno, fast_restore_key, mac2, shallow_mod, &start_time, &eof_time, short_delay)) {
            status_message = 6; //restore of original key successful but unverified
        }
        //Do a readcheck first to reset the cypher state
        start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
        iclass_send_as_reader(read_check_cc2, sizeof(read_check_cc2), &start_time, &eof_time, shallow_mod);
        //need to craft the authentication payload accordingly
        memcpy(msg->req.key, original_mac, 8);
        start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
        res = authenticate_iclass_tag(&msg->req, &hdr, &start_time, &eof_time, mac1);
        if (res == true) {
            status_message = 7; //restore of original key verified - card usable again
            reverted = true;
        }
        revert_retries++;
        if (revert_retries >= 7) { //must always be an odd number!
            DbpString("");
            DbpString(_CYAN_("Last Written Key (fast): "));
            Dbhexdump(8, fast_restore_key, false);
            Dbprintf(_RED_("Attempted to restore original key for %3d times and failed. Stopping. Card is likely unusable."), revert_retries);
        }
        if (recovered) {
            goto restore;
        } else {
            goto out;
        }
    }

restore:
    ;//empty statement for compilation
    uint8_t partialkey[PICOPASS_BLOCK_SIZE] = {0};

    for (int i = 0; i < PICOPASS_BLOCK_SIZE; i++) {
        if (msg->fast) {
            partialkey[i] = fast_restore_key[i] ^ bits_found;
        } else {
            partialkey[i] = genkeyblock[i] ^ bits_found;
        }
    }

    //Print the bits decimal value
    DbpString("");
    DbpString(_RED_("--------------------------------------------------------"));
    Dbprintf("Decimal Value of last 3 bits: " _GREEN_("[%3d]"), bits_found);
    //Print the 24 bits found from k1
    DbpString(_RED_("--------------------------------------------------------"));
    DbpString(_RED_("SUCCESS! Raw Key Partial Bytes: "));
    Dbhexdump(8, partialkey, false);
    DbpString(_RED_("--------------------------------------------------------"));
    switch_off();
    reply_ng(CMD_HF_ICLASS_RECOVER, PM3_SUCCESS, NULL, 0);


out:

    switch_off();
    if (completed) {
        reply_ng(CMD_HF_ICLASS_RECOVER, PM3_EINVARG, NULL, 0);
    } else if (interrupted) {
        reply_ng(CMD_HF_ICLASS_RECOVER, PM3_EOPABORTED, NULL, 0);
    } else {
        reply_ng(CMD_HF_ICLASS_RECOVER, PM3_ESOFT, NULL, 0);
    }

}

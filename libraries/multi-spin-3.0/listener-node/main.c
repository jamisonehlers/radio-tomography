// 
// SPAN Lab (The University of Utah) and Xandem Technology Copyright 2012-2013
// 
// Author(s):
// Maurizio Bocca (maurizio.bocca@utah.edu)
// Joey Wilson (joey@xandem.com)
// Neal Patwari (neal@xandem.com)
// 
// This file is part of multi-Spin.
// multi-Spin is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// multi-Spin is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with multi-Spin. If not, see <http://www.gnu.org/licenses/>.
// 

#include <cc2530.h>
#include <string.h>
#include "hal_defs.h"
#include "hal_led.h"
#include "hal_int.h"
#include "hal_board.h"
#include "hal_assert.h"
#include "hal_mcu.h"
#include "hal_uart.h"
#include "configuration.h"
#include "rf.h"
#include "flush_buffers.h"
#include "timers34.h"
#include "leds.h"
#include "clock.h"
#include "spin_clock.h"
#include "spin_multichannel.h"
#include "channels.h"

// Format of the packet transferred through the serial port of the listener node
typedef struct {
    spinPacket_t spinPacket;
    uint16 suffix;
} serialPacket_t;
serialPacket_t serialPacket;
configurationPacket_t configPacket;
spinPacket_t rxPacket;
static rfConfig_t rfConfig;

// Length (in ticks of the clock counter) of a TDMA slot
#define SLOT_LENGTH 7

// ID number of the listen node (it can be defined when multiple listen nodes are
// simultaneously used to collect data)
// #define THIS_NODE_ID 1
#define ADDR 0x1234
#define PAN 0x2011

int counter = 0;
signed char rssi;
char corr;
char TX_id;
int int_TX_id;
int unconfigured = NUM_NODES;
int configured[NUM_NODES];

int packetSize = sizeof(spinHeader) + sizeof(spinData) * NUM_NODES;
char channel;
static int channel_counter = 0;
int next_channel_time;
int next_reset_radio_time = (NUM_NODES + 3) * SLOT_LENGTH;

// Timer 3: frequency channel hopping
timer34Config_t channel_hoppingConfig;
// Timer 4: reset radio module to default frequency channel
timer34Config_t reset_radio_channelConfig;

void usbirqHandler(void);
void usb_irq_handler(void) __interrupt 6 {
    usbirqHandler();
}

// ISR for resetting the radio to the default channel
// The default channel is the first channel in the array defined in channels.h
void reset_radio_channelISR(void) __interrupt 12 {
    timer4Stop();
    rfConfig.channel = channel_sequence[0];
    radioInit(&rfConfig);
    ledOn(1); // Green LED on
}

// ISR for switching frequency channel
// The listen node loops on the frequency channels defined in channels.h
void channel_hoppingISR(void) __interrupt 11 {
    channel_counter++;
    if(channel_counter == CHANNELS_NUMBER) {
        channel_counter = 0;
    }
    rfConfig.channel = channel_sequence[channel_counter];
    timer3Stop();
    next_channel_time = (NUM_NODES + 3) * SLOT_LENGTH;
    channel_hoppingConfig.tickThresh = next_channel_time;
    timer3Init(&channel_hoppingConfig);
    timer3Start();
    radioInit(&rfConfig);
}

void main(void) {
    int i, j;

    clockInit();
    ledInit();
    setSysTickFreq(TIMER_TICK_FREQ_250KHZ);

    halBoardInit();
    halUartInit(HAL_UART_BAUDRATE_38400);

    // Set up the radio module
    rfConfig.addr = ADDR;
    rfConfig.pan = PAN;
    rfConfig.channel = channel_sequence[channel_counter];
    rfConfig.txPower = 0xF5; // Max. available TX power
    radioInit(&rfConfig);

    // Enable interrupts 
    EA = 1;

    // Set up Timer4
    // The ISR fires when the listener node does not receive a packet from the
    // RF nodes in the interval of time corresponding to an entire TDMA cycle
    reset_radio_channelConfig.tickDivider = 7;
    reset_radio_channelConfig.tickThresh = next_reset_radio_time;
    reset_radio_channelConfig.isrPtr = reset_radio_channelISR;
    timer4Init(&reset_radio_channelConfig);

    // Set up Timer3
    channel_hoppingConfig.tickDivider = 7;
    channel_hoppingConfig.tickThresh = next_channel_time;
    channel_hoppingConfig.isrPtr = channel_hoppingISR;
    timer3Init(&channel_hoppingConfig);

    // Make sure all nodes start unconfigured.
    for(i = 0; i < NUM_NODES; i++) {
        configured[i] = 0;
    }

    // Configure the nodes
    while(unconfigured > 0) {
        HAL_PROCESS();
        if(isPacketReady()) {
            while(isPacketReady()) {
                if(receivePacket((char*)&rxPacket, packetSize, &rssi, &corr) == packetSize) {
                    // Spin packet received; mark the node as configured
                    if(!configured[rxPacket.header.TX_id - 1]) {
                        configured[rxPacket.header.TX_id - 1] = 1;
                        unconfigured--;
                    }
                }
            }
        } else {
            for(i = 1; i <= NUM_NODES; i++) {
                if(!configured[i - 1]) {
                    // The node has not been configured yet; send a configuration packet for the node
                    memcpy(&configPacket.macAddress, &macAddresses[i - 1], sizeof(macAddresses[i - 1]));
                    configPacket.nodeId = i;
                    configPacket.numNodes = NUM_NODES;
                    sendPacket((char*)&configPacket, sizeof(configPacket), rfConfig.pan, 0xFFFF, rfConfig.addr);

                    // Sending a packet takes some time. Adding a bit of delay.
                    for(j = 0; j < 5000; j++) {
                        NOP;
                    }
                }
            }
        }
    }
    flushRXFIFO();
    flushTXFIFO();

    // Process incoming Spin packets
    while(1) {
        HAL_PROCESS();
        if(isPacketReady()) {
            if(receivePacket((char*)&rxPacket, packetSize, &rssi, &corr) == packetSize) {
      	        ledOn(2); // Red LED on
                ledOff(1); // Green LED off

                timer4Stop();
                timer3Stop();

                flushRXFIFO();
                memcpy(&serialPacket.spinPacket, &rxPacket, sizeof(spinPacket_t));
                serialPacket.suffix = 0xBEEF;

                // Transfer the packet through the serial port
                halUartWrite((uint8*)&serialPacket, sizeof(serialPacket));
                setSysTickFreq(TIMER_TICK_FREQ_250KHZ);

    	        // Update next_channel_time
                TX_id = rxPacket.header.TX_id;
                int_TX_id = (int)(TX_id);
                next_channel_time = (NUM_NODES - int_TX_id + 2) * SLOT_LENGTH;
                channel_hoppingConfig.tickThresh = next_channel_time;
                timer3Init(&channel_hoppingConfig);
                timer3Start();

                timer4Init(&reset_radio_channelConfig);
                timer4Start();
                ledOff(2); // Red LED off
            }
        }
    }
}

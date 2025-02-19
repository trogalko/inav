/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
FILE_COMPILE_FOR_SPEED

#ifdef USE_SERIAL_RX

#include "build/debug.h"

#include "common/utils.h"

#include "drivers/time.h"

#include "io/serial.h"

#ifdef USE_TELEMETRY
#include "telemetry/telemetry.h"
#endif

#include "rx/rx.h"
#include "rx/sbus.h"
#include "rx/sbus_channels.h"

/*
 * Observations
 *
 * FrSky X8R
 * time between frames: 6ms.
 * time to send frame: 3ms.
*
 * Futaba R6208SB/R6303SB
 * time between frames: 11ms.
 * time to send frame: 3ms.
 */

enum {
    DEBUG_SBUS_INTERFRAME_TIME = 0,
    DEBUG_SBUS_FRAME_FLAGS = 1,
    DEBUG_SBUS_DESYNC_COUNTER = 2
};

typedef enum {
    STATE_SBUS_SYNC = 0,
    STATE_SBUS_PAYLOAD,
    STATE_SBUS_WAIT_SYNC
} sbusDecoderState_e;

typedef struct sbusFrameData_s {
    sbusDecoderState_e state;
    volatile sbusFrame_t frame;
    volatile bool frameDone;
    uint8_t buffer[SBUS_FRAME_SIZE];
    uint8_t position;
    timeUs_t lastActivityTimeUs;
} sbusFrameData_t;

// Receive ISR callback
static void sbusDataReceive(uint16_t c, void *data)
{
    static uint16_t sbusDesyncCounter = 0;

    sbusFrameData_t *sbusFrameData = data;
    const timeUs_t currentTimeUs = micros();
    const timeDelta_t timeSinceLastByteUs = cmpTimeUs(currentTimeUs, sbusFrameData->lastActivityTimeUs);
    sbusFrameData->lastActivityTimeUs = currentTimeUs;

    // Handle inter-frame gap. We dwell in STATE_SBUS_WAIT_SYNC state ignoring all incoming bytes until we get long enough quite period on the wire
    if (sbusFrameData->state == STATE_SBUS_WAIT_SYNC && timeSinceLastByteUs >= rxConfig()->sbusSyncInterval) {
        DEBUG_SET(DEBUG_SBUS, DEBUG_SBUS_INTERFRAME_TIME, timeSinceLastByteUs);
        sbusFrameData->state = STATE_SBUS_SYNC;
    }

    switch (sbusFrameData->state) {
        case STATE_SBUS_SYNC:
            if (c == SBUS_FRAME_BEGIN_BYTE) {
                sbusFrameData->position = 0;
                sbusFrameData->buffer[sbusFrameData->position++] = (uint8_t)c;
                sbusFrameData->state = STATE_SBUS_PAYLOAD;
            }
            break;

        case STATE_SBUS_PAYLOAD:
            sbusFrameData->buffer[sbusFrameData->position++] = (uint8_t)c;

            if (sbusFrameData->position == SBUS_FRAME_SIZE) {
                const sbusFrame_t * frame = (sbusFrame_t *)&sbusFrameData->buffer[0];
                bool frameValid = false;

                // Do some sanity check
                switch (frame->endByte) {
                    case 0x00:  // This is S.BUS 1
                    case 0x04:  // S.BUS 2 receiver voltage
                    case 0x14:  // S.BUS 2 GPS/baro
                    case 0x24:  // Unknown SBUS2 data
                    case 0x34:  // Unknown SBUS2 data
                        frameValid = true;
                        sbusFrameData->state = STATE_SBUS_WAIT_SYNC;
                        break;

                    default:    // Failed end marker
                        sbusFrameData->state = STATE_SBUS_WAIT_SYNC;
                        sbusDesyncCounter++;
                        DEBUG_SET(DEBUG_SBUS, DEBUG_SBUS_DESYNC_COUNTER, sbusDesyncCounter);
                        break;
                }

                // Frame seems sane, pass data to decoder
                if (!sbusFrameData->frameDone && frameValid) {
                    DEBUG_SET(DEBUG_SBUS, DEBUG_SBUS_FRAME_FLAGS, frame->channels.flags);

                    memcpy((void *)&sbusFrameData->frame, (void *)&sbusFrameData->buffer[0], SBUS_FRAME_SIZE);
                    sbusFrameData->frameDone = true;
                }
            }
            break;

        case STATE_SBUS_WAIT_SYNC:
            // Stay at this state and do nothing. Exit will be handled before byte is processed if the
            // inter-frame gap is long enough
            break;
    }
}

static uint8_t sbusFrameStatus(rxRuntimeConfig_t *rxRuntimeConfig)
{
    sbusFrameData_t *sbusFrameData = rxRuntimeConfig->frameData;

    if (!sbusFrameData->frameDone) {
        return RX_FRAME_PENDING;
    }

    // Decode channel data and store return value
    const uint8_t retValue = sbusChannelsDecode(rxRuntimeConfig, (void *)&sbusFrameData->frame.channels);

    // Reset the frameDone flag - tell ISR that we're ready to receive next frame
    sbusFrameData->frameDone = false;

    // Calculate "virtual link quality based on packet loss metric"
    if (retValue & RX_FRAME_COMPLETE) {
        lqTrackerAccumulate(rxRuntimeConfig->lqTracker, ((retValue & RX_FRAME_DROPPED) || (retValue & RX_FRAME_FAILSAFE)) ? 0 : RSSI_MAX_VALUE);
    }

    return retValue;
}

static bool sbusInitEx(const rxConfig_t *rxConfig, rxRuntimeConfig_t *rxRuntimeConfig, uint32_t sbusBaudRate)
{
    static uint16_t sbusChannelData[SBUS_MAX_CHANNEL];
    static sbusFrameData_t sbusFrameData;

    rxRuntimeConfig->channelData = sbusChannelData;
    rxRuntimeConfig->frameData = &sbusFrameData;

    sbusChannelsInit(rxRuntimeConfig);

    rxRuntimeConfig->channelCount = SBUS_MAX_CHANNEL;

    rxRuntimeConfig->rcFrameStatusFn = sbusFrameStatus;

    const serialPortConfig_t *portConfig = findSerialPortConfig(FUNCTION_RX_SERIAL);
    if (!portConfig) {
        return false;
    }

#ifdef USE_TELEMETRY
    bool portShared = telemetryCheckRxPortShared(portConfig);
#else
    bool portShared = false;
#endif

    serialPort_t *sBusPort = openSerialPort(portConfig->identifier,
        FUNCTION_RX_SERIAL,
        sbusDataReceive,
        &sbusFrameData,
        sbusBaudRate,
        portShared ? MODE_RXTX : MODE_RX,
        SBUS_PORT_OPTIONS |
            (rxConfig->serialrx_inverted ? 0 : SERIAL_INVERTED) |
            (tristateWithDefaultOffIsActive(rxConfig->halfDuplex) ? SERIAL_BIDIR : 0)
        );

#ifdef USE_TELEMETRY
    if (portShared) {
        telemetrySharedPort = sBusPort;
    }
#endif

    return sBusPort != NULL;
}

bool sbusInit(const rxConfig_t *rxConfig, rxRuntimeConfig_t *rxRuntimeConfig)
{
    return sbusInitEx(rxConfig, rxRuntimeConfig, SBUS_BAUDRATE);
}

bool sbusInitFast(const rxConfig_t *rxConfig, rxRuntimeConfig_t *rxRuntimeConfig)
{
    return sbusInitEx(rxConfig, rxRuntimeConfig, SBUS_BAUDRATE_FAST);
}
#endif

/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */


#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "platform.h"

#ifdef USE_DSHOT

#include "build/debug.h"

#include "drivers/dma.h"
#include "drivers/dma_reqmap.h"
#include "drivers/io.h"
#include "drivers/nvic.h"
#include "drivers/rcc.h"
#include "drivers/time.h"
#include "drivers/timer.h"
#if defined(STM32F4)
#include "stm32f4xx.h"
#elif defined(STM32F3)
#include "stm32f30x.h"
#endif

#include "pwm_output.h"
#include "drivers/dshot.h"
#include "drivers/dshot_dpwm.h"
#include "drivers/dshot_command.h"

#include "pwm_output_dshot_shared.h"

#ifdef USE_DSHOT_TELEMETRY_STATS
#define DSHOT_TELEMETRY_QUALITY_WINDOW 1       // capture a rolling 1 second of packet stats
#define DSHOT_TELEMETRY_QUALITY_BUCKET_MS 100  // determines the granularity of the stats and the overall number of rolling buckets
#define DSHOT_TELEMETRY_QUALITY_BUCKET_COUNT (DSHOT_TELEMETRY_QUALITY_WINDOW * 1000 / DSHOT_TELEMETRY_QUALITY_BUCKET_MS)

typedef struct dshotTelemetryQuality_s {
    uint32_t packetCountSum;
    uint32_t invalidCountSum;
    uint32_t packetCountArray[DSHOT_TELEMETRY_QUALITY_BUCKET_COUNT];
    uint32_t invalidCountArray[DSHOT_TELEMETRY_QUALITY_BUCKET_COUNT];
    uint8_t lastBucketIndex;
}  dshotTelemetryQuality_t;

static FAST_RAM_ZERO_INIT dshotTelemetryQuality_t dshotTelemetryQuality[MAX_SUPPORTED_MOTORS];
#endif // USE_DSHOT_TELEMETRY_STATS

FAST_RAM_ZERO_INIT uint8_t dmaMotorTimerCount = 0;
#ifdef STM32F7
FAST_RAM_ZERO_INIT motorDmaTimer_t dmaMotorTimers[MAX_DMA_TIMERS];
FAST_RAM_ZERO_INIT motorDmaOutput_t dmaMotors[MAX_SUPPORTED_MOTORS];
#else
motorDmaTimer_t dmaMotorTimers[MAX_DMA_TIMERS];
motorDmaOutput_t dmaMotors[MAX_SUPPORTED_MOTORS];
#endif

#ifdef USE_DSHOT_TELEMETRY
uint32_t readDoneCount;

// TODO remove once debugging no longer needed
FAST_RAM_ZERO_INIT uint32_t dshotInvalidPacketCount;
FAST_RAM_ZERO_INIT uint32_t inputBuffer[GCR_TELEMETRY_INPUT_LEN];
FAST_RAM_ZERO_INIT uint32_t setDirectionMicros;
FAST_RAM_ZERO_INIT uint32_t inputStampUs;
#endif

motorDmaOutput_t *getMotorDmaOutput(uint8_t index)
{
    return &dmaMotors[index];
}

uint8_t getTimerIndex(TIM_TypeDef *timer)
{
    for (int i = 0; i < dmaMotorTimerCount; i++) {
        if (dmaMotorTimers[i].timer == timer) {
            return i;
        }
    }
    dmaMotorTimers[dmaMotorTimerCount++].timer = timer;
    return dmaMotorTimerCount - 1;
}


FAST_CODE void pwmWriteDshotInt(uint8_t index, uint16_t value)
{
    motorDmaOutput_t *const motor = &dmaMotors[index];

    if (!motor->configured) {
        return;
    }

    /*If there is a command ready to go overwrite the value and send that instead*/
    if (dshotCommandIsProcessing()) {
        value = dshotCommandGetCurrent(index);
#ifdef USE_DSHOT_TELEMETRY
        // reset telemetry debug statistics every time telemetry is enabled
        if (value == DSHOT_CMD_SIGNAL_LINE_CONTINUOUS_ERPM_TELEMETRY) {
            dshotInvalidPacketCount = 0;
            readDoneCount = 0;
        }
#endif
        if (value) {
            motor->protocolControl.requestTelemetry = true;
        }
    }

    motor->protocolControl.value = value;

    uint16_t packet = prepareDshotPacket(&motor->protocolControl);
    uint8_t bufferSize;

#ifdef USE_DSHOT_DMAR
    if (useBurstDshot) {
        bufferSize = loadDmaBuffer(&motor->timer->dmaBurstBuffer[timerLookupChannelIndex(motor->timerHardware->channel)], 4, packet);
        motor->timer->dmaBurstLength = bufferSize * 4;
    } else
#endif
    {
        bufferSize = loadDmaBuffer(motor->dmaBuffer, 1, packet);
        motor->timer->timerDmaSources |= motor->timerDmaSource;
#ifdef USE_FULL_LL_DRIVER
        xLL_EX_DMA_SetDataLength(motor->dmaRef, bufferSize);
        xLL_EX_DMA_EnableResource(motor->dmaRef);
#else
        xDMA_SetCurrDataCounter(motor->dmaRef, bufferSize);
        xDMA_Cmd(motor->dmaRef, ENABLE);
#endif
    }
}


#ifdef USE_DSHOT_TELEMETRY

void dshotEnableChannels(uint8_t motorCount);

static uint32_t decodeTelemetryPacket(uint32_t buffer[], uint32_t count)
{
    uint32_t start = micros();
    uint32_t value = 0;
    uint32_t oldValue = buffer[0];
    int bits = 0;
    int len;
    for (uint32_t i = 1; i <= count; i++) {
        if (i < count) {
            int diff = buffer[i] - oldValue;
            if (bits >= 21) {
                break;
            }
            len = (diff + 8) / 16;
        } else {
            len = 21 - bits;
        }

        value <<= len;
        value |= 1 << (len - 1);
        oldValue = buffer[i];
        bits += len;
    }
    if (bits != 21) {
        return 0xffff;
    }

    static const uint32_t decode[32] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 10, 11, 0, 13, 14, 15,
        0, 0, 2, 3, 0, 5, 6, 7, 0, 0, 8, 1, 0, 4, 12, 0 };

    uint32_t decodedValue = decode[value & 0x1f];
    decodedValue |= decode[(value >> 5) & 0x1f] << 4;
    decodedValue |= decode[(value >> 10) & 0x1f] << 8;
    decodedValue |= decode[(value >> 15) & 0x1f] << 12;

    uint32_t csum = decodedValue;
    csum = csum ^ (csum >> 8); // xor bytes
    csum = csum ^ (csum >> 4); // xor nibbles

    if ((csum & 0xf) != 0xf) {
        setDirectionMicros = micros() - start;
        return 0xffff;
    }
    decodedValue >>= 4;

    if (decodedValue == 0x0fff) {
        setDirectionMicros = micros() - start;
        return 0;
    }
    decodedValue = (decodedValue & 0x000001ff) << ((decodedValue & 0xfffffe00) >> 9);
    if (!decodedValue) {
        return 0xffff;
    }
    uint32_t ret = (1000000 * 60 / 100 + decodedValue / 2) / decodedValue;
    setDirectionMicros = micros() - start;
    return ret;
}

uint16_t getDshotTelemetry(uint8_t index)
{
    return dmaMotors[index].dshotTelemetryValue;
}

#endif

FAST_CODE void pwmDshotSetDirectionOutput(
    motorDmaOutput_t * const motor, bool output
#ifndef USE_DSHOT_TELEMETRY
#ifdef USE_FULL_LL_DRIVER
    , LL_TIM_OC_InitTypeDef* pOcInit, LL_DMA_InitTypeDef* pDmaInit
#else
    , TIM_OCInitTypeDef *pOcInit, DMA_InitTypeDef* pDmaInit
#endif
#endif
);

#ifdef USE_DSHOT_TELEMETRY
#ifdef USE_DSHOT_TELEMETRY_STATS
void updateDshotTelemetryQuality(dshotTelemetryQuality_t *qualityStats, bool packetValid, timeMs_t currentTimeMs)
{
    uint8_t statsBucketIndex = (currentTimeMs / DSHOT_TELEMETRY_QUALITY_BUCKET_MS) % DSHOT_TELEMETRY_QUALITY_BUCKET_COUNT;
    if (statsBucketIndex != qualityStats->lastBucketIndex) {
        qualityStats->packetCountSum -= qualityStats->packetCountArray[statsBucketIndex];
        qualityStats->invalidCountSum -= qualityStats->invalidCountArray[statsBucketIndex];
        qualityStats->packetCountArray[statsBucketIndex] = 0;
        qualityStats->invalidCountArray[statsBucketIndex] = 0;
        qualityStats->lastBucketIndex = statsBucketIndex;
    }
    qualityStats->packetCountSum++;
    qualityStats->packetCountArray[statsBucketIndex]++;
    if (!packetValid) {
        qualityStats->invalidCountSum++;
        qualityStats->invalidCountArray[statsBucketIndex]++;
    }
}
#endif // USE_DSHOT_TELEMETRY_STATS

FAST_CODE_NOINLINE bool pwmStartDshotMotorUpdate(void)
{
    if (!useDshotTelemetry) {
        return true;
    }
#ifdef USE_DSHOT_TELEMETRY_STATS
    const timeMs_t currentTimeMs = millis();
#endif
    const timeUs_t currentUs = micros();
    for (int i = 0; i < dshotPwmDevice.count; i++) {
        timeDelta_t usSinceInput = cmpTimeUs(currentUs, inputStampUs);
        if (usSinceInput >= 0 && usSinceInput < dmaMotors[i].dshotTelemetryDeadtimeUs) {
            return false;
        }
        if (dmaMotors[i].isInput) {
#ifdef USE_FULL_LL_DRIVER
            uint32_t edges = GCR_TELEMETRY_INPUT_LEN - xLL_EX_DMA_GetDataLength(dmaMotors[i].dmaRef);
#else
            uint32_t edges = GCR_TELEMETRY_INPUT_LEN - xDMA_GetCurrDataCounter(dmaMotors[i].dmaRef);
#endif

#ifdef USE_FULL_LL_DRIVER
            LL_EX_TIM_DisableIT(dmaMotors[i].timerHardware->tim, dmaMotors[i].timerDmaSource);
#else
            TIM_DMACmd(dmaMotors[i].timerHardware->tim, dmaMotors[i].timerDmaSource, DISABLE);
#endif

            uint16_t value = 0xffff;

            if (edges > MIN_GCR_EDGES) {
                readDoneCount++;
                value = decodeTelemetryPacket(dmaMotors[i].dmaBuffer, edges);
                
#ifdef USE_DSHOT_TELEMETRY_STATS
                bool validTelemetryPacket = false;
#endif
                if (value != 0xffff) {
                    dmaMotors[i].dshotTelemetryValue = value;
                    dmaMotors[i].dshotTelemetryActive = true;
                    if (i < 4) {
                        DEBUG_SET(DEBUG_DSHOT_RPM_TELEMETRY, i, value);
                    }
#ifdef USE_DSHOT_TELEMETRY_STATS
                    validTelemetryPacket = true;
#endif
                } else {
                    dshotInvalidPacketCount++;
                    if (i == 0) {
                        memcpy(inputBuffer,dmaMotors[i].dmaBuffer,sizeof(inputBuffer));
                    }
                }
#ifdef USE_DSHOT_TELEMETRY_STATS
                updateDshotTelemetryQuality(&dshotTelemetryQuality[i], validTelemetryPacket, currentTimeMs);
            }
#endif
        }
        pwmDshotSetDirectionOutput(&dmaMotors[i], true);
    }
    inputStampUs = 0;
    dshotEnableChannels(dshotPwmDevice.count);
    return true;
}

bool isDshotMotorTelemetryActive(uint8_t motorIndex)
{
    return dmaMotors[motorIndex].dshotTelemetryActive;
}

bool isDshotTelemetryActive(void)
{
    for (unsigned i = 0; i < dshotPwmDevice.count; i++) {
        if (!isDshotMotorTelemetryActive(i)) {
            return false;
        }
    }
    return true;
}

#ifdef USE_DSHOT_TELEMETRY_STATS
int16_t getDshotTelemetryMotorInvalidPercent(uint8_t motorIndex)
{
    int16_t invalidPercent = 0;

    if (dmaMotors[motorIndex].dshotTelemetryActive) {
        const uint32_t totalCount = dshotTelemetryQuality[motorIndex].packetCountSum;
        const uint32_t invalidCount = dshotTelemetryQuality[motorIndex].invalidCountSum;
        if (totalCount > 0) {
            invalidPercent = lrintf(invalidCount * 10000.0f / totalCount);
        }
    } else {
        invalidPercent = 10000;  // 100.00%
    }
    return invalidPercent;
}
#endif // USE_DSHOT_TELEMETRY_STATS
#endif // USE_DSHOT_TELEMETRY
#endif // USE_DSHOT

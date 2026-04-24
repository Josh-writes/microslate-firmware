#include "BatteryMonitor.h"
#include <Arduino.h>
#include <cmath>
#include <driver/adc.h>
#include <esp_adc_cal.h>

BatteryMonitor::BatteryMonitor(uint8_t adcPin, float dividerMultiplier)
  : _adcPin(adcPin), _dividerMultiplier(dividerMultiplier)
{
}

uint16_t BatteryMonitor::readPercentage() const
{
    return percentageFromMillivolts(readMillivolts());
}

uint16_t BatteryMonitor::readMillivolts() const
{
    // Take multiple samples with delays between them to span different noise windows,
    // then use the median to reject outliers from SPI/BLE/charging noise.
    constexpr int NUM_SAMPLES = 7;
    uint16_t samples[NUM_SAMPLES];
    for (int i = 0; i < NUM_SAMPLES; i++) {
        samples[i] = adc1_get_raw(ADC1_CHANNEL_0);
        if (i < NUM_SAMPLES - 1) delayMicroseconds(500);
    }
    // Simple insertion sort for median
    for (int i = 1; i < NUM_SAMPLES; i++) {
        uint16_t key = samples[i];
        int j = i - 1;
        while (j >= 0 && samples[j] > key) {
            samples[j + 1] = samples[j];
            j--;
        }
        samples[j + 1] = key;
    }
    const int raw = samples[NUM_SAMPLES / 2];  // median
    const uint32_t mv = millivoltsFromRawAdc(raw);
    return static_cast<uint32_t>(mv * _dividerMultiplier);
}

uint16_t BatteryMonitor::readRawMillivolts() const
{
    return adc1_get_raw(ADC1_CHANNEL_0);
}

double BatteryMonitor::readVolts() const
{
    return static_cast<double>(readMillivolts()) / 1000.0;
}

uint16_t BatteryMonitor::percentageFromMillivolts(uint16_t millivolts)
{
    // LiPo discharge curve lookup table (voltage in mV → percentage).
    // Points sampled from standard single-cell LiPo discharge data.
    // Voltage range: 3400 mV (empty) to 4200 mV (full).
    // Values outside range are clamped to 0% or 100%.
    static const struct { uint16_t mv; uint8_t pct; } table[] = {
        { 4200, 100 },
        { 4150,  97 },
        { 4110,  93 },
        { 4080,  89 },
        { 4050,  85 },
        { 4000,  79 },
        { 3950,  72 },
        { 3900,  64 },
        { 3850,  55 },
        { 3800,  46 },
        { 3750,  37 },
        { 3700,  28 },
        { 3650,  20 },
        { 3600,  13 },
        { 3550,   7 },
        { 3500,   3 },
        { 3400,   0 },
    };
    constexpr int TABLE_SIZE = sizeof(table) / sizeof(table[0]);

    if (millivolts >= table[0].mv) return 100;
    if (millivolts <= table[TABLE_SIZE - 1].mv) return 0;

    // Linear interpolation between surrounding entries
    for (int i = 0; i < TABLE_SIZE - 1; i++) {
        if (millivolts >= table[i + 1].mv) {
            const float span = table[i].mv - table[i + 1].mv;
            const float frac = (millivolts - table[i + 1].mv) / span;
            const float pct = table[i + 1].pct + frac * (table[i].pct - table[i + 1].pct);
            return static_cast<uint16_t>(round(pct));
        }
    }
    return 0;
}

uint16_t BatteryMonitor::millivoltsFromRawAdc(uint16_t adc_raw)
{
    esp_adc_cal_characteristics_t adc_chars;
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, &adc_chars);
    return esp_adc_cal_raw_to_voltage(adc_raw, &adc_chars);
}

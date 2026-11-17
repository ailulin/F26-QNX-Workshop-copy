#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <unistd.h>
#include "max30102.h"
#include "alarm.h"

/* SparkFun Example5/heartRate: average a few valid BPM readings before display */
#define RATE_SIZE 4

static int coid;
static int32_t hr_rates[RATE_SIZE];
static uint8_t hr_spot;
static uint8_t hr_count;
static int32_t spo2_rates[RATE_SIZE];
static uint8_t spo2_spot;
static uint8_t spo2_count;

static bool connect_channel(void)
{
	coid = name_open(ALARM_NAME, 0);
	if (coid == -1) {
		perror("name_open");
		return false;
	}
	printf("Connected to alarm manager! coid=%d\n", coid);
	fflush(stdout);
	return true;
}

static bool send_pulse(int code, int command_value)
{
	return MsgSendPulse(coid, -1, code, command_value) != -1;
}

static int32_t push_average(int32_t *buf, uint8_t *spot, uint8_t *count,
			    int32_t value)
{
	int32_t sum = 0;
	uint8_t i;

	buf[*spot] = value;
	*spot = (uint8_t)((*spot + 1) % RATE_SIZE);
	if (*count < RATE_SIZE) {
		(*count)++;
	}

	for (i = 0; i < *count; i++) {
		sum += buf[i];
	}
	return sum / *count;
}

int main(void)
{
	while (!connect_channel()) {
		usleep(100000);
	}

	while (!MAX30102_begin()) {
		usleep(100000);
	}

	/*
	 * Match SparkFun Example8_SPO2 setup:
	 *   brightness=60, avg=4, Red+IR, 100 SPS, pw=411, adcRange=4096
	 * Effective rate = 100/4 = 25 Hz == FreqS in Maxim spo2_algorithm.
	 */
	bool ok = MAX30102_sensorConfiguration(
		/*ledBrightness=*/60,
		/*sampleAverage=*/MAX30102_SAMPLEAVG_4,
		/*ledMode=*/MAX30102_MODE_RED_IR,
		/*sampleRate=*/MAX30102_SAMPLERATE_100,
		/*pulseWidth=*/MAX30102_PULSEWIDTH_411,
		/*adcRange=*/MAX30102_ADCRANGE_4096);

	if (ok) {
		send_pulse(EVENT_SENSOR_CONFIGURED, 1);
		printf("[INFO] Sensor configured (SparkFun Example8 settings)\n");
	} else {
		send_pulse(EVENT_SENSOR_CONFIGURED, 0);
		fprintf(stderr, "[ERROR] Sensor configuration failed\n");
		return 1;
	}
	fflush(stdout);

	for (;;) {
		int32_t SPO2 = -999;
		int8_t SPO2Valid = 0;
		int32_t heartRate = -999;
		int8_t heartRateValid = 0;

		MAX30102_heartrateAndOxygenSaturation(&SPO2, &SPO2Valid,
						      &heartRate, &heartRateValid);

		/*
		 * Only publish algorithm-validated readings (SparkFun prints
		 * "--" when valid flags are 0). Send -1 so alarm-manager keeps
		 * its sample watchdog alive without ingesting garbage.
		 */
		if (heartRateValid) {
			int32_t avg = push_average(hr_rates, &hr_spot, &hr_count,
						   heartRate);
			send_pulse(EVENT_HEARTRATE, avg);
			printf("HR=%d (avg=%d) ", heartRate, avg);
		} else {
			hr_count = 0;
			hr_spot = 0;
			send_pulse(EVENT_HEARTRATE, -1);
			printf("HR=-- ");
		}

		if (SPO2Valid) {
			int32_t avg = push_average(spo2_rates, &spo2_spot,
						   &spo2_count, SPO2);
			send_pulse(EVENT_SPO2, avg);
			printf("SpO2=%d (avg=%d)\n", SPO2, avg);
		} else {
			spo2_count = 0;
			spo2_spot = 0;
			send_pulse(EVENT_SPO2, -1);
			printf("SpO2=--\n");
		}
		fflush(stdout);
	}
}

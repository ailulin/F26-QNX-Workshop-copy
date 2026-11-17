#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include "alarm_manager.h"

#define BINARY_PATH "/home/qnxuser/pulse-oximeter"
#define BUFFER_SIZE 512
#define ALARM_COUNT 4
#define HR_LOW_THRESHOLD 40
#define HR_HIGH_THRESHOLD 170
#define SPO2_LOW_THRESHOLD 85
#define TIMEOUT_SEC 1.5

/* Smoothing / outlier rejection */
#define FILTER_LEN 5
#define MAX_HR_JUMP 25
#define MAX_SPO2_JUMP 6
#define MIN_FILTER_SAMPLES 3
#define REJECT_RESET_COUNT 4

/* Physiological plausibility for finger PPG (rejects algorithm garbage) */
#define HR_VALID_MIN 45
#define HR_VALID_MAX 180
#define SPO2_VALID_MIN 70
#define SPO2_VALID_MAX 100

/* Require this many consecutive bad samples before raising an alarm */
#define ALARM_CONFIRM_COUNT 3

typedef enum {
	ALARM_SPO2_LOW = 0,
	ALARM_HR_LOW,
	ALARM_HR_HIGH,
	ALARM_SENSOR_DISCONNECT
} Alarm_Id_t;

typedef enum {
	PRIORITY_LOW = 0,
	PRIORITY_MEDIUM,
	PRIORITY_HIGH
} Alarm_Priority_t;

typedef enum {
	STATE_INACTIVE = 0,
	STATE_ACTIVE,
	STATE_SILENCED
} Alarm_State_t;

typedef struct {
	Alarm_Id_t id;
	Alarm_Priority_t priority;
	Alarm_State_t state;
	char message[BUFFER_SIZE];
} Alarm_t;

static Alarm_t alarms[ALARM_COUNT] = {
	[ALARM_SPO2_LOW] = {
		.id = ALARM_SPO2_LOW,
		.priority = PRIORITY_HIGH,
		.state = STATE_INACTIVE,
		.message = "[Alarm] Low SpO2 detected"
	},
	[ALARM_HR_LOW] = {
		.id = ALARM_HR_LOW,
		.priority = PRIORITY_MEDIUM,
		.state = STATE_INACTIVE,
		.message = "[Alarm] Low heart rate detected"
	},
	[ALARM_HR_HIGH] = {
		.id = ALARM_HR_HIGH,
		.priority = PRIORITY_HIGH,
		.state = STATE_INACTIVE,
		.message = "[Alarm] High heart rate detected"
	},
	[ALARM_SENSOR_DISCONNECT] = {
		.id = ALARM_SENSOR_DISCONNECT,
		.priority = PRIORITY_LOW,
		.state = STATE_INACTIVE,
		.message = "[Alarm] Sensor disconnected"
	}
};

static pid_t tracked_pulse_pid = -1;
static bool disconnect_restart_attempted = false;

static int32_t hr_hist[FILTER_LEN];
static int32_t spo2_hist[FILTER_LEN];
static int hr_count = 0;
static int spo2_count = 0;
static int32_t display_hr = -1;
static int32_t display_spo2 = -1;

static int hr_low_streak = 0;
static int hr_high_streak = 0;
static int spo2_low_streak = 0;
static int hr_reject_streak = 0;
static int spo2_reject_streak = 0;
static pthread_mutex_t vitals_lock = PTHREAD_MUTEX_INITIALIZER;

extern char **environ;

static bool hr_reading_valid(int32_t heartRate)
{
	return heartRate >= HR_VALID_MIN && heartRate <= HR_VALID_MAX;
}

static bool spo2_reading_valid(int32_t SPO2)
{
	return SPO2 >= SPO2_VALID_MIN && SPO2 <= SPO2_VALID_MAX;
}

static void reset_hr_filter(void)
{
	hr_count = 0;
	display_hr = -1;
	hr_reject_streak = 0;
	hr_low_streak = 0;
	hr_high_streak = 0;
}

static void reset_spo2_filter(void)
{
	spo2_count = 0;
	display_spo2 = -1;
	spo2_reject_streak = 0;
	spo2_low_streak = 0;
}

static int32_t abs32(int32_t v)
{
	return v < 0 ? -v : v;
}

static int32_t median_of(const int32_t *src, int n)
{
	int32_t tmp[FILTER_LEN];
	int i, j;

	if (n <= 0) {
		return -1;
	}
	if (n > FILTER_LEN) {
		n = FILTER_LEN;
	}

	memcpy(tmp, src, (size_t)n * sizeof(int32_t));
	for (i = 1; i < n; i++) {
		int32_t key = tmp[i];
		j = i - 1;
		while (j >= 0 && tmp[j] > key) {
			tmp[j + 1] = tmp[j];
			j--;
		}
		tmp[j + 1] = key;
	}

	return tmp[n / 2];
}

static void push_sample(int32_t *hist, int *count, int32_t value)
{
	if (*count < FILTER_LEN) {
		hist[*count] = value;
		(*count)++;
		return;
	}

	memmove(&hist[0], &hist[1], (FILTER_LEN - 1) * sizeof(int32_t));
	hist[FILTER_LEN - 1] = value;
}

void ingest_heart_rate(int32_t raw_hr)
{
	pthread_mutex_lock(&vitals_lock);

	if (!hr_reading_valid(raw_hr)) {
		/* Implausible raw values (e.g. 250) never enter the filter. */
		hr_reject_streak++;
		if (hr_reject_streak >= REJECT_RESET_COUNT) {
			reset_hr_filter();
		}
		pthread_mutex_unlock(&vitals_lock);
		return;
	}

	if (display_hr > 0 && abs32(raw_hr - display_hr) > MAX_HR_JUMP) {
		hr_reject_streak++;
		if (hr_reject_streak >= REJECT_RESET_COUNT) {
			/* Locked onto a bad median — wipe and start over. */
			reset_hr_filter();
			push_sample(hr_hist, &hr_count, raw_hr);
		}
		pthread_mutex_unlock(&vitals_lock);
		return;
	}

	hr_reject_streak = 0;
	push_sample(hr_hist, &hr_count, raw_hr);
	if (hr_count >= MIN_FILTER_SAMPLES) {
		display_hr = median_of(hr_hist, hr_count);
	}
	pthread_mutex_unlock(&vitals_lock);
}

void ingest_spo2(int32_t raw_spo2)
{
	pthread_mutex_lock(&vitals_lock);

	if (!spo2_reading_valid(raw_spo2)) {
		spo2_reject_streak++;
		if (spo2_reject_streak >= REJECT_RESET_COUNT) {
			reset_spo2_filter();
		}
		pthread_mutex_unlock(&vitals_lock);
		return;
	}

	if (display_spo2 > 0 && abs32(raw_spo2 - display_spo2) > MAX_SPO2_JUMP) {
		spo2_reject_streak++;
		if (spo2_reject_streak >= REJECT_RESET_COUNT) {
			reset_spo2_filter();
			push_sample(spo2_hist, &spo2_count, raw_spo2);
		}
		pthread_mutex_unlock(&vitals_lock);
		return;
	}

	spo2_reject_streak = 0;
	push_sample(spo2_hist, &spo2_count, raw_spo2);
	if (spo2_count >= MIN_FILTER_SAMPLES) {
		display_spo2 = median_of(spo2_hist, spo2_count);
	}
	pthread_mutex_unlock(&vitals_lock);
}

int32_t get_display_hr(void)
{
	int32_t value;
	pthread_mutex_lock(&vitals_lock);
	value = display_hr;
	pthread_mutex_unlock(&vitals_lock);
	return value;
}

int32_t get_display_spo2(void)
{
	int32_t value;
	pthread_mutex_lock(&vitals_lock);
	value = display_spo2;
	pthread_mutex_unlock(&vitals_lock);
	return value;
}

static void update_streak(bool condition, int *streak, Alarm_State_t *state)
{
	if (condition) {
		if (*streak < ALARM_CONFIRM_COUNT) {
			(*streak)++;
		}
	} else {
		*streak = 0;
	}

	*state = (*streak >= ALARM_CONFIRM_COUNT) ? STATE_ACTIVE : STATE_INACTIVE;
}

bool update_alarm_states(double elapsed)
{
	int32_t hr;
	int32_t spo2;

	pthread_mutex_lock(&vitals_lock);
	hr = display_hr;
	spo2 = display_spo2;

	const bool hr_ok = hr_reading_valid(hr);
	const bool spo2_ok = spo2_reading_valid(spo2);

	for (int i = 0; i < ALARM_COUNT; i++) {
		switch (alarms[i].id) {
		case ALARM_SPO2_LOW:
			update_streak(spo2_ok && spo2 < SPO2_LOW_THRESHOLD,
				      &spo2_low_streak, &alarms[i].state);
			break;

		case ALARM_HR_HIGH:
			update_streak(hr_ok && hr > HR_HIGH_THRESHOLD,
				      &hr_high_streak, &alarms[i].state);
			break;

		case ALARM_HR_LOW:
			update_streak(hr_ok && hr < HR_LOW_THRESHOLD,
				      &hr_low_streak, &alarms[i].state);
			break;

		case ALARM_SENSOR_DISCONNECT:
			alarms[i].state = (elapsed > TIMEOUT_SEC)
				? STATE_ACTIVE
				: STATE_INACTIVE;

			if (alarms[i].state == STATE_ACTIVE) {
				if (!disconnect_restart_attempted) {
					disconnect_restart_attempted = true;
					printf("[WARN] Sensor timeout (%.2fs). Restarting pulse-oximeter...\n",
					       elapsed);
					pthread_mutex_unlock(&vitals_lock);
					tracked_pulse_pid = restart_pulse_oximeter(tracked_pulse_pid);
					pthread_mutex_lock(&vitals_lock);
				}
			} else {
				disconnect_restart_attempted = false;
			}
			break;

		default:
			alarms[i].state = STATE_INACTIVE;
			break;
		}
	}
	pthread_mutex_unlock(&vitals_lock);

	return true;
}

void display_alarms(void)
{
	int inactive = 0;

	for (int i = 0; i < ALARM_COUNT; i++) {
		if (alarms[i].state == STATE_ACTIVE) {
			printf("%s; ", alarms[i].message);
		} else if (alarms[i].state == STATE_INACTIVE) {
			inactive++;
		}
	}

	if (inactive == ALARM_COUNT) {
		printf("[Alarm] System OK");
	}
}

pid_t restart_pulse_oximeter(pid_t current_pid)
{
	(void)current_pid;

	system("slay -f pulse-oximeter >/dev/null 2>&1");
	usleep(200000);

	pid_t new_pid = -1;
	char *argv[] = { "pulse-oximeter", NULL };
	int ret = posix_spawn(
		&new_pid,
		BINARY_PATH,
		NULL,
		NULL,
		argv,
		environ
	);

	if (ret != 0) {
		fprintf(stderr, "posix_spawn: %s\n", strerror(ret));
		return -1;
	}

	printf("[SYSTEM] Restarted pulse oximeter: PID=%d\n", (int)new_pid);
	tracked_pulse_pid = new_pid;
	return new_pid;
}

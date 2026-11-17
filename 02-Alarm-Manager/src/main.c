#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <unistd.h>
#include <alarm.h>
#include <alarm_manager.h>
#include <time.h>
#include <pthread.h>

static struct timespec last_sample_time;
static bool has_sample = false;
static pthread_mutex_t sample_lock = PTHREAD_MUTEX_INITIALIZER;

static double seconds_since(const struct timespec *then)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - then->tv_sec) +
	       (now.tv_nsec - then->tv_nsec) / 1e9;
}

void *update_alarm_thread(void *arg)
{
	(void)arg;

	for (;;) {
		double local_elapsed;

		pthread_mutex_lock(&sample_lock);
		if (has_sample) {
			local_elapsed = seconds_since(&last_sample_time);
		} else {
			local_elapsed = 0.0;
		}
		pthread_mutex_unlock(&sample_lock);

		if (!update_alarm_states(local_elapsed)) {
			perror("Failed to update alarm state");
			break;
		}

		printf("\033[1;31m[HEART RATE]\033[0m %d BPM", get_display_hr());
		printf("; \033[1;34m[SPO2]\033[0m %d; ", get_display_spo2());
		display_alarms();
		printf("\n");
		fflush(stdout);

		usleep(1000000);
	}

	return NULL;
}

name_attach_t *init_channel(void)
{
	name_attach_t *attach = name_attach(NULL, ALARM_NAME, 0);
	if (attach == NULL) {
		perror("name_attach");
	}
	return attach;
}

int main(void)
{
	pthread_t update_alarm_id;
	int resp = pthread_create(&update_alarm_id, NULL, update_alarm_thread, NULL);
	if (resp != 0) {
		perror("Thread creation failed");
		return 1;
	}

	name_attach_t *attach = init_channel();
	if (attach == NULL) {
		return 1;
	}

	struct _pulse pulse_msg;
	int rcvid;

	for (;;) {
		rcvid = MsgReceive(attach->chid, &pulse_msg, sizeof(pulse_msg), NULL);
		if (rcvid == -1) {
			continue;
		}
		if (rcvid != 0) {
			MsgReply(rcvid, 0, NULL, 0);
		}

		switch (pulse_msg.code) {
		case EVENT_SENSOR_CONFIGURED:
			if (pulse_msg.value.sival_int == 1) {
				printf("[INFO] Sensor reported successful configuration.\n");
			} else {
				printf("[WARN] Sensor reported configuration FAILURE.\n");
			}
			break;

		case EVENT_HEARTRATE:
			pthread_mutex_lock(&sample_lock);
			clock_gettime(CLOCK_MONOTONIC, &last_sample_time);
			has_sample = true;
			ingest_heart_rate(pulse_msg.value.sival_int);
			pthread_mutex_unlock(&sample_lock);
			break;

		case EVENT_SPO2:
			pthread_mutex_lock(&sample_lock);
			clock_gettime(CLOCK_MONOTONIC, &last_sample_time);
			has_sample = true;
			ingest_spo2(pulse_msg.value.sival_int);
			pthread_mutex_unlock(&sample_lock);
			break;

		case _PULSE_CODE_DISCONNECT:
			printf("[SYSTEM] Sender disconnected from the channel.\n");
			ConnectDetach(pulse_msg.scoid);
			break;

		default:
			printf("[SYSTEM] Received unknown pulse code: %d\n", pulse_msg.code);
			break;
		}
	}
}

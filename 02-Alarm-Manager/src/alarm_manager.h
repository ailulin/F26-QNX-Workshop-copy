#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

void ingest_heart_rate(int32_t raw_hr);
void ingest_spo2(int32_t raw_spo2);
int32_t get_display_hr(void);
int32_t get_display_spo2(void);

bool update_alarm_states(double elapsed);
void display_alarms(void);
pid_t restart_pulse_oximeter(pid_t current_pid);

#endif

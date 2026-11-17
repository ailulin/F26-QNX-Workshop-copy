#ifndef PPG_HR_H
#define PPG_HR_H
#include <stdint.h>
#define PPG_WINDOW 200
/* Independent HR path; raw Red/IR remain untouched for Maxim SpO2. */
typedef struct {
    double x1, x2, y1, y2;
} ppg_biquad;
typedef struct {
    ppg_biquad hp, lp;
    double values[PPG_WINDOW], times[PPG_WINDOW];
    unsigned count;
    double last_ms;
    int contact;
} ppg_hr;
void ppg_hr_reset(ppg_hr *s);
void ppg_hr_push(ppg_hr *s, uint32_t ir, double ms, int timing_ok);
int ppg_hr_result(const ppg_hr *s, unsigned *peaks, double *hz);
#endif

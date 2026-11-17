#include "ppg_hr.h"
#include <math.h>
#include <string.h>

void ppg_hr_reset(ppg_hr *s) { memset(s, 0, sizeof(*s)); }

/* Bilinear second-order Butterworth sections, fs=25 Hz. */
static double section(ppg_biquad *s, double x, double cutoff, int high)
{
    double k = tan(3.141592653589793 * cutoff / 25.0);
    double norm = 1.0 / (1.0 + sqrt(2.0)*k + k*k);
    double b0 = (high ? 1.0 : k*k)*norm;
    double b1 = (high ? -2.0 : 2.0)*b0;
    double a1 = 2.0*(k*k - 1.0)*norm;
    double a2 = (1.0 - sqrt(2.0)*k + k*k)*norm;
    double y = b0*x + b1*s->x1 + b0*s->x2 - a1*s->y1 - a2*s->y2;
    s->x2=s->x1; s->x1=x; s->y2=s->y1; s->y1=y;
    return y;
}

void ppg_hr_push(ppg_hr *s, uint32_t ir, double ms, int timing_ok)
{
    if (!timing_ok || ir < 20000 || ir > 260000 ||
        (s->contact && (ms-s->last_ms < 20 || ms-s->last_ms > 80))) {
        ppg_hr_reset(s);
        return;
    }
    if (!s->contact) {
        /* Initialize high-pass at the current DC level, zero output. */
        s->hp.x1=s->hp.x2=ir;
        s->contact=1;
    }
    s->last_ms=ms;
    double v = -section(&s->lp, section(&s->hp, ir, 0.1, 1), 6.0, 0);
    if (s->count == PPG_WINDOW) {
        memmove(s->values, s->values+1, (PPG_WINDOW-1)*sizeof(double));
        memmove(s->times, s->times+1, (PPG_WINDOW-1)*sizeof(double));
        s->count--;
    }
    s->values[s->count]=v; s->times[s->count++]=ms;
}

int ppg_hr_result(const ppg_hr *s, unsigned *peaks, double *hz)
{
    *peaks=0; *hz=0;
    if (s->count < 2) return -1;
    *hz = 1000.0*(s->count-1)/(s->times[s->count-1]-s->times[0]);
    if (s->count < PPG_WINDOW || fabs(*hz-25.0)>1.25) return -1;
    /* Discard first two seconds of window: startup/edge protection.
       No second moving average on this already band-limited HR path. */
    double mean=0, power=0;
    for (unsigned i=50; i<s->count; ++i) mean+=s->values[i];
    mean/=(s->count-50);
    for (unsigned i=50; i<s->count; ++i) power+=pow(s->values[i]-mean,2);
    double rms=sqrt(power/(s->count-50));
    if (rms<30) return -1;
    unsigned locs[PPG_WINDOW], n=0;
    for (unsigned i=51; i+1<s->count; ++i) {
        if (s->values[i] <= mean+0.5*rms || s->values[i]<=s->values[i-1] ||
            s->values[i]<s->values[i+1]) continue;
        if (n && i-locs[n-1]<8) {
            if (s->values[i]>s->values[locs[n-1]]) locs[n-1]=i;
        } else locs[n++]=i;
    }
    *peaks=n;
    if (n<3 || s->times[s->count-1]-s->times[locs[n-1]]>1540) return -1;
    double intervals[5], sum=0;
    unsigned m=0;
    for (unsigned j=n-1; j>0 && m<5; --j) {
        double dt=s->times[locs[j]]-s->times[locs[j-1]];
        if (dt<60000.0/180 || dt>1540) return -1;
        intervals[m++]=dt; sum+=dt;
    }
    double avg=sum/m;
    for (unsigned j=0;j<m;++j) if (fabs(intervals[j]-avg)>0.25*avg) return -1;
    int bpm=(int)lround(60000.0/avg);
    return bpm>=40 && bpm<=180 ? bpm : -1;
}

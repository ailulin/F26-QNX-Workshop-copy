#include "ppg_hr.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static void wave(ppg_hr *s, double bpm, double rate, double start, int samples) {
    for (int i=0;i<samples;++i) {
        double t=i/rate;
        ppg_hr_push(s, (uint32_t)(80000+2000*sin(2*3.141592653589793*bpm*t/60)), start+t*1000, 1);
    }
}
int main(void) {
    ppg_hr s; unsigned peaks; double hz;
    for (int bpm=40;bpm<=180;bpm+=10) {
        ppg_hr_reset(&s); wave(&s,bpm,25,1000,15000);
        int result=ppg_hr_result(&s,&peaks,&hz);
        if (bpm <= 160) assert(abs(result-bpm)<=3);
        else assert(result == -1 || abs(result-bpm)<=5);
        assert(fabs(hz-25)<0.01);
    }
    ppg_hr_reset(&s); wave(&s,75,25,1000,100);
    assert(ppg_hr_result(&s,&peaks,&hz)==-1);
    ppg_hr_reset(&s); wave(&s,75,20,1000,500);
    assert(ppg_hr_result(&s,&peaks,&hz)==-1);
    ppg_hr_reset(&s); wave(&s,250,25,1000,500);
    assert(ppg_hr_result(&s,&peaks,&hz)==-1);
    ppg_hr_reset(&s);
    for(int i=0;i<300;++i) ppg_hr_push(&s,80000,1000+40*i,1);
    assert(ppg_hr_result(&s,&peaks,&hz)==-1);
    wave(&s,75,25,20000,500);
    ppg_hr_push(&s,80000,41000,0);
    assert(ppg_hr_result(&s,&peaks,&hz)==-1);
    ppg_hr_push(&s,0,42000,1);
    assert(ppg_hr_result(&s,&peaks,&hz)==-1);
    puts("PPG synthetic tests passed (including 10-minute input per rate)");
}

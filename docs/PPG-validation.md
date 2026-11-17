# PPG recording and validation

Work is in the copy, on local `dev/workshop`, based on the copy's
`origin/master` (the remote has no `dev/workshop`). Main contains setup only.

## Review and pipeline

Already present: 100 SPS / averaging 4 configuration, IR contact/DC gates,
Maxim four-point moving average, eight-sample peak separation, three-peak HR
minimum, 40–180 BPM and 70–100% validity checks, invalid IPC values of -1,
four-reading display averages, alarm filtering/confirmation/restart logic.

New HR path: untouched raw IR -> contact and receipt timing checks -> continuous
second-order Butterworth high-pass at 0.1 Hz -> second-order Butterworth low-pass
at 6 Hz, both designed for 25 Hz -> inverted waveform -> eight-second rolling
window -> discard first two seconds -> RMS/amplitude gate -> local peaks with
eight-sample minimum separation -> at least three peaks -> most recent up to
five consecutive intervals using measured receipt times -> interval bounds and
25% consistency gate -> 40–180 BPM, otherwise -1. No additional moving average
is applied to this path. Initial DC state avoids a large high-pass step.
Filters reset on lost contact, bad timing, FIFO backlog/overflow and read failure.
A complete clean eight-second window is required after a reset. The existing
four-result average still affects IPC display, separately from calculated CSV HR.

SpO2 retains Maxim's detrending, four-point average and valley positions, then
uses the original raw Red/IR AC/DC ratio and lookup table. Its HR result is
replaced only after Maxim completes. Ratio products now use 64-bit intermediates
to avoid signed overflow. Existing SpO2 gates remain; an additional clean-window
timing gate rejects samples after gaps. SpO2 validity is independent of HR.
Alarm files, thresholds, IPC codes and sensor configuration are unchanged.

## On-board ten-minute run

Build with the QNX SDP 8 environment active using `make build`, then deploy
using your existing process. Start alarm-manager first. Choose writable log
paths (files are overwritten):

```sh
PPG_RAW_CSV=/home/qnxuser/raw.csv \
PPG_MEASUREMENTS_CSV=/home/qnxuser/measurements.csv \
PPG_DURATION_SECONDS=600 PPG_QUIET=1 ./pulse-oximeter
```

Omit duration for continuous recording; omit CSV variables to disable logging.
SIGINT/SIGTERM request shutdown and close buffered files. Duration starts after
sensor configuration, checked between calculation windows, so may overrun by a
window. Receive waits are bounded at one second; startup connection/retry is
not included in the recording duration. Quiet suppresses readings, not faults.
Do not use identical paths for the two logs. Files use monotonic milliseconds,
not wall time. CSV uses stdio buffers (64 KiB raw, 16 KiB results), no per-sample
flush or console print. Disk writes may block; measure latency on target storage.
Abrupt termination can lose buffered rows. Storage capacity bounds duration.

Raw CSV records host FIFO-read completion time, raw Red/IR, FIFO batch size,
and whether the batch has reliable timing. MAX30102 supplies no acquisition
timestamps. Multiple queued readings cannot be assigned actual acquisition times;
these are logged as received, never reconstructed as uniformly spaced samples,
and invalidate the HR window. Polling/IRQ latency still affects single readings.
Every ten seconds sample counts versus elapsed receipt time measure output rate;
>5% deviation from 25 Hz warns and invalidates measurements until recovery.
No physical output-rate verification has been performed here. Overflow/lost
samples compromise this estimate; check timing faults and batch sizes as well.

Results CSV records calculated HR/SpO2 before display averaging, validity flags,
peak count, measured rate and cumulative batch/overflow fault count. Gaps in the
results file can indicate read failures; raw timestamp gaps expose stalls.

## Jupyter example

Copy the CSV files back to the host. With pandas and matplotlib installed:

```python
import pandas as pd
import matplotlib.pyplot as plt
raw = pd.read_csv('raw.csv')
m = pd.read_csv('measurements.csv')
raw['seconds'] = (raw.timestamp_ms - raw.timestamp_ms.iloc[0]) / 1000
m['seconds'] = (m.timestamp_ms - raw.timestamp_ms.iloc[0]) / 1000
print('Host receipt rate:', (len(raw)-1)/(raw.seconds.iloc[-1]))
print('Batch sizes:', raw.batch_size.value_counts())
print('Valid HR fraction:', m.hr_valid.mean())
print('Valid SpO2 fraction:', m.spo2_valid.mean())
raw.plot(x='seconds', y=['red', 'ir'])
m.assign(hr=m.hr.where(m.hr_valid.eq(1)),
         spo2=m.spo2.where(m.spo2_valid.eq(1))).plot(x='seconds', y=['hr','spo2'])
m.plot(x='seconds', y='effective_hz')
plt.show()
```

Compare raw timestamp spacing, batch sizes and measured rate with logging on/off.
Test stationary contact, removal/replacement, motion, low perfusion, disconnect,
restart and alarm confirmation. Compare against a reference instrument before
claiming accuracy. Eight-sample spacing is deliberately preserved: at 25 Hz,
quantization can reject high rates near 170–180 BPM. Amplitude/consistency gates
are engineering heuristics and can still accept periodic motion artifacts.
Filter delay and signal-quality thresholds need physical validation.

## Checks performed on development host

`make build` attempted: fails because QNX SDP is unavailable (`sys/neutrino.h`
missing). No ARM64 binary was built or deployed. Tracked build artifacts restored.
Portable HR tests:

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I01-Pulse-Oximeter/src tests/test_ppg_hr.c \
  01-Pulse-Oximeter/src/ppg_hr.c -lm -o /tmp/qnx-test-ppg
/tmp/qnx-test-ppg
```

Passed synthetic ten-minute signals at 40–180 BPM in ten-BPM steps (upper rates
may be invalid), startup, incorrect 20 Hz rate, 250 BPM, flat signal, timing loss
and no-contact rejection. These test HR code only, not hardware, QNX integration,
SpO2 accuracy, disk timing or physiological accuracy.

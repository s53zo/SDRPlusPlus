# Signal CSV recording

Enable **Signal level CSV** in Recorder. The default measurement is now
continuous filtered IQ power, independent of FFT size/rate, display smoothing,
zoom, window size, demodulator audio AGC, and WAV sample type. Start reception,
select the wanted VFO and bandwidth, then press Record. **Signal CSV only**
uses exactly the same measurement and omits the WAV.

The measurement frequency and bandwidth are captured at Record. Changing the
selected VFO or its filter afterwards does not move the measurement. Stop and
restart recording to measure another band. Receiver retuning, input sample-rate
or decimation changes, IQ-correction/inversion changes, changing input, and
stopping reception stop the recording. Pending IQ is discarded on stop.

Three dedicated IQ filters use the receiver's channel-filter implementation:
the wanted band and two reference bands with the same bandwidth B, centered
2B below and above the wanted band's center. They have a guard gap of B from
the wanted band's edges. Recording requires all three bands and transition
margins to fit inside the captured IQ passband. When IQ correction is enabled,
recording is rejected if any band overlaps the receiver-center DC notch
(including a 50 Hz guard and filter transition margin). Offset the receiver
center away from the carrier/reference bands, or disable IQ correction.

After discarding filter startup transients, each nominal 100 ms interval produces
a row. Integer sample counts alternate as needed using the actual resampler
output rate, keeping interval boundaries within half an output sample of the
100 ms grid. The recorded window duration reports the actual sample duration.
Partial final windows are discarded. Power is
I*I + Q*Q, averaged in linear units before 10*log10 conversion. No samples are
deliberately skipped between windows. File/driver overruns can still lose
samples; CSV does not detect hardware drops.

## Columns

- `signal_avg_db`: primary measurement for antenna plots; mean power in the
  selected band, including noise and interference.
- `signal_peak_db`: maximum instantaneous filtered IQ power within the window.
  This is no longer the old display FFT-bin peak.
- `signal_ema_db`: mean power smoothed in linear units with alpha 0.2, then
  converted to dB.
- `noise_left_db`, `noise_right_db`: mean powers in the reference bands.
- `noise_db`: linear average of the two reference powers, converted to dB.
  This is noise **plus interference**; neighboring carriers bias the estimate.
- `snr_db`: 10*log10((channel power - noise estimate)/noise estimate).
  Empty when channel power does not exceed the reference estimate or the
  reference power is zero. It assumes similar noise in all three bands.
- `frequency_hz`: selected VFO tuning frequency at Record.
- `measurement_center_hz`: actual filter center; accounts for USB/LSB reference
  offsets. For CW this equals the tuning frequency.
- `bandwidth_hz`, `window_ms`, `sample_count`: measurement settings.
- `measurement_method`: `filtered_iq_power_v1`, distinguishing this schema's
  meaning from earlier display-spectrum CSVs.
- `timestamp_unix_ms`: approximate end of each measurement window in UTC,
  anchored to arrival of the first processed IQ block and advanced by sample
  count using the actual resampling ratio. This is not a hardware timestamp;
  buffering and FIR delay affect
  alignment with external flight logs. A zero-power window is represented by
  the -300 dB floor, not a calibrated signal measurement. Importers must not
  interpret these explicitly dB-valued columns as tenths of dB.

Levels are relative to IQ amplitude 1.0 (power 1.0 = 0 dB), **not calibrated
dBm**. Existing filter/resampler gain and source scaling apply. Use fixed
hardware gain for antenna comparisons. The nominal bandwidth includes real
filter transition response; it is not an ideal rectangular integration band.
Do not compare old FFT peak numbers directly with the new integrated powers.

## Verification

From the repository root on a system with VOLK installed:

```sh
for test in signal_power dsp_rate synchronized_event stream_restart; do
  c++ -std=c++17 -O2 -pthread -Icore/src $(pkg-config --cflags volk) \
    misc_modules/recorder/tests/${test}_test.cpp \
    -o /tmp/sdrpp-${test}-test $(pkg-config --libs volk) || exit 1
  /tmp/sdrpp-${test}-test || exit 1
done
```

The synthetic test checks linear averaging, 100 ms boundaries, 300/500 Hz
filters, adjacent reference bands, carrier stability and different input block
sizes. Additional tests cover actual resampling rates, process-only buffer
lifetime and gain, draining event unsubscription, and stale-block removal on
restart. These tests do not validate hardware calibration or RF propagation.

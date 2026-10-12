# Spectrum view checks

`fixtures/cap24.bin` and `cap5.bin`: 256-bin ESP-SDR frames (FFT order, 4-frame average), captured
on the 2.8 board with `BANDWIDTH 48` at the centres listed in the matching `.txt`. They are the
input to `firmware/wlc_sensor/main/spectrum_skirt.h` (the receiver's noise skirt around DC) and to
the host render below. No identifiers: spectrum power only.

```sh
clang++ -std=c++17 -fsanitize=address,undefined -o /tmp/render tests/spectrum/render.cpp
/tmp/render tests/spectrum/fixtures/cap5.bin "$(cat tests/spectrum/fixtures/cap5.txt)" \
  "5170-5330,5490-5730,5735-5835" "5 GHZ  ALL" "36-64  100-144  149" /tmp/sp5.ppm
```

What the captures showed, and why the view is built the way it is:
- At the 80 MHz rate only the middle of a capture is real: the default analog filter passes about
  20 MHz, `BANDWIDTH 48` about 52 MHz flat. Edges fall to the noise floor.
- DC carries a 3-bin LO spike at 2.4 GHz and, at 5 GHz, a +-2.5 MHz comb of spurs over a noise
  skirt about 30 units above the floor 20 MHz out. It appears at every tuned frequency, so it is
  the receiver, not RF.
- So captures are tuned every 20 MHz, each column takes the capture where it sits farthest from DC
  (10-20 MHz), and the remaining skirt is subtracted in linear power.

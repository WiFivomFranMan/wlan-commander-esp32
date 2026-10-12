// SPDX-License-Identifier: GPL-3.0-or-later
// Host render of the sensor image's spectrum view: feeds captured frames (256 bins each, FFT
// order) through SpectrumView exactly as the firmware does and writes a PPM.
// usage: render captures.bin "c1,c2,..." "lo-hi[,lo-hi...]" "TITLE" "AXIS" out.ppm
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../../firmware/wlc_sensor/main/spectrum_view.h"

int main(int argc, char** argv) {
    if (argc != 7) return 2;
    std::vector<unsigned> centers;
    for (char* t = strtok(argv[2], ","); t; t = strtok(nullptr, ",")) centers.push_back(atoi(t));
    uint16_t ranges[3][2]; unsigned nr = 0;
    for (char* t = strtok(argv[3], ","); t && nr < 3; t = strtok(nullptr, ",")) {
        ranges[nr][0] = atoi(t); ranges[nr][1] = atoi(strchr(t, '-') + 1); nr++;
    }
    FILE* f = fopen(argv[1], "rb");
    std::vector<uint8_t> data(1 << 16);
    size_t n = fread(data.data(), 1, data.size(), f);
    fclose(f);
    if (n != centers.size() * 256) { fprintf(stderr, "expected %zu captures, got %zu bytes\n", centers.size(), n); return 1; }
    static uint8_t fall[SpectrumView::FALL_ROWS * SpectrumView::FALL_COLS];
    SpectrumView v; v.fall = fall; v.reset(); v.set_ranges(ranges, nr);
    snprintf(v.title, sizeof(v.title), "%s", argv[4]);
    snprintf(v.axis, sizeof(v.axis), "%s", argv[5]);
    for (int sweep = 0; sweep < 40; sweep++) {      // let the floor tracker and waterfall settle
        for (size_t s = 0; s < centers.size(); s++) v.push_capture(&data[s * 256], 256, centers[s]);
        v.commit();
    }
    FILE* o = fopen(argv[6], "wb");
    fprintf(o, "P6\n%u %u\n255\n", SpectrumView::W, SpectrumView::H);
    for (unsigned y = 0; y < SpectrumView::H; y++)
        for (unsigned x = 0; x < SpectrumView::W; x++) {
            uint16_t c = v.pixel(x, y);
            uint8_t rgb[3] = {(uint8_t)((c >> 11) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31)};
            fwrite(rgb, 1, 3, o);
        }
    fclose(o);
    return 0;
}

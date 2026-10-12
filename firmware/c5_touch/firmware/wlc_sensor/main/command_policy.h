// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <string.h>

// Parse decimal tokens without scanf's sign acceptance or integer wraparound.
static bool command_number(const char*& p, uint32_t& n) {
    while (*p == ' ') ++p;
    if (*p < '0' || *p > '9') return false;
    n = 0;
    do {
        unsigned digit = *p++ - '0';
        if (n > (UINT32_MAX - digit) / 10) return false;
        n = n * 10 + digit;
    } while (*p >= '0' && *p <= '9');
    return !*p || *p == ' ';
}

static bool command_end(const char* p) {
    while (*p == ' ') ++p;
    return !*p;
}

static bool idle_query(const char* p) {
    return !strcmp(p, "INFO") || !strcmp(p, "CAPS") ||
        !strcmp(p, "SPECINFO?") || !strcmp(p, "RANGE?") ||
        !strcmp(p, "TRANSPORT?");
}

static bool spectrum_request(const char* line) {
    if (idle_query(line) || !strcmp(line, "GAIN?") || !strcmp(line, "LIMITS?") ||
        !strcmp(line, "GAIN HARDWARE") || !strcmp(line, "DC?")) return true;
    const char* p = line;
    uint32_t n;
    if (!strncmp(p, "SPEC ", 5)) {
        p += 5;
        uint32_t ms, stride, units, detector, rate, bins, stats;
        if (!command_number(p, ms) || !command_number(p, stride) ||
            !command_number(p, units) || !command_number(p, detector) ||
            !command_number(p, rate) || !command_number(p, bins)) return false;
        if (!command_end(p) && (!command_number(p, stats) || stats > 1)) return false;
        return command_end(p) && ms >= 1 && ms <= 1000 && stride == 1 &&
            units >= 1 && units <= 8 && detector <= 1 && rate <= 5 &&
            (bins == 256 || bins == 512);
    }
    if (!strncmp(p, "FREQ ", 5)) {
        p += 5;
        return command_number(p, n) && command_end(p) &&
            ((n >= 2400 && n <= 2483) || (n >= 5150 && n <= 5895));
    }
    if (!strncmp(p, "BANDWIDTH ", 10)) {
        p += 10;
        return command_number(p, n) && command_end(p) && (!n || (n >= 11 && n <= 48));
    }
    if (!strncmp(p, "GAIN MANUAL ", 12)) {
        p += 12;
        return command_number(p, n) && command_end(p) && n < 90;
    }
    if (!strncmp(p, "DC ", 3)) {
        p += 3;
        return command_number(p, n) && command_end(p) && n <= 1;
    }
    // Raw IQ, register probes and RXRUN are excluded from the combined image.
    return false;
}

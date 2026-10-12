// SPDX-License-Identifier: MIT
#include "wlc_generator.hpp"
#include "generator_policy.h"
#include "generator_view.h"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace wlc::generator;
int main() {
    (void)&spectrum_request;
    Config c;
    assert(valid(c) && c.backoff_qdb == 80 && total_ms(c) == 5000);
    for (auto ch : kChannels) { c.channel = ch; assert(valid(c)); }
    for (auto ch : {0u,12u,14u,35u,54u,66u,102u,145u,169u,177u}) { c.channel = ch; assert(!valid(c)); }
    c = Config{};
    for (unsigned field = 0; field < 12; ++field) {
        Config bad = c;
        uint32_t v[12]; memcpy(v, &bad, sizeof(v));
        v[field] = 0xffffffff; memcpy(&bad, v, sizeof(v)); assert(!valid(bad));
    }
    c.on_ms = c.off_ms = 1; c.count = 1; c.backoff_qdb = 0; assert(valid(c));
    c.on_ms = c.off_ms = 30000; c.count = 100; c.backoff_qdb = 80; c.workflow = Pulse;
    assert(valid(c) && total_ms(c) == 5970000);
    Run run; assert(run.start(100)); assert(!run.start(101));
    assert(!run.due(5099) && run.remaining(5099) == 1);
    run.advance(5100); assert(run.phase == Phase::Complete && !run.running());
    assert(run.start(6000)); run.stop(); run.advance(20000); assert(run.phase == Phase::Stopped);
    run.fail(); assert(!run.start(30000));
    for (unsigned band = 0; band < 2; ++band) {
        for (uint32_t dwell : {5000u,30000u}) {
            run = Run{}; run.config.workflow = Traverse; run.config.band5 = band; run.config.dwell_ms = dwell;
            uint32_t now = 0xfffff800; assert(run.start(now));
            for (unsigned i = 0; i < steps(run.config); ++i) {
                assert(run.index == i && run.phase == Phase::Active);
                assert(channel_at(run.config, i) == kChannels[(band ? 11 : 0) + i]);
                assert(!run.due(now + dwell - 1));
                now += dwell; run.advance(now);
            }
            assert(run.phase == Phase::Complete);
        }
    }
    run = Run{}; run.config.workflow = Pulse; run.config.on_ms = 1000; run.config.off_ms = 1000;
    assert(run.start(0));
    for (unsigned i = 0; i < 3; ++i) {
        assert(run.phase == Phase::Active && run.index == i);
        run.advance(i * 2000 + 1000);
        if (i < 2) { assert(run.phase == Phase::Gap); run.advance(i * 2000 + 2000); }
    }
    assert(run.phase == Phase::Complete); // No final off period.
    for (unsigned i = 0; i < kRadarCount; ++i) {
        const auto &p = kRadar[i];
        assert(p.pulses > 0 && p.pri_count >= 1 && p.pri_count <= 3);
        uint64_t ns = p.width_ns;
        for (unsigned j = 0; j + 1 < p.pulses; ++j) {
            assert(p.pri_ns[j % p.pri_count] > p.width_ns);
            ns += p.pri_ns[j % p.pri_count];
        }
        assert(radar_duration_us(p) == (ns + 999) / 1000 && radar_duration_us(p) < 100000);
        run = Run{}; run.config.workflow = Radar; run.config.channel = 52;
        run.config.tone = 1; run.config.radar_profile = i;
        assert(valid(run.config) && steps(run.config) == 1 && run.start(0));
        assert(!run.due(duration(run.config) - 1));
        run.advance(duration(run.config)); assert(run.phase == Phase::Complete);
        run.config.channel = 36; assert(!valid(run.config));
        run.config.channel = 100; run.config.tone = 0; assert(!valid(run.config));
    }
    assert(radar_duration_us(kRadar[0]) == 24277);
    run = Run{}; run.config.workflow = Pulse; run.config.on_ms = 1;
    assert(run.start(0)); run.advance(1); assert(run.phase == Phase::Gap);
    run.stop(); run.advance(50000); assert(run.phase == Phase::Stopped);
    run = Run{}; run.config.workflow = Traverse; assert(run.start(0));
    run.advance(50000); assert(run.index == 1 && run.deadline == 55000); // Late polling must not skip.
    Press press;
    assert(press.update(true, 20, 200, 320) == Action::None);
    assert(press.update(false, 0, 0, 320) == Action::None);
    assert(press.update(true, 20, 200, 320) == Action::Start);
    assert(press.update(true, 150, 200, 320) == Action::None);
    press.update(false, 0, 0, 320);
    assert(press.update(true, 150, 200, 320) == Action::Stop);
    press.update(false, 0, 0, 320);
    assert(press.update(true, 20, 200, 320, false) == Action::None);
    assert(press.update(true, 20, 200, 320, true) == Action::None);
    press.update(false, 0, 0, 320);
    assert(press.update(true, 20, 200, 320, true) == Action::Start);
    assert(hit(300, 200, 320) == Action::Return && hit(270, 200, 280) == Action::Return);
    assert(hit(400, 200, 320) == Action::None && hit(-1, 200, 320) == Action::None);
    int x, y; assert(landscape(0,319,320,x,y) && x==319 && y==239);
    assert(landscape(239,0,280,x,y) && x==0 && y==0);
    assert(!landscape(240,0,320,x,y) && !landscape(1,280,280,x,y));
    GeneratorRequest request;
    assert(generator_request("TX PACKET 36 80 30000",request));
    assert(generator_request("TX CW 11 0 1",request));
    assert(generator_request("TX PACKET 52 80 1000",request));
    assert(generator_request("TX CW 144 80 1000",request));
    for (auto cmd : {"TX PACKET 54 80 1000", "TX CW 1 81 1", "TX CW 1 0 0", "TX CW 1 0 30001", "TX CW 1 0 1 junk"}) assert(!generator_request(cmd,request));
    assert(!generator_heartbeat("TXINFO?") && !generator_heartbeat("GENLOCAL?") && generator_heartbeat("PING"));
    assert(generator_guard_remaining(100,0,10000000) == 5000);
    assert(generator_guard_remaining(5000001,0,10000000) == 0);
    assert(generator_guard_remaining(0,0,1000) == 1);
    GeneratorView view; run = Run{}; view.update(run,0,true);
    assert(view.start_enabled && !strcmp(view.frame.rows[1],"READY"));
    assert(run.start(0)); view.update(run, 1, true);
    assert(!view.start_enabled && !strcmp(view.frame.rows[1],"RF ACTIVE"));
    for (unsigned yy=0; yy<240; ++yy) for(unsigned xx=0;xx<320;++xx) (void)view.pixel(xx,yy);
    view.update(run,1,false); assert(!view.start_enabled);
    view.update(run,1,true,0); assert(!view.start_enabled && !strcmp(view.frame.rows[1],"RF SESSION LOCKED"));
    std::cout << "generator policy, scheduler, touch and renderer checks passed\n";
}

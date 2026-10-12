#!/usr/bin/env python3
"""Exercise already-flashed touch-board AirHORN firmware; never flashes hardware.

Runs finite maximum-attenuation RF tests. Console state is firmware evidence,
not independent RF measurement or proof that physical touch worked.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../firmware/wlc_sensor'))
from probe_usb import open_native_usb
from device_tool import inventory

class Link:
    def __init__(self, port, board):
        self.port_name, self.board, self.serial = port, board.lower(), None
        self.log = []
        self.connect()
    def connect(self):
        if self.serial:
            self.serial.close()
        self.serial = None
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            matches = [b for b in inventory() if b['serial'].lower() == self.board]
            if len(matches) == 1:
                self.port_name = matches[0]['port']
                try:
                    self.serial = open_native_usb(self.port_name)
                    self.serial.timeout = .3
                    time.sleep(.1); self.serial.reset_input_buffer()
                    return
                except OSError:
                    pass
            time.sleep(.2)
        raise TimeoutError('Physical board did not reconnect: ' + self.board)
    def close(self):
        if self.serial: self.serial.close()
        self.serial = None
    def command(self, text, timeout=3):
        self.serial.write((text + '\n').encode()); self.serial.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.serial.readline().decode(errors='replace').strip()
            if line.startswith(('OK', 'ERR', 'MODE ', 'GENLOCAL ', 'TXINFO ', 'WLCINFO ', 'RADARINFO ', 'RFAPPROVAL ', '{')):
                self.log.append({'command': text, 'reply': line, 'time': time.monotonic()})
                return line
        raise TimeoutError(text)
    def expected(self, text, answer):
        actual = self.command(text)
        if actual != answer: raise RuntimeError(f'{text}: {actual!r}, expected {answer!r}')
    def local(self):
        reply = self.command('GENLOCAL?')
        if not reply.startswith('GENLOCAL '): raise RuntimeError(reply)
        return json.loads(reply[9:])
    def info(self):
        reply = self.command('WLCINFO?')
        if not reply.startswith('WLCINFO '): raise RuntimeError(reply)
        info = json.loads(reply[8:])
        assert info['board'].lower() == self.board, info
        return info
    def launcher(self):
        self.command('LAUNCHER')
        time.sleep(.8); self.connect()
        assert 'heap' in json.loads(self.command('heap'))
    def enter(self, workflow, channel=1, tone=0, band5=0, single=1000, on=500, off=400, count=3, dwell=5000, profile=0):
        reply = self.command(f'generator {workflow} {tone} {band5} {channel} 80 {single} {on} {off} {count} {dwell} {profile}')
        assert 'generator' in json.loads(reply), reply
        assert 'boot' in json.loads(self.command('boot generator shielded'))
        time.sleep(.8); self.connect()
        self.expected('MODE?', 'MODE GENERATOR')
        info = self.info(); assert not info['ble_initialized']
        local = self.local(); assert local['local'] and local['touch'] and local['phase'] == 'READY', local
        assert not json.loads(self.command('TXINFO?')[7:])['active']
        return local
    def start(self): self.expected('LCDTAP 40 210', 'OK')
    def wait(self, phase, timeout):
        deadline = time.monotonic() + timeout
        states = []
        while time.monotonic() < deadline:
            state = self.local(); states.append(state)
            if state['phase'] == phase: return states
            assert state['phase'] != 'ERROR', state
            time.sleep(.05)
        raise TimeoutError(f'Waiting for {phase}: {states[-1:]}')

def screenshot_checks(link):
    result = {}
    link.enter(0, single=5000)
    for _ in range(20):
        time.sleep(.05)  # observe physical release before the next injected press
        link.serial.write(b'LCDTAP 40 210\nTXSTOP\n'); link.serial.flush()
        replies = [link.serial.readline().decode().strip() for _ in range(2)]
        assert replies == ['OK','OK'], replies
        time.sleep(.05)
        assert link.local()['phase'] == 'STOPPED', 'Queued Start survived Stop'
    result['queued_start_stop_cycles'] = 20
    time.sleep(.05); link.start(); link.wait('RF ACTIVE', 1)
    assert 'screenshot' in json.loads(link.command('LCDSHOT?'))
    begin = time.monotonic(); link.expected('TXSTOP', 'OK')
    result['stop_preempted_screenshot_seconds'] = round(time.monotonic() - begin, 4)
    assert link.wait('STOPPED', 1)[-1]['phase'] == 'STOPPED'
    time.sleep(.1); assert link.local()['phase'] == 'STOPPED'
    assert not json.loads(link.command('TXINFO?')[7:])['active']
    link.start(); link.wait('RF ACTIVE', 1)
    assert 'screenshot' in json.loads(link.command('LCDSHOT?'))
    link.close(); time.sleep(6); link.connect()
    result['finite_with_unread_screenshot'] = link.local()
    assert result['finite_with_unread_screenshot']['phase'] == 'COMPLETE'
    assert not json.loads(link.command('TXINFO?')[7:])['active']
    link.launcher()
    return result

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True); p.add_argument('--board', required=True)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--shielded', action='store_true', help='Confirm verified RF containment for this transmitting test suite')
    args = p.parse_args()
    if not args.shielded: p.error("RF hazard: this suite transmits; --shielded requires a verified Faraday cage / conducted setup")
    link = Link(args.port, args.board)
    if not json.loads(link.command("rfapproval"))["rf_approval"]["approved"]:
        link.close(); p.error("Web admin device approval is required; no RF tests were started")
    result = {'board': args.board, 'passed': False, 'tests': {}, 'physical_touch': 'not_tested',
              'independent_rf': 'not_tested', 'injected_phy_stall': 'not_tested'}
    tests = result['tests']
    saved = json.loads(link.command('generator'))['generator']
    try:
        print('single, readiness, stop/restart and USB exclusion', flush=True)
        tests['single_ready'] = link.enter(0)
        time.sleep(5.5); assert link.local()['phase'] == 'READY'
        link.expected('TX PACKET 1 80 1000', 'ERR busy')
        link.start(); tests['single_complete'] = link.wait('COMPLETE', 3)
        link.start(); time.sleep(.15); link.expected('LCDTAP 150 210', 'OK')
        tests['stop_during_rf'] = link.wait('STOPPED', 1)
        assert not json.loads(link.command('TXINFO?')[7:])['active']
        time.sleep(1.1); assert link.local()['phase'] == 'STOPPED'
        link.expected('LCDTAP 290 210', 'OK'); time.sleep(.8); link.connect()
        assert 'heap' in json.loads(link.command('heap'))
        print('CW pulses and cancellation during gap', flush=True)
        tests['pulse_ready'] = link.enter(2, channel=36, tone=1)
        link.start(); states = link.wait('COMPLETE', 5)
        assert {s['index'] for s in states if s['phase'] == 'RF ACTIVE'} == {0,1,2}, states
        tests['pulse_complete'] = states
        link.start(); link.wait('GAP', 2); link.expected('TXSTOP', 'OK')
        tests['stop_during_gap'] = link.wait('STOPPED', 1)
        time.sleep(1.5); assert link.local()['phase'] == 'STOPPED'
        link.launcher()
        print('5 GHz traversal: one pass through 25 channels (including DFS)', flush=True)
        tests['traverse_ready'] = link.enter(1, band5=1)
        link.start(); states = link.wait('COMPLETE', 128)
        seen = []
        for state in states:
            if state['phase'] == 'RF ACTIVE' and (not seen or seen[-1] != state['channel']): seen.append(state['channel'])
        assert seen == [36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165], seen
        tests['traverse_channels'] = seen
        tests['traverse_complete'] = states[-1]
        link.launcher()
        print('FCC and ETSI radar examples: finite events and software pulse counts', flush=True)
        tests['radar'] = []
        for profile, pulses in enumerate([18,102,26,17,14,18,10,15,25,30,45]):
            link.enter(3, channel=52, tone=1, profile=profile)
            link.start(); link.wait('COMPLETE', 3)
            reply = link.command('RADARINFO?')
            assert reply.startswith('RADARINFO '), reply
            radar = json.loads(reply[10:])
            assert radar['profile'] == profile and radar['pulses'] == pulses and not radar['failed'] and not radar['running'], radar
            assert not radar['rf_timing_verified'] and radar['setup_transient'], radar
            assert not json.loads(link.command('TXINFO?')[7:])['active']
            tests['radar'].append(radar)
            link.launcher()
        print('local run continues with the USB console closed, then stops finitely', flush=True)
        link.enter(0, channel=36, single=10000); link.start(); link.close()
        time.sleep(6); link.connect(); assert link.local()['phase'] == 'RF ACTIVE'
        tests['usb_console_closed_local_complete'] = link.wait('COMPLETE', 6)[-1]
        link.expected('RELEASE', 'OK REBOOT'); time.sleep(.8); link.connect()
        link.expected('MODE?', 'MODE SNIFFER'); assert not link.info()['run']
        print('existing host generator: status queries do not extend RF lease', flush=True)
        link.expected('RFUNLOCK SHIELDED', 'OK')
        link.expected('MODE GENERATOR', 'OK REBOOT'); time.sleep(.8); link.connect()
        link.expected('MODE?', 'MODE GENERATOR'); assert not link.local()['local']
        link.expected('TX PACKET 1 80 10000', 'OK')
        started = time.monotonic(); expired = False
        while time.monotonic() - started < 8:
            try:
                state = json.loads(link.command('TXINFO?', timeout=1)[7:])
                if state['mode'] == 'sniffer': expired = True; break
            except (TimeoutError, OSError, ValueError):
                link.connect()
            time.sleep(.3)
        assert expired, 'Observer queries renewed host lease or recovery failed'
        tests['host_return_to_sniffer_seconds'] = round(time.monotonic() - started, 3)
        assert link.info()['ble_initialized']
        link.launcher()
        tests['settings_persisted'] = json.loads(link.command('generator'))
        assert tests['settings_persisted']['generator']['single_ms'] == 10000
        print('Stop cancels queued Start; screenshot output cannot block finite stopping', flush=True)
        tests['screenshot_io'] = screenshot_checks(link)
        result['passed'] = True
    finally:
        try:
            # Restore the operator's settings even after an interrupted test.
            reply = link.command('generator')
            if not reply.startswith('{'):
                link.launcher()
            fields = ['workflow','tone','band5','channel','backoff_qdb','single_ms','on_ms','off_ms','count','dwell_ms','radar_profile']
            restored = link.command('generator ' + ' '.join(str(saved[k]) for k in fields))
            assert json.loads(restored)['generator'] == saved
            result['settings_restored'] = True
        except Exception as error:
            result['settings_restore_error'] = str(error)
        result['console'] = link.log
        args.output.write_text(json.dumps(result, indent=2) + '\n')
        link.close()
    print('AirHORN hardware console checks passed', flush=True)

if __name__ == '__main__': main()

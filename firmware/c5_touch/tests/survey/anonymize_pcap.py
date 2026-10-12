#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Anonymize a classic radiotap pcap for use as a test fixture.

BSSIDs and SSIDs locate real places, so fixtures taken from field captures go through this
first. Every unicast MAC in the 802.11 header is mapped to 00:00:5e:xx:xx:xx (keeping the
I/G and U/L bits, so "randomized" stays meaningful); the same six octets are rewritten wherever
else they occur in the frame, as are RNR and Neighbor Report BSSIDs and the Interworking HESSID.
SSIDs — including those nested in Multiple-BSSID profiles — become same-length placeholders, and
WPS Device Name / Model attributes are blanked. The FCS is recomputed when it was valid on
input, so frames that arrived corrupt stay corrupt.

--mark-fcs sets the radiotap "FCS at end" flag on frames whose trailing four octets are a
valid FCS the capturing driver forgot to declare (seen on a Cisco 6 GHz capture: tshark and the
survey would otherwise both parse the FCS as a bogus trailing element).

Usage: anonymize_pcap.py [--mark-fcs] in.pcap out.pcap   (convert pcapng first: editcap -F pcap)
"""
import struct
import sys
import zlib

macs = {}
ssids = {}


def anon_mac(m):
    if m[0] & 0x01 or m == b"\x00" * 6:
        return m
    if m not in macs:
        n = len(macs) + 1
        macs[m] = bytes([m[0] & 0x03, 0x00, 0x5E, (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF])
    return macs[m]


def anon_ssid(s):
    if not s or all(b == 0 for b in s):
        return s
    if s not in ssids:
        ssids[s] = ("ssid%03d" % (len(ssids) + 1)).ljust(len(s), "_")[: len(s)].encode()
    return ssids[s]


def walk_ies(buf, start, end, fn):
    pos = start
    while pos + 2 <= end:
        eid, ln = buf[pos], buf[pos + 1]
        if pos + 2 + ln > end:
            return
        fn(buf, eid, pos + 2, ln)
        pos += 2 + ln


def scrub_ie(buf, eid, off, ln):
    if eid == 0 and ln:
        buf[off:off + ln] = anon_ssid(bytes(buf[off:off + ln]))
    elif eid == 71 and ln > 1:  # Multiple BSSID: subelement 0 holds a nontransmitted profile
        pos, end = off + 1, off + ln
        while pos + 2 <= end:
            sid, sl = buf[pos], buf[pos + 1]
            if pos + 2 + sl > end:
                break
            if sid == 0:
                walk_ies(buf, pos + 2, pos + 2 + sl, scrub_ie)
            pos += 2 + sl
    elif eid == 107 and ln in (7, 9):
        buf[off + ln - 6:off + ln] = anon_mac(bytes(buf[off + ln - 6:off + ln]))
    elif eid == 52 and ln >= 6:
        buf[off:off + 6] = anon_mac(bytes(buf[off:off + 6]))
    elif eid == 201:  # Reduced Neighbor Report: TBTT info sets carry BSSIDs at offset 1
        pos, end = off, off + ln
        while pos + 4 <= end:
            hdr = buf[pos] | buf[pos + 1] << 8
            count = ((hdr >> 4) & 0x0F) + 1
            info_len = buf[pos + 1]
            pos += 4
            for _ in range(count):
                if pos + info_len > end:
                    return
                if info_len >= 7:
                    buf[pos + 1:pos + 7] = anon_mac(bytes(buf[pos + 1:pos + 7]))
                pos += info_len
    elif eid == 221 and ln >= 4 and bytes(buf[off:off + 4]) == b"\x00\x50\xf2\x04":  # WPS
        pos, end = off + 4, off + ln
        while pos + 4 <= end:
            at, al = struct.unpack(">HH", bytes(buf[pos:pos + 4]))
            if pos + 4 + al > end:
                break
            if at in (0x1011, 0x1023, 0x1024, 0x1042):  # device name, model, model no., serial
                buf[pos + 4:pos + 4 + al] = b"x" * al
            pos += 4 + al


def anonymize(frame):
    buf = bytearray(frame)
    if len(buf) < 10:
        return bytes(buf)
    fc = buf[0] | buf[1] << 8
    ftype, sub = (fc >> 2) & 3, (fc >> 4) & 15
    nmac = 1
    if ftype in (0, 2) and len(buf) >= 24:
        nmac = 4 if ftype == 2 and (fc & 0x0300) == 0x0300 and len(buf) >= 30 else 3
    elif ftype == 1 and len(buf) >= 16 and sub not in (12, 13):  # not CTS / ACK
        nmac = 2
    offsets = [4, 10, 16, 24][:nmac]
    seen = [bytes(buf[o:o + 6]) for o in offsets]
    for o, m in zip(offsets, seen):
        buf[o:o + 6] = anon_mac(m)
    if ftype == 0 and len(buf) >= 24:
        hdr = 28 if fc & 0x8000 else 24
        fixed = {8: 12, 5: 12, 4: 0, 0: 4, 2: 10}.get(sub)
        if fixed is not None:
            walk_ies(buf, hdr + fixed, len(buf), scrub_ie)
    # Any header MAC repeated elsewhere in the body (vendor elements, ANQP payloads).
    body = bytes(buf[24:])
    for m in set(seen):
        if m in macs and m in body:
            buf[24:] = bytes(buf[24:]).replace(m, macs[m])
    return bytes(buf)


def radiotap_flags_offset(rt):
    present = struct.unpack("<I", rt[4:8])[0]
    off = 8
    word = present
    while word & 0x80000000:
        word = struct.unpack("<I", rt[off:off + 4])[0]
        off += 4
    if not present & 0x2:
        return None
    if present & 0x1:
        off = (off + 7) & ~7
        off += 8
    return off


def main(src, dst, mark_fcs=False):
    data = open(src, "rb").read()
    magic = data[:4]
    if magic not in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
        sys.exit("expected a little-endian classic pcap; run editcap -F pcap first")
    linktype = struct.unpack("<I", data[20:24])[0]
    if linktype != 127:
        sys.exit("expected radiotap (linktype 127)")
    out = bytearray(data[:24])
    pos = 24
    while pos + 16 <= len(data):
        ts, tf, incl, orig = struct.unpack("<IIII", data[pos:pos + 16])
        rec = data[pos + 16:pos + 16 + incl]
        pos += 16 + incl
        rt_len = struct.unpack("<H", rec[2:4])[0]
        rt, mpdu = bytearray(rec[:rt_len]), rec[rt_len:]
        fcs_ok = len(mpdu) > 4 and zlib.crc32(mpdu[:-4]) == struct.unpack("<I", mpdu[-4:])[0]
        flags_at = radiotap_flags_offset(rt)
        if mark_fcs and fcs_ok and flags_at is not None:
            rt[flags_at] |= 0x10
        if fcs_ok:
            body = anonymize(mpdu[:-4])
            mpdu = body + struct.pack("<I", zlib.crc32(body))
        else:
            mpdu = anonymize(mpdu)
        rec = bytes(rt) + mpdu
        out += struct.pack("<IIII", ts, tf, len(rec), len(rec)) + rec
    open(dst, "wb").write(out)
    print("%s: %d MACs, %d SSIDs anonymized" % (dst, len(macs), len(ssids)))


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--mark-fcs"]
    main(args[0], args[1], "--mark-fcs" in sys.argv)

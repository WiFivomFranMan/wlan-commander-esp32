#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Golden test: wlc_survey against tshark as an independent oracle.

For each pcap, tshark decodes beacons, probe responses and GAS responses (-T json -x, so string
fields come back as raw bytes) and this script derives, per BSSID, what the survey should hold:
SSID, primary channel, RSN AKM suite types, HT/VHT/HE/EHT presence, Interworking and HS2.0
presence, beacon roaming-consortium OIs, and per-BSSID ANQP domain names, NAI realms (with EAP
methods), venue names, roaming OIs, PLMNs and operator friendly names. survey_dump prints the
survey's view of the same file; any difference fails the test.

Policy that is the survey's by design (latest frame wins, latest ANQP element replaces, list
and string bounds, control octets shown as '?') is applied to tshark's values here, so what is
compared is extraction, not presentation.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

TSHARK = os.environ.get("TSHARK") or shutil.which("tshark") or "/opt/homebrew/bin/tshark"
FILTER = ("wlan.fc.type_subtype == 0x0008 || wlan.fc.type_subtype == 0x0005 || "
          "wlan.fixed.publicact == 0x0b || wlan.fixed.publicact == 0x0d")
WFA_OUI = 0x506F9A
IEEE_OUI = 0x000FAC

# Bounds the survey stores (AnqpRecord / Text capacities, minus the NUL).
DOMAIN_CAP, REALM_CAP, VENUE_CAP, NAME_CAP = 64, 64, 64, 64
MAX_DOMAINS, MAX_REALMS, MAX_EAP, MAX_VENUES, MAX_OIS, MAX_PLMNS, MAX_OPS = 8, 8, 4, 4, 8, 8, 4


def pairs(v):
    return v if isinstance(v, list) and (not v or isinstance(v[0], tuple)) else None


def find(obj, key):
    for k, v in obj:
        if k == key:
            return v
        sub = pairs(v)
        if sub is not None:
            r = find(sub, key)
            if r is not None:
                return r
    return None


def findall(obj, key, out=None):
    out = [] if out is None else out
    for k, v in obj:
        if k == key:
            out.append(v)
        sub = pairs(v)
        if sub is not None:
            findall(sub, key, out)
    return out


def raw(v):
    """`<field>_raw` is [hex, offset, length, ...]."""
    if v is None:
        return b""
    h = v[0] if isinstance(v, list) else v
    return bytes.fromhex(h) if h else b""


def num(v, default=None):
    if v is None:
        return default
    return int(v, 0) if isinstance(v, str) else int(v)


def text(b, cap):
    b = bytes(0x3F if (c < 0x20 or c == 0x7F) else c for c in b)
    if len(b) > cap - 1:
        k = cap - 1
        while k > 0 and (b[k] & 0xC0) == 0x80:
            k -= 1
        b = b[:k]
    return b.hex()


def freq_to_band_channel(f):
    if f == 2484:
        return 0, 14
    if 2412 <= f <= 2472:
        return 0, (f - 2407) // 5
    if f == 5935:
        return 2, 2
    if 5955 <= f <= 7115:
        return 2, (f - 5950) // 5
    if 5150 <= f <= 5925:
        return 1, (f - 5000) // 5
    return 0, 0


def valid_channel(band, ch):
    if band == 0:
        return 1 <= ch <= 14
    if band == 1:
        return (32 <= ch <= 177 and ch % 4 == 0) or (149 <= ch <= 177 and ch % 4 == 1)
    return ch == 2 or (1 <= ch <= 233 and ch % 4 == 1)


def is_wildcard(mac):
    return mac in ("ff:ff:ff:ff:ff:ff", "00:00:00:00:00:00")


def top_level_tags(mgt):
    """Direct children of the tagged parameters only: Multiple-BSSID profiles nest their own."""
    tagged = find(mgt, "wlan.tagged.all") or []
    out = []
    for k, t in tagged:
        if pairs(t) is None:
            continue
        if k == "wlan.tag":
            out.append((num(find(t, "wlan.tag.number")), t))
        elif k == "wlan.ext_tag":  # tshark lifts element 255 into its own node
            out.append((255, t))
    return out


def beacon_view(layers, mgt):
    freq = num(find(layers, "radiotap.channel.freq"), 0)
    band, rx = freq_to_band_channel(freq)
    # tshark abandons the rest of a frame when one element throws, so a False/empty from it after
    # that point means "not decoded", not "absent". Seen on a valid Roaming Consortium element with
    # no beacon OIs (length 2): tshark 4.6 raises on OI #1 length 0 and never reaches what follows.
    v = {"incomplete": find(layers, "_ws.malformed") is not None, "band": band, "ht": False, "vht": False, "he": False, "eht": False, "interworking": False,
         "hs20": False, "akms": [], "rc_ois": [], "ssid": None}
    ds = ht = he6 = 0
    for n, t in top_level_tags(mgt):
        if n == 0:
            v["ssid"] = raw(find(t, "wlan.ssid_raw"))[:32]
        elif n == 3:
            ds = num(find(t, "wlan.ds.current_channel"), 0) if num(find(t, "wlan.tag.length")) == 1 else 0
        elif n == 61:
            v["ht"] = True
            if num(find(t, "wlan.tag.length"), 0) >= 22:
                ht = num(find(t, "wlan.ht.info.primarychannel"), 0)
        elif n == 45:
            v["ht"] = True
        elif n in (191, 192):
            v["vht"] = True
        elif n == 48:
            ouis = [num(o) for o in findall(t, "wlan.rsn.akms.oui")]
            types = [num(x) for x in findall(t, "wlan.rsn.akms.type")]
            v["akms"] = sorted({ty for o, ty in zip(ouis, types) if o == IEEE_OUI and ty < 32})
        elif n == 107:
            v["interworking"] = True
        elif n == 111:
            v["rc_ois"] = [raw(find(t, k)).hex() for k in ("wlan.roaming_consortium.oi1_raw",
                                                             "wlan.roaming_consortium.oi2_raw",
                                                             "wlan.roaming_consortium.oi3_raw")
                           if find(t, k)]
        elif n == 221:
            if num(find(t, "wlan.tag.oui"), 0) == WFA_OUI and num(find(t, "wlan.tag.vendor.oui.type"), 0) == 16:
                v["hs20"] = True
        elif n == 255:
            ext = num(find(t, "wlan.ext_tag.number"), 0)
            if ext in (35, 36):
                v["he"] = True
            if ext == 36:
                he6 = num(find(t, "wlan.ext_tag.he_operation.6ghz.primary_channel"), 0)
            if ext in (106, 108):
                v["eht"] = True
    if band == 2:
        adv = he6
    elif ds and ht:
        adv = ds if ds == ht else 0
    else:
        adv = ds or ht
    v["channel"] = adv if adv and valid_channel(band, adv) else rx
    return v


def anqp_view(qr):
    """Element id -> decoded value, for one reassembled query response."""
    out = {}
    cur = None
    for k, val in qr:
        if k == "wlan.fixed.anqp.info_id":
            cur = num(val)
        elif k == "wlan.fixed.anqp.info_id_tree" and cur is not None:
            t = val
            if cur == 268:
                out["domains"] = [text(raw(x), DOMAIN_CAP)
                                  for x in findall(t, "wlan.fixed.anqp.domain_name_list.name_raw")][:MAX_DOMAINS]
            elif cur == 263:
                realms = []
                for rk, rv in t:
                    if rk.startswith("NAI Realm Data") and pairs(rv) is not None:
                        eaps = [num(e) for e in findall(rv, "wlan.fixed.anqp_nai_realm_list.eap_method")]
                        realms.append({"realm": text(raw(find(rv, "wlan.fixed.anqp_nai_realm_list.realm_raw")),
                                                     REALM_CAP), "eap": eaps[:MAX_EAP]})
                out["nai_realms"] = realms[:MAX_REALMS]
            elif cur == 258:
                out["venue_names"] = [text(raw(x), VENUE_CAP)
                                      for x in findall(t, "wlan.fixed.anqp.venue.name_raw")][:MAX_VENUES]
            elif cur == 261:
                out["roaming_ois"] = [raw(x)[:15].hex()
                                      for x in findall(t, "wlan.fixed.anqp.roaming_consortium.oi_raw")][:MAX_OIS]
            elif cur == 264:
                mcc = findall(t, "e212.mcc")
                mnc = findall(t, "e212.mnc")
                out["plmns"] = ["%s-%s" % (a, b) for a, b in zip(mcc, mnc)][:MAX_PLMNS]
            elif cur == 56797:
                if num(find(t, "wlan.hs20.anqp.subtype"), 0) == 3:
                    out["operator_names"] = [text(raw(x), NAME_CAP)
                                             for x in findall(t, "wlan.hs20.anqp.ofn.name_raw")][:MAX_OPS]
            cur = None
    return out


def tshark_expectations(path):
    proc = subprocess.run([TSHARK, "-r", path, "-o", "wlan.check_checksum:TRUE",
                           "-o", "wlan.retransmitted:FALSE", "-Y", FILTER, "-T", "json", "-x"],
                          capture_output=True)
    if proc.returncode != 0:
        sys.exit("tshark failed on %s" % path)
    packets = json.loads(proc.stdout or b"[]", object_pairs_hook=lambda p: p)
    aps, anqp = {}, {}
    for pkt in packets:
        layers = find(pkt, "layers")
        wlan = find(layers, "wlan")
        mgt = find(layers, "wlan.mgt")
        if wlan is None or mgt is None:
            continue
        if find(layers, "wlan.fcs.status") == "0" or find(layers, "radiotap.flags.badfcs") in ("1", "True"):
            continue
        st = num(find(wlan, "wlan.fc.type_subtype"))
        bssid = find(wlan, "wlan.bssid")
        if st in (5, 8):
            if int(bssid.split(":")[0], 16) & 1:
                continue
            v = beacon_view(layers, mgt)
            prev = aps.get(bssid, {})
            ssid = v.pop("ssid")
            hidden = ssid is not None and not any(ssid)
            v["ssid"] = ssid.hex() if ssid and not hidden else prev.get("ssid", "")
            v["hidden"] = hidden if st == 8 and ssid is not None else prev.get("hidden", False)
            aps[bssid] = v
        elif st == 13:
            if num(find(mgt, "wlan.fixed.status_code"), 1) != 0 or num(find(mgt, "wlan.adv_proto.id"), 0) != 0:
                continue
            target = find(wlan, "wlan.sa") if is_wildcard(bssid) else bssid
            for k, qr in mgt_query_responses(mgt):
                view = anqp_view(qr)
                if view:
                    anqp.setdefault(target, {}).update(view)
    return aps, anqp


def mgt_query_responses(obj, out=None):
    out = [] if out is None else out
    for k, v in obj:
        sub = pairs(v)
        if sub is None:
            continue
        if k.startswith("Query Response") or k.startswith("Reassembled"):
            out.append((k, sub))
        else:
            mgt_query_responses(sub, out)
    return out


def compare(path, dump):
    exp_aps, exp_anqp = tshark_expectations(path)
    got = json.loads(subprocess.run([dump, path], capture_output=True, check=True).stdout)
    got_aps = {a["bssid"]: a for a in got["aps"]}
    got_anqp = {a["bssid"]: a["anqp"] for a in got["aps"] if a["anqp"]}
    got_anqp.update({p["bssid"]: p["anqp"] for p in got["pending"]})

    mismatches, gaps = [], []
    checked = 0
    ap_fields = ("ssid", "hidden", "channel", "akms", "ht", "vht", "he", "eht", "interworking", "hs20", "rc_ois")
    for bssid in sorted(set(exp_aps) | set(got_aps)):
        e, g = exp_aps.get(bssid), got_aps.get(bssid)
        if e is None or g is None:
            mismatches.append("%s: AP present in %s only" % (bssid, "survey" if e is None else "tshark"))
            continue
        for f in ap_fields:
            if e[f] == g[f]:
                checked += 1
            elif e["incomplete"] and e[f] in (False, [], "") and f not in ("ssid", "hidden", "channel"):
                gaps.append("%s %s: tshark stopped at a malformed element; survey=%r" % (bssid, f, g[f]))
            else:
                checked += 1
                mismatches.append("%s %s: tshark=%r survey=%r" % (bssid, f, e[f], g[f]))
    anqp_fields = ("domains", "nai_realms", "venue_names", "roaming_ois", "plmns", "operator_names")
    anqp_checked = {f: 0 for f in anqp_fields}
    for bssid in sorted(set(exp_anqp) | set(got_anqp)):
        e, g = exp_anqp.get(bssid, {}), got_anqp.get(bssid) or {}
        for f in anqp_fields:
            ev, gv = e.get(f, []), g.get(f, [])
            if not ev and not gv:
                continue
            checked += 1
            anqp_checked[f] += 1
            if ev != gv:
                mismatches.append("%s anqp.%s: tshark=%r survey=%r" % (bssid, f, ev, gv))
    print("%s: %d APs, %d ANQP BSSIDs (%d pending), %d frames dropped for bad FCS, %d values compared, "
          "%d mismatches, %d not decidable by tshark" % (path, len(exp_aps), len(exp_anqp), len(got["pending"]),
                                                         got["bad_fcs"], checked, len(mismatches), len(gaps)))
    print("    ANQP lists compared: " + ", ".join("%s %d" % kv for kv in anqp_checked.items() if kv[1]))
    for m in mismatches[:40]:
        print("    MISMATCH " + m)
    for m in gaps:
        print("    oracle gap " + m)
    return checked, mismatches


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", required=True)
    ap.add_argument("pcaps", nargs="+")
    a = ap.parse_args()
    total, bad = 0, 0
    for p in a.pcaps:
        c, m = compare(p, a.dump)
        total += c
        bad += len(m)
    print("golden: %d values compared across %d files, %d mismatches" % (total, len(a.pcaps), bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

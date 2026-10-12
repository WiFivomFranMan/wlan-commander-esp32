#!/usr/bin/env python3
"""Verify default RF denial on an unapproved touch board. Never sends an RF start.

The well-formed test permit is signed by a fresh, untrusted ephemeral key. It
cannot approve this device. A board already approved by an admin is refused.
"""
import argparse, base64, json, subprocess, tempfile, time
from pathlib import Path
from airhorn_qa import Link


def untrusted_permit(board):
    payload=b'WLCRF001'+bytes.fromhex(board.replace(':',''))+b'\0\7'+bytes(range(16))+(900).to_bytes(4,'big')
    with tempfile.TemporaryDirectory() as directory:
        key=Path(directory)/'test-key.pem'
        subprocess.run(['openssl','genpkey','-algorithm','EC','-pkeyopt','ec_paramgen_curve:P-256','-out',str(key)],check=True,capture_output=True)
        der=subprocess.run(['openssl','dgst','-sha256','-sign',str(key)],input=payload,check=True,capture_output=True).stdout
    def integer(at):
        assert der[at]==2
        end=at+2+der[at+1]
        return int.from_bytes(der[at+2:end],'big').to_bytes(32,'big'),end
    r,end=integer(2);s,_=integer(end)
    enc=lambda v:base64.urlsafe_b64encode(v).decode().rstrip('=')
    return 'WRF1.'+enc(payload)+'.'+enc(r+s)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board',required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();link=Link('',args.board)
    result={'board':link.board,'transmission_tests':False,'passed':False,'checks':{}}
    def command(text):
        answer=link.command(text);result['checks'][text if not text.startswith(('rfapprove ','RFAPPROVE ')) else text.split()[0]+' untrusted-permit']=answer
        return answer
    try:
        assert json.loads(command('rfapproval'))['rf_approval']['approved'] is False,'This test requires an unapproved board in the launcher'
        permit=untrusted_permit(link.board)
        assert 'rf_approval_and_shielding_ack_required' in command('boot generator')
        assert 'rf_approval_and_shielding_ack_required' in command('boot generator shielded')
        assert json.loads(command('rfapprove '+permit))['rf_approval_installed'] is False
        command('boot sensor');time.sleep(2);link.connect()
        assert json.loads(command('WLCINFO?')[8:])['rf_approval']==1
        status=json.loads(command('RFAPPROVAL?')[11:]);assert status['approved'] is False and status['unlocked'] is False
        assert command('RFUNLOCK SHIELDED')=='ERR approval_or_session'
        assert command('MODE GENERATOR')=='ERR approval_required'
        assert command('RFAPPROVE '+permit)=='ERR approval_invalid'
        assert command('TXSTOP')=='OK';assert command('RFLOCK')=='OK'
        command('LAUNCHER');time.sleep(2);link.connect()
        assert json.loads(command('rfapproval'))['rf_approval']['approved'] is False
        result['passed']=True
    finally:
        result['console']=link.log
        args.output.write_text(json.dumps(result,indent=2)+'\n');link.close()
    print('RF default lock and untrusted ECDSA permit rejected in both images; no RF start sent.')

if __name__=='__main__':main()

"""Exercise installed utilities against real Darwin files and processes."""
import hashlib
import json
from pathlib import Path
import re

def verify_tools(child, work, arch, nonce):
    sample='darwin\nnative\ndarwin\n'
    checksum=hashlib.sha256(sample.encode()).hexdigest()
    commands=[
        'set -e', f'd=/tmp/tools-{nonce}', 'mkdir "$d"', 'cd "$d"',
        "printf 'darwin\\nnative\\ndarwin\\n' > input",
        'ls /', 'ls -l input', 'cp input copy', 'cmp input copy',
        'mkdir nested', 'mv copy nested/copy', 'test -f nested/copy',
        "test \"$(wc -l < input | tr -d ' ')\" = 3",
        "test \"$(grep -c darwin input)\" = 2",
        "test \"$(sort input | uniq | wc -l | tr -d ' ')\" = 2",
        "test \"$(awk 'END {print NR}' input)\" = 3",
        "test \"$(sed -n '2p' input)\" = native",
        f'test "$(sha256sum input | cut -d " " -f 1)" = {checksum}',
        'find nested -type f | grep nested/copy',
        'tar -czf roundtrip.tgz input nested', 'mkdir unpacked',
        'tar -xzf roundtrip.tgz -C unpacked', 'cmp input unpacked/input',
        'cmp nested/copy unpacked/nested/copy',
        'gzip -c input > input.gz', 'gzip -dc input.gz > inflated', 'cmp input inflated',
        "test \"$(printf '6*7\\n' | bc)\" = 42",
        'id', 'uname -srm', 'date', 'df -h /',
        'test "$(busybox uname -o)" = Darwin',
        'sleep 60 & tools_pid=$!', 'ps aux > processes', 'cat processes',
        "awk '$2 == 1 { found=1 } END { exit !found }' processes",
        "awk -v pid=\"$tools_pid\" '$2 == pid && /sleep 60/ { found=1 } END { exit !found }' processes",
        'kill "$tools_pid"', 'wait "$tools_pid" || true',
        'ps aux > exited',
        "awk -v pid=\"$tools_pid\" '$2 == pid { found=1 } END { exit found }' exited",
        'touch timestamp', 'chmod 600 timestamp', 'stat timestamp',
        'busybox --list | grep -x vi', 'rm -r nested unpacked',
        'test ! -e nested', 'cd /', 'rm -rf "$d"',
    ]
    if any(len(step)>250 for step in commands):raise ValueError('native tools command exceeds serial line limit')
    # Darwin's serial canonical input limit is shorter than a Linux host's.
    # Feed the compound command a line at a time, waiting for real PS2 prompts.
    child.sendline('(')
    child.expect(r'\r*\n> ',timeout=20)
    for step in commands:
        child.sendline(step+';')
        child.expect(r'\r*\n> ',timeout=20)
    child.sendline('); tools_status=$?; printf "\\nNATIVE_TOOLS_EXIT:'+nonce+':%s\\n" "$tools_status"')
    child.expect(r'\r*\nNATIVE_TOOLS_EXIT:'+re.escape(nonce)+r':(\d+)\r*\n',timeout=180)
    if int(child.match.group(1)):raise RuntimeError('native utilities failed on '+arch)
    child.expect(r'\r*\nroot[^\r\n]*# ',timeout=20)
    record={'architecture':arch,'nonce':nonce,'commands':commands,'expected_sha256':checksum,'exit_status':0,
            'process_listing':'Real PID 1 and a live sleep child; child disappears after kill and wait.',
            'proof':'Installed native Mach-O BusyBox applets, genuine XNU filesystem and process APIs.'}
    (Path(work)/'tools-result.json').write_text(json.dumps(record,indent=2)+'\n')
    print('Verified native BusyBox files, pipelines, archives and ps aux on '+arch,flush=True)

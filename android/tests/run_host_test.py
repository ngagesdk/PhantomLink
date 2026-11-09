#!/usr/bin/env python3
"""Android bridge host test: gamecommsd + the Android bridge engine
(BridgeEngine.java + libgcbridge, TCP test devices instead of Bluetooth) +
gcsim --bridge. Same cases as tests/net/run_pcbridge_test.py: explicit
server and LAN discovery, both API generations, dropped device connection.

  run_host_test.py GAMECOMMSD GCSIM LIBDIR CLASSDIR [JAVA]
"""
import socket, subprocess, sys, time

server, gcsim, libdir, classes = sys.argv[1:5]
java = sys.argv[5] if len(sys.argv) > 5 else 'java'


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


sport = free_port()
srv = subprocess.Popen([server, f'--port={sport}', '--log_level=1', '--access_key=test-key'])
procs = [srv]
rc = 0
try:
    time.sleep(0.5)
    for mode in ('explicit', 'auto'):
        dport = free_port()
        host = '127.0.0.1' if mode == 'explicit' else 'auto'
        br = subprocess.Popen([java, f'-Djava.library.path={libdir}', '-cp', classes, 'HostBridge',
                               f'--device_tcp={dport}', f'--server={host}', f'--port={sport}',
                               '--key=test-key', '--log_level=2'])
        procs.append(br)
        time.sleep(1.5)
        cases = [['--api=1', '--players=4'], ['--api=2', '--players=4']]
        if mode == 'explicit':
            cases.append(['--api=1', '--players=3', '--messages=200', '--reconnect'])
            cases.append(['--api=2', '--players=2', '--messages=200', '--reconnect'])
        for args in cases:
            r = subprocess.run([gcsim, f'--bridge=127.0.0.1:{dport}'] + args, timeout=120)
            print(f'android bridge ({mode} server) gcsim', ' '.join(args), '->', r.returncode, flush=True)
            rc |= r.returncode
        br.terminate()
        br.wait(timeout=10)
        procs.remove(br)
finally:
    for p in procs:
        p.terminate()
print('PASS' if rc == 0 else 'FAIL')
sys.exit(rc)

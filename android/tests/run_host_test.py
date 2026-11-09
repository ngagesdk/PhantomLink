#!/usr/bin/env python3
"""Android bridge host test: gamecommsd + the Android bridge engine
(BridgeEngine.java + libgcbridge, TCP test devices instead of Bluetooth) +
gcsim --bridge. Same cases as tests/net/run_pcbridge_test.py: explicit
server and LAN discovery, both API generations, dropped device connection.

  run_host_test.py GAMECOMMSD GCSIM LIBDIR CLASSDIR [JAVA]
"""
import socket, subprocess, sys, threading, time

server, gcsim, libdir, classes = sys.argv[1:5]
java = sys.argv[5] if len(sys.argv) > 5 else 'java'


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait_port(port, timeout=30):
    """until something accepts TCP connections on 127.0.0.1:port"""
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(('127.0.0.1', port), timeout=1).close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def start_bridge(args, timeout=60):
    """start HostBridge and wait until it listens (the JVM can take a while
    on CI); its output is passed through"""
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    ready = threading.Event()

    def pump():
        for line in p.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            if 'listening on' in line:
                ready.set()

    threading.Thread(target=pump, daemon=True).start()
    if not ready.wait(timeout):
        print('android bridge did not start within', timeout, 's', flush=True)
        p.terminate()
        return None
    return p


sport = free_port()
srv = subprocess.Popen([server, f'--port={sport}', '--log_level=1', '--access_key=test-key'])
procs = [srv]
rc = 0
results = []
try:
    if not wait_port(sport):
        print('server did not start', flush=True)
        sys.exit(1)
    for mode in ('explicit', 'auto'):
        dport = free_port()
        host = '127.0.0.1' if mode == 'explicit' else 'auto'
        br = start_bridge([java, f'-Djava.library.path={libdir}', '-cp', classes, 'HostBridge',
                           f'--device_tcp={dport}', f'--server={host}', f'--port={sport}',
                           '--key=test-key', '--log_level=2'])
        if br is None:
            rc = 1
            break
        procs.append(br)
        if mode == 'auto':
            time.sleep(1.0)  # first discovery round, like the PC bridge test
        cases = [['--api=1', '--players=4'], ['--api=2', '--players=4']]
        if mode == 'explicit':
            cases.append(['--api=1', '--players=3', '--messages=200', '--reconnect'])
            cases.append(['--api=2', '--players=2', '--messages=200', '--reconnect'])
        for args in cases:
            r = subprocess.run([gcsim, f'--bridge=127.0.0.1:{dport}'] + args, timeout=120)
            print(f'android bridge ({mode} server) gcsim', ' '.join(args), '->', r.returncode, flush=True)
            results.append(f'{mode:8} {" ".join(args):45} -> {"ok" if r.returncode == 0 else "FAILED (%d)" % r.returncode}')
            rc |= r.returncode
        br.terminate()
        br.wait(timeout=10)
        procs.remove(br)
finally:
    for p in procs:
        p.terminate()
print('\nandroid bridge host test summary:')
for line in results:
    print('  ' + line)
print('PASS' if rc == 0 else 'FAIL')
sys.exit(rc)

#!/usr/bin/env python3
"""Exercise server restart with an existing log and report startup errors."""
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

server = sys.argv[1]


def wait_for_server(proc, port):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise AssertionError(f'server exited early: {proc.communicate()}')
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.05)
    raise AssertionError('server did not start listening')


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


with tempfile.TemporaryDirectory() as tmp:
    log = pathlib.Path(tmp) / 'server.log'
    log.write_text('existing log entry\n', encoding='utf-8')
    args = [server, '--bind=127.0.0.1', '--discovery=0', '--log_level=2',
            '--arena_accounts=', '--rifts_data=', f'--log_file={log}']
    for _ in range(2):
        port = free_port()
        proc = subprocess.Popen(args + [f'--port={port}'], cwd=tmp,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            wait_for_server(proc, port)
        finally:
            proc.terminate()
            proc.communicate(timeout=5)
    content = log.read_text(encoding='utf-8')
    assert content.startswith('existing log entry\n'), content
    assert content.count('listening on 127.0.0.1:') == 2, content

    with socket.socket() as occupied:
        if hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
            occupied.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        occupied.bind(('127.0.0.1', 0))
        occupied.listen()
        port = occupied.getsockname()[1]
        result = subprocess.run(args + [f'--port={port}'], cwd=tmp,
                                capture_output=True, text=True, timeout=5)
    assert result.returncode != 0, 'server unexpectedly bound an occupied port'
    assert 'cannot listen on' in result.stderr, result.stderr
    assert 'cannot listen on' in log.read_text(encoding='utf-8')

    port = free_port()
    bad_log_args = args[:-1] + [f'--log_file={pathlib.Path(tmp) / "missing" / "server.log"}']
    proc = subprocess.Popen(bad_log_args + [f'--port={port}'], cwd=tmp,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        wait_for_server(proc, port)
    finally:
        proc.terminate()
        stdout, stderr = proc.communicate(timeout=5)
    assert b'cannot open log file' in stderr and b'logging to stdout' in stderr, stderr
    assert b'listening on 127.0.0.1:' in stdout, stdout

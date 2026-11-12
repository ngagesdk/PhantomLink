#!/usr/bin/env python3
"""Direct Arena clients (GPRS) with real sockets: starts gamecommsd with
--direct=1 on free ports and plays the N-Gage: Community login over HTTP,
Jabber connect/close, SNAP login over UDP, a datagram from an address that
never connected over TCP is not answered (only loopback here, so that part
is covered by tests/server/test_direct.c)."""
import os, socket, subprocess, sys, tempfile, time

server = sys.argv[1]


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


def soap(port, op, inner):
    body = ("<?xml version='1.0' encoding='UTF-8'?>\n<soapenv:Envelope "
            "xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">\n<soapenv:Body>\n"
            f"<ns1:{op} xmlns:ns1=\"urn:CommunityApp\">\n{inner}</ns1:{op}>\n</soapenv:Body>\n</soapenv:Envelope>\n")
    req = ("POST /n-gage/axis/services/Community HTTP/1.1\r\nHost: arena.n-gage.com\r\nSOAPAction: \"\"\r\n"
           f"Content-Type: text/xml\r\nContent-Length: {len(body)}\r\n\r\n{body}")
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    s.sendall(req.encode())
    data = b''
    while True:  # the server closes after the answer
        k = s.recv(4096)
        if not k:
            break
        data += k
    s.close()
    return data.decode('latin-1')


def login(port, user, pw, imei):
    return soap(port, 'authenticateUser2', f'<username xsi:type="xsd:string">{user}</username>\n'
                f'<password xsi:type="xsd:string">{pw}</password>\n<imei xsi:type="xsd:string">{imei}</imei>\n')


def snap_login(user):
    u = bytearray(16 + 120 + 4)
    u[0], u[1], u[3] = 0x30, 136, 44
    u[16:16 + len(user)] = user.encode()
    u[-4:] = b'\xba\x47\x66\x11'
    return bytes(u)


failures = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what, flush=True)
    if not cond:
        failures.append(what)


tmp = tempfile.mkdtemp()
http, xmpp, snap = free_port(), free_port(), free_port(socket.SOCK_DGRAM)
srv = subprocess.Popen([server, f'--port={free_port()}', '--bind=127.0.0.1', '--discovery=0', '--log_level=3',
                        '--direct=1', '--direct_bind=127.0.0.1', f'--direct_http_port={http}',
                        f'--direct_xmpp_port={xmpp}', f'--direct_snap_port={snap}',
                        f'--arena_accounts={os.path.join(tmp, "acc.txt")}'],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
try:
    time.sleep(0.5)
    check(srv.poll() is None, 'server runs with --direct=1')

    # SNAP before any Community login: not accepted
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.settimeout(1.0)
    u.sendto(snap_login('alice'), ('127.0.0.1', snap))
    try:
        d, _ = u.recvfrom(2048)
        check(d[3] != 41, 'SNAP login without Community login refused')
    except socket.timeout:
        check(True, 'SNAP login without Community login refused (no answer)')

    r = login(http, 'alice', 'pw', '350000000000021')
    check('HTTP/1.1 200 OK' in r and 'xsd:boolean">true<' in r, 'Community login over direct HTTP')
    check('Set-Cookie: JSESSIONID=PL' in r, 'session cookie')
    r = login(http, 'alice', 'wrong', '350000000000021')
    check('xsd:boolean">false<' in r, 'wrong password refused')

    # Jabber: connect, close; the server closes its side too
    j = socket.create_connection(('127.0.0.1', xmpp), timeout=5)
    j.sendall(b"<?xml version='1.0'?><stream:stream to='ngage-auth' xmlns='jabber:client' "
              b"xmlns:stream='http://etherx.jabber.org/streams'>")
    time.sleep(0.2)
    j.shutdown(socket.SHUT_WR)
    data = b''
    try:
        while True:
            k = j.recv(4096)
            if not k:
                break
            data += k
        check(b'stream:stream' in data, 'Jabber stream answered and closed')
    except socket.timeout:
        check(False, 'Jabber connection closed by the server')
    j.close()

    u.sendto(snap_login('alice'), ('127.0.0.1', snap))
    try:
        d, _ = u.recvfrom(2048)
        check(len(d) == 32 and d[3] == 41 and d[-4:-1] == b'\xba\x47\x66', 'SNAP login over direct UDP')
    except socket.timeout:
        check(False, 'SNAP login over direct UDP (no answer)')
    u.close()
finally:
    srv.terminate()
    out, _ = srv.communicate(timeout=10)
if failures:
    print(out)
    sys.exit(1)
sys.exit(0)

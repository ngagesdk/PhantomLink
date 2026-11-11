#!/usr/bin/env python3
"""PPP mode with real processes: gamecommsd + gcbridge (TCP test device) +
pppsim, which speaks PPP to the bridge like an N-Gage and hands the IP
packets to a TUN interface, so the Linux kernel's TCP/IP stack plays the
N-Gage's.  This script then uses the Arena services through it as the game
does: DNS, Community SOAP (login, profile edit kept on the server), a device
page, the Jabber login and a SNAP datagram.  Linux only, needs root for TUN
(exit code 77 = skipped).

usage: run_ppp_test.py <gamecommsd> <pppsim> <bridge command...>
The bridge command may use {dport} (TCP test device port) and {sport}
(server port), e.g. gcbridge --bluetooth=0 --device_tcp={dport}
--server=127.0.0.1 --port={sport}; the Android engine (HostBridge) works
the same way."""
import hashlib
import os, re, socket, struct, subprocess, sys, tempfile, time

server, pppsim, bridge_cmd = sys.argv[1], sys.argv[2], sys.argv[3:]
if not os.path.exists('/dev/net/tun') or os.geteuid() != 0:
    print('needs root and /dev/net/tun: skipped')
    sys.exit(77)


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


tmp = tempfile.mkdtemp()
acc = os.path.join(tmp, 'arena.txt')
sport, dport = free_port(), free_port()
procs = []
ok = True


def check(cond, what):
    global ok
    print(('ok    ' if cond else 'FAIL  ') + what, flush=True)
    ok = ok and cond


def dns(name):
    q = struct.pack('>HHHHHH', 0x4242, 0x0100, 1, 0, 0, 0)
    for label in name.split('.'):
        q += bytes([len(label)]) + label.encode()
    q += b'\0\0\1\0\1'
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    s.sendto(q, ('10.64.0.1', 53))
    r = s.recv(512)
    s.close()
    rcode = r[3] & 15
    return rcode, (socket.inet_ntoa(r[-4:]) if rcode == 0 and struct.unpack('>H', r[6:8])[0] else None)


def http(req):
    s = socket.create_connection(('10.64.0.1', 80), timeout=10)
    s.sendall(req.encode())
    out = b''
    while True:
        d = s.recv(4096)
        if not d:
            break
        out += d
    s.close()
    return out.decode('utf-8', 'replace')


def soap(op, inner='', cookie=None):
    body = ("<?xml version='1.0' encoding='UTF-8'?>\n<soapenv:Envelope "
            'xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/">\n<soapenv:Body>\n'
            f'<ns1:{op} xmlns:ns1="urn:CommunityApp">\n{inner}</ns1:{op}>\n</soapenv:Body>\n</soapenv:Envelope>\n')
    ck = f'Cookie: JSESSIONID={cookie}\r\n' if cookie else ''
    return http(f'POST /n-gage/axis/services/Community HTTP/1.1\r\nHost: arena.n-gage.com\r\nSOAPAction: ""\r\n{ck}'
                f'Content-Length: {len(body.encode())}\r\n\r\n{body}')


try:
    procs.append(subprocess.Popen([server, f'--port={sport}', '--log_level=2', f'--arena_accounts={acc}']))
    time.sleep(0.5)
    procs.append(subprocess.Popen([a.format(dport=dport, sport=sport) for a in bridge_cmd]))
    end = time.time() + 60  # the JVM may take a while
    while time.time() < end:
        try:
            socket.create_connection(('127.0.0.1', dport), timeout=1).close()  # a short test device
            break
        except OSError:
            time.sleep(0.2)
    time.sleep(0.5)
    ng = subprocess.Popen([pppsim, '127.0.0.1', str(dport), f'plppp{os.getpid() % 1000}'], stdout=subprocess.PIPE,
                          text=True)
    procs.append(ng)
    line = ng.stdout.readline().strip()
    if line.startswith('no usable'):
        print(line + ': skipped')
        sys.exit(77)
    check(line == 'UP', 'PPP up between pppsim and gcbridge')
    time.sleep(1.0)  # registration with the server

    check(dns('arena.n-gage.com') == (0, '10.64.0.1'), 'DNS: arena.n-gage.com -> 10.64.0.1')
    check(dns('snap.prod.pk.nokia.sf.yav4.com') == (0, '10.64.0.1'), 'DNS: snap.prod.pk.nokia.sf.yav4.com')
    check(dns('example.org')[0] == 3, 'DNS: other names NXDOMAIN')

    r = soap('authenticateUser2', '<username xsi:type="xsd:string">mupf</username>\n'
             '<password xsi:type="xsd:string">secret</password>\n<imei xsi:type="xsd:string">350000000000001</imei>\n')
    check('<username xsi:type="xsd:string">mupf</username>' in r, 'SOAP login returns the user name')
    bad = soap('authenticateUser2', '<username xsi:type="xsd:string">mupf</username>\n'
               '<password xsi:type="xsd:string">wrong</password>\n<imei xsi:type="xsd:string">350000000000001</imei>\n')
    check('xsd:boolean">false<' in bad and 'JSESSIONID' not in bad, 'SOAP login with a wrong password fails')
    m = re.search(r'JSESSIONID=([^;\s]+)', r)
    cookie = m.group(1) if m else None
    check(cookie is not None, 'SOAP login sets a session cookie')
    r = soap('editAccount', '<firstName xsi:type="xsd:string">Michael</firstName>\n'
             '<lastName xsi:type="xsd:string">F</lastName>\n<emailAddress xsi:type="xsd:string">m@x</emailAddress>\n',
             cookie)
    check('<errorCode xsi:type="xsd:int">0</errorCode>' in r, 'SOAP editAccount succeeds')
    r = soap('viewAccount', '', cookie)
    check('<item>Michael</item><item>F</item><item>m@x</item>' in r, 'SOAP viewAccount returns the saved profile')
    check('mupf\tfirstName\tMichael' in open(acc).read(), 'profile kept in the accounts file')
    check('secret' not in open(acc).read(), 'no plain password in the accounts file')

    big = soap('editQuote', '<quote xsi:type="xsd:string">' + 'q' * 120 + '</quote>\n' + '<!--' + 'x' * 5000 + '-->',
               cookie)
    check('errorCode' in big, 'large request over several TCP segments')

    r = http('GET /n-gage/device/community/ranking_list_x.jsp;jsessionid=x?username=mupf HTTP/1.1\r\n\r\n')
    check('<listTotal value="0"/>' in r, 'device list page')

    s = socket.create_connection(('10.64.0.1', 5222), timeout=10)
    s.sendall(b"<?xml version='1.0'?><stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
              b"xmlns='jabber:client' to='ngage-auth'><iq type='get' id='q'><query xmlns='jabber:iq:auth'>"
              b"<username>mupf</username></query></iq>")
    got = b''
    t0 = time.time()
    while b"id='q'" not in got and time.time() - t0 < 5:
        got += s.recv(4096)
    pwsha1 = hashlib.sha1(b'secret').hexdigest().encode()
    s.sendall(b"<iq type='set' id='a'><query xmlns='jabber:iq:auth'><username>mupf</username><password>" + pwsha1 +
              b"</password><resource>segachat</resource></query></iq>")
    while b"id='a'" not in got and time.time() - t0 < 5:
        got += s.recv(4096)
    check(b"<iq type='result' id='a'/>" in got, 'Jabber login')
    s.close()

    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.sendto(b'\x01\x02snap', ('10.64.0.1', 8080))
    u.close()
    time.sleep(0.5)
    check(procs[0].poll() is None and procs[1].poll() is None, 'server and bridge still running')
finally:
    for p in reversed(procs):
        p.terminate()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
print('PPP mode', 'ok' if ok else 'FAILED')
sys.exit(0 if ok else 1)

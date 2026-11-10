#!/usr/bin/env python3
"""N-Gage Arena native endpoints for retail games over PPP (docs/ARENA.md).

No longer needed: the bridges now have a built-in PPP peer and tunnel the
game's traffic to gamecommsd, which answers it (server/src/gcs_native.c).
Kept for experiments with pppd.

Runs on the PC bridge next to pppd and answers the original Arena stack
of the game in its own protocols:

  DNS   udp/53    the Arena host names -> this machine (other names: refused)
  HTTP  tcp/80    Community SOAP service (login, account, alerts: success
                  answers in the shapes the Arena DLLs parse), empty
                  lists for the *_x.jsp pages, a stub page for other JSPs
  XMPP  tcp/5222  SegaChat: minimal Jabber server (stream, jabber:iq:auth,
                  empty roster, every other iq answered with an empty result)
  SNAP  udp/8080  game server: logs every datagram as hex (not answered yet)

The logs of the XMPP and SNAP ports are the material for reverse
engineering those protocols.  Needs root for ports 53 and 80.

  sudo python3 arena_native.py --addr 10.64.0.1 [--log arena.log]

By default the ports are bound to --addr only (with IP_FREEBIND, so this
works before pppd has created the interface).  That avoids clashes with
resolvers already listening on port 53, such as WSL's DNS tunnel or
systemd-resolved.  --bind 0.0.0.0 listens on every address instead.
"""
import argparse, datetime, re, select, socket, struct, sys
import json
from xml.sax.saxutils import escape as xml_escape, unescape as xml_unescape

ARENA_HOSTS = ('arena.n-gage.com', 'im.prod.pk.nokia.sf.yav4.com', 'snap.prod.pk.nokia.sf.yav4.com',
               'ngage-auth')
SOAP_HEAD = ('<?xml version="1.0" encoding="utf-8"?>'
             '<soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/" '
             'xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">'
             '<soapenv:Body><ns1:{op}Response soapenv:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/" '
             'xmlns:ns1="urn:CommunityApp">')
SOAP_TAIL = '</ns1:{op}Response></soapenv:Body></soapenv:Envelope>'
BOOL_TRUE = '<{op}Return xsi:type="xsd:boolean">true</{op}Return>'
ERR_OK = '<{op}Return><errorCode xsi:type="xsd:int">0</errorCode></{op}Return>'


def items(*vals):
    """Plain <item>v</item> lists, as cafmcustomize.dll searches for literally."""
    return ''.join(f'<item>{v}</item>' for v in vals)


def typed_items(*vals):
    return ''.join(f'<item xsi:type="xsd:string">{v}</item>' for v in vals)


ACCOUNT_FIELDS = {
    # view op: (edit op, request fields, kind) - order is the order of the <item>s
    'viewAccount': ('editAccount', ('firstName', 'lastName', 'emailAddress'), 'str'),
    'viewDisplayOptions': ('editDisplayOptions', ('displayFirstName', 'displayLastName', 'displayCity',
                                                  'displayState', 'displayCountry'), 'bool'),
    'viewAlertSubscriptions': ('editAlertSubscriptions', ('gameReleaseNews', 'tournamentNews', 'ladderNews',
                                                          'downloadNews', 'activityNews', 'ngageNews'), 'bool'),
    'viewAlertOptions': ('editAlertOptions', ('sendEmail', 'emailAddress'), 'mixed'),
    'viewFavorites': ('editFavorites', ('favGame', 'favMusic', 'favFood', 'hobbies', 'zodiac'), 'nil'),
    'viewQuote': ('editQuote', ('quote',), 'quote'),
}
EDIT_OPS = {v[0]: v[1] for v in ACCOUNT_FIELDS.values()}


class Accounts:
    """Per user profile data, kept in a JSON file so edits survive restarts."""

    def __init__(self, path):
        self.path, self.data = path, {}
        if path:
            try:
                with open(path) as f:
                    self.data = json.load(f)
            except (OSError, ValueError):
                pass

    def get(self, user):
        return self.data.setdefault(user, {})

    def save(self):
        if self.path:
            with open(self.path, 'w') as f:
                json.dump(self.data, f, indent=1)


accounts = Accounts(None)
sessions = {}  # JSESSIONID -> user name
last_user = 'player'


def field(body, name):
    m = re.search(r'<%s\b[^>]*?(/>|>(.*?)</%s>)' % (name, name), body, re.S)
    return None if not m else (m.group(2) or '')


def boolstr(v):
    return 'true' if (v or '').strip().lower() in ('true', 'yes', '1') else 'false'


def soap_body(op, user, body=''):
    """Return element(s) for one Community operation, shaped after the parsers in
    cafmlogin.dll, cafmmenumanager.dll and cafmcustomize.dll (docs/ARENA.md)."""
    ret = '<{op}Return>{v}</{op}Return>'
    acc = accounts.get(user)
    if op in ('authenticateUser2', 'createUser'):
        # cafmlogin: "xsd:boolean" + "true", then the first "xsd:string" after it
        # up to '<' is the Arena user name (Jabber login, username= in URLs)
        return (BOOL_TRUE.format(op=op) +
                f'<username xsi:type="xsd:string">{xml_escape(user)}</username>')
    if op == 'checkImei':
        return BOOL_TRUE.format(op=op)
    if op == 'getNewAlertStatus':  # compared with "true"/"false"
        return ret.format(op=op, v='false')
    if op in ACCOUNT_FIELDS:
        _, names, kind = ACCOUNT_FIELDS[op]
        vals = [acc.get(n, '') for n in names]
        if op == 'viewAccount' and not vals[0] and not acc:
            vals[0] = user
        if kind == 'bool':
            return ret.format(op=op, v=items(*[boolstr(v) for v in vals]))
        if kind == 'mixed':
            return ret.format(op=op, v=items(boolstr(vals[0]), xml_escape(vals[1])))
        if kind == 'nil':
            return ret.format(op=op, v=typed_items(*[xml_escape(v) for v in vals] + ['']))
        if kind == 'quote':
            return f'<viewQuoteReturn xsi:type="xsd:string">{xml_escape(vals[0])}</viewQuoteReturn>'
        return ret.format(op=op, v=items(*[xml_escape(v) for v in vals]))
    if op in EDIT_OPS:
        for n in EDIT_OPS[op]:
            v = field(body, n)
            if v is not None:
                acc[n] = xml_unescape(v)
        accounts.save()
        log(f'soap: {op} saved for {user!r}')
    elif op == 'changePassword':
        acc['password'] = xml_unescape(field(body, 'newPassword') or '')
        accounts.save()
    elif op == 'getSecurityQuestion':
        return f'<{op}Return xsi:type="xsd:string">{xml_escape(acc.get("question", ""))}</{op}Return>'
    elif op == 'updateSecurityQA':
        acc['question'] = xml_unescape(field(body, 'question') or '')
        acc['answer'] = xml_unescape(field(body, 'answer') or '')
        accounts.save()
    # edit*/change*/delete*/unlink*/update*: errorCode 0 means success
    return ERR_OK.format(op=op)


# list pages (*_x.jsp) are XML read by cafmplay.dll: elements currentStart and
# listTotal with a value attribute, and itemNode elements with index,
# contentId, gameId, mediaPath and username attributes.  An empty list for now.
LIST_XML = ('<?xml version="1.0" encoding="utf-8"?><list>'
            '<currentStart value="0"/><listTotal value="0"/></list>')
PAGE_HTML = ('<?xml version="1.0" encoding="utf-8"?>'
             '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>N-Gage Arena</title></head>'
             '<body><p>PhantomLink Arena emulator: nothing here yet.</p></body></html>')

logf = sys.stdout


def log(*a):
    print(datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3], *a, file=logf, flush=True)


def hexdump(data):
    return ' '.join(f'{b:02x}' for b in data)


def dns_answer(q, addr):
    """Minimal DNS: A record for the Arena names, REFUSED for others."""
    if len(q) < 12:
        return None
    tid, flags, qd = struct.unpack('>HHH', q[:6])
    if qd != 1:
        return None
    i, labels = 12, []
    while i < len(q) and q[i]:
        n = q[i]
        labels.append(q[i + 1:i + 1 + n].decode('ascii', 'replace'))
        i += 1 + n
    qend = i + 5
    if qend > len(q):
        return None
    name = '.'.join(labels).lower()
    qtype = struct.unpack('>H', q[i + 1:i + 3])[0]
    question = q[12:qend]
    if name in ARENA_HOSTS and qtype == 1:
        log(f'dns: {name} -> {addr}')
        ans = b'\xc0\x0c' + struct.pack('>HHIH', 1, 1, 60, 4) + socket.inet_aton(addr)
        return struct.pack('>HHHHHH', tid, 0x8180, 1, 1, 0, 0) + question + ans
    log(f'dns: {name} type {qtype} refused')
    return struct.pack('>HHHHHH', tid, 0x8185, 1, 0, 0, 0) + question




def http_reply(req):
    head, _, body = req.partition(b'\r\n\r\n')
    first = head.split(b'\r\n', 1)[0].decode('latin-1')
    action = re.search(rb'(?im)^SOAPAction:\s*(.*)$', head)
    action = action.group(1).decode('latin-1').strip() if action else ''
    log(f'http: {first} SOAPAction {action!r} ({len(body)} bytes)')
    log(f'http body: {show(body[:2000])}')
    path = first.split(' ')[1] if ' ' in first else ''
    if '/services/Community' in path:
        m = re.search(rb'<ns1:(\w+)', body) or re.search(rb'<(\w+) [^>]*urn:CommunityApp', body)
        op = m.group(1).decode() if m else (action.strip('"').rsplit('#', 1)[-1] or 'unknown')
        global last_user
        text = body.decode('utf-8', 'replace')
        ck = re.search(rb'(?i)JSESSIONID=([^;\s]+)', head)
        u = field(text, 'username')
        if op in ('authenticateUser2', 'createUser') and u:
            user = last_user = xml_unescape(u)
        else:
            user = sessions.get(ck.group(1).decode('latin-1'), last_user) if ck else last_user
        sid = session_id(user)
        sessions[sid] = user
        log(f'soap: {op} ({user!r})')
        out = (SOAP_HEAD.format(op=op) + soap_body(op, user, text) + SOAP_TAIL.format(op=op)).encode()
        return reply(out, 'text/xml; charset=utf-8', cookie=sid)
    base = path.split(';', 1)[0].split('?', 1)[0]
    if base.endswith('_x.jsp'):
        return reply(LIST_XML.encode(), 'text/xml; charset=utf-8')
    if base.endswith('.jsp') or '/servlets/' in base:
        return reply(PAGE_HTML.encode(), 'application/xhtml+xml; charset=utf-8')
    return b'HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n'


def session_id(user):
    return 'PL' + ''.join(f'{b:02X}' for b in user.encode('utf-8'))[:40]


def reply(out, ctype, cookie=None):
    # cafmlogin.dll keeps the Set-Cookie value (up to ';') after the login
    c = f'Set-Cookie: JSESSIONID={cookie}; Path=/n-gage\r\n' if cookie else ''
    return (f'HTTP/1.1 200 OK\r\nContent-Type: {ctype}\r\n{c}'
            f'Content-Length: {len(out)}\r\nConnection: close\r\n\r\n').encode() + out


def show(d):
    """Text with every non printable byte as \\xNN (the client sends binary junk at times)."""
    return ''.join(chr(b) if 32 <= b < 127 or b in (9, 10, 13) else f'\\x{b:02x}' for b in d)


ATTR = re.compile(r"""(\w+)=['"]([^'"]*)['"]""")


class Xmpp:
    """Just enough Jabber 1.x (jabber:iq:auth era) for the SegaChat client."""

    def __init__(self, sock):
        self.s = sock
        self.buf = ''
        self.started = False
        self.user = ''

    def send(self, text):
        log(f'xmpp -> {text}')
        self.s.sendall(text.encode())

    def feed(self, data):
        self.buf += data.decode('utf-8', 'replace')
        if not self.started and '<stream:stream' in self.buf:
            self.started = True
            self.buf = self.buf[self.buf.index('<stream:stream'):]
            end = self.buf.index('>') + 1 if '>' in self.buf else 0
            if not end:
                return
            self.buf = self.buf[end:]
            self.send("<?xml version='1.0'?><stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                      "xmlns='jabber:client' from='ngage-auth' id='pl0001'>")
        while True:
            m = re.match(r'\s*<(iq|presence|message)\b([^>]*?)(/>|>)', self.buf, re.S)
            if not m:
                if '</stream:stream>' in self.buf:
                    self.send('</stream:stream>')
                    self.buf = ''
                return
            tag, attrs, close = m.group(1), m.group(2), m.group(3)
            if close == '/>':
                end = m.end()
            else:
                e = self.buf.find(f'</{tag}>', m.end())
                if e < 0:
                    return  # incomplete
                end = e + len(tag) + 3
            stanza, self.buf = self.buf[:end], self.buf[end:]
            self.stanza(tag, dict(ATTR.findall(attrs)), stanza)

    def stanza(self, tag, a, xml):
        if tag != 'iq':
            return  # presence/messages: nobody else is online yet
        sid = a.get('id', '')
        typ = a.get('type', '')
        idattr = f" id='{sid}'" if sid else ''
        if 'jabber:iq:auth' in xml and typ == 'get':
            self.send(f"<iq type='result'{idattr}><query xmlns='jabber:iq:auth'>"
                      "<username/><password/><resource/></query></iq>")
        elif 'jabber:iq:auth' in xml and typ == 'set':
            u = re.search(r'<username>(.*?)</username>', xml, re.S)
            self.user = u.group(1) if u else ''
            log(f'xmpp: auth user {self.user!r}')
            self.send(f"<iq type='result'{idattr}/>")
        elif 'jabber:iq:roster' in xml and typ == 'get':
            self.send(f"<iq type='result'{idattr}><query xmlns='jabber:iq:roster'/></iq>")
        elif typ in ('get', 'set'):
            ns = re.search(r"<query xmlns=['\"]([^'\"]+)", xml)
            body = f"<query xmlns='{ns.group(1)}'/>" if ns else ''
            self.send(f"<iq type='result'{idattr}>{body}</iq>")


def http_complete(buf):
    head, sep, body = buf.partition(b'\r\n\r\n')
    if not sep:
        return False
    m = re.search(rb'(?im)^Content-Length:\s*(\d+)', head)
    return len(body) >= (int(m.group(1)) if m else 0)


IP_FREEBIND = getattr(socket, 'IP_FREEBIND', 15)  # Linux


def bound(kind, addr, port):
    s = socket.socket(socket.AF_INET, kind)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if addr not in ('', '0.0.0.0'):
        try:
            s.setsockopt(socket.IPPROTO_IP, IP_FREEBIND, 1)
        except OSError:
            pass  # not Linux: the address must exist already
    try:
        s.bind((addr, port))
    except OSError as e:
        sys.exit(f'cannot bind {addr}:{port}: {e} (check with: sudo ss -lnp sport = :{port})')
    if kind == socket.SOCK_STREAM:
        s.listen(16)
    return s


def main():
    global logf
    ap = argparse.ArgumentParser()
    ap.add_argument('--addr', default='10.64.0.1', help='address the Arena names resolve to')
    ap.add_argument('--bind', help='listen address (default: --addr)')
    ap.add_argument('--dns', type=int, default=53)
    ap.add_argument('--http', type=int, default=80)
    ap.add_argument('--xmpp', type=int, default=5222)
    ap.add_argument('--snap', type=int, default=8080)
    ap.add_argument('--log')
    ap.add_argument('--accounts', default='arena_accounts.json', help='profile data file')
    a = ap.parse_args()
    if a.log:
        logf = open(a.log, 'a')
    global accounts
    accounts = Accounts(a.accounts)

    where = a.bind or a.addr
    dns = bound(socket.SOCK_DGRAM, where, a.dns)
    snap = bound(socket.SOCK_DGRAM, where, a.snap)
    http = bound(socket.SOCK_STREAM, where, a.http)
    xmpp = bound(socket.SOCK_STREAM, where, a.xmpp)
    conns = {}  # socket -> [kind, buffer]
    log(f'listening on {where}: dns {a.dns}, http {a.http}, xmpp {a.xmpp}, snap {a.snap}; names -> {a.addr}')
    while True:
        r, _, _ = select.select([dns, snap, http, xmpp] + list(conns), [], [], 1.0)
        for s in r:
            if s is dns:
                q, src = dns.recvfrom(1500)
                out = dns_answer(q, a.addr)
                if out:
                    dns.sendto(out, src)
            elif s is snap:
                d, src = snap.recvfrom(65535)
                log(f'snap: {src[0]}:{src[1]} {len(d)} bytes: {hexdump(d)}')
            elif s in (http, xmpp):
                c, src = s.accept()
                kind = 'http' if s is http else 'xmpp'
                log(f'{kind}: connection from {src[0]}:{src[1]}')
                conns[c] = [kind, b'', Xmpp(c) if kind == 'xmpp' else None]
            else:
                kind, buf, xm = conns[s]
                try:
                    d = s.recv(65535)
                except OSError:
                    d = b''
                if not d:
                    log(f'{kind}: closed')
                    s.close()
                    del conns[s]
                    continue
                buf += d
                conns[s][1] = buf
                if kind == 'xmpp':
                    log(f'xmpp <- {show(d)}')
                    try:
                        xm.feed(d)
                    except OSError:
                        pass
                elif http_complete(buf):
                    s.sendall(http_reply(buf))
                    s.close()
                    del conns[s]


if __name__ == '__main__':
    main()

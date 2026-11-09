/*
 * Android bridge runtime: the equivalent of pcbridge/src/pcb_main.c.
 *
 * One native bridge engine per N-Gage. The N-Gage connections come in
 * through deviceConnected() (Bluetooth RFCOMM on Android, TCP in the host
 * tests); each N-Gage gets its own TCP connection to the server, which is
 * given explicitly or found on the local network (GCDISCOVER1 broadcast).
 *
 * Threading: all engine state lives on one thread (the loop). Blocking
 * socket reads, connects and writes run on I/O threads and post their
 * results back to the loop, tagged with a generation number so results of
 * a connection that was replaced in the meantime are ignored.
 *
 * Plain Java (no Android classes), so the host tests run it unchanged.
 *
 * SPDX-License-Identifier: MIT
 */
package org.ngagesdk.phantomlink.bridge;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

public final class BridgeEngine
{
    public static final int LOG_ERROR = 0;
    public static final int LOG_WARN = 1;
    public static final int LOG_INFO = 2;
    public static final int LOG_DEBUG = 3;

    private static final int DEVICE_GRACE_MS = 15000; /* bridge_default_cfg() */
    private static final int FREE_AFTER_MS = 3000;    /* PCB_FREE_AFTER_MS */
    private static final int CONNECT_TIMEOUT_MS = 8000;
    private static final int MAX_PENDING_BYTES = 256 * 1024;
    private static final int STATUS_INTERVAL_MS = 250;
    private static final String DISCOVER_REQ = "GCDISCOVER1";
    private static final String DISCOVER_ANS = "GCSERVER1 ";

    public interface Listener
    {
        void onLog(int level, String line);

        /** called on the engine thread; keep it short */
        void onStatus(Status status);
    }

    /** One N-Gage as shown to the user. */
    public static final class DeviceInfo
    {
        public final String address;
        public final String name;
        public final boolean present;       /* Bluetooth connection is there */
        public final boolean linkUp;        /* GameComms link to the N-Gage is up */
        public final int serverState;       /* NativeBridge.SRV_* */
        public final boolean registered;
        public final long toServer;
        public final long fromServer;

        DeviceInfo(String address, String name, boolean present, boolean linkUp, int serverState, boolean registered,
                   long toServer, long fromServer)
        {
            this.address = address;
            this.name = name;
            this.present = present;
            this.linkUp = linkUp;
            this.serverState = serverState;
            this.registered = registered;
            this.toServer = toServer;
            this.fromServer = fromServer;
        }
    }

    public static final class Status
    {
        public final boolean running;
        public final boolean networkUp;
        public final boolean autoServer;
        /** "host:port" of the server in use or found, null while searching */
        public final String serverAddress;
        public final List<DeviceInfo> devices;

        Status(boolean running, boolean networkUp, boolean autoServer, String serverAddress, List<DeviceInfo> devices)
        {
            this.running = running;
            this.networkUp = networkUp;
            this.autoServer = autoServer;
            this.serverAddress = serverAddress;
            this.devices = devices;
        }

        public int presentCount()
        {
            int n = 0;
            for (DeviceInfo d : devices) {
                if (d.present) {
                    n++;
                }
            }
            return n;
        }
    }

    private final BridgeConfig cfg;
    private final Listener listener;
    private final SecureRandom random = new SecureRandom();
    private final long t0 = System.nanoTime();

    private final ScheduledExecutorService loop;
    private final ExecutorService io;

    /* loop thread only */
    private final List<Session> sessions = new ArrayList<>();
    private ScheduledFuture<?> pollFuture;
    private boolean running;
    private boolean stopped;
    private boolean networkUp = true;
    private DatagramSocket udp;
    private InetSocketAddress found;
    private int tDiscover;
    private boolean discoverySent;
    private int tStatus;
    private boolean statusDirty = true;
    private int tStatusLog;

    public BridgeEngine(BridgeConfig cfg, Listener listener)
    {
        this.cfg = cfg;
        this.listener = listener;
        loop = Executors.newSingleThreadScheduledExecutor(named("gc-engine"));
        io = Executors.newCachedThreadPool(named("gc-io"));
    }

    private static ThreadFactory named(final String name)
    {
        final AtomicInteger n = new AtomicInteger();
        return r -> {
            Thread t = new Thread(r, name + "-" + n.incrementAndGet());
            t.setDaemon(true);
            return t;
        };
    }

    public static String version()
    {
        return NativeBridge.nativeVersion();
    }

    /* ------------------------------------------------------------------ */
    /* public API, any thread                                             */
    /* ------------------------------------------------------------------ */

    public void start()
    {
        post(() -> {
            if (running || stopped) {
                return;
            }
            running = true;
            log(LOG_INFO, "PhantomLink bridge " + version() + " started");
            if (cfg.isAuto()) {
                log(LOG_INFO, "looking for a server on the local network (port " + cfg.port + ")");
                discoverySend();
            } else {
                log(LOG_INFO, "server " + cfg.server + ":" + cfg.port);
            }
            statusDirty = true;
            kick();
        });
    }

    /** Disconnects everything; the engine cannot be restarted. */
    public void stop()
    {
        post(() -> {
            if (stopped) {
                return;
            }
            for (Session s : new ArrayList<>(sessions)) {
                free(s);
            }
            if (udp != null) {
                udp.close();
                udp = null;
            }
            boolean was = running;
            running = false;
            stopped = true;
            if (was) {
                log(LOG_INFO, "stopped");
            }
            publishStatus();
            loop.shutdown();
            io.shutdownNow();
        });
    }

    /** A new connection from an N-Gage. The engine owns it from now on. */
    public void deviceConnected(final DeviceConnection c)
    {
        if (!post(() -> accepted(c))) {
            c.close();
        }
    }

    /** Internet/LAN connectivity of the phone (the ESP32's Wi-Fi state). */
    public void setNetwork(final boolean up)
    {
        post(() -> {
            if (up == networkUp) {
                return;
            }
            networkUp = up;
            log(LOG_INFO, up ? "network available" : "no network");
            if (up && cfg.isAuto()) {
                found = null;
                discoverySend();
            }
            for (Session s : sessions) {
                NativeBridge.nativeSetNetwork(s.h, up, 0, now());
            }
            statusDirty = true;
            kick();
        });
    }

    /** Requests a fresh status callback. */
    public void refresh()
    {
        post(() -> {
            statusDirty = true;
            publishStatus();
        });
    }

    /* ------------------------------------------------------------------ */
    /* loop helpers                                                       */
    /* ------------------------------------------------------------------ */

    private boolean post(Runnable r)
    {
        try {
            loop.execute(() -> {
                try {
                    r.run();
                } catch (RuntimeException e) {
                    log(LOG_ERROR, "internal error: " + e);
                }
            });
            return true;
        } catch (java.util.concurrent.RejectedExecutionException e) {
            return false;
        }
    }

    private int now()
    {
        return (int)((System.nanoTime() - t0) / 1000000L);
    }

    private void log(int level, String line)
    {
        if (level <= cfg.logLevel && listener != null) {
            listener.onLog(level, line);
        }
    }

    /** Schedules the next poll according to the engines' timers. */
    private void kick()
    {
        if (stopped) {
            return;
        }
        int now = now();
        long delay = 1000;
        for (Session s : sessions) {
            delay = Math.min(delay, NativeBridge.nativeNextTimeout(s.h, now) & 0xffffffffL);
        }
        if (statusDirty) {
            delay = Math.min(delay, Math.max(0, STATUS_INTERVAL_MS - (now - tStatus)));
        }
        if (delay < 1) {
            delay = 1;
        }
        if (pollFuture != null) {
            pollFuture.cancel(false);
        }
        pollFuture = loop.schedule(this::pollAll, delay, TimeUnit.MILLISECONDS);
    }

    private void pollAll()
    {
        if (stopped) {
            return;
        }
        int now = now();
        for (Session s : new ArrayList<>(sessions)) {
            if (s.srvWanted) {
                srvStart(s);
            }
            NativeBridge.nativePoll(s.h, now);
            if (s.srvWanted) {
                srvStart(s);
            }
            if (s.devError) {
                devLost(s, "write error");
            }
            /* forget an N-Gage that did not come back */
            if (s.dev == null) {
                int[] st = status(s);
                if (st[NativeBridge.ST_DEV_GRACE] == 0 && now - s.tGone > DEVICE_GRACE_MS + FREE_AFTER_MS) {
                    log(LOG_INFO, "N-Gage " + s.label() + " removed");
                    free(s);
                }
            }
        }
        /* keep looking for the server while it is unknown */
        if (running && cfg.isAuto() && found == null && networkUp && now - tDiscover > (sessions.isEmpty() ? 3000 : 2000)) {
            discoverySend();
        }
        if (now - tStatusLog > 60000) {
            tStatusLog = now;
            logStatus();
        }
        publishStatus();
        kick();
    }

    /* ------------------------------------------------------------------ */
    /* sessions (one per N-Gage)                                          */
    /* ------------------------------------------------------------------ */

    private final class Session implements BridgeCallbacks
    {
        final byte[] addr;
        final String addrStr;
        String name;
        long h;
        boolean freed;

        DeviceConnection dev;
        int devGen;
        ExecutorService devWriter;
        final AtomicInteger devPending = new AtomicInteger();
        boolean devError;
        int tGone;

        Socket srv;
        int srvGen;
        ExecutorService srvWriter;
        final AtomicInteger srvPending = new AtomicInteger();
        boolean srvWanted;
        boolean srvConnecting;
        boolean srvUp;
        String srvAddr;

        Session(byte[] addr)
        {
            this.addr = addr.clone();
            this.addrStr = formatAddress(addr);
        }

        String label()
        {
            return name != null && !name.isEmpty() ? name + " (" + addrStr + ")" : addrStr;
        }

        @Override
        public void devWrite(byte[] data)
        {
            if (dev == null || devError || devWriter == null) {
                return;
            }
            if (devPending.addAndGet(data.length) > MAX_PENDING_BYTES) {
                devError = true; /* nobody reads: drop the connection, the link resumes */
                post(() -> {
                    if (!freed && devError) {
                        devLost(this, "not reading");
                    }
                });
                return;
            }
            final DeviceConnection c = dev;
            final int gen = devGen;
            devWriter.execute(() -> {
                try {
                    OutputStream o = c.output();
                    o.write(data);
                    o.flush();
                    devPending.addAndGet(-data.length);
                } catch (IOException e) {
                    post(() -> {
                        if (!freed && gen == devGen) {
                            devLost(this, "write error");
                        }
                    });
                }
            });
        }

        @Override
        public void srvWrite(byte[] data)
        {
            if (!srvUp || srvWriter == null) {
                return;
            }
            if (srvPending.addAndGet(data.length) > MAX_PENDING_BYTES) {
                final int gen = srvGen;
                post(() -> srvLost(this, gen, "server does not read"));
                return;
            }
            final Socket sock = srv;
            final int gen = srvGen;
            srvWriter.execute(() -> {
                try {
                    OutputStream o = sock.getOutputStream();
                    o.write(data);
                    o.flush();
                    srvPending.addAndGet(-data.length);
                } catch (IOException e) {
                    post(() -> srvLost(this, gen, "write error"));
                }
            });
        }

        @Override
        public void srvConnect()
        {
            srvWanted = true; /* started after the current native call */
            post(() -> {
                if (!freed && srvWanted) {
                    srvStart(this);
                    kick();
                }
            });
        }

        @Override
        public void srvDisconnect()
        {
            srvWanted = false;
            srvClose(this);
        }

        @Override
        public void log(String line)
        {
            BridgeEngine.this.log(LOG_DEBUG, "[" + addrStr + "] " + line);
        }
    }

    private int[] status(Session s)
    {
        int[] st = new int[NativeBridge.ST_COUNT];
        NativeBridge.nativeStatus(s.h, st);
        return st;
    }

    private Session find(byte[] addr)
    {
        for (Session s : sessions) {
            if (java.util.Arrays.equals(s.addr, addr)) {
                return s;
            }
        }
        return null;
    }

    private Session newSession(byte[] addr)
    {
        if (sessions.size() >= cfg.maxDevices) {
            /* full: an N-Gage that is present wins over one that is away */
            Session old = null;
            int now = now();
            for (Session q : sessions) {
                if (q.dev == null && (old == null || now - q.tGone > now - old.tGone)) {
                    old = q;
                }
            }
            if (old == null) {
                return null;
            }
            log(LOG_INFO, "N-Gage " + old.label() + " removed to make room");
            free(old);
        }
        Session s = new Session(addr);
        int linkId;
        do {
            linkId = random.nextInt();
        } while (linkId == 0);
        s.h = NativeBridge.nativeCreate(s, linkId, cfg.accessKey);
        if (s.h == 0) {
            log(LOG_ERROR, "cannot create the bridge engine");
            return null;
        }
        NativeBridge.nativeSetNetwork(s.h, networkUp, 0, now());
        sessions.add(s);
        return s;
    }

    private void free(Session s)
    {
        if (s.freed) {
            return;
        }
        if (s.dev != null) {
            s.dev.close();
            s.dev = null;
        }
        if (s.devWriter != null) {
            s.devWriter.shutdownNow();
            s.devWriter = null;
        }
        srvClose(s);
        s.freed = true;
        sessions.remove(s);
        NativeBridge.nativeDestroy(s.h);
        s.h = 0;
        statusDirty = true;
    }

    /* ------------------------------------------------------------------ */
    /* N-Gage side                                                        */
    /* ------------------------------------------------------------------ */

    private void accepted(DeviceConnection c)
    {
        if (!running) {
            c.close();
            return;
        }
        byte[] addr = c.address();
        Session s = find(addr);
        if (s == null) {
            s = newSession(addr);
            if (s == null) {
                log(LOG_WARN, "N-Gage " + formatAddress(addr) + " refused: " + sessions.size()
                                  + " N-Gages connected (maximum)");
                c.close();
                return;
            }
        } else if (s.dev != null) {
            devLost(s, "replaced by a new connection");
        }
        if (c.name() != null) {
            s.name = c.name();
        }
        s.dev = c;
        s.devGen++;
        s.devError = false;
        s.devPending.set(0);
        s.devWriter = Executors.newSingleThreadExecutor(named("gc-btw"));
        log(LOG_INFO, "N-Gage " + s.label() + " connected");
        NativeBridge.nativeDevConnected(s.h, addr, now());
        startDeviceReader(s, c, s.devGen);
        statusDirty = true;
        kick();
    }

    private void startDeviceReader(final Session s, final DeviceConnection c, final int gen)
    {
        io.execute(() -> {
            String why = "disconnected";
            try {
                InputStream in = c.input();
                byte[] buf = new byte[2048];
                for (;;) {
                    int n = in.read(buf);
                    if (n < 0) {
                        break;
                    }
                    if (n == 0) {
                        continue;
                    }
                    final byte[] d = java.util.Arrays.copyOf(buf, n);
                    post(() -> {
                        if (!s.freed && gen == s.devGen) {
                            NativeBridge.nativeDevInput(s.h, d, 0, d.length, now());
                            kick();
                        }
                    });
                }
            } catch (IOException e) {
                why = "connection lost";
            }
            final String reason = why;
            post(() -> {
                if (!s.freed && gen == s.devGen) {
                    devLost(s, reason);
                    kick();
                }
            });
        });
    }

    private void devLost(Session s, String why)
    {
        if (s.dev == null) {
            return;
        }
        s.dev.close();
        s.dev = null;
        s.devGen++;
        if (s.devWriter != null) {
            s.devWriter.shutdownNow();
            s.devWriter = null;
        }
        s.devPending.set(0);
        s.devError = false;
        s.tGone = now();
        log(LOG_INFO, "N-Gage " + s.label() + " disconnected (" + why + ")");
        NativeBridge.nativeDevDisconnected(s.h, now());
        statusDirty = true;
    }

    /* ------------------------------------------------------------------ */
    /* server side                                                        */
    /* ------------------------------------------------------------------ */

    private void srvClose(Session s)
    {
        s.srvGen++;
        if (s.srv != null) {
            try {
                s.srv.close();
            } catch (IOException ignored) {
            }
            s.srv = null;
        }
        if (s.srvWriter != null) {
            s.srvWriter.shutdownNow();
            s.srvWriter = null;
        }
        s.srvPending.set(0);
        if (s.srvUp || s.srvConnecting) {
            statusDirty = true;
        }
        s.srvUp = false;
        s.srvConnecting = false;
    }

    private void srvStart(final Session s)
    {
        s.srvWanted = false;
        srvClose(s);
        final InetSocketAddress target;
        final String host;
        if (cfg.isAuto()) {
            if (found == null) {
                if (now() - tDiscover > 500) {
                    discoverySend();
                }
                NativeBridge.nativeSrvDisconnected(s.h, now()); /* retried with back-off */
                return;
            }
            target = found;
            host = null;
        } else {
            target = null;
            host = cfg.server;
        }
        final Socket sock = new Socket();
        final int gen = s.srvGen;
        s.srv = sock;
        s.srvConnecting = true;
        io.execute(() -> {
            String err = null;
            InetSocketAddress to = target;
            try {
                if (to == null) {
                    to = new InetSocketAddress(InetAddress.getByName(host), cfg.port); /* DNS, may block */
                }
                sock.setTcpNoDelay(true);
                sock.setKeepAlive(true);
                sock.connect(to, CONNECT_TIMEOUT_MS);
            } catch (java.net.UnknownHostException e) {
                err = "cannot resolve server '" + host + "'";
            } catch (IOException e) {
                err = "server connection failed (" + e.getMessage() + ")";
            }
            final String error = err;
            final InetSocketAddress addr = to;
            post(() -> srvConnectDone(s, gen, sock, addr, error));
        });
    }

    private void srvConnectDone(Session s, int gen, Socket sock, InetSocketAddress addr, String error)
    {
        if (s.freed || gen != s.srvGen) {
            try {
                sock.close();
            } catch (IOException ignored) {
            }
            return;
        }
        s.srvConnecting = false;
        if (error != null) {
            log(error.startsWith("cannot resolve") ? LOG_WARN : LOG_DEBUG, "[" + s.addrStr + "] " + error);
            srvClose(s);
            if (cfg.isAuto()) {
                found = null; /* search again */
            }
            NativeBridge.nativeSrvDisconnected(s.h, now());
            kick();
            return;
        }
        s.srvUp = true;
        s.srvAddr = hostPort(addr);
        s.srvWriter = Executors.newSingleThreadExecutor(named("gc-tcpw"));
        log(LOG_DEBUG, "[" + s.addrStr + "] server connection established");
        statusDirty = true;
        startServerReader(s, sock, gen);
        NativeBridge.nativeSrvConnected(s.h, now());
        kick();
    }

    private void startServerReader(final Session s, final Socket sock, final int gen)
    {
        io.execute(() -> {
            String why = "closed by the server";
            try {
                InputStream in = sock.getInputStream();
                byte[] buf = new byte[8192];
                for (;;) {
                    int n = in.read(buf);
                    if (n < 0) {
                        break;
                    }
                    final byte[] d = java.util.Arrays.copyOf(buf, n);
                    post(() -> {
                        if (!s.freed && gen == s.srvGen && s.srvUp) {
                            NativeBridge.nativeSrvInput(s.h, d, 0, d.length, now());
                            kick();
                        }
                    });
                }
            } catch (IOException e) {
                why = "connection lost";
            }
            final String reason = why;
            post(() -> srvLost(s, gen, reason));
        });
    }

    private void srvLost(Session s, int gen, String why)
    {
        if (s.freed || gen != s.srvGen || !s.srvUp) {
            return;
        }
        log(LOG_DEBUG, "[" + s.addrStr + "] server connection lost (" + why + ")");
        srvClose(s);
        NativeBridge.nativeSrvDisconnected(s.h, now());
        kick();
    }

    /* ------------------------------------------------------------------ */
    /* LAN discovery                                                      */
    /* ------------------------------------------------------------------ */

    private void discoverySend()
    {
        tDiscover = now();
        if (!cfg.isAuto()) {
            return;
        }
        if (udp == null) {
            try {
                udp = new DatagramSocket();
                udp.setBroadcast(true);
            } catch (IOException e) {
                log(LOG_WARN, "LAN discovery not possible: " + e.getMessage());
                udp = null;
                return;
            }
            final DatagramSocket u = udp;
            io.execute(() -> discoveryReceive(u));
        }
        final DatagramSocket u = udp;
        final boolean first = !discoverySent;
        discoverySent = true;
        io.execute(() -> {
            byte[] req = DISCOVER_REQ.getBytes(StandardCharsets.US_ASCII);
            int sent = 0;
            for (InetAddress a : broadcastAddresses()) {
                try {
                    u.send(new DatagramPacket(req, req.length, a, cfg.port));
                    sent++;
                } catch (IOException ignored) {
                    /* unreachable network, try the others */
                }
            }
            if (sent == 0 && first) {
                post(() -> log(LOG_WARN, "LAN discovery: no usable network"));
            }
        });
    }

    private static Set<InetAddress> broadcastAddresses()
    {
        Set<InetAddress> out = new LinkedHashSet<>();
        try {
            Enumeration<NetworkInterface> it = NetworkInterface.getNetworkInterfaces();
            while (it != null && it.hasMoreElements()) {
                NetworkInterface ni = it.nextElement();
                try {
                    if (!ni.isUp() || ni.isLoopback()) {
                        continue;
                    }
                } catch (IOException e) {
                    continue;
                }
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    if (ia.getBroadcast() != null) {
                        out.add(ia.getBroadcast());
                    }
                }
            }
        } catch (IOException | RuntimeException ignored) {
            /* fall back to the defaults below */
        }
        try {
            out.add(InetAddress.getByAddress(new byte[] {(byte)255, (byte)255, (byte)255, (byte)255}));
            out.add(InetAddress.getByAddress(new byte[] {127, 0, 0, 1})); /* server on this phone */
        } catch (IOException ignored) {
        }
        return out;
    }

    private void discoveryReceive(DatagramSocket u)
    {
        byte[] buf = new byte[128];
        while (!u.isClosed()) {
            DatagramPacket p = new DatagramPacket(buf, buf.length);
            try {
                u.receive(p);
            } catch (IOException e) {
                return;
            }
            String s = new String(p.getData(), p.getOffset(), p.getLength(), StandardCharsets.US_ASCII).trim();
            if (!s.startsWith(DISCOVER_ANS)) {
                continue;
            }
            int port;
            try {
                /* "GCSERVER1 <port> [server name]" */
                port = Integer.parseInt(s.substring(DISCOVER_ANS.length()).trim().split("\\s+", 2)[0]);
            } catch (NumberFormatException e) {
                continue;
            }
            if (port <= 0 || port >= 65536) {
                continue;
            }
            final InetSocketAddress a = new InetSocketAddress(p.getAddress(), port);
            post(() -> {
                if (found == null || !found.equals(a)) {
                    log(LOG_INFO, "server found at " + hostPort(a));
                    statusDirty = true;
                }
                found = a;
                kick();
            });
        }
    }

    /* ------------------------------------------------------------------ */
    /* status                                                             */
    /* ------------------------------------------------------------------ */

    private void publishStatus()
    {
        int now = now();
        if (listener == null || (!statusDirty && now - tStatus < 1000)) {
            return;
        }
        if (now - tStatus < STATUS_INTERVAL_MS && !stopped && running) {
            statusDirty = true; /* picked up by the next poll */
            return;
        }
        tStatus = now;
        statusDirty = false;
        List<DeviceInfo> list = new ArrayList<>();
        String server = cfg.isAuto() ? (found != null ? hostPort(found) : null) : cfg.server + ":" + cfg.port;
        for (Session s : sessions) {
            int[] st = status(s);
            if (s.srvUp && s.srvAddr != null) {
                server = s.srvAddr;
            }
            list.add(new DeviceInfo(s.addrStr, s.name, s.dev != null, st[NativeBridge.ST_DEV_LINK] == NativeBridge.LINK_UP,
                                    st[NativeBridge.ST_SRV_STATE], st[NativeBridge.ST_REGISTERED] != 0,
                                    st[NativeBridge.ST_TO_SERVER] & 0xffffffffL,
                                    st[NativeBridge.ST_FROM_SERVER] & 0xffffffffL));
        }
        listener.onStatus(new Status(running, networkUp, cfg.isAuto(), server, Collections.unmodifiableList(list)));
    }

    private void logStatus()
    {
        if (sessions.isEmpty()) {
            if (running) {
                log(LOG_INFO, "status: no N-Gage connected");
            }
            return;
        }
        for (Session s : sessions) {
            int[] st = status(s);
            log(LOG_INFO, String.format(Locale.ROOT, "status: N-Gage %s %s, server %s, messages to server %d, from server %d",
                                        s.label(), s.dev != null ? "connected" : "away",
                                        st[NativeBridge.ST_SRV_STATE] == NativeBridge.SRV_UP ? "up" : "down",
                                        st[NativeBridge.ST_TO_SERVER] & 0xffffffffL,
                                        st[NativeBridge.ST_FROM_SERVER] & 0xffffffffL));
        }
    }

    /* ------------------------------------------------------------------ */

    public static String formatAddress(byte[] a)
    {
        StringBuilder sb = new StringBuilder(17);
        for (int i = 0; i < a.length; i++) {
            if (i > 0) {
                sb.append(':');
            }
            sb.append(String.format(Locale.ROOT, "%02X", a[i] & 0xff));
        }
        return sb.toString();
    }

    /** "AA:BB:CC:DD:EE:FF" to 6 bytes, or null */
    public static byte[] parseAddress(String s)
    {
        if (s == null) {
            return null;
        }
        String[] p = s.split(":");
        if (p.length != 6) {
            return null;
        }
        byte[] out = new byte[6];
        try {
            for (int i = 0; i < 6; i++) {
                out[i] = (byte)Integer.parseInt(p[i], 16);
            }
        } catch (NumberFormatException e) {
            return null;
        }
        return out;
    }

    private static String hostPort(InetSocketAddress a)
    {
        if (a == null) {
            return null;
        }
        InetAddress ia = a.getAddress();
        String h = ia != null ? ia.getHostAddress() : a.getHostString();
        return h + ":" + a.getPort();
    }
}

/*
 * Host test front end for the Android bridge engine: runs BridgeEngine
 * (unchanged app code) and the JNI library on a PC, with "N-Gages"
 * connecting over TCP instead of Bluetooth, like gcbridge --device_tcp.
 *
 *   java -Djava.library.path=DIR -cp CLASSES HostBridge --device_tcp=PORT
 *        [--server=auto|HOST] [--port=N] [--key=TEXT] [--max_devices=N] [--log_level=N]
 *
 * SPDX-License-Identifier: MIT
 */
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;

import org.ngagesdk.phantomlink.bridge.BridgeConfig;
import org.ngagesdk.phantomlink.bridge.BridgeEngine;
import org.ngagesdk.phantomlink.bridge.DeviceConnection;

public final class HostBridge
{
    public static void main(String[] args) throws Exception
    {
        String server = "auto", key = "";
        int port = 9888, dport = 0, max = 4, level = 2;
        for (String a : args) {
            String[] kv = a.replaceFirst("^--", "").split("=", 2);
            String v = kv.length > 1 ? kv[1] : "";
            switch (kv[0]) {
            case "server": server = v; break;
            case "port": port = Integer.parseInt(v); break;
            case "key": key = v; break;
            case "device_tcp": dport = Integer.parseInt(v); break;
            case "max_devices": max = Integer.parseInt(v); break;
            case "log_level": level = Integer.parseInt(v); break;
            default: throw new IllegalArgumentException("unknown option " + a);
            }
        }
        BridgeEngine engine = new BridgeEngine(new BridgeConfig(server, port, key, max, level), new BridgeEngine.Listener() {
            @Override
            public void onLog(int lv, String line)
            {
                System.out.println("[android-bridge] " + "EWID".charAt(lv) + " " + line);
                System.out.flush();
            }

            @Override
            public void onStatus(BridgeEngine.Status s)
            {
            }
        });
        engine.start();
        ServerSocket ls = new ServerSocket(dport, 16, InetAddress.getByName("127.0.0.1"));
        System.out.println("[android-bridge] listening on 127.0.0.1:" + ls.getLocalPort()); /* run_host_test.py waits for this */
        System.out.flush();
        for (;;) {
            final Socket s = ls.accept();
            s.setTcpNoDelay(true);
            final int sp = s.getPort();
            /* test devices get 02:47:43:54:pp:pp, pp = TCP source port (as gcbridge) */
            final byte[] addr = {0x02, 0x47, 0x43, 0x54, (byte)(sp >> 8), (byte)sp};
            engine.deviceConnected(new DeviceConnection() {
                public byte[] address() { return addr; }
                public String name() { return null; }
                public InputStream input() throws IOException { return s.getInputStream(); }
                public OutputStream output() throws IOException { return s.getOutputStream(); }
                public void close() { try { s.close(); } catch (IOException ignored) { } }
            });
        }
    }
}

/*
 * Bridge settings, the same as gcbridge.ini (pcbridge) where they apply.
 *
 * SPDX-License-Identifier: MIT
 */
package org.ngagesdk.phantomlink.bridge;

public final class BridgeConfig
{
    public static final int DEFAULT_PORT = 9888;
    public static final int MAX_DEVICES = 7;

    /** host name or address, "auto" or empty = find the server on the local network */
    public final String server;
    public final int port;
    /** access key, empty if the server does not need one */
    public final String accessKey;
    /** N-Gages at the same time, 1 to 7 */
    public final int maxDevices;
    /** 0 errors, 1 warnings, 2 information, 3 debug */
    public final int logLevel;

    public BridgeConfig(String server, int port, String accessKey, int maxDevices, int logLevel)
    {
        this.server = server == null ? "" : server.trim();
        this.port = port > 0 && port < 65536 ? port : DEFAULT_PORT;
        this.accessKey = accessKey == null ? "" : accessKey;
        this.maxDevices = Math.max(1, Math.min(MAX_DEVICES, maxDevices));
        this.logLevel = Math.max(0, Math.min(3, logLevel));
    }

    public boolean isAuto()
    {
        return server.isEmpty() || server.equalsIgnoreCase("auto");
    }
}

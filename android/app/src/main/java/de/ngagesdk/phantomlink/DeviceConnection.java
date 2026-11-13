/*
 * A connection from an N-Gage: Bluetooth RFCOMM on Android, TCP in the
 * host tests.
 *
 * SPDX-License-Identifier: MIT
 */
package de.ngagesdk.phantomlink;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public interface DeviceConnection
{
    /** Bluetooth address, 6 bytes in display order */
    byte[] address();

    /** friendly name, or null */
    String name();

    InputStream input() throws IOException;

    OutputStream output() throws IOException;

    void close();
}

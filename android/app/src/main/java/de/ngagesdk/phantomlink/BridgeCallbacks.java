/*
 * Output of one native bridge handle. Called synchronously from inside the
 * NativeBridge calls, on the engine thread. Must not call back into the
 * same handle.
 *
 * SPDX-License-Identifier: MIT
 */
package de.ngagesdk.phantomlink;

public interface BridgeCallbacks
{
    void devWrite(byte[] data);
    void srvWrite(byte[] data);
    /** start an asynchronous server connection, report the result later */
    void srvConnect();
    void srvDisconnect();
    void log(String line);
}

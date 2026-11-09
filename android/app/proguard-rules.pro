# JNI: jni_bridge.c looks up these methods by name on the callback object.
-keep interface org.ngagesdk.phantomlink.bridge.BridgeCallbacks { *; }
-keepclassmembers class * implements org.ngagesdk.phantomlink.bridge.BridgeCallbacks {
    public void devWrite(byte[]);
    public void srvWrite(byte[]);
    public void srvConnect();
    public void srvDisconnect();
    public void log(java.lang.String);
}
-keep class org.ngagesdk.phantomlink.bridge.NativeBridge { native <methods>; }

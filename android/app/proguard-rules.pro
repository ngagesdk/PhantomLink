# JNI: jni_bridge.c looks up these methods by name on the callback object.
-keep interface de.ngagesdk.phantomlink.BridgeCallbacks { *; }
-keepclassmembers class * implements de.ngagesdk.phantomlink.BridgeCallbacks {
    public void devWrite(byte[]);
    public void srvWrite(byte[]);
    public void srvConnect();
    public void srvDisconnect();
    public void log(java.lang.String);
}
-keep class de.ngagesdk.phantomlink.NativeBridge { native <methods>; }

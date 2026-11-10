/** @file jni_bridge.c
 *
 *  JNI wrapper around the bridge engine (firmware/src/bridge_core.c), so
 *  the Android bridge runs exactly the same forwarding, resume and local
 *  answer logic as the ESP32 and the PC bridge.
 *
 *  One native handle per N-Gage. All calls for a handle must come from one
 *  thread (the engine thread). Output is delivered synchronously through
 *  the BridgeCallbacks object passed to nativeCreate; the callbacks must
 *  not call back into this handle.
 *
 *  SPDX-License-Identifier: MIT
 **/

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bridge_core.h"

#define JB_RX_CHUNK 4096

typedef struct
{
    bridge br;
    JavaVM *vm;
    jobject cb;
    jmethodID m_dev_write;
    jmethodID m_srv_write;
    jmethodID m_srv_connect;
    jmethodID m_srv_disconnect;
    jmethodID m_log;
    gc_u8 rx[JB_RX_CHUNK];
} jb;

static JNIEnv *jb_env(jb *j)
{
    JNIEnv *env = NULL;
    if ((*j->vm)->GetEnv(j->vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        return NULL;
    }
    return env;
}

static void jb_clear(JNIEnv *env)
{
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
}

static void jb_bytes(jb *j, jmethodID m, const gc_u8 *data, size_t len)
{
    JNIEnv *env = jb_env(j);
    jbyteArray a;
    if (!env) {
        return;
    }
    a = (*env)->NewByteArray(env, (jsize)len);
    if (!a) {
        jb_clear(env);
        return;
    }
    (*env)->SetByteArrayRegion(env, a, 0, (jsize)len, (const jbyte *)data);
    (*env)->CallVoidMethod(env, j->cb, m, a);
    jb_clear(env);
    (*env)->DeleteLocalRef(env, a);
}

static void cb_dev_write(void *ctx, const gc_u8 *data, size_t len)
{
    jb *j = (jb *)ctx;
    jb_bytes(j, j->m_dev_write, data, len);
}

static void cb_srv_write(void *ctx, const gc_u8 *data, size_t len)
{
    jb *j = (jb *)ctx;
    jb_bytes(j, j->m_srv_write, data, len);
}

static void cb_void(jb *j, jmethodID m)
{
    JNIEnv *env = jb_env(j);
    if (!env) {
        return;
    }
    (*env)->CallVoidMethod(env, j->cb, m);
    jb_clear(env);
}

static void cb_srv_connect(void *ctx)
{
    jb *j = (jb *)ctx;
    cb_void(j, j->m_srv_connect);
}

static void cb_srv_disconnect(void *ctx)
{
    jb *j = (jb *)ctx;
    cb_void(j, j->m_srv_disconnect);
}

static void cb_log(void *ctx, const char *line)
{
    jb *j = (jb *)ctx;
    JNIEnv *env = jb_env(j);
    jstring s;
    if (!env) {
        return;
    }
    s = (*env)->NewStringUTF(env, line);
    if (!s) {
        jb_clear(env);
        return;
    }
    (*env)->CallVoidMethod(env, j->cb, j->m_log, s);
    jb_clear(env);
    (*env)->DeleteLocalRef(env, s);
}

#define JB(h)    ((jb *)(intptr_t)(h))
#define FN(name) Java_org_ngagesdk_phantomlink_bridge_NativeBridge_##name

JNIEXPORT jlong JNICALL FN(nativeCreate)(JNIEnv *env, jclass cls, jobject callbacks, jint linkId, jstring key)
{
    jb *j;
    jclass c;
    bridge_cfg cfg;
    bridge_io io;
    (void)cls;

    if (!callbacks || linkId == 0) {
        return 0;
    }
    j = (jb *)calloc(1, sizeof(*j));
    if (!j) {
        return 0;
    }
    (*env)->GetJavaVM(env, &j->vm);
    c = (*env)->GetObjectClass(env, callbacks);
    j->m_dev_write = (*env)->GetMethodID(env, c, "devWrite", "([B)V");
    j->m_srv_write = (*env)->GetMethodID(env, c, "srvWrite", "([B)V");
    j->m_srv_connect = (*env)->GetMethodID(env, c, "srvConnect", "()V");
    j->m_srv_disconnect = (*env)->GetMethodID(env, c, "srvDisconnect", "()V");
    j->m_log = (*env)->GetMethodID(env, c, "log", "(Ljava/lang/String;)V");
    (*env)->DeleteLocalRef(env, c);
    if (!j->m_dev_write || !j->m_srv_write || !j->m_srv_connect || !j->m_srv_disconnect || !j->m_log) {
        /* NoSuchMethodError is pending and is thrown on return */
        free(j);
        return 0;
    }
    j->cb = (*env)->NewGlobalRef(env, callbacks);

    bridge_default_cfg(&cfg);
    cfg.server_link_id = (gc_u32)linkId;
    if (key) {
        const char *k = (*env)->GetStringUTFChars(env, key, NULL);
        if (k) {
            strncpy(cfg.access_key, k, GC_MAX_NAME);
            cfg.access_key[GC_MAX_NAME] = '\0';
            (*env)->ReleaseStringUTFChars(env, key, k);
        }
    }
    io.dev_write = cb_dev_write;
    io.srv_write = cb_srv_write;
    io.srv_connect = cb_srv_connect;
    io.srv_disconnect = cb_srv_disconnect;
    io.log = cb_log;
    bridge_init(&j->br, &cfg, &io, j);
    return (jlong)(intptr_t)j;
}

JNIEXPORT void JNICALL FN(nativeDestroy)(JNIEnv *env, jclass cls, jlong h)
{
    jb *j = JB(h);
    (void)cls;
    if (!j) {
        return;
    }
    (*env)->DeleteGlobalRef(env, j->cb);
    free(j);
}

JNIEXPORT void JNICALL FN(nativeDevConnected)(JNIEnv *env, jclass cls, jlong h, jbyteArray addr, jint now)
{
    gc_u8 a[GC_DEVICE_ID_LEN];
    (void)cls;
    memset(a, 0, sizeof(a));
    if (addr && (*env)->GetArrayLength(env, addr) >= GC_DEVICE_ID_LEN) {
        (*env)->GetByteArrayRegion(env, addr, 0, GC_DEVICE_ID_LEN, (jbyte *)a);
    }
    bridge_dev_connected(&JB(h)->br, a, (gc_u32)now);
}

/* Input arrives in arbitrary pieces; the link layer reassembles frames. */
static void jb_input(JNIEnv *env, jb *j, jbyteArray data, jint off, jint len, jint now, int dev)
{
    jsize total = data ? (*env)->GetArrayLength(env, data) : 0;
    if (off < 0 || len < 0 || off > total || len > total - off) {
        return;
    }
    while (len > 0) {
        jint n = len < JB_RX_CHUNK ? len : JB_RX_CHUNK;
        (*env)->GetByteArrayRegion(env, data, off, n, (jbyte *)j->rx);
        if (dev) {
            bridge_dev_input(&j->br, j->rx, (size_t)n, (gc_u32)now);
        } else {
            bridge_srv_input(&j->br, j->rx, (size_t)n, (gc_u32)now);
        }
        off += n;
        len -= n;
    }
}

JNIEXPORT void JNICALL FN(nativeDevInput)(JNIEnv *env, jclass cls, jlong h, jbyteArray data, jint off, jint len, jint now)
{
    (void)cls;
    jb_input(env, JB(h), data, off, len, now, 1);
}

JNIEXPORT void JNICALL FN(nativeDevDisconnected)(JNIEnv *env, jclass cls, jlong h, jint now)
{
    (void)env;
    (void)cls;
    bridge_dev_disconnected(&JB(h)->br, (gc_u32)now);
}

JNIEXPORT void JNICALL FN(nativeSetNetwork)(JNIEnv *env, jclass cls, jlong h, jboolean up, jint rssi, jint now)
{
    (void)env;
    (void)cls;
    bridge_set_wifi(&JB(h)->br, up ? 1 : 0, (gc_i8)rssi, (gc_u32)now);
}

JNIEXPORT void JNICALL FN(nativeSrvConnected)(JNIEnv *env, jclass cls, jlong h, jint now)
{
    (void)env;
    (void)cls;
    bridge_srv_connected(&JB(h)->br, (gc_u32)now);
}

JNIEXPORT void JNICALL FN(nativeSrvInput)(JNIEnv *env, jclass cls, jlong h, jbyteArray data, jint off, jint len, jint now)
{
    (void)cls;
    jb_input(env, JB(h), data, off, len, now, 0);
}

JNIEXPORT void JNICALL FN(nativeSrvDisconnected)(JNIEnv *env, jclass cls, jlong h, jint now)
{
    (void)env;
    (void)cls;
    bridge_srv_disconnected(&JB(h)->br, (gc_u32)now);
}

JNIEXPORT void JNICALL FN(nativePoll)(JNIEnv *env, jclass cls, jlong h, jint now)
{
    (void)env;
    (void)cls;
    bridge_poll(&JB(h)->br, (gc_u32)now);
}

JNIEXPORT jint JNICALL FN(nativeNextTimeout)(JNIEnv *env, jclass cls, jlong h, jint now)
{
    gc_u32 t;
    (void)env;
    (void)cls;
    t = bridge_next_timeout(&JB(h)->br, (gc_u32)now);
    return t > 0x7fffffffu ? 0x7fffffff : (jint)t;
}

/* out: srv_state, registered, dev link state, srv link state, messages to
 * the server, messages from the server, SPP present, device grace pending */
JNIEXPORT void JNICALL FN(nativeStatus)(JNIEnv *env, jclass cls, jlong h, jintArray out)
{
    bridge *b = &JB(h)->br;
    jint v[8];
    jsize n;
    (void)cls;
    v[0] = b->srv_state;
    v[1] = b->registered;
    v[2] = b->dev.state;
    v[3] = b->srv.state;
    v[4] = (jint)b->st_dev_msgs;
    v[5] = (jint)b->st_srv_msgs;
    v[6] = b->dev_connected;
    v[7] = b->dev_lost_pending;
    n = (*env)->GetArrayLength(env, out);
    (*env)->SetIntArrayRegion(env, out, 0, n < 8 ? n : 8, v);
}

JNIEXPORT jstring JNICALL FN(nativeVersion)(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, BRIDGE_VERSION);
}

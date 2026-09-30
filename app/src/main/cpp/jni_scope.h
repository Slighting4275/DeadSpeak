#ifndef DEADSPEAK_JNI_SCOPE_H
#define DEADSPEAK_JNI_SCOPE_H

#include <android/native_activity.h>
#include <stdbool.h>

/* NativeActivity.env belongs to Android's UI thread, not android_main. */
typedef struct {
    JavaVM *vm;
    JNIEnv *env;
    bool attached;
} DSJniScope;

static inline bool ds_jni_scope_begin(DSJniScope *scope, ANativeActivity *activity) {
    *scope = (DSJniScope){0};
    if (!activity || !activity->vm || !activity->clazz) return false;
    scope->vm = activity->vm;
    const jint status = (*scope->vm)->GetEnv(scope->vm, (void **)&scope->env, JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if ((*scope->vm)->AttachCurrentThread(scope->vm, &scope->env, NULL) != JNI_OK) return false;
        scope->attached = true;
    } else if (status != JNI_OK) {
        return false;
    }
    if (!scope->env || (*scope->env)->PushLocalFrame(scope->env, 32) < 0) {
        if (scope->env && (*scope->env)->ExceptionCheck(scope->env)) (*scope->env)->ExceptionClear(scope->env);
        if (scope->attached) (*scope->vm)->DetachCurrentThread(scope->vm);
        return false;
    }
    return true;
}

static inline bool ds_jni_scope_end(DSJniScope *scope) {
    const bool okay = !(*scope->env)->ExceptionCheck(scope->env);
    if (!okay) {
        (*scope->env)->ExceptionDescribe(scope->env);
        (*scope->env)->ExceptionClear(scope->env);
    }
    (*scope->env)->PopLocalFrame(scope->env, NULL);
    if (scope->attached) (*scope->vm)->DetachCurrentThread(scope->vm);
    return okay;
}

#endif

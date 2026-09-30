#include "jni_scope.h"

/* Keep the NDK's lifecycle glue, wrapping only its UI-thread focus callback. */
#define ANativeActivity_onCreate ds_glue_on_create
#include "android_native_app_glue.c"
#undef ANativeActivity_onCreate

static void ds_hide_status_bar(ANativeActivity *activity) {
    DSJniScope scope;
    if (!ds_jni_scope_begin(&scope, activity)) return;
    JNIEnv *env = scope.env;
    jclass activity_class = (*env)->GetObjectClass(env, activity->clazz);
    if (!activity_class || (*env)->ExceptionCheck(env)) goto done;
    jmethodID get_window = (*env)->GetMethodID(env, activity_class, "getWindow", "()Landroid/view/Window;");
    if (!get_window || (*env)->ExceptionCheck(env)) goto done;
    jobject window = (*env)->CallObjectMethod(env, activity->clazz, get_window);
    if (!window || (*env)->ExceptionCheck(env)) goto done;
    jclass window_class = (*env)->GetObjectClass(env, window);
    if (!window_class || (*env)->ExceptionCheck(env)) goto done;
    jmethodID get_controller = (*env)->GetMethodID(env, window_class, "getInsetsController", "()Landroid/view/WindowInsetsController;");
    if (!get_controller || (*env)->ExceptionCheck(env)) goto done;
    jobject controller = (*env)->CallObjectMethod(env, window, get_controller);
    if (!controller || (*env)->ExceptionCheck(env)) goto done;
    jclass type_class = (*env)->FindClass(env, "android/view/WindowInsets$Type");
    if (!type_class || (*env)->ExceptionCheck(env)) goto done;
    jmethodID status_bars = (*env)->GetStaticMethodID(env, type_class, "statusBars", "()I");
    if (!status_bars || (*env)->ExceptionCheck(env)) goto done;
    jint types = (*env)->CallStaticIntMethod(env, type_class, status_bars);
    if ((*env)->ExceptionCheck(env)) goto done;
    jclass controller_class = (*env)->FindClass(env, "android/view/WindowInsetsController");
    if (!controller_class || (*env)->ExceptionCheck(env)) goto done;
    jmethodID hide = (*env)->GetMethodID(env, controller_class, "hide", "(I)V");
    if (!hide || (*env)->ExceptionCheck(env)) goto done;
    (*env)->CallVoidMethod(env, controller, hide, types);
done:
    if (!ds_jni_scope_end(&scope))
        __android_log_print(ANDROID_LOG_ERROR, "DeadSpeak", "Could not hide status bar using WindowInsetsController");
}

static void ds_on_window_focus_changed(ANativeActivity *activity, int focused) {
    onWindowFocusChanged(activity, focused);
    if (focused) ds_hide_status_bar(activity);
}

JNIEXPORT void ANativeActivity_onCreate(ANativeActivity *activity, void *saved_state, size_t saved_state_size) {
    ds_glue_on_create(activity, saved_state, saved_state_size);
    activity->callbacks->onWindowFocusChanged = ds_on_window_focus_changed;
}

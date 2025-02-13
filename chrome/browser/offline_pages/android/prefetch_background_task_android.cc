// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "chrome/browser/offline_pages/android/prefetch_background_task_android.h"

#include <memory>

#include "base/android/jni_string.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "chrome/browser/offline_pages/prefetch/prefetch_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_android.h"
#include "components/offline_pages/core/prefetch/prefetch_background_task.h"
#include "components/offline_pages/core/prefetch/prefetch_dispatcher.h"
#include "components/offline_pages/core/prefetch/prefetch_service.h"
#include "jni/PrefetchBackgroundTask_jni.h"

using base::android::JavaParamRef;
using base::android::ScopedJavaGlobalRef;
using base::android::ScopedJavaLocalRef;

namespace offline_pages {

namespace prefetch {

// JNI call to start request processing in scheduled mode.
static jboolean JNI_PrefetchBackgroundTask_StartPrefetchTask(
    JNIEnv* env,
    const JavaParamRef<jobject>& jcaller,
    const JavaParamRef<jobject>& jprofile,
    const JavaParamRef<jstring>& gcm_token) {
  Profile* profile = ProfileAndroid::FromProfileAndroid(jprofile);
  DCHECK(profile);

  PrefetchService* prefetch_service =
      PrefetchServiceFactory::GetForBrowserContext(profile);
  if (!prefetch_service)
    return false;

  prefetch_service->SetCachedGCMToken(
      base::android::ConvertJavaStringToUTF8(env, gcm_token));

  prefetch_service->GetPrefetchDispatcher()->BeginBackgroundTask(
      std::make_unique<PrefetchBackgroundTaskAndroid>(env, jcaller,
                                                      prefetch_service));
  return true;
}

}  // namespace prefetch

PrefetchBackgroundTaskAndroid::PrefetchBackgroundTaskAndroid(
    JNIEnv* env,
    const JavaParamRef<jobject>& java_prefetch_background_task,
    PrefetchService* service)
    : PrefetchBackgroundTask(service),
      java_prefetch_background_task_(java_prefetch_background_task) {
  // Give the Java side a pointer to the new background task object.
  prefetch::Java_PrefetchBackgroundTask_setNativeTask(
      env, java_prefetch_background_task_, reinterpret_cast<jlong>(this));
}

PrefetchBackgroundTaskAndroid::~PrefetchBackgroundTaskAndroid() {
  JNIEnv* env = base::android::AttachCurrentThread();
  prefetch::Java_PrefetchBackgroundTask_doneProcessing(
      env, java_prefetch_background_task_, false);
}

bool PrefetchBackgroundTaskAndroid::OnStopTask(
    JNIEnv* env,
    const JavaParamRef<jobject>& jcaller) {
  SetReschedule(PrefetchBackgroundTaskRescheduleType::RESCHEDULE_DUE_TO_SYSTEM);
  service()->GetPrefetchDispatcher()->StopBackgroundTask();
  return false;
}

void PrefetchBackgroundTaskAndroid::SetTaskReschedulingForTesting(
    JNIEnv* env,
    const base::android::JavaParamRef<jobject>& jcaller,
    int reschedule_type) {
  SetReschedule(
      static_cast<PrefetchBackgroundTaskRescheduleType>(reschedule_type));
}

void PrefetchBackgroundTaskAndroid::SignalTaskFinishedForTesting(
    JNIEnv* env,
    const base::android::JavaParamRef<jobject>& jcaller) {
  service()->GetPrefetchDispatcher()->StopBackgroundTask();
}

}  // namespace offline_pages

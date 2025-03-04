// Copyright (c) 2020-2024 ByteDance, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

// Created by Kelun Cai (caikelun@bytedance.com) on 2020-06-02.

#include "bh_linker.h"

#include <android/api-level.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "bh_const.h"
#include "bh_dl.h"
#include "bh_linker_mutex.h"
#include "bh_log.h"
#include "bh_util.h"

typedef struct {
  // bits:     name:    description:
  // 15-14     type     mutex type, can be 0 (normal), 1 (recursive), 2 (errorcheck)
  // 13        shared   process-shared flag
  // 12-2      counter  <number of times a thread holding a recursive Non-PI mutex > - 1
  // 1-0       state    lock state, can be 0 (unlocked), 1 (locked contended), 2 (locked uncontended)
  uint16_t state;
#if defined(__LP64__)
  uint16_t pad;
  int owner_tid;
  char reserved[28];
#else
  uint16_t owner_tid;
#endif
} bh_linker_mutex_internal_t __attribute__((aligned(4)));

bh_linker_dlopen_ext_t bh_linker_dlopen_ext = NULL;
bh_linker_do_dlopen_t bh_linker_do_dlopen = NULL;
bh_linker_get_error_buffer_t bh_linker_get_error_buffer = NULL;
bh_linker_bionic_format_dlerror_t bh_linker_bionic_format_dlerror = NULL;

static pthread_mutex_t *bh_linker_g_dl_mutex = NULL;

#if BH_LINKER_MAYBE_NOT_SUPPORT_DL_INIT_FINI_MONITOR

#define BH_LINKER_MUTEX_IS_UNLOCKED(v) (((v) & 0x3) == 0)
#define BH_LINKER_MUTEX_COUNTER(v)     (((v) & 0x1FFC) >> 2)

static bool bh_linker_check_lock_compatible_helper(bh_linker_mutex_internal_t *m, bool unlocked,
                                                   uint16_t counter, int owner_tid) {
  return BH_LINKER_MUTEX_IS_UNLOCKED(m->state) == unlocked && BH_LINKER_MUTEX_COUNTER(m->state) == counter &&
         m->owner_tid == owner_tid;
}

static bool bh_linker_is_mutex_compatible(void) {
  bool result = true;
  int tid = gettid();
  pthread_mutex_t mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
  bh_linker_mutex_internal_t *m = (bh_linker_mutex_internal_t *)&mutex;

  if (!bh_linker_check_lock_compatible_helper(m, true, 0, 0)) result = false;
  pthread_mutex_lock(&mutex);
  if (!bh_linker_check_lock_compatible_helper(m, false, 0, tid)) result = false;
  pthread_mutex_lock(&mutex);
  if (!bh_linker_check_lock_compatible_helper(m, false, 1, tid)) result = false;
  pthread_mutex_lock(&mutex);
  if (!bh_linker_check_lock_compatible_helper(m, false, 2, tid)) result = false;
  pthread_mutex_unlock(&mutex);
  if (!bh_linker_check_lock_compatible_helper(m, false, 1, tid)) result = false;
  pthread_mutex_unlock(&mutex);
  if (!bh_linker_check_lock_compatible_helper(m, false, 0, tid)) result = false;
  pthread_mutex_unlock(&mutex);
  if (!bh_linker_check_lock_compatible_helper(m, true, 0, 0)) result = false;

  return result;
}
#endif

int bh_linker_init(void) {
#if BH_LINKER_MAYBE_NOT_SUPPORT_DL_INIT_FINI_MONITOR
  if (!bh_linker_is_mutex_compatible()) return -1;
#endif

  int api_level = bh_util_get_api_level();

  // for Android 4.x
#if __ANDROID_API__ < __ANDROID_API_L__
  if (api_level < __ANDROID_API_L__) {
    return NULL == (bh_linker_g_dl_mutex = bh_linker_mutex_get_by_stack()) ? -1 : 0;
  }
#endif

  void *linker = bh_dl_open_linker();
  if (NULL == linker) goto err;

  // for Android 5.0, 5.1, 7.0, 7.1
  if (__ANDROID_API_L__ == api_level || __ANDROID_API_L_MR1__ == api_level ||
      __ANDROID_API_N__ == api_level || __ANDROID_API_N_MR1__ == api_level ||
      !bh_linker_is_support_dl_init_fini_monitor()) {
    if (api_level > __ANDROID_API_U__) {
      bh_linker_g_dl_mutex = (pthread_mutex_t *)(bh_dl_dsym(linker, BH_CONST_SYM_G_DL_MUTEX_U_QPR2));
    } else if (api_level == __ANDROID_API_U__) {
      bh_linker_g_dl_mutex = (pthread_mutex_t *)(bh_dl_dsym(linker, BH_CONST_SYM_G_DL_MUTEX));
      if (NULL == bh_linker_g_dl_mutex)
        bh_linker_g_dl_mutex = (pthread_mutex_t *)(bh_dl_dsym(linker, BH_CONST_SYM_G_DL_MUTEX_U_QPR2));
    } else if (api_level < __ANDROID_API_U__ && api_level >= __ANDROID_API_L__) {
      bh_linker_g_dl_mutex = (pthread_mutex_t *)(bh_dl_dsym(linker, BH_CONST_SYM_G_DL_MUTEX));
    }

    if (NULL == bh_linker_g_dl_mutex) goto err;
  }

  // for Android 7.0, 7.1
  if (__ANDROID_API_N__ == api_level || __ANDROID_API_N_MR1__ == api_level) {
    bh_linker_dlopen_ext = (bh_linker_dlopen_ext_t)(bh_dl_dsym(linker, BH_CONST_SYM_DLOPEN_EXT));
    if (NULL == bh_linker_dlopen_ext) {
      if (NULL == (bh_linker_do_dlopen = (bh_linker_do_dlopen_t)(bh_dl_dsym(linker, BH_CONST_SYM_DO_DLOPEN))))
        goto err;
      bh_linker_get_error_buffer =
          (bh_linker_get_error_buffer_t)(bh_dl_dsym(linker, BH_CONST_SYM_LINKER_GET_ERROR_BUFFER));
      bh_linker_bionic_format_dlerror =
          (bh_linker_bionic_format_dlerror_t)(bh_dl_dsym(linker, BH_CONST_SYM_BIONIC_FORMAT_DLERROR));
    }
  }

  bh_dl_close(linker);
  return 0;

err:
  if (NULL != linker) bh_dl_close(linker);
  bh_linker_do_dlopen = NULL;
  bh_linker_dlopen_ext = NULL;
  bh_linker_g_dl_mutex = NULL;
  bh_linker_get_error_buffer = NULL;
  bh_linker_bionic_format_dlerror = NULL;
  return -1;
}

void bh_linker_lock(void) {
  if (__predict_true(NULL != bh_linker_g_dl_mutex)) pthread_mutex_lock(bh_linker_g_dl_mutex);
}

void bh_linker_unlock(void) {
  if (__predict_true(NULL != bh_linker_g_dl_mutex)) pthread_mutex_unlock(bh_linker_g_dl_mutex);
}

bool bh_linker_is_in_lock(void) {
#if BH_LINKER_MAYBE_NOT_SUPPORT_DL_INIT_FINI_MONITOR
  bh_linker_mutex_internal_t *m = (bh_linker_mutex_internal_t *)bh_linker_g_dl_mutex;
  uint16_t state = __atomic_load_n(&(m->state), __ATOMIC_RELAXED);
  int owner_tid = (int)__atomic_load_n(&(m->owner_tid), __ATOMIC_RELAXED);
  return !BH_LINKER_MUTEX_IS_UNLOCKED(state) && owner_tid == gettid();
#else
  return false;
#endif
}

bool bh_linker_is_use_basename(void) {
#if defined(__LP64__)
  return false;
#else
  return bh_util_get_api_level() < __ANDROID_API_M__;
#endif
}

bool bh_linker_elf_is_match(const char *linker_elf_name, const char *external_elf_name) {
  if (__predict_false(bh_linker_is_use_basename())) {
    if ('/' == linker_elf_name[0] && '/' != external_elf_name[0])
      return bh_util_ends_with(linker_elf_name, external_elf_name);
    else if ('/' != linker_elf_name[0] && '/' == external_elf_name[0])
      return bh_util_ends_with(external_elf_name, linker_elf_name);
    else
      return 0 == strcmp(linker_elf_name, external_elf_name);
  } else {
    if (__predict_true('/' != external_elf_name[0])) {
      return bh_util_ends_with(linker_elf_name, external_elf_name);
    } else {
      return 0 == strcmp(external_elf_name, linker_elf_name);
    }
  }
}

bool bh_linker_is_support_dl_init_fini_monitor(void) {
#if defined(__aarch64__)
  return true;
#elif defined(__arm__)
#if __ANDROID_API__ >= __ANDROID_API_L__
  return true;
#else
  return bh_util_get_api_level() >= __ANDROID_API_L__;
#endif
#else
  return false;
#endif
}

/*
 * Copyright (c) 2015, EURECOM (www.eurecom.fr)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are those
 * of the authors and should not be interpreted as representing official policies,
 * either expressed or implied, of the FreeBSD Project.
 */

/*! \file log.c
   \brief Thread safe logging utility, log output can be redirected to stdout, file or remote host through TCP.
   \author  Lionel GAUTHIER
   \date 2015
   \email: lionel.gauthier@eurecom.fr
*/
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <inttypes.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>

#include "liblfds611.h"
#include <libxml/xmlwriter.h>
#include <libxml/xpath.h>
#include "bstrlib.h"

#include "hashtable.h"
#include "obj_hashtable.h"
#include "mme_scenario_player.h"
#include "3gpp_23.003.h"
#include "3gpp_24.008.h"
#include "3gpp_33.401.h"
#include "3gpp_24.007.h"
#include "3gpp_36.401.h"
#include "3gpp_36.331.h"
#include "3gpp_24.301.h"
#include "security_types.h"
#include "common_types.h"
#include "emm_msg.h"
#include "esm_msg.h"
#include "intertask_interface.h"
#include "timer.h"
#include "hashtable.h"
#include "log.h"
#include "msc.h"
#include "shared_ts_log.h"
#include "assertions.h"
#include "dynamic_memory_check.h"
#include "gcc_diag.h"
//-------------------------------
#define LOG_MAX_QUEUE_ELEMENTS                1024
#define LOG_MESSAGE_MIN_ALLOC_SIZE             256

#define LOG_FLUSH_PERIOD_SEC                     0
#define LOG_FLUSH_PERIOD_MICRO_SEC           50000
//-------------------------------


typedef unsigned long                   log_message_number_t;


/*! \struct  oai_shared_log_t
* \brief Structure containing all the logging utility internal variables.
*/
typedef struct oai_shared_log_s {
  // may be good to use stream instead of file descriptor when
  // logging somewhere else of the console.

  int                                     log_start_time_second;                                       /*!< \brief Logging utility reference time              */

  log_message_number_t                    log_message_number;                                          /*!< \brief Counter of log message        */
  struct lfds611_queue_state             *log_message_queue_p;                                         /*!< \brief Thread safe log message queue */
  struct lfds611_stack_state             *log_free_message_queue_p;                                          /*!< \brief Thread safe memory pool       */

  hash_table_ts_t                           *thread_context_htbl;                                         /*!< \brief Container for log_thread_ctxt_t */

  void (*logger_callback[MAX_SH_TS_LOG_CLIENT])(shared_log_queue_item_t*);
} oai_shared_log_t;

static oai_shared_log_t g_shared_log={0};    /*!< \brief  logging utility internal variables global var definition*/


//------------------------------------------------------------------------------
int shared_log_get_start_time_sec (void)
{
  return g_shared_log.log_start_time_second;
}

//------------------------------------------------------------------------------
void shared_log_reuse_item(shared_log_queue_item_t * item_p)
{
  int         rv = 0;
  btrunc(item_p->bstr, 0);
  rv = lfds611_stack_guaranteed_push (g_shared_log.log_free_message_queue_p, item_p);
  if (0 == rv) {
    OAI_FPRINTF_ERR("Error while reusing shared_log_queue_item_t (lfds611_stack_guaranteed_push() returned %d)\n", rv);
    free_wrapper ((void**)&item_p);
  }
}

//------------------------------------------------------------------------------
static shared_log_queue_item_t * create_new_log_queue_item(sh_ts_log_app_id_t app_id)
{
  shared_log_queue_item_t * item_p = calloc(1, sizeof(shared_log_queue_item_t));
  AssertFatal((item_p), "Allocation of log container failed");
  AssertFatal((app_id >= MIN_SH_TS_LOG_CLIENT), "Allocation of log container failed");
  AssertFatal((app_id < MAX_SH_TS_LOG_CLIENT), "Allocation of log container failed");
  item_p->app_id = app_id;
  item_p->bstr = bfromcstralloc(LOG_MESSAGE_MIN_ALLOC_SIZE, "");
  AssertFatal((item_p->bstr), "Allocation of buf in log container failed");
  return item_p;
}

//------------------------------------------------------------------------------
shared_log_queue_item_t * get_new_log_queue_item(sh_ts_log_app_id_t app_id)
{
  int                       rv = 0;
  shared_log_queue_item_t * item_p = NULL;

  rv = lfds611_stack_pop (g_shared_log.log_free_message_queue_p, (void **)&item_p);
  if (0 == rv) {
    item_p = create_new_log_queue_item(app_id);
    AssertFatal(item_p,  "Out of memory error");
  } else {
    item_p->app_id = app_id;
    btrunc(item_p->bstr, 0);
  }
  return item_p;
}
//------------------------------------------------------------------------------
void* shared_log_task (__attribute__ ((unused)) void *args_p)
{
  MessageDef                             *received_message_p = NULL;
  long                                    timer_id = 0;
  int                                     rc = 0;

  itti_mark_task_ready (TASK_SHARED_TS_LOG);
  shared_log_start_use ();
  timer_setup (LOG_FLUSH_PERIOD_SEC,
               LOG_FLUSH_PERIOD_MICRO_SEC,
               TASK_SHARED_TS_LOG, INSTANCE_DEFAULT, TIMER_ONE_SHOT, NULL, &timer_id);

  while (1) {
    itti_receive_msg (TASK_SHARED_TS_LOG, &received_message_p);

    if (received_message_p != NULL) {

      switch (ITTI_MSG_ID (received_message_p)) {
      case TIMER_HAS_EXPIRED:{
        shared_log_flush_messages ();
        timer_setup (LOG_FLUSH_PERIOD_SEC,
            LOG_FLUSH_PERIOD_MICRO_SEC,
            TASK_SHARED_TS_LOG, INSTANCE_DEFAULT, TIMER_ONE_SHOT, NULL, &timer_id);
        }
        break;

      case TERMINATE_MESSAGE:{
          timer_remove (timer_id);
          shared_log_exit ();
          itti_exit_task ();
        }
        break;

      default:{
        }
        break;
      }
      // Freeing the memory allocated from the memory pool
      rc = itti_free (ITTI_MSG_ORIGIN_ID (received_message_p), received_message_p);
      AssertFatal (rc == EXIT_SUCCESS, "Failed to free memory (%d)!\n", rc);
      received_message_p = NULL;
    }
  }

  OAI_FPRINTF_ERR("Task Log exiting\n");
  return NULL;
}

//------------------------------------------------------------------------------
void shared_log_get_elapsed_time_since_start(struct timeval * const elapsed_time)
{
  // no thread safe but do not matter a lot
  gettimeofday(elapsed_time, NULL);
  // no timersub call for fastest operations
  elapsed_time->tv_sec = elapsed_time->tv_sec - g_shared_log.log_start_time_second;
}


//------------------------------------------------------------------------------
int shared_log_init (const int max_threadsP)
{
  int                                     i = 0;
  int                                     rv = 0;
  shared_log_queue_item_t                *item_p = NULL;
  struct timeval                          start_time = {.tv_sec=0, .tv_usec=0};

  rv = gettimeofday(&start_time, NULL);
  g_shared_log.log_start_time_second = start_time.tv_sec;
  g_shared_log.logger_callback[SH_TS_LOG_TXT] = log_flush_message;
#if MESSAGE_CHART_GENERATOR
  g_shared_log.logger_callback[SH_TS_LOG_MSC] = msc_flush_message;
#endif
  OAI_FPRINTF_INFO("Initializing shared logging\n");

  bstring b = bfromcstr("Logging thread context hashtable");
  g_shared_log.thread_context_htbl = hashtable_ts_create (LOG_MESSAGE_MIN_ALLOC_SIZE, NULL, free_wrapper, b);
  bdestroy_wrapper (&b);
  AssertFatal (NULL != g_shared_log.thread_context_htbl, "Could not create hashtable for Log!\n");
  g_shared_log.thread_context_htbl->log_enabled = false;


  log_thread_ctxt_t *thread_ctxt = calloc(1, sizeof(log_thread_ctxt_t));
  AssertFatal(NULL != thread_ctxt, "Error Could not create log thread context\n");
  pthread_t p = pthread_self();
  thread_ctxt->tid = p;
  hashtable_rc_t hash_rc = hashtable_ts_insert(g_shared_log.thread_context_htbl, (hash_key_t) p, thread_ctxt);
  if (HASH_TABLE_OK != hash_rc) {
    OAI_FPRINTF_ERR("Error Could not register log thread context\n");
    free_wrapper((void**)&thread_ctxt);
  }

  rv = lfds611_stack_new (&g_shared_log.log_free_message_queue_p, (lfds611_atom_t) max_threadsP + 2);

  if (0 >= rv) {
    AssertFatal (0, "lfds611_stack_new failed!\n");
  }

  rv = lfds611_queue_new (&g_shared_log.log_message_queue_p, (lfds611_atom_t) LOG_MAX_QUEUE_ELEMENTS);
  AssertFatal (rv, "lfds611_queue_new failed!\n");
  AssertFatal (g_shared_log.log_message_queue_p != NULL, "g_shared_log.log_message_queue_p is NULL!\n");
  shared_log_start_use ();

  for (i = 0; i < max_threadsP * 30; i++) {
    item_p = create_new_log_queue_item(MIN_SH_TS_LOG_CLIENT); // any logger
    rv = lfds611_stack_guaranteed_push (g_shared_log.log_free_message_queue_p, item_p);
    AssertFatal (rv, "lfds611_stack_guaranteed_push failed for item %u\n", i);
  }


  OAI_FPRINTF_INFO("Initializing OAI logging Done\n");
  log_message (thread_ctxt, OAILOG_LEVEL_INFO, LOG_UTIL, __FILE__, __LINE__, "Initializing OAI logging Done\n");
  return 0;
}

//------------------------------------------------------------------------------
void shared_log_itti_connect(void)
{
  int                                     rv = 0;
  rv = itti_create_task (TASK_SHARED_TS_LOG, shared_log_task, NULL);
  AssertFatal (rv == 0, "Create task for OAI logging failed!\n");
}

//------------------------------------------------------------------------------
void shared_log_start_use (void)
{
  pthread_t      p       = pthread_self();
  hashtable_rc_t hash_rc = hashtable_ts_is_key_exists (g_shared_log.thread_context_htbl, (hash_key_t) p);
  if (HASH_TABLE_KEY_NOT_EXISTS == hash_rc) {
    lfds611_queue_use (g_shared_log.log_message_queue_p);
    lfds611_stack_use (g_shared_log.log_free_message_queue_p);
    log_thread_ctxt_t *thread_ctxt = calloc(1, sizeof(log_thread_ctxt_t));
    if (thread_ctxt) {
      thread_ctxt->tid = p;
      hash_rc = hashtable_ts_insert(g_shared_log.thread_context_htbl, (hash_key_t) p, thread_ctxt);
      if (HASH_TABLE_OK != hash_rc) {
        OAI_FPRINTF_ERR("Error Could not register log thread context\n");
        free_wrapper((void**)&thread_ctxt);
      }
    } else {
      OAI_FPRINTF_ERR("Error Could not create log thread context\n");
    }
  }
}

//------------------------------------------------------------------------------
void shared_log_flush_messages (void)
{
  int                                     rv = 0;
  shared_log_queue_item_t                *item_p = NULL;

  while ((rv = lfds611_queue_dequeue (g_shared_log.log_message_queue_p, (void **)&item_p)) == 1) {
    if ((item_p->app_id >= MIN_SH_TS_LOG_CLIENT) && (item_p->app_id < MAX_SH_TS_LOG_CLIENT)) {
      (*g_shared_log.logger_callback[item_p->app_id])(item_p);
    } else {
      OAI_FPRINTF_ERR("Error bad logger identifier: %d\n", item_p->app_id);
    }
    shared_log_reuse_item(item_p);
  }
}

//------------------------------------------------------------------------------
void shared_log_exit (void)
{
  OAI_FPRINTF_INFO("[TRACE] Entering %s\n", __FUNCTION__);
  shared_log_flush_messages ();
  hashtable_ts_destroy(g_shared_log.thread_context_htbl);
  OAI_FPRINTF_INFO("[TRACE] Leaving %s\n", __FUNCTION__);
}

//------------------------------------------------------------------------------
void shared_log_item(shared_log_queue_item_t * messageP)
{
  int                                     rv = 0;

  if (messageP) {
    shared_log_start_use();

    rv = lfds611_queue_enqueue (g_shared_log.log_message_queue_p, messageP);

    if (0 == rv) {
      btrunc(messageP->bstr, 0);
      rv = lfds611_stack_guaranteed_push (g_shared_log.log_free_message_queue_p, messageP);
      if (0 == rv) {
        bdestroy_wrapper (&messageP->bstr);
        free_wrapper ((void**)&messageP);
      }
    }
  }
}


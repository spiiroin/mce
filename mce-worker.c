/**
 * @file mce-worker.c
 *
 * Mode Control Entity - Offload blocking operations to a worker thread
 *
 * <p>
 *
 * Copyright (C) 2015 Jolla Ltd.
 *
 * <p>
 *
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 *
 * mce is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * mce is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mce.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mce-worker.h"
#include "mce-log.h"

#include <sys/eventfd.h>

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

#include <glib.h>

/* ========================================================================= *
 * FUNCTIONALITY
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * MISC_UTIL
 * ------------------------------------------------------------------------- */

static guint mw_add_iowatch(int fd, bool close_on_unref, GIOCondition cnd, GIOFunc io_cb, gpointer aptr);

/* ------------------------------------------------------------------------- *
 * MCE_JOB
 * ------------------------------------------------------------------------- */

typedef struct mce_job_t mce_job_t;

struct mce_job_t
{
    mce_job_t  *mj_next;

    char       *mj_context;
    char       *mj_name;

    void     *(*mj_handle)(void *);
    void      (*mj_notify)(void *, void *);

    void       *mj_param;
    void       *mj_reply;
};

static const char    *mce_job_context       (const mce_job_t *self);
static const char    *mce_job_name          (const mce_job_t *self);

static void           mce_job_notify        (mce_job_t *self);
static void           mce_job_execute       (mce_job_t *self);

static void           mce_job_delete        (mce_job_t *self);
static mce_job_t     *mce_job_create        (const char *context, const char *name, void *(*handle)(void *), void (*notify)(void *, void *), void *param);

/* ------------------------------------------------------------------------- *
 * MCE_JOBLIST
 * ------------------------------------------------------------------------- */

typedef struct {
    mce_job_t  *mjl_head;
    mce_job_t  *mjl_tail;
} mce_joblist_t;

static mce_job_t     *mce_joblist_pull      (mce_joblist_t *self);
static void           mce_joblist_push      (mce_joblist_t *self, mce_job_t *job);
static void           mce_joblist_delete    (mce_joblist_t *self);
static mce_joblist_t *mce_joblist_create    (void);

/* ------------------------------------------------------------------------- *
 * MCE_WORKER
 * ------------------------------------------------------------------------- */

static gboolean       mce_worker_notify_cb  (GIOChannel *chn, GIOCondition cnd, gpointer data);
static void           mce_worker_execute    (void);
static void          *mce_worker_main       (void *aptr);

void                  mce_worker_add_job    (const char *context, const char *name, void *(*handle)(void *), void (*notify)(void *, void *), void *param);

void                  mce_worker_add_context(const char *context);
void                  mce_worker_rem_context(const char *context);
static bool           mce_worker_has_context(const char *context);

bool                  mce_worker_init       (void);
void                  mce_worker_quit       (void);

/** Flag for: Worker thread is running */
static bool             mw_is_ready = false;

static mce_joblist_t   *mw_req_list  = 0;
static pthread_mutex_t  mw_req_mutex = PTHREAD_MUTEX_INITIALIZER;
static int              mw_req_evfd  = -1;
static pthread_t        mw_req_tid   = 0;

static mce_joblist_t   *mw_rsp_list  = 0;
static pthread_mutex_t  mw_rsp_mutex = PTHREAD_MUTEX_INITIALIZER;
static int              mw_rsp_evfd  = -1;
static guint            mw_rsp_wid   = 0;

static GHashTable      *mw_ctx_lut   = 0;
static pthread_mutex_t  mw_ctx_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ========================================================================= *
 * MISC_UTIL
 * ========================================================================= */

/** Helper for creating I/O watch for file descriptor
 */
static guint
mw_add_iowatch(int fd, bool close_on_unref,
               GIOCondition cnd, GIOFunc io_cb, gpointer aptr)
{
    guint         wid = 0;
    GIOChannel   *chn = 0;

    if( !(chn = g_io_channel_unix_new(fd)) )
        goto cleanup;

    g_io_channel_set_close_on_unref(chn, close_on_unref);

    cnd |= G_IO_ERR | G_IO_HUP | G_IO_NVAL;

    if( !(wid = g_io_add_watch(chn, cnd, io_cb, aptr)) )
        goto cleanup;

cleanup:
    if( chn != 0 ) g_io_channel_unref(chn);

    return wid;

}

/* ========================================================================= *
 * MCE_JOB
 * ========================================================================= */

static const char *
mce_job_name(const mce_job_t *self)
{
    const char *name = 0;
    if( self )
        name = self->mj_name;
    return name ?: "unknown";
}

static const char *
mce_job_context(const mce_job_t *self)
{
    const char *context = 0;
    if( self )
        context = self->mj_context;
    return context ?: "global";
}

static void
mce_job_notify(mce_job_t *self)
{
    if( !self )
        goto EXIT;

    if( !self->mj_notify )
        goto EXIT;

    mce_log(LL_DEBUG, "job(%s:%s) notify", mce_job_context(self), mce_job_name(self));

    pthread_mutex_lock(&mw_ctx_mutex);
    if( mce_worker_has_context(self->mj_context) )
        self->mj_notify(self->mj_param, self->mj_reply);
    pthread_mutex_unlock(&mw_ctx_mutex);

EXIT:
    return;
}

static void
mce_job_execute(mce_job_t *self)
{
    if( !self )
        goto EXIT;

    if( !self->mj_handle )
        goto EXIT;

    mce_log(LL_DEBUG, "job(%s:%s) execute", mce_job_context(self), mce_job_name(self));

    pthread_mutex_lock(&mw_ctx_mutex);
    if( mce_worker_has_context(self->mj_context) )
        self->mj_reply = self->mj_handle(self->mj_param);
    pthread_mutex_unlock(&mw_ctx_mutex);

EXIT:
    return;
}

static void
mce_job_delete(mce_job_t *self)
{
    if( !self )
        goto EXIT;

    mce_log(LL_DEBUG, "job(%s:%s) deleted", mce_job_context(self), mce_job_name(self));

    free(self->mj_name);
    free(self);

EXIT:
    return;
}

static mce_job_t *
mce_job_create(const char *context,
               const char *name,
               void *(*handle)(void *),
               void (*notify)(void *, void *),
               void *param)
{
    mce_job_t *self = calloc(1, sizeof *self);

    self->mj_next    = 0;
    self->mj_context = context ? strdup(context) : 0;
    self->mj_name    = name ? strdup(name) : 0;
    self->mj_handle  = handle;
    self->mj_notify  = notify;
    self->mj_param   = param;
    self->mj_reply   = 0;

    mce_log(LL_DEBUG, "job(%s:%s) created", mce_job_context(self), mce_job_name(self));

    return self;
}

/* ========================================================================= *
 * MCE_JOBLIST
 * ========================================================================= */

static mce_job_t *
mce_joblist_pull(mce_joblist_t *self)
{
    mce_job_t *job = 0;

    if( !self )
        goto EXIT;

    if( !(job = self->mjl_head) )
        goto EXIT;

    if( !(self->mjl_head = job->mj_next) )
        self->mjl_tail = 0;
    job->mj_next = 0;

EXIT:
    return job;
}

static void
mce_joblist_push(mce_joblist_t *self, mce_job_t *job)
{
    if( !self || !job )
        goto EXIT;

    if( self->mjl_tail )
        self->mjl_tail->mj_next = job;
    else
        self->mjl_head = job;
    self->mjl_tail = job;

EXIT:
    return;
}

static void
mce_joblist_delete(mce_joblist_t *self)
{
    mce_job_t *job;

    if( !self )
        goto EXIT;

    while( (job = mce_joblist_pull(self)) )
        mce_job_delete(job);

EXIT:
    return;
}

static mce_joblist_t *
mce_joblist_create(void)
{
    mce_joblist_t *self = calloc(1, sizeof *self);

    self->mjl_head = 0;
    self->mjl_tail = 0;

    return self;
}

/* ========================================================================= *
 * MCE_WORKER
 * ========================================================================= */

static bool
mce_worker_has_context(const char *context)
{
    if( !mw_is_ready )
        return false;

    if( !context )
        return true;

    if( !mw_ctx_lut )
        return false;;

    return g_hash_table_lookup(mw_ctx_lut, context) != 0;
}

void
mce_worker_add_context(const char *context)
{
    if( !mw_is_ready )
        goto EXIT;

    if( !context )
        goto EXIT;

    if( !mw_ctx_lut )
        goto EXIT;

    pthread_mutex_lock(&mw_ctx_mutex);
    g_hash_table_replace(mw_ctx_lut, g_strdup(context), GINT_TO_POINTER(1));
    pthread_mutex_unlock(&mw_ctx_mutex);

    mce_log(LL_DEBUG, "%s: context enabled", context);

EXIT:
    return;
}

void
mce_worker_rem_context(const char *context)
{
    if( !mw_ctx_lut )
        goto EXIT;

    pthread_mutex_lock(&mw_ctx_mutex);
    g_hash_table_remove(mw_ctx_lut, context);
    pthread_mutex_unlock(&mw_ctx_mutex);

    mce_log(LL_DEBUG, "%s: context disabled", context);

EXIT:
    return;
}

static gboolean
mce_worker_notify_cb(GIOChannel *chn, GIOCondition cnd, gpointer data)
{
    (void)data;

    gboolean keep_going = FALSE;

    if( !mw_rsp_wid )
        goto cleanup_nak;

    int fd = g_io_channel_unix_get_fd(chn);

    if( fd < 0 )
        goto cleanup_nak;

    if( cnd & ~G_IO_IN )
        goto cleanup_nak;

    if( !(cnd & G_IO_IN) )
        goto cleanup_ack;

    uint64_t cnt = 0;

    int rc = read(fd, &cnt, sizeof cnt);

    if( rc == 0 ) {
        mce_log(LL_ERR, "unexpected eof");
        goto cleanup_nak;
    }

    if( rc == -1 ) {
        if( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK )
            goto cleanup_ack;

        mce_log(LL_ERR, "read error: %m");
        goto cleanup_nak;
    }

    if( rc != sizeof cnt )
        goto cleanup_nak;

    for( ;; ) {
        pthread_mutex_lock(&mw_rsp_mutex);
        mce_job_t *job = mce_joblist_pull(mw_rsp_list);
        pthread_mutex_unlock(&mw_rsp_mutex);

        if( !job )
            break;

        mce_job_notify(job);
        mce_job_delete(job);
    }

cleanup_ack:
    keep_going = TRUE;

cleanup_nak:

    if( !keep_going ) {
        mw_rsp_wid = 0;
        mce_log(LL_CRIT, "worker notifications disabled");
    }

    return keep_going;
}

static void mce_worker_execute(void)
{
    for( ;; ) {
        pthread_mutex_lock(&mw_req_mutex);
        mce_job_t *job = mce_joblist_pull(mw_req_list);
        pthread_mutex_unlock(&mw_req_mutex);

        if( !job )
            break;

        mce_job_execute(job);

        pthread_mutex_lock(&mw_rsp_mutex);
        mce_joblist_push(mw_rsp_list, job);
        pthread_mutex_unlock(&mw_rsp_mutex);

        uint64_t cnt = 1;
        write(mw_rsp_evfd, &cnt, sizeof cnt);
    }
}

static void *mce_worker_main(void *aptr)
{
    (void)aptr;

    /* Allow quick and dirty cancellation */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, 0);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, 0);

    for( ;; ) {
        uint64_t cnt = 0;
        int rc = read(mw_req_evfd, &cnt, sizeof cnt);

        if( rc == -1 ) {
            if( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK )
                continue;
            mce_log(LL_ERR, "read: %m");
            goto EXIT;
        }

        if( rc != sizeof cnt )
            continue;

        if( cnt > 0 )
            mce_worker_execute();
    }

EXIT:
    return 0;
}

void
mce_worker_add_job(const char *context, const char *name,
                   void *(*handle)(void *),
                   void (*notify)(void *, void *),
                   void *param)
{
    if( !mw_is_ready ) {
        mce_log(LL_ERR, "job(%s:%s) scheduled while not ready", context, name);
        goto EXIT;
    }

    mce_job_t *job = mce_job_create(context, name, handle, notify, param);
    pthread_mutex_lock(&mw_req_mutex);
    mce_joblist_push(mw_req_list, job);
    pthread_mutex_unlock(&mw_req_mutex);

    uint64_t cnt = 1;
    write(mw_req_evfd, &cnt, sizeof cnt);

EXIT:
    return;
}

void mce_worker_quit(void)
{
    /* No longer ready to accept jobs */
    mw_is_ready = false;

    /* Stop worker thread */

    if( mw_req_tid ) {
        if( pthread_cancel(mw_req_tid) != 0 ) {
            mce_log(LOG_ERR, "failed to stop worker thread");
        }
        else {
            void *status = 0;
            pthread_join(mw_req_tid, &status);
            mce_log(LOG_DEBUG, "worker stopped, status = %p", status);
        }
        mw_req_tid = 0;
    }

    /* Note: The worker thread is killed asynchronously, so it is
     *       possible that the mutexes are left in locked state
     *       and thus must not be used after this stage.
     */

    /* Remove request pipeline */

    mce_joblist_delete(mw_req_list),
        mw_req_list = 0;

    if( mw_req_evfd != -1 )
        close(mw_req_evfd), mw_req_evfd = -1;

    /* Remove notify pipeline */

    if( mw_rsp_wid )
        g_source_remove(mw_rsp_wid), mw_rsp_wid = 0;

    mce_joblist_delete(mw_rsp_list),
        mw_rsp_list = 0;

    if( mw_rsp_evfd != -1 )
        close(mw_rsp_evfd), mw_req_evfd = -1;

    /* Remove context lookup table */

    if( mw_ctx_lut )
        g_hash_table_unref(mw_ctx_lut), mw_ctx_lut = 0;
}

static void dummy_notify(void *aptr, void *reply)
{
    mce_log(LL_DEBUG, "aptr=%p reply=%p", aptr, reply);
}

static void *dummy_handle(void *aptr)
{
    mce_log(LL_DEBUG, "aptr=%p", aptr);
    return aptr;
}

bool mce_worker_init(void)
{
    bool ack = false;

    /* Setup context lookup table */

    mw_ctx_lut = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, 0);

    /* Setup notify pipeline */

    if( !(mw_rsp_list = mce_joblist_create()) )
        goto EXIT;

    if( (mw_rsp_evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) == -1 ) {
        goto EXIT;
    }

    mw_rsp_wid = mw_add_iowatch(mw_rsp_evfd, false, G_IO_IN,
                                mce_worker_notify_cb, 0);
    if( !mw_rsp_wid )
        goto EXIT;

    /* Setup request pipeline */

    if( !(mw_req_list = mce_joblist_create()) )
        goto EXIT;

    if( (mw_req_evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) == -1 ) {
        goto EXIT;
    }

    /* Start worker thread */
    if( pthread_create(&mw_req_tid, 0, mce_worker_main, 0) != 0 ) {
        mw_req_tid = 0;
        goto EXIT;
    }

    /* Note: From now on joblist access must use mutex locking */

    /* Ready to accept jobs */
    mw_is_ready = true;

    ack = true;

    mce_worker_add_job(0, "dummy", dummy_handle, dummy_notify, (void*)"helloworld");

EXIT:

    /* All or nothing */
    if( !ack )
        mce_worker_quit();

    return ack;
}

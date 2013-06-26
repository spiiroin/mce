/**
 * @file battery-statefs.c
 * Battery module -- this implements battery and charger logic for MCE
 * <p>
 * Copyright (C) 2013 Jolla Ltd.
 * <p>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * <p>
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
 *
 * <p>
 * Rough diagram of data/control flow within this module
 * @verbatim
 *
 *           .------.      .-------.
 *           |sfsctl|      |statefs|
 *           `------'      `-------'
 *              |              |
 *           .-------.    .--------.
 *           |tracker|.---|inputset|
 *           `-------'|.  `--------'
 *            `-------'|
 *             `-------'
 *                |
 *             .------.
 *             |sfsbat|
 *             `------'
 *                |
 *             .------.
 *             |mcebat|
 *             `------'
 *                |
 *           .---------.
 *           |datapipes|
 *           `---------'
 * @endverbatim
 */

#include "../mce.h"
#include "../mce-log.h"

#include <sys/types.h>
#include <sys/epoll.h>

#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <gmodule.h>

/* ------------------------------------------------------------------------- *
 * config
 * ------------------------------------------------------------------------- */

/** Delay between re-open attempts while statefs entries are missing; [ms] */
#define START_DELAY  (5 * 1000)

/** Delay from 1st property change to state machine update; [ms] */
#define UPDATE_DELAY 300

/** Delay from 1st property change to forced property re-read; [ms] */
#define REREAD_DELAY 50

/** Whether to support legacy pattery low led pattern; nonzero for yes */
#define SUPPORT_BATTERY_LOW_LED_PATTERN 0

/* ------------------------------------------------------------------------- *
 * misc utils
 * ------------------------------------------------------------------------- */

static bool parse_int(const char *data, int *res);
static bool parse_bool(const char *data, bool *res);

#define numof(a) (sizeof(a)/sizeof*(a))

/* ------------------------------------------------------------------------- *
 * generic epoll set as glib io watch input listener
 * ------------------------------------------------------------------------- */

static bool     inputset_init(bool (*input_cb)(struct epoll_event *, int));
static void     inputset_quit(void);
static bool     inputset_insert(int fd, void *data);
static void     inputset_remove(int fd);

static gboolean inputset_watch_cb(GIOChannel *srce, GIOCondition cond,
                                  gpointer data);

/* ------------------------------------------------------------------------- *
 * struct StatefsBattery holds battery data available from statefs
 * ------------------------------------------------------------------------- */

/** Battery properties available via statefs */
typedef struct
{
    bool OnBattery;
    bool IsCharging;
    bool LowBattery;
    int  TimeUntilFull;
    int  TimeUntilLow;
    int  ChargePercentage;
} StatefsBattery;

static void        sfsbat_init(void);

/* ------------------------------------------------------------------------- *
 * struct MceBattery holds mce legacy compatible battery data
 * ------------------------------------------------------------------------- */

/** Battery properties in mce statemachine compatible form */
typedef struct
{
    /** Battery charge percentage; for use with battery_level_pipe */
    int         level;

    /** Battery FULL/OK/LOW/EMPTY; for use with battery_status_pipe */
    int         status;

    /** Charger connected; for use with charger_state_pipe */
    bool        charger;
} MceBattery;

static void     mcebat_init(void);
static void     mcebat_update_from_sfsbat(void);
static gboolean mcebat_update_cb(gpointer user_data);
static void     mcebat_update_cancel(void);
static void     mcebat_update_schedule(void);

/* ------------------------------------------------------------------------- *
 * struct StatefsTracker binds statefs file to StatefsBattery member
 * ------------------------------------------------------------------------- */

typedef struct StatefsTracker StatefsTracker;

/** Bind statefs file to member of StatefsBattery structure */
struct StatefsTracker
{
    /** Basename of the input file */
    const char *name;

    /** Path to input file, set at tracker_init() / sfsctl_init() */
    char       *path;

    /** Pointer to a StatefsBattery member */
    void       *value;

    /** Value type specific input parser hook */
    bool      (*update_cb)(StatefsTracker *, const char *);

    /** File descriptor for the input file */
    int         fd;

    /** For use with debugging with pipes instead of real statefs */
    bool        seekable;
};

static const char *tracker_propdir(void);

static void tracker_init(StatefsTracker *self);
static void tracker_quit(StatefsTracker *self);
static bool tracker_open(StatefsTracker *self);
static void tracker_close(StatefsTracker *self);
static bool tracker_start(StatefsTracker *self);
static bool tracker_read_data(StatefsTracker *self, char *data, size_t size);
static bool tracker_parse_int(StatefsTracker *self, const char *data);
static bool tracker_parse_bool(StatefsTracker *self, const char *data);
static bool tracker_update(StatefsTracker *self);

/* ------------------------------------------------------------------------- *
 * controls for statefs tracking
 * ------------------------------------------------------------------------- */

static void     sfsctl_init(void);
static void     sfsctl_quit(void);

static void     sfsctl_start(void);
static bool     sfsctl_start_try(void);
static gboolean sfsctl_start_cb(gpointer aptr);

static bool     sfsctl_watch_cb(struct epoll_event *eve, int cnt);

static void     sfsctl_schedule_reread(void);
static void     sfsctl_cancel_reread(void);
static gboolean sfsctl_reread_cb(gpointer aptr);

/* ------------------------------------------------------------------------- *
 * misc utils
 * ------------------------------------------------------------------------- */

/** String to int helper
 *
 * @param data text to parse
 * @param res  where to store parsed value
 *
 * @return true if data could be parse and stored to res, false otherwise
 */
static bool
parse_int(const char *data, int *res)
{
    char *pos = (char *)data;
    int   val = strtol(pos, &pos, 0);

    if( pos == data || *pos != 0 )
        return false;

    return *res = val, true;
}

/** String to bool helper
 *
 * @param data text to parse
 * @param res  where to store parsed value
 *
 * @return true if data could be parse and stored to res, false otherwise
 */
static bool
parse_bool(const char *data, bool *res)
{
    if( !strcmp(data, "true") )
        return *res = true, true;

    if( !strcmp(data, "false") )
        return *res = false, true;

    return false;
}

/* ------------------------------------------------------------------------- *
 * generic epoll set as glib io watch input listener
 * ------------------------------------------------------------------------- */

/** epoll fd for tracking a set of input files */
static int inputset_epoll_fd = -1;

/** glib io watch for inputset_epoll_fd */
static guint inputset_watch_id = 0;

/** Handle statefs change notifications received via epoll set
 *
 * @param srce (not used)
 * @param cond wakeup reason
 * @param data event handler function as void pointer
 *
 * @return FALSE if the io watch must be disabled, TRUE otherwise
 */
static gboolean
inputset_watch_cb(GIOChannel *srce, GIOCondition cond, gpointer data)
{
    (void)srce;

    gboolean keep_going = TRUE;

    struct epoll_event eve[16];

    if( cond & ~G_IO_IN ) {
        mce_log(LL_ERR, "unexpected io cond: 0x%x", (unsigned)cond);
        keep_going = FALSE;
    }

    int rc = epoll_wait(inputset_epoll_fd, eve, numof(eve), 0);

    if( rc == -1 ) {
        switch( errno ) {
        case EINTR:
        case EAGAIN:
            break;

        default:
            mce_log(LL_ERR, "statfs io wait: %m");
            keep_going = FALSE;
        }
        goto cleanup;
    }

    bool (*input_cb)(struct epoll_event *, int) = data;

    if( !input_cb(eve, rc) )
        keep_going = FALSE;

cleanup:

    if( !keep_going ) {
        mce_log(LL_CRIT, "disabling statfs io watch");
        inputset_watch_id = 0;
    }

    return keep_going;
}

/** Initialize epoll set and io watch for it
 *
 * @param input_cb event handler function
 *
 * @return true on success, false on failure
 */
static bool
inputset_init(bool (*input_cb)(struct epoll_event *, int))
{
    bool success = false;
    GIOChannel *chn = 0;

    if( (inputset_epoll_fd = epoll_create1(EPOLL_CLOEXEC)) == -1 ) {
        mce_log(LL_WARN, "epoll_create: %m");
        goto cleanup;
    }

    if( !(chn = g_io_channel_unix_new(inputset_epoll_fd)) )
        goto cleanup;

    g_io_channel_set_close_on_unref(chn, FALSE);

    inputset_watch_id = g_io_add_watch(chn, G_IO_IN, inputset_watch_cb, input_cb);
    if( !inputset_watch_id )
        goto cleanup;

    success = true;

cleanup:

    if( chn )
        g_io_channel_unref(chn);

    if( !success )
        inputset_quit();

    return success;
}

/** Remove epoll set and io watch for it
 */
static void
inputset_quit(void)
{
    if( inputset_watch_id )
        g_source_remove(inputset_watch_id), inputset_watch_id = 0;

    if( inputset_epoll_fd != -1 )
        close(inputset_epoll_fd), inputset_epoll_fd = -1;
}

/** Add tracking object to epoll set
 *
 * @param fd   input file descriptor
 * @param data data to associate with the fd
 *
 * @return true on success, false on failure
 */
static bool
inputset_insert(int fd, void *data)
{
    bool success = false;

    struct epoll_event eve;

    if( fd == -1 )
        goto cleanup;

    //mce_log(LL_DEBUG, "%s(%s)", __FUNCTION__, self->name);

    memset(&eve, 0, sizeof eve);
    eve.events = EPOLLIN;
    eve.data.ptr = data;

    int rc = epoll_ctl(inputset_epoll_fd, EPOLL_CTL_ADD, fd, &eve);

    if( rc == -1 ) {
        mce_log(LL_WARN, "EPOLL_CTL_ADD(%d): %m", fd);
        goto cleanup;
    }

    success = true;

cleanup:

    return success;
}

/** Remove tracking object from epoll set
 *
 * @param fd   input file descriptor
 */
static void
inputset_remove(int fd)
{
    if( fd == -1 )
        return;

    //mce_log(LL_DEBUG, "%s(%s)", __FUNCTION__, self->name);

    if( epoll_ctl(inputset_epoll_fd, EPOLL_CTL_DEL, fd, 0) == -1 )
        mce_log(LL_WARN, "EPOLL_CTL_DEL(%d): %m", fd);
}

/* ------------------------------------------------------------------------- *
 * struct StatefsBattery
 * ------------------------------------------------------------------------- */

/** Battery status, as available via statefs */
static StatefsBattery sfsbat;

/** Provide intial guess of statefs battery status
 */
static void
sfsbat_init(void)
{
    memset(&sfsbat, 0, sizeof sfsbat);
    sfsbat.OnBattery        = true;
    sfsbat.IsCharging       = false;
    sfsbat.LowBattery       = false;
    sfsbat.TimeUntilFull    = 123;
    sfsbat.TimeUntilLow     = 321;
    sfsbat.ChargePercentage = 50;
}

/* ------------------------------------------------------------------------- *
 * struct MceBattery
 * ------------------------------------------------------------------------- */

/** Timer for processing battery status changes */
static guint mcebat_update_id = 0;

/** Current battery status in mce legacy compatible form */
static MceBattery mcebat;

/** Provide intial guess of mce battery status
 */
static void
mcebat_init(void)
{
    memset(&mcebat, 0, sizeof mcebat);
    mcebat.level   = 50;
    mcebat.status  = BATTERY_STATUS_UNDEF;
    mcebat.charger = false;
}

/** Update mce battery status from statefs battery data
 */
static void
mcebat_update_from_sfsbat(void)
{
    mcebat.level = sfsbat.ChargePercentage;

    mcebat.charger = sfsbat.IsCharging || !sfsbat.OnBattery;

    if( sfsbat.LowBattery ) {
        mcebat.status = BATTERY_STATUS_LOW;

        /* TODO: Using arbitrary limit of 5% for "battery empty"
         *       within mce, but really this should come directly
         *       from the component that knows that we're doomed */
        if( sfsbat.OnBattery && sfsbat.ChargePercentage < 5 )
            mcebat.status = BATTERY_STATUS_EMPTY;
    }
    else {
        mcebat.status = BATTERY_STATUS_OK;

        /* TODO: Using "battery full" derived from several
         *       values can cause state machine hiccups ... */
        if( mcebat.charger && sfsbat.TimeUntilFull == 0 )
            mcebat.status = BATTERY_STATUS_FULL;
    }
}

/** Process accumulated statefs battery status changes
 *
 * @param user_data (not used)
 *
 * @return FALSE (to stop timer from repeating)
 */
static gboolean
mcebat_update_cb(gpointer user_data)
{
    (void)user_data;

    if( !mcebat_update_id )
        return FALSE;

    mce_log(LL_INFO, "----( state machine )----");

    /* Get a copy of current status */
    MceBattery prev = mcebat;

    /* Update from statefs based information */
    mcebat_update_from_sfsbat();

    /* Process changes */
    if( mcebat.charger != prev.charger ) {
        mce_log(LL_INFO, "charger: %d -> %d", prev.charger, mcebat.charger);

        /* Charger connected state */
        execute_datapipe(&charger_state_pipe, GINT_TO_POINTER(mcebat.charger),
                         USE_INDATA, CACHE_INDATA);

        /* Charging led pattern */
        if( mcebat.charger ) {
            execute_datapipe_output_triggers(&led_pattern_activate_pipe,
                                             MCE_LED_PATTERN_BATTERY_CHARGING,
                                             USE_INDATA);
        }
        else {
            execute_datapipe_output_triggers(&led_pattern_deactivate_pipe,
                                             MCE_LED_PATTERN_BATTERY_CHARGING,
                                             USE_INDATA);
        }

        /* Generate activity */
        (void)execute_datapipe(&device_inactive_pipe, GINT_TO_POINTER(FALSE),
                               USE_INDATA, CACHE_INDATA);
    }

    if( mcebat.status != prev.status ) {
        mce_log(LL_INFO, "status: %d -> %d", prev.status, mcebat.status);

        /* Battery full led pattern */
        if( mcebat.status == BATTERY_STATUS_FULL ) {
            execute_datapipe_output_triggers(&led_pattern_activate_pipe,
                                             MCE_LED_PATTERN_BATTERY_FULL,
                                             USE_INDATA);
        }
        else {
            execute_datapipe_output_triggers(&led_pattern_deactivate_pipe,
                                             MCE_LED_PATTERN_BATTERY_FULL,
                                             USE_INDATA);
        }

#if SUPPORT_BATTERY_LOW_LED_PATTERN
        /* Battery low led pattern */
        if( mcebat.status == BATTERY_STATUS_LOW ||
            mcebat.status == BATTERY_STATUS_EMPTY ) {
            execute_datapipe_output_triggers(&led_pattern_activate_pipe,
                                             MCE_LED_PATTERN_BATTERY_LOW,
                                             USE_INDATA);
        }
        else {
            execute_datapipe_output_triggers(&led_pattern_deactivate_pipe,
                                             MCE_LED_PATTERN_BATTERY_LOW,
                                             USE_INDATA);
        }
#endif /* SUPPORT_BATTERY_LOW_LED_PATTERN */

        /* Battery charge state */
        execute_datapipe(&battery_status_pipe,
                         GINT_TO_POINTER(mcebat.status),
                         USE_INDATA, CACHE_INDATA);

    }

    if( mcebat.level != prev.level ) {
        mce_log(LL_INFO, "level: %d -> %d", prev.level, mcebat.level);

        /* Battery charge percentage */
        execute_datapipe(&battery_level_pipe,
                         GINT_TO_POINTER(mcebat.level),
                         USE_INDATA, CACHE_INDATA);
    }

    /* Clear the timer id and do not repeat */
    return mcebat_update_id = 0, FALSE;
}

/** Cancel processing of statefs battery status changes
 */
static void
mcebat_update_cancel(void)
{
    if( mcebat_update_id )
        g_source_remove(mcebat_update_id), mcebat_update_id = 0;
}

/** Initiate delayed processing of statefs battery status changes
 */
static void
mcebat_update_schedule(void)
{
    if( !mcebat_update_id )
        mcebat_update_id = g_timeout_add(UPDATE_DELAY, mcebat_update_cb, 0);
}

/* ------------------------------------------------------------------------- *
 * struct StatefsTracker
 * ------------------------------------------------------------------------- */

/** Locate directory where battery properties are
 */
static const char *
tracker_propdir(void)
{
    // TODO: system statefs is not available yet, try the one for "nemo" user
    static const char def[] = "/run/user/100000/state/namespaces/Battery";

    static char *res = 0;

    if( !res ) {
        // TODO: debug stuff, remove later
        const char *env = getenv("BATTERY_BASEDIR");
        res = strdup(env ?: def);
    }
    return res;
}

/** Read string from statefs input file
 *
 * @param self statefs input file tracking object
 * @param data buffer where to read to
 * @param size length of the buffer
 *
 * @return true on success, or false in case of errors
 */
static bool
tracker_read_data(StatefsTracker *self, char *data, size_t size)
{
    bool  res = false;
    int   rc;

    if( self->fd == -1 )
        goto cleanup;

    if( (rc = read(self->fd, data, size-1)) == -1 ) {
        mce_log(LL_WARN, "%s: read: %m", self->path);
        goto cleanup;
    }

    if( self->seekable && lseek(self->fd, 0, SEEK_SET) == -1 ) {
        mce_log(LL_WARN, "%s: rewind: %m", self->path);
        goto cleanup;
    }

    data[rc] = 0, res = true;

    data[strcspn(data, "\r\n")] = 0;

    mce_log(LL_DEBUG, "%s: read '%s'", self->name, data);

cleanup:

    return res;
}

/** Parse statefs file content to int value
 *
 * @param self statefs input file tracking object
 * @param data string to parse
 *
 * @return true if data could be read and the value changed, false otherwise
 */
static bool
tracker_parse_int(StatefsTracker *self, const char *data)
{
    int  *now = self->value;
    int   zen = *now;

    if( !parse_int(data, &zen) ) {
        mce_log(LL_WARN, "%s: can't convert '%s' to int", self->name, data);
        return false;
    }

    if( *now == zen )
        return false;

    mce_log(LL_INFO, "%s: %d -> %d", self->name, *now, zen);
    return *now = zen, true;
}

/** Parse statefs file content to bool value
 *
 * @param self statefs input file tracking object
 * @param data string to parse
 *
 * @return true if data could be read and the value changed, false otherwise
 */
static bool
tracker_parse_bool(StatefsTracker *self, const char *data)
{
    bool *now = self->value;
    bool  zen = *now;

    if( !parse_bool(data, &zen) ) {
        mce_log(LL_WARN, "%s: can't convert '%s' to bool", self->name, data);
        return false;
    }

    if( *now == zen )
        return false;

    mce_log(LL_INFO, "%s: %s -> %s", self->name,
            *now ? "true" : "false",
            zen ? "true" : "false");

    return *now = zen, true;
}

/** Update value from statefs content and schedule state machine update
 *
 * @param self statefs input file tracking object
 *
 * @return true if io was successfull, but the value did not change;
 *         false otherwise
 */
static bool
tracker_update(StatefsTracker *self)
{
    bool dummy = false; // assume: io failed or value changed

    char data[64];

    if( !tracker_read_data(self, data, sizeof data) ) {
        tracker_close(self);
        goto cleanup;
    }

    if( self->update_cb(self, data) )
        mcebat_update_schedule();
    else
        dummy = true; // io succeesfull, but value did not change

cleanup:

    return dummy;
}

/** Open statefs file
 *
 * @param self statefs input file tracking object
 *
 * @return true if file is open, false otherwise
 */
static bool
tracker_open(StatefsTracker *self)
{
    bool success = true;

    if( self->fd == -1 ) {
        self->seekable = false;

        mce_log(LL_DEBUG, "%s(%s)", __FUNCTION__, self->name);

        self->fd = open(self->path, O_RDONLY);
        if( self->fd == -1 ) {
            mce_log(LL_ERR, "%s: open: %m", self->path);
            success = false;
        }

        if( lseek(self->fd, 0, SEEK_CUR) != -1 )
            self->seekable = true;
    }

    return success;
}

/** Close statefs file
 *
 * @param self statefs input file tracking object
 */
static void
tracker_close(StatefsTracker *self)
{
    if( self->fd != -1 ) {
        mce_log(LL_DEBUG, "%s(%s)", __FUNCTION__, self->name);
        inputset_remove(self->fd);
        close(self->fd), self->fd = -1;
    }
}

/** Initialize StatefsTracker dynamic data
 *
 * @param self statefs input file tracking object
 */
static void
tracker_init(StatefsTracker *self)
{
    self->path = g_strdup_printf("%s/%s", tracker_propdir(), self->name);
}

/** Release dynamic resources associated with StatefsTracker
 *
 * @param self statefs input file tracking object
 */
static void
tracker_quit(StatefsTracker *self)
{
    tracker_close(self);

    free(self->path), self->path = 0;
}

/** Start tracking statefs property file
 *
 * @param self statefs input file tracking object
 *
 * @return true if statefs file is open and tracked, false otherwise
 */
static bool
tracker_start(StatefsTracker *self)
{
    if( self->fd != -1 )
        return true;

    if( !tracker_open(self) )
        return false;

    mce_log(LL_DEBUG, "%s(%s)", __FUNCTION__, self->name);
    tracker_update(self);

    if( !inputset_insert(self->fd, self) ) {
        tracker_close(self);
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------------- *
 * Controls for statefs tracking
 * ------------------------------------------------------------------------- */

/** Initializer macro for int properties */
#define INIT_PROP_INT(NAME)\
     {\
         .name      = #NAME,\
         .path      = 0,\
         .value     = &sfsbat.NAME,\
         .update_cb = tracker_parse_int,\
         .fd        = -1,\
     }

/** Initializer macro for bool properties */
#define INIT_PROP_BOOL(NAME)\
     {\
        .name      = #NAME,\
        .path      = 0,\
        .value     = &sfsbat.NAME,\
        .update_cb = tracker_parse_bool,\
        .fd        = -1,\
    }

/** Lookup table for statefs based properties */
static StatefsTracker sfsctl_props[] =
{
    INIT_PROP_BOOL(IsCharging),
    INIT_PROP_BOOL(OnBattery),
    INIT_PROP_BOOL(LowBattery),

    INIT_PROP_INT(ChargePercentage),
    INIT_PROP_INT(TimeUntilFull),
    INIT_PROP_INT(TimeUntilLow),

    // sentinel
    { .name = 0, }
};

/** timeout for waiting statefs to come available */
static guint sfsctl_start_id = 0;

/** timeout for handling missed epoll io notifications */
static guint sfsctl_reread_id = 0;

/** Initialize dynamic data for statefs tracking objects
 */
static void
sfsctl_init(void)
{
    for( StatefsTracker *prop = sfsctl_props; prop->name; ++prop )
        tracker_init(prop);
}

/** Stop statefs change tracking
 */
static void
sfsctl_quit(void)
{
    if( sfsctl_start_id )
        g_source_remove(sfsctl_start_id), sfsctl_start_id = 0;

    for( StatefsTracker *prop = sfsctl_props; prop->name; ++prop )
        tracker_quit(prop);
}

/** Helper for starting/restarting statefs change tracking
 *
 * @return true if all properties could be bound to statefs files
 */
static bool
sfsctl_start_try(void)
{
    bool success = true;

    mce_log(LL_INFO, "---- connect ----");

    for( StatefsTracker *prop = sfsctl_props; prop->name; ++prop ) {
        if( !tracker_start(prop) )
            success = false;
    }

    return success;
}

/** Timeout for retrying start of statefs change tracking
 *
 * @return FALSE on success (to stop the timer), or
 *         TRUE on failure (to keep timer repeating)
 */
static gboolean
sfsctl_start_cb(gpointer aptr)
{
    (void)aptr;
    if( !sfsctl_start_id ) {
        /* The timer was already cancelled */
        return FALSE;
    }

    if( !sfsctl_start_try() ) {
        /* Failed, keep timer active for another try */
        return TRUE;
    }

    /* Success, disable timer */
    return sfsctl_start_id = 0, FALSE;
}

/** Start statefs change tracking
 *
 * If all properties are not available immediately, retry
 * timer will be started
 */
static void
sfsctl_start(void)
{
    if( sfsctl_start_id ) {
        /* Already have an active retry timer */
        return;
    }

    if( sfsctl_start_try() ) {
        /* Success, no need for retry timer */
        return;
    }

    /* Try again later */
    sfsctl_start_id = g_timeout_add(START_DELAY,
                                    sfsctl_start_cb,
                                    0);
}

/** Handle statefs change notifications received via epoll set
 *
 * @param eve array of epoll events
 * @param cnt number of epoll events
 *
 * @return false if io watch should be disabled, otherwise true
 */
static bool
sfsctl_watch_cb(struct epoll_event *eve, int cnt)
{
    bool keep_going   = true;
    bool statefs_lost = false;

    mce_log(LL_INFO, "----( process %d events )----", cnt);

    for( int i = 0; i < cnt; ++i ) {
        StatefsTracker *prop = eve[i].data.ptr;

        if( eve[i].events & ~EPOLLIN )
            tracker_close(prop), statefs_lost = true;
        else
            tracker_update(prop);
    }

    /* HACK: Depending on kernel & fuse versions there are
     *       varying problems with epoll wakeups. It is
     *       possible that we get woken up, but do not
     *       receive events identifying the input file
     *       with changed content. To overcome this we
     *       schedule forced re-read of all battery
     *       properties if we get any kind of wakeup from
     *       epoll fd */
    sfsctl_schedule_reread();

    if( statefs_lost ) {
        /* ASSUME: Loss of inputs == statefs restart */

        /* Start timer based re-open attempts */
        sfsctl_start();

        /* Forced re-read makes no sense, cancel it */
        sfsctl_cancel_reread();
    }

    return keep_going;
}

/** Timeout for forced re-read of statefs properties
 *
 * @param aptr (not used)
 *
 * @return FALSE (to stop the timer from repeating)
 */
static gboolean
sfsctl_reread_cb(gpointer aptr)
{
    (void)aptr;

    if( !sfsctl_reread_id )
        return FALSE;

    mce_log(LL_INFO, "----( forced update )----");

    for( StatefsTracker *prop = sfsctl_props; prop->name; ++prop )
        tracker_update(prop);

    return sfsctl_reread_id = 0, FALSE;
}

/** Cancel forced re-read of statefs properties
 */
static void
sfsctl_cancel_reread(void)
{
    if( sfsctl_reread_id )
        g_source_remove(sfsctl_reread_id), sfsctl_reread_id = 0;
}

/** Schedule forced re-read of statefs properties
 */
static void
sfsctl_schedule_reread(void)
{
    if( sfsctl_reread_id )
        return;

    sfsctl_reread_id = g_timeout_add(REREAD_DELAY, sfsctl_reread_cb, 0);
}

/** Stop battery/charging tracking
 */
static void
battery_quit(void)
{
    /* stop statefs change tracking */
    sfsctl_quit();

    /* cancel pending state machine updates */
    mcebat_update_cancel();

    /* cancel pending property re-reads */
    sfsctl_cancel_reread();

    /* remove epoll input listener */
    inputset_quit();
}

/** Start battery/charging tracking
 *
 * @return true on success, false otherwise
 */
static bool
battery_init(void)
{
    bool success = false;

    /* initialize epoll input listener */
    if( !inputset_init(sfsctl_watch_cb) )
        goto cleanup;

    /* reset data used by the state machine */
    mcebat_init();

    /* reset data that should come from statefs */
    sfsbat_init();

    /* initialize statefs paths etc */
    sfsctl_init();

    /* start statefs change tracking */
    sfsctl_start();

    success = true;

cleanup:

    return success;
}

/** Module name */
#define MODULE_NAME "battery_statefs"

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Module information */
G_MODULE_EXPORT module_info_struct module_info =
{
    /** Name of the module */
    .name = MODULE_NAME,
    /** Module provides */
    .provides = provides,
    /** Module priority */
    .priority = 100
};

/** Init function for the battery and charger module
 *
 * @todo XXX status needs to be set on error!
 *
 * @param module Unused
 *
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    if( !battery_init() )
        mce_log(LL_WARN, MODULE_NAME" module initialization failed");
    else
        mce_log(LL_INFO, MODULE_NAME" module initialized ");

    return NULL;
}

/** Exit function for the battery and charger module
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
    (void)module;

    battery_quit();
}

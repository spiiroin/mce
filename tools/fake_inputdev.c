/**
 * @file fake_inputdev.c
 *
 * Mode Control Entity - Tool for debugging dynamic input devices
 *
 * <p>
 *
 * Copyright (c) 2026 Jolla Mobile Ltd
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

#include <linux/input.h>
#include <linux/uinput.h>

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define log_err(FMT, ARGS...) fprintf(stderr, "E: " FMT "\n", ##ARGS)

/* ========================================================================= *
 * Types
 * ========================================================================= */

#define MAX_CODES 16

typedef struct {
    const char *idt_name;
    uint16_t    idt_vendor_id;
    uint16_t    idt_product_id;
    int16_t     idt_event_codes[EV_CNT][MAX_CODES];
    int16_t     idt_switch_on[MAX_CODES];
} DevConfig;

/* ========================================================================= *
 * Fakedev
 * ========================================================================= */

static int
fakedev_array_length(const int16_t *arr)
{
    if( arr && (arr[0] || arr[1]) )
        for( int idx = 0; idx < MAX_CODES ; ++idx )
            if( arr[idx] < 0 )
                return idx;
    return 0;
}

static void
fakedev_send_event(int fd, int type, int code, int value)
{
    struct input_event eve = {
        .type = type,
        .code = code,
        .value = value,
    };
    if( write(fd, &eve, sizeof eve) == -1 )
        log_err("failed to write event: %m");
}

static void
fakedev_toggle_switches(const DevConfig *conf, int fd)
{
    int type  = EV_SW;
    int value = 1;
    int count = fakedev_array_length(conf->idt_switch_on);

    for( int idx = 0; idx < count; ++idx ) {
        int code = conf->idt_switch_on[idx];
        fakedev_send_event(fd, type, code, value);
    }
    fakedev_send_event(fd, EV_SYN, SYN_REPORT, 0);
}

static bool
fakedev_configure_device(const DevConfig *conf, int fd)
{
    bool ack = false;

    for( int type = 0; type < EV_CNT; ++type ) {
        int count = fakedev_array_length(conf->idt_event_codes[type]);
        if( count <= 0 )
            continue;

        // Set event types and codes
        ioctl(fd, UI_SET_EVBIT, type);

        for( int idx = 0; idx < count; ++idx ) {
            int code = conf->idt_event_codes[type][idx];
            switch( type ) {
            case EV_SYN:
                // N/A
                break;
            case EV_ABS:
                ioctl(fd, UI_SET_ABSBIT, code);
                break;
            case EV_KEY:
                ioctl(fd, UI_SET_KEYBIT, code);
                break;
            case EV_REL:
                ioctl(fd, UI_SET_RELBIT, code);
                break;
            case EV_MSC:
                ioctl(fd, UI_SET_MSCBIT, code);
                break;
            case EV_SW:
                ioctl(fd, UI_SET_SWBIT, code);
                break;
            case EV_LED:
                ioctl(fd, UI_SET_LEDBIT, code);
                break;
            case EV_SND:
                ioctl(fd, UI_SET_SNDBIT, code);
                break;
            case EV_REP:
                // todo
                break;
            case EV_FF:
                ioctl(fd, UI_SET_FFBIT, code);
                break;
            case EV_PWR:
                // todo
                break;
            case EV_FF_STATUS:
                // todo
                break;
            default:
                // dontcare
                break;
            }
        }
    }

    struct uinput_user_dev dev = {};

    strncpy(dev.name, conf->idt_name, sizeof dev.name - 1);

    dev.id.bustype = BUS_VIRTUAL;
    dev.id.vendor  = conf->idt_vendor_id;
    dev.id.product = conf->idt_product_id;
    dev.id.version = 1;

    for( int code = 0; code < ABS_CNT; ++code ) {
        dev.absmin[code]  = 0;
        dev.absmax[code]  = 255;
        dev.absfuzz[code] = 0;
        dev.absflat[code] = 0;
    }

    if( write(fd, &dev, sizeof(dev)) == -1 ) {
        log_err("writing uinput config block failed: %m");
        goto EXIT;
    }

    ack = true;

EXIT:
    return ack;
}

static void
fakedev_remove_device(int fd)
{
    if( fd != -1 ) {
        if( ioctl(fd, UI_DEV_DESTROY) != -1 )
            printf("Device removed\n");
        close(fd);
    }
}

static int
fakedev_create_device(const DevConfig *conf)
{
    bool ack = false;
    int  fd  = -1;

    if( (fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK)) == -1 ) {
        log_err("failed to open /dev/uinput: %m");
        goto EXIT;
    }

    if( !fakedev_configure_device(conf, fd) )
        goto EXIT;

    if( ioctl(fd, UI_DEV_CREATE) == -1 ) {
        log_err("UI_DEV_CREATE failed: %m");
        goto EXIT;
    }

    fakedev_toggle_switches(conf, fd);

    printf("Device created: %s\n", conf->idt_name);
    ack = true;

EXIT:
    if( !ack )
        fakedev_remove_device(fd), fd = -1;

    return fd;
}

/* ========================================================================= *
 * Signals
 * ========================================================================= */

static volatile int running = 1;

static const int signal_list[] = {
    SIGINT,
    SIGTERM,
    -1
};

static void
signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

static void
signal_init(void)
{
    for( size_t i = 0; signal_list[i] != -1; ++i )
        signal(signal_list[i], signal_handler);
}

static void
signal_quit(void)
{
    for( size_t i = 0; signal_list[i] != -1; ++i )
        signal(signal_list[i], SIG_DFL);
}

/* ========================================================================= *
 * Application
 * ========================================================================= */

static const DevConfig conf_lut[] = {
    {
        .idt_name        = "fake als device",
        .idt_vendor_id   = 0x1234,
        .idt_product_id  = 0x5678,
        .idt_event_codes = {
            [EV_ABS] = { ABS_MISC, -1 },
        },
    },
    {
        .idt_name        = "fake ps device",
        .idt_vendor_id   = 0x1234,
        .idt_product_id  = 0x5678,
        .idt_event_codes = {
            [EV_ABS] = { ABS_DISTANCE, -1 },
        },
    },
    {
        .idt_name        = "fake front proximity device",
        .idt_vendor_id   = 0x1234,
        .idt_product_id  = 0x5678,
        .idt_event_codes = {
            [EV_SW] = { SW_FRONT_PROXIMITY, -1 },
        },
        //.idt_switch_on = { SW_FRONT_PROXIMITY, SW_LID, -1 },
    },
    {
        .idt_name = NULL,
    }
};

static const struct option optL[] = {
    { "template", required_argument, 0,  't' },
    { "timeout",  required_argument ,0,  'T' },
    { NULL, }
};

static const char optS[] = "t:T:";

int
main(int argc, char *argv[])
{
    int xc = EXIT_FAILURE;

    const char *opt_template = NULL;
    const char *opt_timeout  = NULL;

    const DevConfig *conf = NULL;

    int timeout   = -1;
    int device_fd = -1;

    setlinebuf(stdout);
    setlinebuf(stderr);

    if( argc == 1 ) {
        printf("available templates:\n");
        for( size_t i = 0; conf_lut[i].idt_name; ++i )
            printf("[%zd] %s\n", i, conf_lut[i].idt_name);
        goto SUCCESS;
    }

    for( ;; ) {
        int opt = getopt_long(argc, argv, optS, optL, NULL);
        if( opt == -1 )
            break;
        switch( opt ) {
        case 't':
            opt_template = optarg;
            break;
        case 'T':
            opt_timeout = optarg;
            break;

        case '?':
            goto FAILURE;

        default:
            log_err("unhandled option code %d", opt);
            goto FAILURE;
        }
    }

    if (optind < argc) {
        printf("non-option ARGV-elements: ");
        while (optind < argc)
            printf("%s ", argv[optind++]);
        printf("\n");
        goto FAILURE;
    }

    if( opt_timeout )
        timeout = atoi(opt_timeout);

    if( opt_template ) {
        for( size_t i = 0; conf_lut[i].idt_name; ++i ) {
            if( strstr(conf_lut[i].idt_name, opt_template) ) {
                conf = &conf_lut[i];
                break;
            }
        }
    }

    if( !conf ) {
        if( opt_template )
            log_err("template %s not found", opt_template);
        else
            log_err("template not specified");
        goto FAILURE;
    }

    if( (device_fd = fakedev_create_device(conf)) == -1 )
        goto FAILURE;

    printf("Device active. Press ctrl-c to exit%s\n",
           timeout > 0 ? " or wait for timeout..." : "");

    signal_init();

    while( running && timeout-- )
            sleep(1);

    signal_quit();

SUCCESS:
    xc = EXIT_SUCCESS;

FAILURE:
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);

    fakedev_remove_device(device_fd);

    return xc;
}

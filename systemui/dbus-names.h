/**
 * @file dbus-names.h
 * D-Bus Interface for communicating with SystemUI
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012-2019 Jolla Ltd.
 *
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
 * @author Vesa Halttunen <vesa.halttunen@jollamobile.com>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * @author Andrew den Exter <andrew.den.exter@jolla.com>
 *
 * These headers are free software; you can redistribute them
 * and/or modify them under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation.
 *
 * These headers are distributed in the hope that they will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mce.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef  SYSTEMUI_DBUS_NAMES_H_
# define SYSTEMUI_DBUS_NAMES_H_

# if 0
/* - - - - - - - - - - - - - - - - - - - *
 * SAILFISH
 * - - - - - - - - - - - - - - - - - - - */

/** The System UI service */
#  define SYSTEMUI_SERVICE         "org.nemomobile.lipstick"

/** The System UI request interface. */
#  define SYSTEMUI_REQUEST_IF      "org.nemomobile.lipstick.screenlock"

/** The System UI request path. */
#  define SYSTEMUI_REQUEST_PATH    "/screenlock"

/** The device lock service */
#  define DEVICELOCK_SERVICE       "org.nemomobile.devicelock"

/** The device lock request interface. */
#  define DEVICELOCK_REQUEST_IF    "org.nemomobile.lipstick.devicelock"

/** The device lock request path. */
#  define DEVICELOCK_REQUEST_PATH  "/devicelock"

# else
/* - - - - - - - - - - - - - - - - - - - *
 * MAEMO
 * - - - - - - - - - - - - - - - - - - - */

/* systemui/dbus-names.h */
#  define SYSTEMUI_SERVICE           "com.nokia.system_ui"
#  define SYSTEMUI_REQUEST_IF        "com.nokia.system_ui.request"
#  define SYSTEMUI_SIGNAL_IF         "com.nokia.system_ui.signal"
#  define SYSTEMUI_REQUEST_PATH      "/com/nokia/system_ui/request"
#  define SYSTEMUI_SIGNAL_PATH       "/com/nokia/system_ui/signal"
#  define SYSTEMUI_QUIT_REQ          "quit"
#  define SYSTEMUI_STARTED_SIG       "system_ui_started"

// QUARANTINE /* include/systemui/tklock-dbus-names.h */
// QUARANTINE #  define SYSTEMUI_TKLOCK_OPEN_REQ   "tklock_open"
// QUARANTINE #  define SYSTEMUI_TKLOCK_CLOSE_REQ  "tklock_close"
// QUARANTINE #  define TKLOCK_SIGNAL_IF           "com.nokia.tklock.signal"
// QUARANTINE #  define TKLOCK_SIGNAL_PATH         "/com/nokia/tklock/signal"
// QUARANTINE #  define TKLOCK_MM_KEY_PRESS_SIG    "mm_key_press"

#  define DEVICELOCK_SERVICE         SYSTEMUI_SERVICE
#  define DEVICELOCK_REQUEST_IF      SYSTEMUI_REQUEST_IF
#  define DEVICELOCK_REQUEST_PATH    SYSTEMUI_REQUEST_PATH

# endif

#endif /* SYSTEMUI_DBUS_NAMES_H_ */

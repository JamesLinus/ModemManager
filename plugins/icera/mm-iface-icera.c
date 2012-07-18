/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2010 - 2012 Red Hat, Inc.
 * Copyright (C) 2012 Aleksander Morgado <aleksander@gnu.org>
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "ModemManager.h"
#include "mm-log.h"
#include "mm-modem-helpers.h"
#include "mm-iface-icera.h"
#include "mm-base-modem-at.h"

/*****************************************************************************/
/* Load initial allowed/preferred modes (Modem interface) */

gboolean
mm_iface_icera_modem_load_allowed_modes_finish (MMIfaceModem *self,
                                                GAsyncResult *res,
                                                MMModemMode *allowed,
                                                MMModemMode *preferred,
                                                GError **error)
{
    const gchar *response;
    const gchar *str;
    gint mode, domain;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
    if (!response)
        return FALSE;

    str = mm_strip_tag (response, "%IPSYS:");

    if (!sscanf (str, "%d,%d", &mode, &domain)) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Couldn't parse %%IPSYS response: '%s'",
                     response);
        return FALSE;
    }

    switch (mode) {
    case 0:
        *allowed = MM_MODEM_MODE_2G;
        *preferred = MM_MODEM_MODE_NONE;
        return TRUE;
    case 1:
        *allowed = MM_MODEM_MODE_3G;
        *preferred = MM_MODEM_MODE_NONE;
        return TRUE;
    case 2:
        *allowed = (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G);
        *preferred = MM_MODEM_MODE_2G;
        return TRUE;
    case 3:
        *allowed = (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G);
        *preferred = MM_MODEM_MODE_3G;
        return TRUE;
    case 5: /* any */
        *allowed = (MM_MODEM_MODE_CS | MM_MODEM_MODE_2G | MM_MODEM_MODE_3G);
        *preferred = MM_MODEM_MODE_NONE;
        return TRUE;
    default:
        break;
    }

    g_set_error (error,
                 MM_CORE_ERROR,
                 MM_CORE_ERROR_FAILED,
                 "Couldn't parse unexpected %%IPSYS response: '%s'",
                 response);
    return FALSE;
}

void
mm_iface_icera_modem_load_allowed_modes (MMIfaceModem *self,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data)
{
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "%IPSYS?",
                              3,
                              FALSE,
                              callback,
                              user_data);
}

/*****************************************************************************/
/* Set allowed modes (Modem interface) */

gboolean
mm_iface_icera_modem_set_allowed_modes_finish (MMIfaceModem *self,
                                               GAsyncResult *res,
                                               GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
allowed_mode_update_ready (MMBaseModem *self,
                           GAsyncResult *res,
                           GSimpleAsyncResult *operation_result)
{
    GError *error = NULL;

    mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (error)
        /* Let the error be critical. */
        g_simple_async_result_take_error (operation_result, error);
    else
        g_simple_async_result_set_op_res_gboolean (operation_result, TRUE);
    g_simple_async_result_complete (operation_result);
    g_object_unref (operation_result);
}

void
mm_iface_icera_modem_set_allowed_modes (MMIfaceModem *self,
                                        MMModemMode allowed,
                                        MMModemMode preferred,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data)
{
    GSimpleAsyncResult *result;
    gchar *command;
    gint icera_mode = -1;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        mm_iface_icera_modem_set_allowed_modes);

    /* There is no explicit config for CS connections, we just assume we may
     * have them as part of 2G when no GPRS is available */
    if (allowed & MM_MODEM_MODE_CS) {
        allowed |= MM_MODEM_MODE_2G;
        allowed &= ~MM_MODEM_MODE_CS;
    }

    /*
     * The core has checked the following:
     *  - that 'allowed' are a subset of the 'supported' modes
     *  - that 'preferred' is one mode, and a subset of 'allowed'
     */
    if (allowed == MM_MODEM_MODE_2G)
        icera_mode = 0;
    else if (allowed == MM_MODEM_MODE_3G)
        icera_mode = 1;
    else if (allowed == (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G)) {
        if (preferred == MM_MODEM_MODE_2G)
            icera_mode = 2;
        else if (preferred == MM_MODEM_MODE_3G)
            icera_mode = 3;
        else /* none preferred, so AUTO */
            icera_mode = 5;
    }

    if (icera_mode < 0) {
        gchar *allowed_str;
        gchar *preferred_str;

        allowed_str = mm_modem_mode_build_string_from_mask (allowed);
        preferred_str = mm_modem_mode_build_string_from_mask (preferred);
        g_simple_async_result_set_error (result,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_FAILED,
                                         "Requested mode (allowed: '%s', preferred: '%s') not "
                                         "supported by the modem.",
                                         allowed_str,
                                         preferred_str);
        g_free (allowed_str);
        g_free (preferred_str);

        g_simple_async_result_complete_in_idle (result);
        g_object_unref (result);
        return;
    }

    command = g_strdup_printf ("%%IPSYS=%d", icera_mode);
    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        command,
        3,
        FALSE,
        (GAsyncReadyCallback)allowed_mode_update_ready,
        result);
    g_free (command);
}

/*****************************************************************************/

static void
iface_icera_init (gpointer g_iface)
{
}

GType
mm_iface_icera_get_type (void)
{
    static GType iface_icera_type = 0;

    if (!G_UNLIKELY (iface_icera_type)) {
        static const GTypeInfo info = {
            sizeof (MMIfaceIcera), /* class_size */
            iface_icera_init,      /* base_init */
            NULL,                  /* base_finalize */
        };

        iface_icera_type = g_type_register_static (G_TYPE_INTERFACE,
                                                   "MMIfaceIcera",
                                                   &info,
                                                   0);

        g_type_interface_add_prerequisite (iface_icera_type, MM_TYPE_IFACE_MODEM);
    }

    return iface_icera_type;
}
